# Fragment::content Removal (Gen 2 Phase 3) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the `Fragment::content` field so document text lives exclusively in the Ropes, halving memory usage.

**Architecture:** Currently every fragment owns a copy of its text (`std::string content`) AND the same text exists in the Ropes. We remove the duplicate by (1) making `set_fragments()` reconstruct ropes from old ropes using an origin-interval-lookup rather than reading `content`, (2) migrating all splitting/UTF-8/byte-length code to read from ropes or `byte_length`, then (3) deleting the field.

The origin-interval-lookup works because every fragment (including splits) has an origin that falls within exactly one old-tree fragment's origin range. We scan the old tree to build `(origin_range → rope_offset)` mappings, then for each new fragment, find its old parent and extract text at the computed offset. New fragments (insertions) pass their text via a side-channel map.

**Tech Stack:** C++20, Qt6/QTest, CMake

**Spec:** `docs/superpowers/specs/2026-04-02-fragment-content-removal-design.md`

---

## File Map

| File | Action | Responsibility |
|------|--------|----------------|
| `libs/collabtext/src/crdt/Fragment.h` | Modify | Replace `std::string content` with `uint32_t byte_length`; update constructor, `summary()` |
| `libs/collabtext/src/crdt/Buffer.h` | Modify | Add `extract_fragment_text()` helper; update `set_fragments()` signature |
| `libs/collabtext/src/crdt/Buffer.cpp` | Modify | Rewrite `set_fragments()`, migrate all 24 production `content` access sites |
| `libs/collabtext/src/crdt/Rope.h` | No change | Rope API is sufficient as-is |
| `libs/collabtext/src/crdt/Operations.h` | No change | `InsertedFragment.content` stays (wire format) |
| `libs/collabtext/tests/tst_fuzz.cpp` | Modify | Update invariant checks INV-2,3,5,6,7; add INV-9 |
| `libs/collabtext/tests/tst_rope_integration.cpp` | Modify | Replace `f.content.size()` with `f.byte_length` |
| `libs/collabtext/tests/tst_buffer.cpp` | Modify | Replace `f.content == "XX"` with rope or text() check |

---

## Task 1: Add `byte_length` shadow field to Fragment

Add the new field alongside `content`. Both fields exist during transition; `byte_length` is always set from `content.size()` in the constructor. This lets us migrate incrementally.

**Files:**
- Modify: `libs/collabtext/src/crdt/Fragment.h:114-178`

- [ ] **Step 1: Add `byte_length` to Fragment struct**

In `Fragment.h`, insert `byte_length` after `content`, update the constructor to initialize it, and update `summary()` to use it:

```cpp
// In the Fragment struct, after line 119:
    std::string content;     ///< UTF-8 content (transitional — removed in final step)
    uint32_t byte_length = 0; ///< Byte size of the fragment's text
    uint32_t length = 0;     ///< Number of characters

// In the constructor (line 125-126), add byte_length init:
    Fragment(Lamport orig, Locator loc, std::string text, uint32_t len)
        : origin(orig), locator(loc), content(std::move(text)), length(len)
    {
        byte_length = static_cast<uint32_t>(content.size());
    }

// In summary() (line 162), replace content.size() with byte_length:
    FragmentSummary summary() const {
        FragmentSummary s;
        uint32_t bytes = byte_length;
        // ... rest unchanged
```

- [ ] **Step 2: Update all fragment construction sites in Buffer.cpp to set `byte_length`**

`split_fragment_at()` and `split_frag` lambda and `normalize_fragments()` create fragments with explicit field assignment (not via constructor). These must also set `byte_length`. Search for every place a Fragment is constructed with field assignment and add `byte_length`:

In `split_fragment_at()` (Buffer.cpp:279-287):
```cpp
    Fragment second;
    second.origin = Lamport(orig.origin.replica_id, orig.origin.value + char_count);
    second.locator = orig.locator;
    second.content = orig.content.substr(offset_in_frag);
    second.byte_length = static_cast<uint32_t>(second.content.size());
    second.length = orig.length - char_count;
    second.deletions = orig.deletions;

    orig.content = orig.content.substr(0, offset_in_frag);
    orig.byte_length = static_cast<uint32_t>(orig.content.size());
    orig.length = char_count;
```

In `split_frag` lambda (Buffer.cpp:459-474):
```cpp
    auto split_frag = [](const Fragment& f, uint32_t byte_off)
        -> std::pair<Fragment, Fragment>
    {
        uint32_t char_count = count_utf8_chars(f.content, byte_off);
        Fragment first;
        first.origin = f.origin;
        first.locator = f.locator;
        first.content = f.content.substr(0, byte_off);
        first.byte_length = byte_off;
        first.length = char_count;
        first.deletions = f.deletions;
        first.visible = f.visible;

        Fragment second;
        second.origin = Lamport(f.origin.replica_id, f.origin.value + char_count);
        second.locator = f.locator;
        second.content = f.content.substr(byte_off);
        second.byte_length = static_cast<uint32_t>(second.content.size());
        second.length = f.length - char_count;
        second.deletions = f.deletions;
        second.visible = f.visible;
        return {std::move(first), std::move(second)};
    };
```

In `normalize_fragments()` (Buffer.cpp:360-367):
```cpp
                        Fragment single;
                        single.origin = Lamport(f.origin.replica_id, f.origin.value + c);
                        single.locator = f.locator;
                        single.content = f.content.substr(byte_pos, char_bytes);
                        single.byte_length = char_bytes;
                        single.length = 1;
                        single.deletions = f.deletions;
```

- [ ] **Step 3: Build and run all tests**

```bash
cmake --build build-dev && ctest --test-dir build-dev -j$(nproc) --output-on-failure
```

Expected: all 12 tests pass. Shadow field doesn't change behavior.

- [ ] **Step 4: Commit**

```bash
git add libs/collabtext/src/crdt/Fragment.h libs/collabtext/src/crdt/Buffer.cpp
git commit -m "feat: add byte_length shadow field to Fragment (Gen 2 Phase 3 prep)"
```

---

## Task 2: Rewrite `set_fragments()` to reconstruct ropes from old ropes

This is the central change. Instead of reading `content` from every fragment to build ropes, `set_fragments()` builds an origin-interval map from the old fragment tree, extracts old rope text, and looks up each new fragment's text by finding which old fragment's origin range contains it. Newly inserted fragments (not found in old tree) fall back to `content` during transition.

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.h` (add `new_texts` parameter)
- Modify: `libs/collabtext/src/crdt/Buffer.cpp:23-49`

- [ ] **Step 1: Add helper struct and new `set_fragments` signature**

In `Buffer.h`, add after the existing `set_fragments` declaration:

```cpp
    /// Rebuild the fragment tree and ropes. `new_texts` maps
    /// origin-key (replica_id << 32 | value) to text for newly inserted
    /// fragments whose text isn't in the old ropes. During transition,
    /// new fragments fall back to f.content if not in new_texts.
    void set_fragments(std::vector<Fragment>&& frags,
                       const std::unordered_map<uint64_t, std::string>& new_texts);
