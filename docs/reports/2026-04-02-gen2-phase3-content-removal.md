# Development Report: Fragment::content Removal (Gen 2 Phase 3)

**Date:** 2026-04-02
**Base commit:** `6f803c5` (docs: Fragment::content removal design spec)
**Final commit:** `2e716f9` (feat: remove Fragment::content)
**Scope:** Remove `Fragment::content` (per-fragment owned string), making Fragment a metadata-only record. Text lives exclusively in the Ropes. Completes the Gen 2 engine redesign.

---

## 1. Starting Point

After Gen 2 Phases 1 and 2, the engine had:
- **Unified deletion tracking** (Phase 1): `std::vector<Lamport> deletions` replaced `uint32_t delete_count`. Single parity-based visibility model via UndoMap.
- **Offset-based remote edits** (Phase 2): Run-length encoded deletion runs replaced per-character timestamp lookup. Wire format transmits byte ranges instead of k timestamps.
- **Text duplication**: Every character stored twice — once in `Fragment::content` (per-fragment owned string) and once in the Ropes (`m_visible_text`, `m_deleted_text`).

The design spec (§6.2 of CRDT_ENGINE_SPEC.md) had always envisioned fragments as metadata-only records. Phase 3 closes that gap.

Fragment before:
```cpp
struct Fragment {
    Lamport origin;
    Locator locator;
    std::string content;     // OWNED TEXT — every character duplicated
    uint32_t length;
    std::vector<Lamport> deletions;
    bool visible;
};
```

38 access sites read `content` across 4 source files and 3 test files.

---

## 2. The Rope Reconstruction Problem

The design spec proposed a streaming `RopeBuilder` (§6.3) that walks old and new rope/fragment lists in parallel. Early analysis revealed this doesn't work for `set_fragments()`:

**The problem:** `set_fragments()` receives fragments in their *new* order (after sort, normalize, split-relocation). The old ropes have text in the *old* fragment order. A streaming RopeBuilder advances linearly through old ropes, requiring fragments in old-rope order. After sorting, that order is lost.

**Approaches considered:**

1. **Streaming RopeBuilder** (spec §6.3) — works only when fragment order doesn't change (undo/redo). Breaks for local/remote edits where fragments are reordered.

2. **Per-caller rope building** — each mutation function (apply_local_edit, apply_remote_edit, undo, redo) builds ropes in its own loop. Eliminates the order problem but duplicates rope-building logic across 4 call sites.

3. **Origin-interval-lookup** — scan the old fragment tree to build a map of `(origin_range → rope_offset)`, then for each new fragment, find which old fragment's origin range contains it and extract text at the computed offset. Handles splits, reordering, normalization uniformly in one place.

**Decision: origin-interval-lookup (option 3).** It handles all cases in a single `set_fragments()` implementation. The key insight: every fragment (including split children) has an origin that falls within exactly one old-tree fragment's origin range. A fragment with origin `(rid, val)` and length `N` covers timestamps `val..val+N-1`. A split child at `(rid, val+K)` falls within that range. UTF-8 byte offset within the parent is computed by walking characters in the old rope text.

**Trade-off:** O(n*m) worst case for the lookup (n new fragments, m old fragments) vs O(n) for the streaming approach. Acceptable because `set_fragments()` is already O(n) and the inner loop terminates on first match. In practice, most fragments match on the first or second probe.

---

## 3. The normalize_fragments Text Access Problem

`normalize_fragments()` atomizes multi-character fragments at shared locators into single-character fragments. It needs to read each character's bytes from the fragment's text. After content removal, where does it get the text?

**The problem:** normalize runs *after* the fragment vector has been sorted, so the vector order doesn't match the rope order. `extract_fragment_text()` (which walks the vector to compute rope offsets) gives wrong results.

**Solution:** normalize uses the same origin-interval-lookup against the old tree that `set_fragments()` uses. Since normalize has access to `m_fragment_tree` (the old tree) and `m_visible_text`/`m_deleted_text` (the old ropes), it walks the old tree's `for_each` to find the parent fragment, extracts the text from the correct old rope, and atomizes from there. For newly inserted fragments (not in old tree), text comes from a `new_texts` map passed through from the caller.

---

## 4. The apply_local_edit Pending Fragment Problem

`apply_local_edit()` walks a cursor over the old fragment tree, processing ranges left-to-right. When a range boundary falls mid-fragment, the fragment is split and the remainder becomes "pending" — carried across to the next range. After content removal, the pending fragment has no text.

**Solution:** Changed `pending` from `std::optional<Fragment>` to `std::optional<PendingFrag>` where `PendingFrag` carries both the fragment and its extracted text:

```cpp
struct PendingFrag {
    Fragment frag;
    std::string text;
};
```

Rope position trackers (`vis_rope_pos`, `del_rope_pos`) are maintained alongside the cursor walk. When a fragment is extracted from the cursor, its text is extracted from the old rope at the current position. When `cursor.slice()` bulk-copies a prefix, the rope positions are advanced using the sliced tree's summary byte counts.

---

## 5. Bug Found: Missing Visibility Flag on Split

