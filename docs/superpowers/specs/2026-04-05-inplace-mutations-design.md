# In-Place SumTree Mutations — Design Spec

**Date:** 2026-04-05
**Status:** Design ready
**Prereqs:** Phase 1 (inline text) complete

---

## 1. Motivation

Phase 1 eliminated the O(n*m) origin-interval-lookup but the engine is still
O(n) per edit because every operation:

1. `get_fragments()` — copies all n fragments (including text strings)
2. Mutates the vector (linear scans for deletions, insertions, splits)
3. `set_fragments()` — rebuilds the SumTree from scratch

With 1,200 fragments (typical 3-client scenario), each remote edit copies
1,200 Fragment objects with strings, scans them multiple times, and rebuilds
the tree. This gives 17 ops/sec for 3-client realistic editing.

The competitive engines do O(log n) per edit via in-place tree mutations.
With n=1,200 and a branching factor of 12, that's ~3 node operations instead
of 1,200 — a potential 400x improvement.

---

## 2. Scope

### In scope (this spec)
- SumTree in-place mutation operations (insert, remove, update)
- Increase FRAG_TREE_B from 2 to 6 (MaxChildren 4 → 12)
- Buffer fast paths for common-case remote edits
- Buffer fast path for apply_local_edit (no-relocation case)
- Buffer fast path for undo/redo

### Out of scope
- Full optimization of apply_local_edit relocation/normalization paths
- InsertionIndex incremental updates (it has no readers currently)
- Rope removal from the codebase (already done in Phase 1)

---

## 3. SumTree In-Place Operations

### 3.1 MutableCursor

The existing `Cursor<D>` stores `const Node*` pointers and is read-only.
We need a mutable variant that can modify items and propagate changes.

```cpp
template<typename D>
    requires DimensionOf<D, Summary>
class MutableCursor {
public:
    explicit MutableCursor(SumTree* tree);

    /// Seek to the item at the given target position.
    bool seek(const D& target, Bias bias = Bias::Left);

    /// Current item (mutable), or nullptr if at end.
    Item* item();

    /// Cumulative position at the START of the current item.
    const D& position() const;

    /// True if cursor is past all items.
    bool at_end() const;

    /// Insert an item BEFORE the current cursor position.
    /// If the cursor is at_end, inserts at the end.
    /// Handles leaf splitting and summary propagation.
    void insert(Item item);

    /// Remove the item at the current cursor position.
    /// Cursor advances to the next item.
    /// Handles leaf underflow and rebalancing.
    void remove();

    /// Update the item at the current cursor position in place.
    /// Calls fn(item), then recomputes summaries up to root.
    template<typename F>
    void update(F&& fn);

private:
    SumTree* m_tree;
    D m_position;

    struct StackEntry {
        NodePtr* node_ptr;  // Mutable pointer to the node
        uint16_t index;     // Index into this node
    };
    std::vector<StackEntry> m_stack;

    // Seek recursively, calling ensure_mutable at each level.
    bool seek_internal(NodePtr& node, const D& target, Bias bias);

    // Propagate summary changes from leaf up to root.
    void propagate_summaries();

    // Insert at leaf, handle splitting.
    void insert_at_leaf(Item item);

    // Remove from leaf, handle underflow.
    void remove_from_leaf();
};
```

Key differences from read-only Cursor:
- Stores `NodePtr*` (mutable shared_ptr references) instead of `const Node*`
- Calls `ensure_mutable()` at each level during seek
- Can modify items and propagate summary changes
- Can insert/remove items with rebalancing

### 3.2 insert() — O(log n)

Insert an item before the current cursor position:

1. If leaf has space (count < MaxChildren): shift items right, insert, update
   summaries up to root.
2. If leaf is full: split leaf at midpoint. Insert item into appropriate half.
   Propagate the new node up the stack (may split internal nodes too, up to root).

### 3.3 remove() — O(log n)

Remove the item at the current cursor position:

1. Shift items left in the leaf, decrement count.
2. If leaf drops below minimum (count < B): merge with a sibling or borrow
   from sibling to restore balance.
3. Propagate summary changes up to root.
4. If root has only one child after rebalancing, lower the tree height.

**Simplification**: Since the CRDT engine never bulk-removes (individual
fragment removal is rare — only in GC), we can use a simpler rebalancing
strategy: just merge with the left sibling if possible, otherwise merge with
right sibling. No borrowing needed for correctness (the tree stays valid
with varying node sizes between B and 2*B).

### 3.4 update() — O(log n)

Modify an item in place:

1. Call the update function on the item.
2. Recompute the item's summary.
3. Propagate summary changes up the stack to root.

This is the simplest operation — no structural changes, just summary propagation.