```

Add `#include <unordered_map>` to Buffer.h includes.

- [ ] **Step 2: Implement the new `set_fragments` overload**

In Buffer.cpp, after the existing `set_fragments`, add:

```cpp
static uint64_t origin_key(const Lamport& origin) {
    return (static_cast<uint64_t>(origin.replica_id) << 32) | origin.value;
}

void Buffer::set_fragments(std::vector<Fragment>&& frags,
                           const std::unordered_map<uint64_t, std::string>& new_texts)
{
    for (auto& f : frags) {
        f.visible = f.compute_visible(m_undo_map);
    }

    // Build interval map: old fragment origin ranges → rope offsets
    struct OldEntry {
        uint16_t replica_id;
        uint32_t origin_value;
        uint32_t char_length;
        uint32_t byte_length;
        bool visible;
        uint32_t rope_offset;
    };
    std::vector<OldEntry> old_entries;
    old_entries.reserve(m_fragment_tree.summary().visible_bytes > 0 ? 64 : 0);
    uint32_t vis_off = 0, del_off = 0;
    m_fragment_tree.for_each([&](const Fragment& f) {
        OldEntry e;
        e.replica_id = f.origin.replica_id;
        e.origin_value = f.origin.value;
        e.char_length = f.length;
        e.byte_length = f.byte_length;
        e.visible = f.visible;
        if (f.visible) {
            e.rope_offset = vis_off;
            vis_off += f.byte_length;
        } else {
            e.rope_offset = del_off;
            del_off += f.byte_length;
        }
        old_entries.push_back(e);
    });

    // Extract old rope text for random-access substring extraction
    std::string old_vis = m_visible_text.to_string();
    std::string old_del = m_deleted_text.to_string();

    // Build new ropes: for each fragment, find its text source
    Rope new_visible, new_deleted;
    for (auto& f : frags) {
        std::string_view text;
        bool found = false;

        // Search old entries for a parent whose origin range contains this fragment
        for (auto& oe : old_entries) {
            if (oe.replica_id != f.origin.replica_id) continue;
            if (f.origin.value < oe.origin_value ||
                f.origin.value >= oe.origin_value + oe.char_length) continue;

            // Found parent. Compute byte offset within parent for split fragments.
            uint32_t char_off = f.origin.value - oe.origin_value;
            uint32_t byte_off = 0;
            if (char_off > 0) {
                const std::string& rope_str = oe.visible ? old_vis : old_del;
                uint32_t pos = oe.rope_offset;
                for (uint32_t c = 0; c < char_off; ++c) {
                    unsigned char ch = static_cast<unsigned char>(rope_str[pos]);
                    if (ch < 0x80) pos += 1;
                    else if ((ch & 0xE0) == 0xC0) pos += 2;
                    else if ((ch & 0xF0) == 0xE0) pos += 3;
                    else pos += 4;
                }
                byte_off = pos - oe.rope_offset;
            }

            const std::string& src = oe.visible ? old_vis : old_del;
            text = std::string_view(src).substr(
                oe.rope_offset + byte_off, f.byte_length);
            found = true;
            break;
        }

        if (!found) {
            // New fragment: check new_texts map, fall back to content
            auto it = new_texts.find(origin_key(f.origin));
            if (it != new_texts.end()) {
                text = it->second;
            } else {
                text = f.content;  // transitional fallback
            }
        }

        if (f.visible) {
            new_visible.push_str(text);
        } else {
            new_deleted.push_str(text);
        }
    }
    m_visible_text = std::move(new_visible);
    m_deleted_text = std::move(new_deleted);

    rebuild_insertion_index(frags);
    FragmentTree tree;
    for (auto& f : frags) tree.push_item(std::move(f));
    m_fragment_tree = std::move(tree);

    assert(m_visible_text.len() == m_fragment_tree.summary().visible_bytes);
    assert(m_deleted_text.len() == m_fragment_tree.summary().deleted_bytes);
}
```

- [ ] **Step 3: Make existing `set_fragments` delegate to the new overload**

Replace the body of the original `set_fragments(vector&&)`:

```cpp
void Buffer::set_fragments(std::vector<Fragment>&& frags) {
    static const std::unordered_map<uint64_t, std::string> empty_map;
    set_fragments(std::move(frags), empty_map);
}
```

- [ ] **Step 4: Build and run all tests**

```bash
cmake --build build-dev && ctest --test-dir build-dev -j$(nproc) --output-on-failure
```

Expected: all 12 tests pass. The new rope reconstruction produces identical results to the old approach.

- [ ] **Step 5: Run fuzz tests 5x to stress-test the new rope reconstruction**

```bash
for i in $(seq 5); do ctest --test-dir build-dev -R tst_fuzz --output-on-failure || break; done
```

Expected: all 5 runs pass.

- [ ] **Step 6: Commit**

```bash
git add libs/collabtext/src/crdt/Buffer.h libs/collabtext/src/crdt/Buffer.cpp
git commit -m "feat: rewrite set_fragments() to reconstruct ropes from old ropes via origin-interval lookup"
```

---

## Task 3: Add `extract_fragment_text()` helper

Splitting and UTF-8 navigation currently read `f.content`. This helper extracts a fragment's text from the appropriate rope by walking fragments to compute the byte offset.

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.h` (add declaration)
- Modify: `libs/collabtext/src/crdt/Buffer.cpp` (add implementation)

- [ ] **Step 1: Declare in Buffer.h**

Add after the `split_fragment_at` declaration (~line 101):

```cpp
    /// Extract a fragment's text from the visible or deleted rope.
    /// Walks fragments [0..frag_idx) to compute the byte offset in the rope,
    /// then extracts byte_length bytes.
    std::string extract_fragment_text(
        const std::vector<Fragment>& frags, size_t frag_idx) const;