During Task 4 (migrating `split_fragment_at` to rope extraction), a latent bug was discovered: `split_fragment_at()` was not propagating the `visible` flag to the second half of a split. With the old `content`-based approach, this was harmless — `extract_fragment_text()` doesn't depend on visibility when content is self-contained. But with rope extraction, the visibility flag determines which rope to read from. An invisible fragment looked up in the visible rope produced out-of-bounds access.

**Fix:** Added `second.visible = orig.visible` to `split_fragment_at()`. This is a correctness fix independent of the content removal — it just happened to be masked by the old approach.

---

## 6. Implementation Sequence

| Step | Commit | What | Risk |
|------|--------|------|------|
| 1 | `51b4919` | Add `byte_length` shadow field | None — additive |
| 2 | `50e1e98` | Rewrite `set_fragments()` with origin-interval-lookup | High — central rebuild path |
| 3-5 | `4c04671` | Migrate 24 production `content` sites to byte_length/rope | Medium — wide surface area |
| 6 | `1a0220e` | Update test invariants (INV-2,3,5,6,7; add INV-9) | Low — test-only |
| 7 | `2e716f9` | **Remove `Fragment::content`** | High — the big bang |

The shadow-field approach was critical: `byte_length` existed alongside `content` through steps 1-6, allowing incremental migration with full test validation at each step. Only step 7 removed the field in one atomic change.

---

## 7. What Changed

### Fragment (before → after)

```
Before:                              After:
  Lamport origin                       Lamport origin
  Locator locator                      Locator locator
  std::string content  ← REMOVED      uint32_t byte_length  ← NEW
  uint32_t length                      uint32_t length
  vector<Lamport> deletions            vector<Lamport> deletions
  bool visible                         bool visible
```

### Buffer.cpp — Key method changes

- **`set_fragments()`** — Rewritten to reconstruct ropes from old ropes via origin-interval-lookup. New overload accepts `new_texts` map for freshly inserted fragment text.
- **`split_fragment_at()`** — Reads text from rope via `extract_fragment_text()`. Computes byte_length by subtraction rather than `content.size()`.
- **`normalize_fragments()`** — Extracts text from old ropes via origin-interval-lookup for atomization. Accepts `new_texts` map for new fragments.
- **`apply_local_edit()`** — Introduced `PendingFrag` struct to carry extracted text alongside fragments. Tracks `vis_rope_pos`/`del_rope_pos` through cursor walk.
- **`anchor_at()` / `resolve_anchor()`** — Track visible rope position through `for_each` walk, extract text for UTF-8 char/byte conversion.
- **`apply_remote_edit()`** / **`apply_local_edit()`** — Build `new_texts` maps from `EditOperation::InsertedFragment` entries and pass to `normalize_fragments()` and `set_fragments()`.

### Test invariants

- **INV-3** (visible fragment concat == text): Removed — subsumed by INV-1 + INV-2 + INV-8.
- **INV-5** (non-empty fragments): `content.empty()` → `byte_length == 0`.
- **INV-6** (char count): Per-fragment → global visible char sum vs UTF-8 char count of text().
- **INV-7** (UTF-8 boundary): `content[0]` check → `byte_length >= length` check.
- **INV-9** (NEW): `byte_length` sums match rope byte lengths.

---

## 8. Verification

| Test suite | Result |
|------------|--------|
| tst_buffer (47 tests) | Pass |
| tst_convergence (7 tests) | Pass |
| tst_fuzz (15 tests, 9 invariants each) | **20/20 runs** |
| tst_convergence (multi-replica) | **20/20 runs** |
| tst_rope + tst_rope_integration | Pass |
| tst_anchor (21 tests) | Pass |
| tst_undomap, tst_clock, tst_locator, tst_sumtree, tst_opqueue, tst_utf8 | Pass |
| **Total: 12 suites, 200+ tests** | **All pass** |

---

## 9. Gen 2 Completion Summary

With Phase 3 complete, all three Gen 2 goals from the redesign spec are achieved:

| Goal | Phase | Status | Key metric |
|------|-------|--------|-----------|
| **Unified visibility model** | Phase 1 | Complete | One mechanism (UndoMap parity) instead of two |
| **Efficient remote deletes** | Phase 2 | Complete | Byte ranges instead of per-character timestamps |
| **Single-copy text storage** | Phase 3 | Complete | Text in Ropes only, not duplicated in Fragments |

The engine now matches the CRDT_ENGINE_SPEC's target design: fragments are metadata-only records (origin, locator, byte_length, char_length, deletions, visible) with text living exclusively in the Ropes.

---

## 10. Open Considerations

1. **Performance of origin-interval-lookup.** The O(n*m) scan in `set_fragments()` is fine for current document sizes but could be optimized with a sorted lookup (binary search by replica_id + origin_value) if profiling shows it matters for large documents.

2. **Rope-to-string extraction.** `set_fragments()` calls `m_visible_text.to_string()` and `m_deleted_text.to_string()` to get random-access old rope text. This is O(n) allocation. A cursor-based approach that walks the rope chunks directly would avoid this copy, at the cost of more complex code.

3. **normalize_fragments rarity.** The origin-interval-lookup in normalize is relatively expensive but normalize only triggers for multi-replica shared-locator conflicts — a rare case in practice. No optimization needed unless profiling indicates otherwise.