### 3.5 Public SumTree API

```cpp
template<typename D>
MutableCursor<D> mutable_cursor() { return MutableCursor<D>(this); }
```

### 3.6 ensure_mutable Path Cloning

When the MutableCursor seeks, it must clone shared nodes (copy-on-write).
The `ensure_mutable()` function already handles this:

```cpp
static NodePtr& ensure_mutable(NodePtr& ptr) {
    if (ptr.use_count() > 1) {
        ptr = std::make_shared<Node>(*ptr);
    }
    return ptr;
}
```

The MutableCursor calls this at each level during seek, creating a unique
path from root to the target leaf. Sibling subtrees remain shared.

---

## 4. Increase FRAG_TREE_B

Change `FRAG_TREE_B` from 2 to 6:

```cpp
static constexpr std::size_t FRAG_TREE_B = 6;
```

This changes MaxChildren from 4 to 12. Effects:
- Tree depth for 1,500 fragments: ~6 levels → ~3 levels
- Each node holds more items, reducing pointer chasing
- Summary recomputation per node touches more children but happens less often

This is a one-line change with major impact on all tree operations.

---

## 5. Buffer Fast Paths

### 5.1 apply_remote_edit — Fast Path

The common case for a remote edit is: 0-few deletion runs + 0 relocations + 1
insertion. For this case, we bypass get_fragments/set_fragments entirely:

```
fn apply_remote_edit_fast(op):
    // 1. Apply deletion runs via mutable cursor
    for each deletion_run in op.deletion_runs:
        cursor = m_fragment_tree.mutable_cursor<FragmentOrderDim>()
        seek to (run.replica_id, run.start_value)
        for each character in run:
            if fragment needs splitting: split via cursor
            mark fragment as deleted via cursor.update()

    // 2. Insert new fragments via mutable cursor
    for each insertion in op.inserted_fragments:
        cursor = m_fragment_tree.mutable_cursor<FragmentOrderDim>()
        seek to (ins.locator, ins.origin)
        cursor.insert(Fragment{...})

    // 3. Check if normalization needed (rare)
    if has_split_relocations(op):
        // Fall back to full path
        return apply_remote_edit_full(op)

    // 4. Update clocks/version (unchanged)
```

**When to use fast path:** No split_relocations in the operation. Split
relocations indicate concurrent same-locator insertions which require
normalization — the complex case.

**When to fall back:** Any split_relocations present. This is rare in
practice (requires two replicas inserting at the exact same document position
concurrently).

### 5.2 apply_remote_undo — In-Place Visibility

Currently undo/redo extracts all fragments and rebuilds to recompute
visibility. With in-place updates:

```
fn apply_remote_undo(op):
    // 1. Update undo map (unchanged)
    for each (edit_id, count) in op.counts:
        m_undo_map.insert(...)

    // 2. Walk all fragments, recompute visibility in place
    m_fragment_tree.for_each_mut([&](Fragment& f) {
        f.visible = f.compute_visible(m_undo_map);
    });

    // 3. Recompute all summaries bottom-up
    m_fragment_tree.recompute_all_summaries();
```

This avoids copying all fragments to a vector and rebuilding the tree.
Still O(n) for visibility, but avoids the copy + rebuild overhead.

A `for_each_mut` method and `recompute_all_summaries` method are needed on
SumTree.

### 5.3 apply_local_edit — Skip Rebuild When No Relocations

The current `apply_local_edit` builds a new tree via cursor operations
(O(log n) prefix copy, deletions, insertions, suffix copy), then extracts
to vector, applies deferred relocations, sorts, normalizes, and rebuilds.

When there are no deferred relocations (the common case — no same-locator
conflicts):

```
fn apply_local_edit(...):
    // ... existing cursor-based prefix/delete/insert/suffix ...
    // Build new_tree via push_item/push_tree

    if deferred_relocs.empty():
        // The new_tree is already correctly ordered!
        // Just set visibility and use it directly.
        m_fragment_tree = std::move(new_tree)
        // Recompute visibility for any fragments that changed
        set_fragments_from_tree()  // O(n) visibility pass, no vector copy
        return op

    // Fall back to extract-sort-rebuild for relocation case
    // ... existing code ...
```

A `set_fragments_from_tree()` helper walks the tree in-place to set
visibility (using `for_each_mut`), then recomputes summaries. Avoids the
vector extraction and tree rebuild.

### 5.4 Deletion Run Optimization

`apply_deletion_runs` currently does a linear scan of all fragments for each
deletion run. With a mutable cursor that can seek by FragmentOrderDim:

