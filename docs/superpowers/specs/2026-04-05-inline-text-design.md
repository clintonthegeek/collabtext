# Inline Fragment Text — Design Spec

**Date:** 2026-04-05
**Status:** Design ready
**Prereqs:** Gen 2 complete, GC implemented, realistic testing in place

---

## 1. Motivation

The dominant performance bottleneck is the O(n*m) origin-interval-lookup in
`set_fragments()`. For every fragment in the new list (n), it linearly scans
all old fragments (m) to locate the fragment's text in two separate Rope
objects. This is called on every edit — local, remote, and undo.

With 3,000 fragments, each edit scans 3,000 old entries × 3,000 new entries =
9 million iterations. This puts us at 8 ops/sec on a 5K document with 50%
tombstones, and 17 ops/sec for 3-client realistic editing.

The competitive engines (diamond-types, cola, Loro) all store text inline in
their tree entries. This is Phase 1 of a two-phase performance refactor:

- **Phase 1 (this spec):** Inline text in fragments, eliminate Ropes from
  Buffer. Turns set_fragments from O(n*m) to O(n).
- **Phase 2 (future):** In-place SumTree mutations. Turns the O(n) full
  rebuild into O(log n) per edit.

---

## 2. Core Change: Fragment Gains Text

### 2.1 Fragment struct

```cpp
struct Fragment {
    Lamport origin;
    Locator locator;
    uint32_t byte_length = 0;
    uint32_t length = 0;
    std::vector<Lamport> deletions;
    bool visible = true;
    std::string text;  // NEW: owns its content
};
```

`byte_length` is kept for use in FragmentSummary (visible_bytes, deleted_bytes
aggregation) and to avoid touching every summary consumer. Debug assertion:
`assert(byte_length == text.size())`.

The constructor gains a text parameter:

```cpp
Fragment(Lamport orig, Locator loc, uint32_t byte_len, uint32_t char_len,
         std::string text)
    : origin(orig), locator(loc), byte_length(byte_len), length(char_len),
      text(std::move(text)) {}
```

The existing 4-argument constructor is kept for backward compatibility with
test code that doesn't need text (fragments created for comparison/search).

### 2.2 Memory Impact

std::string uses small-string optimization (typically 15-22 bytes inline).
Single-character ASCII fragments (the common case after normalization) incur
zero heap allocation. Total text memory is the same as before — it moves from
two Ropes to inline in fragments.

---

## 3. Eliminate Ropes from Buffer

### 3.1 Removed Members

```cpp
// REMOVED from Buffer:
Rope m_visible_text;
Rope m_deleted_text;
```

The `#include "crdt/Rope.h"` is removed from Buffer.h. The Rope class itself
and its tests (tst_rope.cpp, tst_rope_integration.cpp) are kept — they
validate SumTree cursor operations independently.

### 3.2 set_fragments() — Simplified

Both overloads collapse into one. The O(n*m) origin-interval-lookup is
eliminated entirely:

```cpp
void Buffer::set_fragments(std::vector<Fragment>&& frags) {
    for (auto& f : frags)
        f.visible = f.compute_visible(m_undo_map);
    rebuild_insertion_index(frags);
    FragmentTree tree;
    for (auto& f : frags) {
        assert(f.byte_length == f.text.size());
        tree.push_item(std::move(f));
    }
    m_fragment_tree = std::move(tree);
}
```

The second overload (`set_fragments(frags, new_texts)`) is removed. All
callers are updated to set `fragment.text` directly before calling
set_fragments.

### 3.3 extract_fragment_text() — Removed

This private helper extracted text from ropes by walking preceding fragments
to compute byte offsets. Callers now read `fragment.text` directly.

### 3.4 text() — Walks Fragments

```cpp
std::string Buffer::text() const {
    std::string result;
    result.reserve(m_fragment_tree.summary().visible_bytes);
    m_fragment_tree.for_each([&](const Fragment& f) {
        if (f.visible) result += f.text;
    });
    return result;
}
```

O(n) in fragment count + O(visible_bytes) in string ops. Not on the edit
hot path.

### 3.5 visible_rope_len() / deleted_rope_len()

Renamed conceptually but keep the same public signature for test compat:

```cpp
uint32_t Buffer::visible_rope_len() const {
    return m_fragment_tree.summary().visible_bytes;
}
uint32_t Buffer::deleted_rope_len() const {
    return m_fragment_tree.summary().deleted_bytes;
}
```

### 3.6 anchor_at() / resolve_anchor()

These currently extract text from `m_visible_text` for char↔byte conversion.
Updated to read from `fragment.text` directly:

```cpp
// In anchor_at(), replace:
//   std::string ftext = m_visible_text.substr(vis_rope_pos, f.byte_length);
// With:
//   const std::string& ftext = f.text;

// Same change in resolve_anchor().
```