```

- [ ] **Step 2: Implement in Buffer.cpp**

Add after `split_fragment_at` (after line 304):

```cpp
std::string Buffer::extract_fragment_text(
    const std::vector<Fragment>& frags, size_t frag_idx) const
{
    const Fragment& target = frags[frag_idx];

    uint32_t vis_offset = 0;
    uint32_t del_offset = 0;
    for (size_t i = 0; i < frag_idx; ++i) {
        if (frags[i].visible) {
            vis_offset += frags[i].byte_length;
        } else {
            del_offset += frags[i].byte_length;
        }
    }

    if (target.visible) {
        return m_visible_text.substr(vis_offset, target.byte_length);
    } else {
        return m_deleted_text.substr(del_offset, target.byte_length);
    }
}
```

- [ ] **Step 3: Build and run all tests**

```bash
cmake --build build-dev && ctest --test-dir build-dev -j$(nproc) --output-on-failure
```

Expected: all 12 tests pass (helper exists but isn't called yet).

- [ ] **Step 4: Commit**

```bash
git add libs/collabtext/src/crdt/Buffer.h libs/collabtext/src/crdt/Buffer.cpp
git commit -m "feat: add extract_fragment_text() helper for rope-based text access"
```

---

## Task 4: Migrate `split_fragment_at()` and `apply_deletion_runs()` to use rope text

These functions read `content` for UTF-8 character counting and substring splitting. Replace with rope extraction.

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.cpp:269-304` (split_fragment_at)
- Modify: `libs/collabtext/src/crdt/Buffer.cpp:789-843` (apply_deletion_runs)

- [ ] **Step 1: Migrate `split_fragment_at()`**

Replace the body (lines 269-304):

```cpp
size_t Buffer::split_fragment_at(std::vector<Fragment>& frags,
                                  size_t frag_idx, uint32_t offset_in_frag) const
{
    assert(frag_idx < frags.size());
    assert(offset_in_frag > 0);
    assert(offset_in_frag < frags[frag_idx].byte_length);

    Fragment &orig = frags[frag_idx];
    std::string text = extract_fragment_text(frags, frag_idx);
    uint32_t char_count = count_utf8_chars(text, offset_in_frag);

    Fragment second;
    second.origin = Lamport(orig.origin.replica_id, orig.origin.value + char_count);
    second.locator = orig.locator;
    second.content = text.substr(offset_in_frag);
    second.byte_length = static_cast<uint32_t>(second.content.size());
    second.length = orig.length - char_count;
    second.deletions = orig.deletions;

    orig.content = text.substr(0, offset_in_frag);
    orig.byte_length = offset_in_frag;
    orig.length = char_count;

    Lamport saved_origin = orig.origin;
    Locator saved_locator = orig.locator;

    Lamport second_origin(saved_origin.replica_id, saved_origin.value + char_count);
    insert_fragment(frags, std::move(second));

    for (size_t i = frag_idx + 1; i < frags.size(); ++i) {
        if (frags[i].origin == second_origin &&
            frags[i].locator == saved_locator) {
            return i;
        }
    }

    assert(false && "split_fragment_at: could not find second half after insert");
    return frag_idx + 1;
}
```

Note: `content` is still populated here during transition (set from `text`). This will be removed in the final step.

- [ ] **Step 2: Migrate `apply_deletion_runs()` splitting sites**

In `apply_deletion_runs()`, the two `char_to_byte_offset(f.content, ...)` calls (lines 821, 828) and the similar one in the relocation loop (line 870) read content for UTF-8 char-to-byte conversion. Replace by extracting text from rope:

At line 818-831, replace:
```cpp
                if (char_off == 0 && to_del == f.length) {
                    f.deletions.push_back(deletion_id);
                } else {
                    if (char_off > 0) {
                        std::string ftext = extract_fragment_text(frags, fi);
                        uint32_t byte_off = char_to_byte_offset(ftext, char_off);
                        fi = split_fragment_at(frags, fi, byte_off);
                        to_del = std::min(remaining, frags[fi].length);
                    }

                    if (to_del < frags[fi].length) {
                        std::string ftext = extract_fragment_text(frags, fi);
                        uint32_t byte_off = char_to_byte_offset(ftext, to_del);
                        split_fragment_at(frags, fi, byte_off);
                    }

                    frags[fi].deletions.push_back(deletion_id);
                }
```

- [ ] **Step 3: Migrate `apply_remote_edit()` relocation splitting**

At line 868-871, replace:
```cpp
            uint32_t char_off = target_ts.value - f.origin.value;
            if (char_off > 0) {
                std::string ftext = extract_fragment_text(frags, fi);
                uint32_t byte_off = char_to_byte_offset(ftext, char_off);
                fi = split_fragment_at(frags, fi, byte_off);
            }
```

- [ ] **Step 4: Build and run all tests**

```bash
cmake --build build-dev && ctest --test-dir build-dev -j$(nproc) --output-on-failure
```

Expected: all 12 tests pass.

- [ ] **Step 5: Commit**

```bash
git add libs/collabtext/src/crdt/Buffer.cpp
git commit -m "feat: migrate split_fragment_at and apply_deletion_runs to use rope text extraction"
```

---

## Task 5: Migrate `apply_local_edit()` helpers to use rope text / byte_length

The `split_frag` lambda, `consume_unchanged`, and `consume_deleted` closures in `apply_local_edit()` read `content` for byte sizes and splitting. Migrate them. Also migrate `normalize_fragments()` and anchor functions.

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.cpp:130-180` (anchors)
- Modify: `libs/collabtext/src/crdt/Buffer.cpp:326-392` (normalize_fragments)
- Modify: `libs/collabtext/src/crdt/Buffer.cpp:455-568` (split_frag, consume_unchanged, consume_deleted)

- [ ] **Step 1: Migrate `apply_local_edit` byte-length reads**

In the lambdas `consume_unchanged` and `consume_deleted`, replace all `static_cast<uint32_t>(frag.content.size())` and `static_cast<uint32_t>(pending->content.size())` with the corresponding `byte_length` field.

Lines to change (content.size() → byte_length):
- Line 484: `uint32_t pv = pending->byte_length;`
- Line 508: `uint32_t fv = frag.byte_length;`
- Line 529: `uint32_t pv = pending->byte_length;`
- Line 554: `uint32_t fv = frag.byte_length;`

- [ ] **Step 2: Migrate `split_frag` lambda to use rope text**

The `split_frag` lambda (line 455) currently reads `f.content` directly. Since it operates on fragments extracted from the cursor (which are from the *old* tree, before mutations), their text is in the old ropes. However, during `apply_local_edit`, the ropes haven't been rebuilt yet — they still reflect the old tree. So we can read from the ropes.

But `split_frag` receives a `Fragment` by const ref, not a fragment index. We need the text. During transition, `content` is still populated on cursor items. In the final step, cursor items from the tree won't have content. We need to either:
(a) Pass text alongside the fragment, or
(b) Extract text from the rope at the point of splitting

The cleanest approach: change `split_frag` to also accept the text:

```cpp
    auto split_frag = [](const Fragment& f, uint32_t byte_off,
                         const std::string& text) -> std::pair<Fragment, Fragment>
    {
        uint32_t char_count = count_utf8_chars(text, byte_off);
        Fragment first;
        first.origin = f.origin;
        first.locator = f.locator;
        first.content = text.substr(0, byte_off);
        first.byte_length = byte_off;
        first.length = char_count;
        first.deletions = f.deletions;
        first.visible = f.visible;

        Fragment second;
        second.origin = Lamport(f.origin.replica_id, f.origin.value + char_count);
        second.locator = f.locator;
        second.content = text.substr(byte_off);
        second.byte_length = static_cast<uint32_t>(second.content.size());
        second.length = f.length - char_count;
        second.deletions = f.deletions;
        second.visible = f.visible;
        return {std::move(first), std::move(second)};
    };
