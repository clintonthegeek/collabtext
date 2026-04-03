# Fragment::content Removal — Design Spec

**Date:** 2026-04-02
**Status:** Complete. Implemented in commits `51b4919`..`2e716f9`.
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

## 7. Open Design Questions — RESOLVED

1. **Caching rope offsets.** Not done. Offsets invalidate on every
   mutation, and the origin-interval-lookup is fast enough.

2. **Splitting without text access.** Text is extracted from the rope
   via `extract_fragment_text()` (walks preceding fragments to compute
   offset). For `normalize_fragments()`, which runs after sort, the
   same origin-interval-lookup used by `set_fragments()` is used.

3. **Test migration.** INV-3 removed (redundant with INV-1+2+8).
   INV-5/6/7 rewritten to use `byte_length` and global checks.
   INV-9 added for byte_length↔rope consistency.

4. **InsertionIndex.** `rebuild_insertion_index` now uses `f.byte_length`.

5. **RopeBuilder vs origin-interval-lookup.** The spec's streaming
   RopeBuilder (§6.3) was replaced with an origin-interval-lookup
   approach. See `docs/reports/2026-04-02-gen2-phase3-content-removal.md`
   §2 for rationale.

## 8. Risk Assessment — POST-MORTEM

Risk was high as predicted (38 access sites, not 34 — tests had
more references than initially counted). The incremental shadow-field
approach (adding `byte_length` alongside `content`, migrating site by
site, removing `content` last) was critical for managing the risk.

One latent bug was discovered: `split_fragment_at()` was not propagating
the `visible` flag to the second half. This was harmless with content-based
text access but caused out-of-bounds rope reads after migration. Fixed
in commit `4c04671`.

The fuzz suite (9 invariants, 16 adversarial scenarios, 20x random seeds)
caught zero regressions during the migration, validating the incremental
approach.

## 9. Actual Task Decomposition

1. Add `byte_length` shadow field to Fragment (`51b4919`)
2. Rewrite `set_fragments()` with origin-interval-lookup (`50e1e98`)
3. Add `extract_fragment_text()` helper
4. Migrate `split_fragment_at`, `apply_deletion_runs` splitting
5. Migrate `apply_local_edit` helpers, anchors, byte-length sites
   (3-5 in single commit `4c04671`)
6. Update test invariants (`1a0220e`)
7. Remove `Fragment::content` field (`2e716f9`)
8. Full verification — 20/20 fuzz, 20/20 convergence
