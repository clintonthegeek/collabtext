# Fragment::content Removal — Design Spec

**Date:** 2026-04-02
**Status:** Design ready. Implementation deferred to fresh session.
**Prereqs:** Gen 2 Phases 1-2 complete (unified deletions, deletion runs)

---

## 1. Problem

Every character in the document is stored twice:
- In `Fragment::content` (per-fragment owned string)
- In the Ropes (`m_visible_text`, `m_deleted_text`)

This doubles memory usage for text data. The Ropes were added in
Optimization 4 as a stepping stone — the spec (§6.2) envisions
fragments as metadata-only records with text living exclusively
in the Ropes.

## 2. Solution

Remove `Fragment::content`. Replace with `uint32_t byte_length`.
Text is accessed via the Ropes. A RopeBuilder (spec §6.3) handles
incremental rope updates during mutations.

## 3. Fragment Changes

### Before
```cpp
struct Fragment {
    Lamport origin;
    Locator locator;
    std::string content;     // OWNED TEXT — removed
    uint32_t length;         // character count
    std::vector<Lamport> deletions;
    bool visible;
};
```

### After
```cpp
struct Fragment {
    Lamport origin;
    Locator locator;
    uint32_t byte_length;    // byte size (replaces content.size())
    uint32_t length;         // character count (unchanged)
    std::vector<Lamport> deletions;
    bool visible;
};
```

## 4. Access Site Categories (34 sites)

### Category A: Byte length queries (12 sites)
Replace `content.size()` with `byte_length`.
**Trivial migration.** No text access needed.

### Category B: Text splitting (8 sites)
`split_fragment_at()`, `split_frag` lambda, `normalize_fragments()`
need to split text. Currently: `content.substr(0, N)` and
`content.substr(N)`.
**Requires rope text lookup.** Given a fragment's position in the
fragment tree, look up its text from the appropriate rope (visible
or deleted), then split.

### Category C: UTF-8 navigation (6 sites)
`char_to_byte_offset(content, offset)`, `count_utf8_chars(content, N)`,
`first_char_bytes(content)`.
**Requires rope text lookup.** Same as Category B — extract text
from rope, then navigate.

### Category D: Rope rebuild (2 sites)
`push_str(f.content)` in `set_fragments()`.
**Replaced by RopeBuilder.** The RopeBuilder walks old ropes and
new fragment metadata to construct new ropes without needing
fragment content.

### Category E: Fragment construction (4 sites)
Creating new fragments from text (insertions).
**Text goes directly to rope.** New fragments store byte_length
instead of owning the string.

### Category F: Test assertions (2 sites)
`f.content.size()` and `f.content` in tests.
**Use byte_length or rope lookup.**

## 5. RopeBuilder (spec §6.3)

The RopeBuilder walks old ropes and new fragment lists in parallel,
constructing new ropes without needing Fragment::content:

```cpp
class RopeBuilder {
public:
    RopeBuilder(const Rope& old_visible, const Rope& old_deleted);

    // Old fragment: extract bytes from old rope, place in new rope
    void push_fragment(uint32_t byte_length,
                       bool was_visible, bool now_visible);

    // New text (insertion): place directly in new visible rope
    void push_new_text(std::string_view text);

    Rope finish_visible();
    Rope finish_deleted();

private:
    uint32_t m_old_visible_pos = 0;
    uint32_t m_old_deleted_pos = 0;
    const Rope& m_old_visible;
    const Rope& m_old_deleted;
    Rope m_new_visible;
    Rope m_new_deleted;
};
```

**Key requirement:** The caller must know `was_visible` for each
fragment (its visibility BEFORE the mutation). This requires
snapshotting visibility before applying changes.

## 6. Text Lookup for Splitting

When splitting a fragment, we need its text to compute UTF-8
character boundaries. The text lives in a rope. To find it:

1. Walk fragments up to the target fragment, accumulating visible
   and deleted byte offsets
2. Use `rope.substr(offset, length)` to extract the text
3. Compute the split point
4. Create two fragments with updated byte_length values

This is O(n) in the worst case (walking to find the offset), but
splitting only happens during edits where we're already walking
the fragment list.

## 7. Open Design Questions

1. **Caching rope offsets.** Should fragments cache their rope
   offset? This would make lookups O(1) but offsets invalidate on
   every mutation. Probably not worth it.

2. **Splitting without text access.** For `apply_remote_edit`, we
   split at character offsets within a fragment. We need to convert
   character offset → byte offset, which requires reading the text.
   Could we store a byte-per-character array? Overkill for now.

3. **Test migration.** The fuzz suite's `check_invariants()` reads
   `f.content` for INV-3 (concat check), INV-5 (non-empty check),
   INV-6 (char count check), INV-7 (UTF-8 boundary check). These
   need to use rope extraction instead.

4. **InsertionIndex.** `rebuild_insertion_index` reads
   `f.content.size()`. Replace with `f.byte_length`.

## 8. Risk Assessment

**HIGH RISK.** This is the most invasive change in the Gen 2
redesign. 34 access sites across production code and tests.
Every splitting operation needs rope text lookup. The RopeBuilder
pattern is new and untested.

**Mitigation:** The fuzz suite checks 8 structural invariants after
every operation. Add INV-9 (fragment byte_length matches rope
accounting). Test incrementally — start with RopeBuilder, then
migrate splitting, then remove content.

## 9. Recommended Task Decomposition

1. Implement RopeBuilder class + unit tests
2. Wire RopeBuilder into set_fragments (alongside existing content)
3. Add rope text lookup helper
4. Migrate Category A sites (byte_length — trivial)
5. Migrate Category B sites (splitting — requires rope lookup)
6. Migrate Category C sites (UTF-8 nav — requires rope lookup)
7. Migrate Category D (set_fragments uses RopeBuilder only)
8. Migrate Category E (fragment construction)
9. Remove Fragment::content field
10. Update test assertions
11. Full verification
