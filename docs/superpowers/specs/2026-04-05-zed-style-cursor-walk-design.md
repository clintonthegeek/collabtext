# Zed-Style Cursor Walk — Design Spec

**Date:** 2026-04-05
**Status:** Design ready
**Prereqs:** Phase 3 (origin index + Locator fix) complete

---

## 1. Motivation

`apply_local_edit` is O(n) per edit because of the post-processing phase:
extract to vector, apply deferred relocations, sort, normalize, rebuild tree.
This exists because the cursor-built tree can produce out-of-order fragments
(~2.3% of edits in fuzz testing). The current safety check catches these and
falls back to the O(n) full path, but the check itself is O(n).

Zed's implementation avoids this entirely by reassigning locators during the
cursor walk. Every fragment pushed to the new tree gets a locator =
`Locator::between(max_id_in_new_tree, original_id)`. This guarantees
monotonically increasing locators. No sort or post-processing needed.

This is the last major optimization needed to make local edits O(k log n)
where k = fragments touched by the edit (typically 1-5).

---

## 2. How Zed Does It

Zed's `apply_edit` (local) and `apply_remote_edit` both follow this pattern:

```
cursor = old_tree.cursor()
new_tree = cursor.slice(first_edit_position)  // O(log n) prefix

for each edit range:
    // Split fragment at range start
    if fragment straddles range start:
        prefix_half = clone(current_fragment)
        prefix_half.len = truncated
        prefix_half.id = Locator::between(new_tree.max_id, prefix_half.id)
        new_tree.push(prefix_half)

    // Insert new text
    for each chunk of new text:
        frag.id = Locator::between(new_tree.max_id, next_fragment.id)
        new_tree.push(frag)

    // Mark deleted fragments
    while fragment intersects range:
        deleted = clone(current_fragment)
        deleted.id = Locator::between(new_tree.max_id, deleted.id)
        deleted.deletions.push(timestamp)
        new_tree.push(deleted)

new_tree.append(cursor.suffix())  // O(log n) suffix
old_tree = new_tree
```

Key: `Locator::between(max_id, original_id)` always returns a value where
`max_id < result < original_id` (or at a deeper digit level if equal). This
ensures the new tree has monotonically increasing locators, and the suffix
(which keeps original locators) is always > everything in the new tree.

---

## 3. Changes to Our Engine

### 3.1 Locator::between — Handle Equal Arguments

Zed's `between()` handles `lo == hi` by descending to the next digit level
(lo extends with DMIN, hi with DMAX, creating space). Our version asserts
`lo < hi`. Change to handle `lo == hi`:

```cpp
Locator Locator::between(const Locator &lo, const Locator &hi) {
    assert(lo <= hi);  // Changed from lo < hi — allow equal
    // ... existing algorithm, but when ld == hd at every level,
    // descend until lhs extends with DMIN and rhs extends with DMAX
}
```

The existing descent logic already handles this case — the loop encounters
`ld == hd` at each level, pushes `ld`, and continues. At the level beyond
both locators' lengths, lo extends with DMIN and hi with DMAX, creating
room. The biased_mid fix from Phase 3 ensures correct behavior at that
level.

### 3.2 apply_local_edit — Zed-Style Locator Reassignment

Rewrite the cursor walk to reassign locators during fragment processing.
The new pattern for the insert phase:

```cpp
// When splitting a fragment at edit boundary:
if (fragment_start < range.start) {
    Fragment prefix = *cursor.item();  // clone
    prefix.text = prefix.text.substr(0, split_bytes);
    prefix.byte_length = split_bytes;
    prefix.length = split_chars;
    prefix.locator = Locator::between(
        new_tree.empty() ? Locator::min() : new_tree.last().locator,
        prefix.locator);
    new_tree.push_item(std::move(prefix));
    // cursor still at original fragment, advance past consumed portion
}

// When inserting new text:
Locator next_loc = (pending || cursor.item())
    ? (pending ? pending->locator : cursor.item()->locator)
    : Locator::max();
Locator new_loc = Locator::between(
    new_tree.empty() ? Locator::min() : new_tree.last().locator,
    next_loc);
Fragment frag(origin, new_loc, byte_len, char_len, text);
new_tree.push_item(std::move(frag));

// When marking a fragment as deleted:
Fragment deleted = *cursor.item();  // clone
deleted.locator = Locator::between(
    new_tree.last().locator,
    deleted.locator);
deleted.deletions.push_back(deletion_ts);
deleted.visible = false;
new_tree.push_item(std::move(deleted));
```

### 3.3 Remove Deferred Relocations

The `deferred_relocs` vector, the `DeferredReloc` struct, and the
deferred relocation application loop are removed. Same-locator conflicts
are handled inline by the Zed-style locator reassignment. The
`needs_relocation` flag and associated code are removed.

### 3.4 Remove Post-Processing Sort/Normalize

The entire block after the cursor walk that extracts to vector, applies
deferred relocations, sorts, normalizes, and rebuilds the tree is removed.
The cursor-built tree IS the final tree.

The `normalize_fragments` call is removed. Normalization (atomizing
multi-character fragments at shared locators) is no longer needed because:
- Each fragment pushed to new_tree gets a unique locator (via between())
- No two fragments from different replicas share a locator
- The only shared-locator case was handled by SplitRelocations, which we
  no longer generate

### 3.5 Stop Generating SplitRelocations

`op.split_relocations` is always empty for new local edits. The field
remains in the EditOperation struct for backward compatibility — old
operations in flight may still contain SplitRelocations.

The SplitRelocation handling in `apply_remote_edit` is kept unchanged
for backward compatibility.