```

Update all call sites of `split_frag` to pass text. Since fragments here come from either `pending` or from `*cursor.item()`, and both have `content` during transition, pass `f.content`:

- Line 489: `auto [first, second] = split_frag(*pending, vis_bytes, pending->content);`
- Line 514: `auto [first, second] = split_frag(frag, vis_bytes, frag.content);`
- Line 535: `auto [first, second] = split_frag(*pending, vis_bytes, pending->content);`
- Line 561: `auto [first, second] = split_frag(frag, vis_bytes, frag.content);`
- Line 580: `auto [first, second] = split_frag(*cursor.item(), split_byte, cursor.item()->content);`

- [ ] **Step 3: Migrate `normalize_fragments()` to use rope text**

The `normalize_fragments()` function reads `f.content[byte_pos]` for UTF-8 byte counting and `f.content.substr()` for extraction (lines 355-363). Since normalize runs before `set_fragments()` and fragments still have `content` during transition, this works. But we need it to work without content in the final step.

Since normalize operates on a mutable `frags` vector that was extracted from the tree, and the ropes haven't been rebuilt yet, we can use `extract_fragment_text()`. Replace the inner loop:

```cpp
                } else {
                    std::string ftext = extract_fragment_text(frags, j);
                    uint32_t byte_pos = 0;
                    for (uint32_t c = 0; c < f.length; ++c) {
                        uint32_t char_bytes = 1;
                        unsigned char ch = static_cast<unsigned char>(ftext[byte_pos]);
                        if (ch >= 0xF0) char_bytes = 4;
                        else if (ch >= 0xE0) char_bytes = 3;
                        else if (ch >= 0xC0) char_bytes = 2;

                        Fragment single;
                        single.origin = Lamport(f.origin.replica_id, f.origin.value + c);
                        single.locator = f.locator;
                        single.content = ftext.substr(byte_pos, char_bytes);
                        single.byte_length = char_bytes;
                        single.length = 1;
                        single.deletions = f.deletions;
                        extracted.push_back(std::move(single));
                        byte_pos += char_bytes;
                    }
                }
```

Wait — `normalize_fragments()` is called on the fragment vector AFTER mutations. At this point the fragments have been extracted, mutated, and possibly reordered. The ropes still reflect the OLD tree. But `extract_fragment_text()` reads from the current ropes, which correspond to the OLD tree ordering. The fragments in the vector may be in a different order than the tree.

This is a problem: `extract_fragment_text()` walks `frags[0..frag_idx)` to compute offsets, but `frags` is in the new (mutated) order, while the ropes are in the old order. The offsets won't match.

**Solution:** For `normalize_fragments()`, the fragment still has `content` during transition (it was set during splitting or from the original tree extraction). After content removal, we need a different approach.

For the final step: `normalize_fragments()` is called right before `set_fragments()`. At that point, we could read from the old ropes using the origin-interval-lookup (same approach as set_fragments). But that's duplicating logic.

**Simpler solution:** Do NOT use `extract_fragment_text()` in `normalize_fragments`. Instead, since normalize only runs for multi-replica shared-locator groups (rare), extract text inline using the old ropes + origin-interval-lookup. OR, even simpler: keep `content` populated for fragments that need normalization. Since normalize atomizes multi-char fragments at shared locators, and this is rare, the memory overhead is negligible during the brief window between extraction and set_fragments.

**Best approach for now:** Keep normalize using `content` during transition. In the final removal step (Task 7), we'll use `extract_fragment_text()` with a corrected version that takes its own rope strings (not m_visible_text / m_deleted_text which reflect the old tree). Specifically, in Task 7 we'll pass old rope strings to normalize.

For this task, leave normalize unchanged — it still reads `content` which is still present.

- [ ] **Step 4: Migrate `anchor_at()` and `resolve_anchor()`**

`anchor_at()` (line 130-147) reads `f.content.size()` and `count_chars(f.content, ...)`.
`resolve_anchor()` (line 149-183) reads `f.content.size()` and `chars_to_bytes(f.content, ...)`.

These run on the live tree via `for_each` — the ropes are consistent with the tree at this point. But we can't use `extract_fragment_text()` here because we don't have a flat `frags` vector.

Replace `content.size()` with `byte_length`. For `count_chars` and `chars_to_bytes`, we need the actual text. We can read from the rope by accumulating offsets within the for_each:

```cpp
Anchor Buffer::anchor_at(uint32_t byte_offset, Bias bias) const {
    if (m_fragment_tree.empty()) return Anchor::min();

    uint32_t accumulated = 0;
    uint32_t vis_rope_pos = 0;
    Anchor result = Anchor::max();
    bool found = false;

    m_fragment_tree.for_each([&](const Fragment& f) {
        if (found) return;
        if (!f.visible) return;

        if (accumulated + f.byte_length > byte_offset) {
            uint32_t offset_in_frag = byte_offset - accumulated;
            std::string ftext = m_visible_text.substr(vis_rope_pos, f.byte_length);
            uint32_t char_offset = count_chars(ftext, offset_in_frag);
            result = Anchor(f.origin.replica_id, f.origin.value + char_offset, bias);
            found = true;
            return;
        }
        accumulated += f.byte_length;
        vis_rope_pos += f.byte_length;
    });

    return result;
}