```
fn apply_deletion_run_fast(run, deletion_id):
    cursor = m_fragment_tree.mutable_cursor<FragmentOrderDim>()
    seek to (run.replica_id, run.start_value)

    remaining = run.count
    while remaining > 0 and cursor.item():
        fragment = cursor.item()
        if fragment covers the target range:
            if needs splitting at start:
                // Split: cursor now points at second half
                split_at_cursor(cursor, offset)
            if needs splitting at end:
                split_at_cursor(cursor, offset)
            cursor.update([&](Fragment& f) {
                f.deletions.push_back(deletion_id);
            })
            remaining -= chars_consumed
            cursor.next()
```

This is O(log n + k) where k is the number of fragments touched by the run,
instead of O(n) for a linear scan.

---

## 6. SumTree Additional Methods

### 6.1 for_each_mut

```cpp
template<typename F>
void for_each_mut(F&& fn) {
    if (m_root) for_each_node_mut(ensure_mutable(m_root), fn);
}
```

Walks all items, calling `ensure_mutable` to allow modification.

### 6.2 recompute_all_summaries

```cpp
void recompute_all_summaries() {
    if (m_root) recompute_all_recursive(m_root);
}
```

Bottom-up recomputation of all summaries after in-place item modifications.

### 6.3 split_item

A cursor operation that splits the current item into two items:

```cpp
/// Split the current item into two at byte_off.
/// split_fn(item, byte_off) returns the second half; item is truncated to first half.
template<typename F>
void split_item(F&& split_fn);
```

This is needed for deletion runs that need to split fragments at character
boundaries.

---

## 7. File Map

| File | Action | Change |
|------|--------|--------|
| `libs/collabtext/src/crdt/SumTree.h` | Modify | Add MutableCursor, for_each_mut, recompute_all_summaries, split_item |
| `libs/collabtext/src/crdt/Buffer.h` | Modify | Change FRAG_TREE_B to 6 |
| `libs/collabtext/src/crdt/Buffer.cpp` | Modify | Add fast paths for apply_remote_edit, apply_remote_undo, apply_local_edit |

---

## 8. What NOT to Change

- **Fragment.h** — no changes (Phase 1 is complete)
- **Existing tests** — all must continue to pass
- **apply_local_edit relocation/normalization** — kept as fallback
- **InsertionIndex** — no readers, skip optimization
- **NetworkSim, EditStrategy** — no changes
- **Rope.h** — no changes

---

## 9. Testing Strategy

### Unit tests for SumTree mutations

New test file `tst_sumtree_mutations.cpp` or extend `tst_sumtree.cpp`:
- insert at beginning/middle/end
- insert causing leaf split
- insert causing cascading splits (root growth)
- remove from beginning/middle/end
- remove causing leaf underflow and merge
- update in place with summary verification
- split_item at various positions
- for_each_mut modifies all items correctly
- Mixed operations: insert, update, remove in sequence

### Regression tests

All 14 existing test targets must pass unchanged. The fast paths produce
identical results to the slow paths — they're optimizations, not new behavior.

### Benchmark validation

Compare before/after on:
- single_replica_throughput (all doc sizes)
- realistic_3_client_throughput (target: >10x improvement)
- reconnect_sync_cost
- gc_under_sustained_editing
- tombstone_degradation (target: complete without timeout)

---

## 10. Expected Performance Impact

### Per-operation cost

| Operation | Before (Phase 1) | After (Phase 2) |
|-----------|-------------------|------------------|
| Remote edit (common) | O(n) | O(log n) |
| Remote edit (relocation) | O(n) | O(n) fallback |
| Remote undo | O(n) copy+rebuild | O(n) in-place |
| Local edit (no reloc) | O(n) copy+rebuild | O(n) visibility pass |
| Local edit (relocation) | O(n) copy+rebuild | O(n) fallback |
| GC | O(n) | O(n) unchanged |

### Projected benchmark improvement

With n=1,200 fragments and B=6 (tree depth ~3):

- **Remote edit (common case):** 59ms → ~0.1ms (O(n) → O(log n))
- **3-client realistic:** 17 ops/sec → target 500+ ops/sec
- **Tombstone degradation 50% (3,390 frags):** 105ms/op → target <1ms/op

The O(n) paths (undo, local edit, GC) benefit from avoiding the vector copy
and tree rebuild, but the improvement is constant-factor (2-3x), not
asymptotic.

---

## 11. Success Criteria

- All 14 existing test targets pass unchanged
- New SumTree mutation tests pass
- 3-client realistic benchmark: >100 ops/sec (6x improvement minimum)
- tombstone_degradation 50%: completes in <10s (was timing out at 300s)
- Stability: 5 tst_fuzz runs + 5 tst_realistic runs all pass