### 3.6 Visibility and Origin Index

After building the new tree:

```cpp
// Visibility pass: O(n) but no vector copy or tree rebuild
new_tree.for_each_mut([this](Fragment& f) {
    f.visible = f.compute_visible(m_undo_map);
});
m_fragment_tree = std::move(new_tree);

// Origin index: incremental update for new/modified fragments
for (auto& ins : op.inserted_fragments) {
    m_origin_index[ins.origin.replica_id][ins.origin.value] = ins.locator;
}
```

The O(n) visibility pass remains (fragments can have complex undo state
that depends on the undo map). The O(n) origin index rebuild is replaced
with incremental updates for insertions only.

Note: split fragments' origin index entries are NOT updated here because
their origin didn't change — they just have new locators. The origin index
maps origin → locator, and the locator changed. However, the origin index
is only used for remote edit deletions (origin → locator → tree seek), and
the locator change means the old index entry would point to the wrong tree
position. **We must rebuild the origin index** for correctness, OR update
the entries for all fragments that were reassigned locators.

The simplest correct approach: rebuild the origin index after the cursor
walk. This is O(n) but with a very small constant (just walking the tree
and updating a map). No vector extraction, no sorting, no tree rebuild.

### 3.7 Impact on apply_local_edit Structure

The current apply_local_edit is ~300 lines with a complex structure:
cursor phase → suffix → deferred relocs → sort → normalize → rebuild.

After this change:
cursor phase → suffix → visibility pass → replace tree → rebuild origin index.

The cursor phase itself is simplified: no `needs_relocation` flag, no
`deferred_relocs` vector, no SplitRelocation recording. When a same-
locator situation is encountered, locators are simply reassigned via
`Locator::between(max_id, original_id)`.

---

## 4. Wire Format Compatibility

### What changes
- `EditOperation.split_relocations` is always empty for new operations

### What stays the same
- `EditOperation.deletion_runs` — unchanged
- `EditOperation.inserted_fragments` — unchanged (the locator in the wire
  format is the Zed-style locator, which is valid for all replicas)
- `EditOperation.split_relocations` — field exists, old operations may
  contain entries, apply_remote_edit still handles them

### Convergence proof sketch
- Old replica receives new-style operation (no SplitRelocations):
  - Applies deletion runs: splits fragments, keeps original locators
  - Inserts new fragments at specified locator: locator < existing fragments
  - No SplitRelocation to apply
  - Result: same text content, same ordering ✓
- New replica receives old-style operation (with SplitRelocations):
  - apply_remote_edit handles SplitRelocations as before ✓
- Both replicas produce the same visible text ✓

---

## 5. File Map

| File | Action | Change |
|------|--------|--------|
| `libs/collabtext/src/crdt/Locator.cpp` | Modify | Handle equal arguments in between() |
| `libs/collabtext/src/crdt/Buffer.cpp` | Modify | Rewrite apply_local_edit cursor walk, remove deferred relocs + post-processing |

---

## 6. What NOT to Change

- **SumTree.h** — no changes
- **Fragment.h** — no changes
- **Buffer.h** — no changes (unless removing private declarations for deferred reloc helpers)
- **apply_remote_edit** — keep SplitRelocation handling for backward compat
- **EditOperation struct** — keep split_relocations field
- **NetworkSim, EditStrategy** — no changes
- **Existing tests** — all must pass unchanged

---

## 7. Testing

The key correctness test: **tst_fuzz**. The fuzz test exercises random
editing with multiple replicas and checks convergence + ordering invariants
(INV-4). If the Zed-style cursor walk produces correctly ordered trees
without the safety-check fallback, the fuzz test will pass consistently.

- tst_fuzz: 5 stability runs (must pass all 16 tests × 5 runs)
- tst_gc: GC after edits
- tst_realistic: multi-client scenarios
- tst_convergence: multi-replica convergence
- All 14 test targets

### Benchmark targets

- Single-replica 1K: >500 ops/sec (from 361)
- Single-replica 100K: >100 ops/sec (from 68)
- 3-client realistic: >80 ops/sec (from 53)

---

## 8. Expected Performance Impact

### Per-edit cost breakdown (current vs new)

| Step | Current | New |
|------|---------|-----|
| Cursor seek | O(log n) | O(log n) |
| Prefix slice | O(log n) | O(log n) |
| Consume/delete/insert | O(k) | O(k) + k × O(log n) for Locator::between |
| Suffix | O(log n) | O(log n) |
| Ordering check | O(n) | **removed** |
| Extract to vector | O(n) | **removed** |
| Sort | O(n log n) | **removed** |
| Normalize | O(n) | **removed** |
| Tree rebuild | O(n) | **removed** |
| Visibility pass | O(n) | O(n) (for_each_mut) |
| Origin index | O(n) | O(n) (rebuild) |
| **Total** | **O(n log n)** | **O(n)** |

The O(n) visibility and origin index passes remain. These are tree walks
with tiny per-item cost (a bool computation and a map insert). The heavy
O(n) operations (vector extraction, sort, tree rebuild) are eliminated.

For a 1K doc with ~400 fragments: current ≈ 2.8ms. New ≈ 0.5ms (estimated
5-6x improvement). Diminishing returns at the O(n) visibility/index floor.

---

## 9. Success Criteria

- All 14 test targets pass
- tst_fuzz: 5/5 stability runs (no INV-4 violations)
- No ordering check needed (no fallback path)
- SplitRelocations not generated in local edits
- Backward compatibility with old SplitRelocation-containing operations
- Single-replica benchmark: measurable improvement