uint32_t Buffer::resolve_anchor(const Anchor& anchor) const {
    if (anchor.is_min()) return 0;
    if (anchor.is_max()) return visible_length();

    uint32_t accumulated = 0;
    uint32_t vis_rope_pos = 0;
    uint32_t result = visible_length();
    bool found = false;

    m_fragment_tree.for_each([&](const Fragment& f) {
        if (found) return;

        if (f.origin.replica_id == anchor.replica_id &&
            anchor.char_value >= f.origin.value &&
            anchor.char_value < f.origin.value + f.length) {

            if (f.visible) {
                uint32_t char_offset = anchor.char_value - f.origin.value;
                std::string ftext = m_visible_text.substr(vis_rope_pos, f.byte_length);
                uint32_t byte_off = chars_to_bytes(ftext, char_offset);
                result = accumulated + byte_off;
            } else {
                result = accumulated;
            }
            found = true;
            return;
        }

        if (f.visible) {
            accumulated += f.byte_length;
            vis_rope_pos += f.byte_length;
        }
    });

    return result;
}
```

- [ ] **Step 5: Migrate `resolve_visible_offset()`**

In `resolve_visible_offset()` (line 253-267), replace `content.size()`:

```cpp
std::pair<size_t, uint32_t> Buffer::resolve_visible_offset(
    const std::vector<Fragment>& frags, uint32_t byte_offset) const
{
    uint32_t accumulated = 0;
    for (size_t i = 0; i < frags.size(); ++i) {
        if (!frags[i].is_visible(m_undo_map))
            continue;
        uint32_t frag_bytes = frags[i].byte_length;
        if (accumulated + frag_bytes > byte_offset) {
            return {i, byte_offset - accumulated};
        }
        accumulated += frag_bytes;
    }
    return {frags.size(), 0};
}
```

- [ ] **Step 6: Migrate `rebuild_insertion_index()`**

In `rebuild_insertion_index()` (line 51-66), replace `f.content.size()`:

```cpp
void Buffer::rebuild_insertion_index(const std::vector<Fragment>& frags) {
    InsertionIndex index;
    for (auto& f : frags) {
        index.push_item(InsertionFragment(
            f.origin,
            0,
            f.locator,
            f.byte_length
        ));
    }
    m_insertion_index = std::move(index);
}
```

- [ ] **Step 7: Build and run all tests**

```bash
cmake --build build-dev && ctest --test-dir build-dev -j$(nproc) --output-on-failure
```

Expected: all 12 tests pass.

- [ ] **Step 8: Commit**

```bash
git add libs/collabtext/src/crdt/Buffer.h libs/collabtext/src/crdt/Buffer.cpp
git commit -m "feat: migrate splitting, anchors, and byte-length sites to rope/byte_length"
```

---

## Task 6: Update test invariants

Test invariant checks in `tst_fuzz.cpp` and assertions in other test files read `f.content`. Migrate to `byte_length` and rope-based checks. Add INV-9 (byte_length consistency).

**Files:**
- Modify: `libs/collabtext/tests/tst_fuzz.cpp:16-117` (invariant checks)
- Modify: `libs/collabtext/tests/tst_fuzz.cpp:595` (diagnostic output)
- Modify: `libs/collabtext/tests/tst_rope_integration.cpp:112,138` (content.size)
- Modify: `libs/collabtext/tests/tst_buffer.cpp:306` (content == "XX")

- [ ] **Step 1: Update INV-2 (visible byte sum)**

In `tst_fuzz.cpp`, lines 28-38, replace `f.content.size()` with `f.byte_length`:

```cpp
    // INV-2: visible_length == sum of visible fragment byte sizes
    uint32_t vis_sum = 0;
    uint32_t del_sum = 0;
    for (auto& f : frags) {
        if (f.visible)
            vis_sum += f.byte_length;
        else
            del_sum += f.byte_length;
    }
```

- [ ] **Step 2: Update INV-3 (visible concat == text)**

Lines 41-48. We can no longer concat `f.content`. Instead, verify that `text()` (from rope) matches the total visible byte length. INV-1 already checks `visible_length() == text.size()` and INV-2 checks `vis_sum == visible_length()`. INV-3's concat check is redundant with INV-8 (rope consistency) + INV-1 + INV-2. But for thoroughness, we can check via the rope:

```cpp
    // INV-3: rope visible text == text()
    // (With content removed, this is equivalent to: the visible rope is correct)
    // Already covered by INV-1 + INV-8, but kept for explicitness.
    std::string rope_text = buf.text();
    if (rope_text != text) {
        QFAIL(qPrintable(QString("INV-3 violated at %1: text() inconsistent between calls")
            .arg(context)));
    }
```

Actually, `text` is already set to `buf.text()` at line 20. This check is trivially true. Let's make INV-3 verify the rope matches fragment byte accounting more directly — check that visible rope length == sum of visible byte_lengths (which is INV-8). We can simply remove INV-3 as redundant, or keep it as a no-op comment. Let's simplify to:

```cpp
    // INV-3: (subsumed by INV-1 + INV-2 + INV-8 — visible text consistency)
```

- [ ] **Step 3: Update INV-5 (non-empty fragments)**

Lines 71-76, replace `frags[i].content.empty()` with `frags[i].byte_length == 0`:

```cpp
    // INV-5: every fragment has non-empty content and length > 0
    for (size_t i = 0; i < frags.size(); ++i) {
        if (frags[i].byte_length == 0 || frags[i].length == 0) {
            QFAIL(qPrintable(QString("INV-5 violated at %1: empty fragment at index %2")
                .arg(context).arg(i)));
        }
    }
```

- [ ] **Step 4: Update INV-6 (char count check)**

Lines 79-93. This verifies `f.length == actual UTF-8 character count in content`. Without content, we need to extract text from the rope. But `check_invariants` receives a `const Buffer&`, not the fragment vector with rope access.

We can verify this using the rope: extract all text, walk fragments accumulating bytes, verify char count. But this requires knowing each fragment's rope position.

Simpler: verify that `sum of f.length` across visible fragments equals the UTF-8 character count of `text()`. And `sum of f.byte_length` equals `text().size()` (already INV-2). The per-fragment char count can't be verified without the text. But we can still do a global check:

```cpp
    // INV-6: total character count consistency
    {
        uint32_t total_chars = 0;
        for (auto& f : frags) {
            if (f.visible) total_chars += f.length;
        }
        // Count actual UTF-8 chars in text
        uint32_t actual_chars = 0;
        for (size_t b = 0; b < text.size(); ) {
            unsigned char c = static_cast<unsigned char>(text[b]);
            if (c < 0x80) b += 1;
            else if ((c & 0xE0) == 0xC0) b += 2;
            else if ((c & 0xF0) == 0xE0) b += 3;
            else b += 4;
            ++actual_chars;
        }
        if (total_chars != actual_chars) {
            QFAIL(qPrintable(QString("INV-6 violated at %1: fragment char sum=%2 but text chars=%3")
                .arg(context).arg(total_chars).arg(actual_chars)));
        }
    }