The `vis_rope_pos` tracking variable is eliminated.

---

## 4. Call Site Changes

### 4.1 apply_local_edit()

When creating fragments for inserted text, set `fragment.text`:

```cpp
Fragment frag(origin, locator, byte_len, char_len, content_substring);
```

The `new_texts` map construction at the end is removed. The call changes from
`set_fragments(frags, new_texts)` to `set_fragments(std::move(frags))`.

When building the new tree via cursor slicing (prefix/suffix copy), fragments
from the old tree already carry their text — no change needed for those.

### 4.2 apply_remote_edit()

Insertions: set `fragment.text = ins.content`.

Split relocations: `split_fragment_at()` handles splitting the text (see 4.5).

The `new_texts` map and `extract_fragment_text()` calls are removed.

### 4.3 apply_remote_undo()

No text involvement. The simplified set_fragments just recomputes visibility.

### 4.4 normalize_fragments()

Signature changes — remove the `new_texts` parameter:

```cpp
void normalize_fragments(std::vector<Fragment>& frags) const;
```

When atomizing multi-character fragments at shared locators, read from
`fragment.text` directly instead of scanning old ropes:

```cpp
// For each character in the multi-char fragment:
Fragment single;
single.text = ftext.substr(byte_pos, char_bytes);
single.byte_length = char_bytes;
// ...
```

The nested O(n*m) bottleneck in normalize_fragments disappears.

### 4.5 split_fragment_at()

Splits the text string along with the fragment:

```cpp
size_t Buffer::split_fragment_at(std::vector<Fragment>& frags,
                                  size_t frag_idx, uint32_t byte_off) const {
    Fragment& f = frags[frag_idx];
    Fragment second;
    second.text = f.text.substr(byte_off);
    second.byte_length = static_cast<uint32_t>(second.text.size());
    // ... existing logic for origin, locator, length, deletions ...
    f.text = f.text.substr(0, byte_off);
    f.byte_length = static_cast<uint32_t>(f.text.size());
    // ... insert second after f ...
}
```

### 4.6 sweep_and_coalesce()

Pass 1 (GC removal): No change — fragments carry their text, removed
fragments' text is simply discarded.

Pass 2 (coalescing): Concatenate texts directly:

```cpp
prev.text += curr.text;
prev.byte_length += curr.byte_length;
prev.length += curr.length;
```

The rope extraction loop and `new_texts` map are eliminated. The second
set_fragments call uses the simplified single-overload version.

### 4.7 resolve_visible_offset()

No rope involvement — this works with fragment byte_lengths and the fragment
vector. No change needed.

---

## 5. File Map

| File | Action | Change |
|------|--------|--------|
| `libs/collabtext/src/crdt/Fragment.h` | Modify | Add `std::string text` field, update constructor |
| `libs/collabtext/src/crdt/Buffer.h` | Modify | Remove Rope includes/members, remove extract_fragment_text, simplify set_fragments signature, remove new_texts from normalize_fragments |
| `libs/collabtext/src/crdt/Buffer.cpp` | Modify | Major: rewrite set_fragments, update all call sites, eliminate rope usage |
| `libs/collabtext/CMakeLists.txt` | No change | Rope.h is header-only, not in source list. Rope tests kept. |

---

## 6. What NOT To Change

- **SumTree** — no changes. Still build-once-from-vector (Phase 2 scope).
- **InsertionIndex** — no text involvement, no change.
- **Locator, Clock, UndoMap** — no change.
- **Rope.h** — kept as-is. Removed from Buffer but file preserved for its
  test coverage of SumTree cursor operations.
- **tst_rope.cpp, tst_rope_integration.cpp** — kept as-is.
- **Existing test files** — tst_fuzz.cpp, tst_gc.cpp, tst_realistic.cpp,
  tst_benchmark.cpp, etc. all use the public Buffer API and should pass
  unchanged.
- **NetworkSim, EditStrategy** — no change.

---

## 7. Expected Performance Impact

The O(n*m) inner loop in set_fragments disappears. What remains is O(n):

- Visibility computation: O(n)
- InsertionIndex rebuild: O(n)
- SumTree construction from vector: O(n)

For 3,000 fragments: current = ~9M iterations (3K × 3K), new = ~3K iterations.
Expected 1,000x improvement on the set_fragments path alone.

The O(n) full rebuild remains (Phase 2 scope), but the constant factor drops
dramatically since we're not scanning old entries or walking UTF-8 bytes per
fragment.

---

## 8. Success Criteria

- All 14 existing test targets pass unchanged
- tst_realistic 5 stability runs pass (random seeds)
- tst_fuzz 5 stability runs pass
- Benchmark numbers improve measurably (expect 10-100x on tombstone-heavy
  scenarios)
- No new test files needed — this is a pure internal refactor