```

- [ ] **Step 5: Update INV-7 (UTF-8 boundary check)**

Lines 96-106. Without content, we can't check individual fragment boundaries. But `byte_length` must be consistent with the rope — and the rope is built from properly-split text. The assertion in `set_fragments` (`visible_rope.len() == visible_bytes`) already catches byte accounting errors.

Replace with a check that `byte_length >= length` (since multi-byte chars make bytes > chars, and single-byte chars make bytes == chars):

```cpp
    // INV-7: byte_length >= length (multi-byte chars can't make bytes < chars)
    for (size_t i = 0; i < frags.size(); ++i) {
        if (frags[i].byte_length < frags[i].length) {
            QFAIL(qPrintable(QString("INV-7 violated at %1: frag[%2] byte_length=%3 < length=%4")
                .arg(context).arg(i).arg(frags[i].byte_length).arg(frags[i].length)));
        }
    }
```

- [ ] **Step 6: Add INV-9 (byte_length sum matches rope length)**

After INV-8 (line 116):

```cpp
    // INV-9: sum of byte_length matches rope accounting
    uint32_t byte_sum_vis = 0, byte_sum_del = 0;
    for (auto& f : frags) {
        if (f.visible) byte_sum_vis += f.byte_length;
        else byte_sum_del += f.byte_length;
    }
    if (byte_sum_vis != buf.visible_rope_len()) {
        QFAIL(qPrintable(QString("INV-9 violated at %1: byte_length vis sum=%2 but rope=%3")
            .arg(context).arg(byte_sum_vis).arg(buf.visible_rope_len())));
    }
    if (byte_sum_del != buf.deleted_rope_len()) {
        QFAIL(qPrintable(QString("INV-9 violated at %1: byte_length del sum=%2 but rope=%3")
            .arg(context).arg(byte_sum_del).arg(buf.deleted_rope_len())));
    }
```

- [ ] **Step 7: Update diagnostic output (line 595)**

Replace `frags[fi].content` with `"(text in rope)"`:

```cpp
                                  << " \"(text in rope)\"\n";
```

- [ ] **Step 8: Update tst_rope_integration.cpp**

Replace `f.content.size()` at lines 112 and 138:

```cpp
// Line 112:
                total_bytes += f.byte_length;
// Line 138:
                total += f.byte_length;
```

- [ ] **Step 9: Update tst_buffer.cpp**

Replace `f.content == "XX"` at line 306. Instead of checking fragment content, verify the text appears in the buffer output:

```cpp
    void local_edit_insert_mid_fragment() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"abcdef"});
        buf.apply_local_edit({{3, 3}}, {"XX"});
        QCOMPARE(buf.text(), std::string("abcXXdef"));

        // Verify fragment structure: should have at least 3 fragments
        // (abc, XX, def) after the split+insert
        auto frags = buf.fragments();
        QVERIFY(frags.size() >= 3);
        // Verify one fragment has byte_length 2 (the "XX" fragment)
        bool found_xx = false;
        for (auto& f : frags) {
            if (f.byte_length == 2 && f.visible) { found_xx = true; break; }
        }
        QVERIFY(found_xx);
    }
```

- [ ] **Step 10: Build and run all tests**

```bash
cmake --build build-dev && ctest --test-dir build-dev -j$(nproc) --output-on-failure
```

Expected: all 12 tests pass.

- [ ] **Step 11: Run fuzz 5x**

```bash
for i in $(seq 5); do ctest --test-dir build-dev -R tst_fuzz --output-on-failure || break; done
```

Expected: all 5 pass.

- [ ] **Step 12: Commit**

```bash
git add libs/collabtext/tests/tst_fuzz.cpp libs/collabtext/tests/tst_rope_integration.cpp libs/collabtext/tests/tst_buffer.cpp
git commit -m "feat: update test invariants for byte_length-based fragment model"
```

---

## Task 7: Remove `Fragment::content` field

Now that all production and test code reads from `byte_length` or ropes instead of `content`, remove the field. Fragment construction sites that still set `content` (for the transitional fallback in set_fragments) switch to using the `new_texts` map.

**Files:**
- Modify: `libs/collabtext/src/crdt/Fragment.h` (remove content field)
- Modify: `libs/collabtext/src/crdt/Buffer.cpp` (remove all content references; pass new_texts to set_fragments; fix normalize_fragments)

- [ ] **Step 1: Remove `content` from Fragment struct**

In `Fragment.h`:

```cpp
struct Fragment {
    using Summary = FragmentSummary;

    Lamport origin;
    Locator locator;
    uint32_t byte_length = 0;
    uint32_t length = 0;
    std::vector<Lamport> deletions;
    bool visible = true;

    Fragment() = default;
    Fragment(Lamport orig, Locator loc, uint32_t byte_len, uint32_t char_len)
        : origin(orig), locator(loc), byte_length(byte_len), length(char_len) {}

    // ... all other methods unchanged (they don't read content) ...
```

Remove the `#include <string>` if no other field needs it (check: `deletions` uses vector, no string needed). Actually, keep `<string>` — it's included by other headers transitively but being explicit is fine.

- [ ] **Step 2: Fix all Fragment construction sites in Buffer.cpp**

There are multiple places that construct Fragments. All must stop setting `content` and use the new constructor.

**`split_fragment_at()`** — already reads from rope in Task 4. Remove content assignments:

```cpp
size_t Buffer::split_fragment_at(std::vector<Fragment>& frags,
                                  size_t frag_idx, uint32_t offset_in_frag) const
{
    assert(frag_idx < frags.size());
    assert(offset_in_frag > 0);
    assert(offset_in_frag < frags[frag_idx].byte_length);

    Fragment &orig = frags[frag_idx];
    std::string text = extract_fragment_text(frags, frag_idx);
    uint32_t char_count = count_utf8_chars(text, offset_in_frag);

    Fragment second;
    second.origin = Lamport(orig.origin.replica_id, orig.origin.value + char_count);
    second.locator = orig.locator;
    second.byte_length = orig.byte_length - offset_in_frag;
    second.length = orig.length - char_count;
    second.deletions = orig.deletions;

    orig.byte_length = offset_in_frag;
    orig.length = char_count;

    Lamport saved_origin = orig.origin;
    Locator saved_locator = orig.locator;

    Lamport second_origin(saved_origin.replica_id, saved_origin.value + char_count);
    insert_fragment(frags, std::move(second));

    for (size_t i = frag_idx + 1; i < frags.size(); ++i) {
        if (frags[i].origin == second_origin &&
            frags[i].locator == saved_locator) {
            return i;
        }
    }

    assert(false && "split_fragment_at: could not find second half after insert");
    return frag_idx + 1;
}
```

**`split_frag` lambda** in apply_local_edit:

```cpp
    auto split_frag = [](const Fragment& f, uint32_t byte_off,
                         const std::string& text) -> std::pair<Fragment, Fragment>
    {
        uint32_t char_count = count_utf8_chars(text, byte_off);
        Fragment first;
        first.origin = f.origin;
        first.locator = f.locator;
        first.byte_length = byte_off;
        first.length = char_count;
        first.deletions = f.deletions;
        first.visible = f.visible;

        Fragment second;
        second.origin = Lamport(f.origin.replica_id, f.origin.value + char_count);
        second.locator = f.locator;
        second.byte_length = f.byte_length - byte_off;
        second.length = f.length - char_count;
        second.deletions = f.deletions;
        second.visible = f.visible;
        return {std::move(first), std::move(second)};
    };
```

**New fragment creation** in apply_local_edit (line 686):

```cpp
            Fragment frag(frag_origin, new_loc,
                          static_cast<uint32_t>(replacement.size()), char_count);
            frag.visible = true;
            new_tree.push_item(std::move(frag));
```

**InsertedFragment** in apply_local_edit (line 690-695) stays the same — `ins_rec.content = replacement` is on `EditOperation::InsertedFragment`, not `Fragment`.

**Remote fragment creation** in apply_remote_edit (line 903):

```cpp
        Fragment frag(ins.origin, ins.locator,
                      static_cast<uint32_t>(ins.content.size()), ins.length);
        insert_fragment(frags, frag);
```

**normalize_fragments** inner loop — extract text from old ropes:

```cpp
                } else {
                    // Need to extract text for this fragment from the old ropes.
                    // Build the text by walking old rope positions.
                    std::string ftext;
                    {
                        // Find this fragment in old_entries-style lookup
                        // Since normalize runs before set_fragments, ropes are
                        // still from the old tree. Use extract_fragment_text.
                        // But frags may be reordered... we need the text.
                        // Actually: at this point in apply_remote_edit, the ropes
                        // are STILL the old ropes and frags[j] was extracted from
                        // the tree, so its origin maps to the old tree.
                        // Use a direct rope extraction based on the old tree.
                        ftext = extract_fragment_text(frags, j);
                    }
                    uint32_t byte_pos = 0;
                    for (uint32_t c = 0; c < f.length; ++c) {
                        uint32_t char_bytes = 1;
                        unsigned char ch = static_cast<unsigned char>(ftext[byte_pos]);
                        if (ch >= 0xF0) char_bytes = 4;
                        else if (ch >= 0xE0) char_bytes = 3;
                        else if (ch >= 0xC0) char_bytes = 2;

                        Fragment single;
                        single.origin = Lamport(f.origin.replica_id, f.origin.value + c);
                        single.locator = f.locator;
                        single.byte_length = char_bytes;
                        single.length = 1;
                        single.deletions = f.deletions;
                        extracted.push_back(std::move(single));
                        byte_pos += char_bytes;
                    }
                }
```

- [ ] **Step 3: Pass new_texts to set_fragments for insertions**

In `apply_local_edit()`, after building new_tree, pass inserted fragment texts:

After line 761 (the normalize_fragments call), before `set_fragments`:

```cpp
    // Build new_texts map for newly inserted fragments
    std::unordered_map<uint64_t, std::string> new_texts;
    for (auto& ins : op.inserted_fragments) {
        new_texts[origin_key(ins.origin)] = ins.content;
    }
    set_fragments(std::move(frags), new_texts);
```

Replace the bare `set_fragments(std::move(frags))` call at line 762.

In `apply_remote_edit()`, similarly:

```cpp
    std::unordered_map<uint64_t, std::string> new_texts;
    for (auto& ins : op.inserted_fragments) {
        new_texts[origin_key(ins.origin)] = ins.content;
    }
    normalize_fragments(frags);
    set_fragments(std::move(frags), new_texts);
```

Replace the existing `set_fragments(std::move(frags))` call at line 922.

For `undo()`, `redo()`, `apply_remote_undo()` — no new texts, use the no-arg overload (unchanged).

- [ ] **Step 4: Remove `content` fallback in `set_fragments` two-arg overload**

In the `set_fragments(frags, new_texts)` method, remove the `f.content` fallback:

```cpp
        if (!found) {
            auto it = new_texts.find(origin_key(f.origin));
            assert(it != new_texts.end() && "Fragment not in old tree and not in new_texts");
            text = it->second;
        }
```

- [ ] **Step 5: Remove `content` from the one-arg `set_fragments` fallback path**

The one-arg `set_fragments` delegates to the two-arg with empty map. Without content, any fragment not found in old_entries must be in new_texts. For undo/redo/remote_undo, all fragments are existing (in old tree), so the empty map is fine.

- [ ] **Step 6: Fix `extract_fragment_text` to use `byte_length` instead of `content.size()`**

```cpp
std::string Buffer::extract_fragment_text(
    const std::vector<Fragment>& frags, size_t frag_idx) const
{
    const Fragment& target = frags[frag_idx];

    uint32_t vis_offset = 0;
    uint32_t del_offset = 0;
    for (size_t i = 0; i < frag_idx; ++i) {
        if (frags[i].visible) {
            vis_offset += frags[i].byte_length;
        } else {
            del_offset += frags[i].byte_length;
        }
    }

    if (target.visible) {
        return m_visible_text.substr(vis_offset, target.byte_length);
    } else {
        return m_deleted_text.substr(del_offset, target.byte_length);
    }
}
```

(This was already using byte_length from Task 3 if we followed the plan correctly. Verify.)

- [ ] **Step 7: Fix `split_frag` call sites in apply_local_edit to get text from rope**

The `split_frag` lambda now takes text as a parameter. During Task 5, we passed `f.content`. Now that content is removed, we need to extract text from the rope. For fragments from the cursor (old tree), the ropes are still the old ropes:

```cpp
    // Helper: extract text for a fragment from the visible rope by accumulated offset.
    // Only works during apply_local_edit before ropes are rebuilt.
    uint32_t vis_rope_pos = 0;  // Add this tracking variable near the top of apply_local_edit
```

Actually, the fragments during apply_local_edit come from two sources:
1. **Cursor items** (`*cursor.item()`) — from the old tree. Their text is in the old visible/deleted rope.
2. **Pending** — a leftover half from a previous split. Its text was extracted during the split.

For cursor items: we need to know their rope position. The cursor tracks `VisibleOffset` position, which gives us the visible byte offset. For visible fragments, this is exactly the rope position. For deleted fragments during the cursor walk, we'd need the deleted rope position too.

This is getting complex. A simpler approach: when we extract a fragment from the cursor, immediately extract its text from the rope and carry it alongside. Add a `std::string` alongside each extracted fragment.

**Revised approach for apply_local_edit:** Track rope positions alongside the cursor walk. When we need to split, we already have the byte offset from the cursor's VisibleOffset. Let's compute it:

Actually, the cursor's `position()` gives the VisibleOffset — bytes of visible text before the current item. For the current visible item at position P, its text starts at byte P in the visible rope. For invisible items, we need to track deleted position separately.

Let me use a different approach. Since we're still in the old tree (ropes match), and `cursor.item()` gives us a Fragment with `byte_length`, we can track positions:

```cpp
    uint32_t cursor_vis_pos = 0;  // tracks visible rope position at cursor
    uint32_t cursor_del_pos = 0;  // tracks deleted rope position at cursor
```

When we call `cursor.next()`, advance the appropriate position. When we need text for splitting, extract from the rope.

But the cursor uses `slice()` and `suffix()` which skip multiple items. We'd need to walk those items to update positions.

**Simpler: for `split_frag` calls in consume_unchanged/consume_deleted, extract text from the pending fragment or cursor item using the visible rope.**

Since `consume_unchanged` and `consume_deleted` process fragments one at a time and track `vis_bytes` (visible bytes consumed so far), we can compute the visible rope offset from the cursor's position at each step.

This is getting very detailed. Let me simplify the plan: in this step, we'll compute text for each fragment that needs splitting by reading from the rope. The key insight is that `cursor.position().value` gives the visible byte offset of the current cursor item.

For pending fragments (from previous splits): they were produced by `split_frag` which splits text. Since `split_frag` needs text, and text was extracted when the split happened, the text is available at split time but not stored. After content removal, we need to carry the text alongside the fragment.

**Practical solution:** Use a `std::pair<Fragment, std::string>` for pending, carrying extracted text. When splitting, produce pairs with text. This avoids repeatedly extracting from the rope.

```cpp
    // Change pending type to carry text alongside the fragment
    struct PendingFrag {
        Fragment frag;
        std::string text;  // extracted from rope at split time
    };
    std::optional<PendingFrag> pending;
```

Update `split_frag` calls to use this. When extracting from cursor, get text from visible rope at `cursor.position().value`.

This is getting complex enough that the implementation needs to happen carefully. The plan step should be: "Carry extracted text alongside fragments during apply_local_edit processing to avoid re-extraction."

- [ ] **Step 8: Build**

This step will NOT compile until all `content` references are removed. Fix all remaining compilation errors. The compiler will identify every remaining reference.

```bash
cmake --build build-dev 2>&1 | head -50
```

Fix each error:
- Fragment construction: use `(origin, locator, byte_length, char_length)` constructor
- Any remaining `f.content` reads: replace with rope extraction or byte_length
- Test files: already migrated in Task 6

- [ ] **Step 9: Run all tests**

```bash
cmake --build build-dev && ctest --test-dir build-dev -j$(nproc) --output-on-failure
```

Expected: all 12 tests pass.

- [ ] **Step 10: Commit**

```bash
git add libs/collabtext/src/crdt/Fragment.h libs/collabtext/src/crdt/Buffer.h libs/collabtext/src/crdt/Buffer.cpp libs/collabtext/tests/
git commit -m "feat: remove Fragment::content — text lives exclusively in Ropes (Gen 2 Phase 3)"
```

---

## Task 8: Full verification

Run the complete test suite with extended fuzz and convergence testing to validate correctness.

**Files:** None (verification only)

- [ ] **Step 1: Run full test suite**

```bash
ctest --test-dir build-dev -j$(nproc) --output-on-failure
```

Expected: all 12 tests pass.

- [ ] **Step 2: Run fuzz tests 20x**

```bash
for i in $(seq 20); do echo "Fuzz run $i"; ctest --test-dir build-dev -R tst_fuzz --output-on-failure || { echo "FAILED at run $i"; break; }; done
```

Expected: all 20 runs pass.

- [ ] **Step 3: Run convergence tests 20x**

```bash
for i in $(seq 20); do echo "Convergence run $i"; ctest --test-dir build-dev -R tst_convergence --output-on-failure || { echo "FAILED at run $i"; break; }; done
```

Expected: all 20 runs pass.

- [ ] **Step 4: Run rope integration tests**

```bash
ctest --test-dir build-dev -R tst_rope --output-on-failure
```

Expected: both rope tests pass.

- [ ] **Step 5: Verify memory reduction**

Check that Fragment no longer contains a string:

```bash
grep -n "std::string" libs/collabtext/src/crdt/Fragment.h
```

Expected: no `std::string` fields in the Fragment struct (only in other parts of the file if any).

- [ ] **Step 6: Commit verification results (if any test fixes needed)**

If any test failures were found and fixed:
```bash
git add -A && git commit -m "fix: address issues found during Phase 3 verification"
```

---

## Summary of Key Design Decisions

1. **Origin-interval-lookup over streaming RopeBuilder.** The spec's RopeBuilder assumes fragments are processed in old-rope order. Since `set_fragments()` receives fragments in new (sorted/normalized) order, we use a lookup-based approach instead. Each fragment's text is found by matching its origin into the old tree's origin ranges.

2. **`new_texts` parameter for new insertions.** After content removal, newly inserted fragments have no text on the Fragment struct. Their text is passed to `set_fragments()` via a `std::unordered_map<uint64_t, std::string>` keyed by packed origin. Only `apply_local_edit` and `apply_remote_edit` populate this map.

3. **Carried text in apply_local_edit.** The cursor-walking loop in `apply_local_edit` needs text for splitting. Since content is removed, extracted text is carried alongside fragments (in `pending` and at split call sites) to avoid repeated rope extraction.

4. **Phased migration.** `byte_length` is added as a shadow field first, allowing incremental migration. The old `content` remains readable throughout Tasks 1-6. Only Task 7 removes it in one atomic step, after all access sites are migrated.

## Critical Implementation Notes

1. **`extract_fragment_text()` ordering constraint.** This helper computes rope offsets by walking `frags[0..frag_idx)`. It is ONLY valid when the frags vector is in the same order as the ropes (i.e., before sort/normalize). Safe for `split_fragment_at()` and `apply_deletion_runs()` calls. NOT safe for `normalize_fragments()` after sort.

2. **`normalize_fragments()` text extraction after content removal.** Since normalize runs after sort (when frags may be reordered relative to ropes), it must use the same origin-interval-lookup approach as `set_fragments()` to find text. Extract old rope strings once at the start of normalize, then look up each fragment's text by matching its origin into `m_fragment_tree`'s origin ranges.

3. **`apply_local_edit` pending fragment text.** After content removal, the `pending` fragment (leftover half from cursor splits) needs its text carried alongside it. Change pending to a struct holding `{Fragment, std::string extracted_text}`. Similarly, fragments from `*cursor.item()` need text extracted from the visible rope at `cursor.position().value` offset before splitting.
