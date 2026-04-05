# CRDT Engine Garbage Collection — Design Spec

**Date:** 2026-04-05
**Status:** Design ready
**Prereqs:** Gen 2 complete (all 3 phases), benchmark suite establishing tombstones as #1 bottleneck

---

## 1. Motivation

Benchmark results (`docs/reports/2026-04-03-benchmark-results.md`) show that
tombstone accumulation is the dominant performance bottleneck:

- 50% tombstones (3,390 fragments) = **21x slower** than clean (50 fragments)
- 90% tombstones (4,422 fragments) = **30x slower**
- Any document with >3,000 fragments drops below 100 ops/sec
- Cost grows **super-linearly** with fragment count due to O(n*m)
  origin-interval-lookup in `set_fragments()`

The root cause: deleted fragments (tombstones) are never removed from the
fragment tree. Every operation pays the cost of scanning all tombstones even
though they contribute nothing to the visible document.

### 1.1 Industry Context

Research into production CRDT systems reveals:

| System | GC Strategy |
|--------|-------------|
| Zed | **None** — tombstones accumulate forever |
| Automerge | **None** — columnar RLE for storage only |
| Diamond-types | **None** — RLE spans reduce effective count |
| Yjs | Content-clearing (free bytes, keep identity stub) |
| Loro | Explicit operator watermark → physical removal |
| Antimatter | Distributed 2-RTT ack protocol → physical removal |

Zed (our reference implementation) has no GC. We would be the first
Zed-derived CRDT to implement tombstone collection.

### 1.2 Why Yjs-style Content-Clearing Doesn't Work Here

Yjs zeros out tombstone bytes but keeps the node in the data structure.
This works for Yjs's doubly-linked list where stub identity is cheap.

In our SumTree-based engine, **fragment count drives cost, not byte size**.
The O(n*m) origin-interval-lookup in `set_fragments()` scans every
`old_entries` element regardless of its `byte_length`. A zero-byte stub
is just as expensive to scan as a full fragment. Only **physical removal**
(reducing the number of entries in the tree) fixes the benchmark bottleneck.

---

## 2. Design: Bounded-Undo Physical Tombstone Removal

### 2.1 Core Idea

Cap the undo stack at a configurable `MAX_UNDO_DEPTH`. When an undo entry
ages out, its deletion becomes permanent — the tombstones it created can
never become visible again. A `collect_garbage()` sweep removes those
tombstones from the fragment tree, shrinking both the tree and the
deleted-text rope.

### 2.2 GC Safety Condition

A tombstone fragment F is **GC-eligible** when:

1. `F.visible == false` (it is a tombstone)
2. For EVERY `D` in `F.deletions`:
   a. `D.replica_id == m_replica_id` (the deletion is local), AND
   b. `D` does not appear as the `deletion_id` of any entry in `m_undo_stack`

In other words: every deletion that made F invisible is local AND permanent
(not undoable through any future sequence of undo/redo operations).

**Why remote deletions block GC:** A tombstone with a remote deletion_id
could be revived by a remote undo. Additionally, future remote
SplitRelocations may reference the tombstone's fragment position. Without
watermark-based coordination (Option D), we cannot know if a remote
replica might still reference the tombstone. Tombstones with any remote
deletion are preserved until watermark-based GC confirms they're permanent.

**Why we don't check F.origin:** Undoing the insertion of an already-deleted
fragment makes it "more invisible" (insertion undone), not visible. The
fragment becomes visible only when ALL its deletions are undone AND its
insertion is NOT undone. So only the deletions vector determines GC safety.

### 2.3 Protected Set

Before each GC sweep, build a set of protected deletion IDs:

```
protected_ids = { entry.deletion_id | entry in m_undo_stack }
```

A tombstone is GC-eligible iff none of its `deletions` are in
`protected_ids`.

### 2.4 GC Mechanism

`collect_garbage()` leverages the existing `set_fragments()` infrastructure:

```
fn collect_garbage():
    protected = build_protected_set()
    frags = get_fragments()
    frags.remove_if(|f| is_gc_eligible(f, protected))
    set_fragments(frags)   // rebuilds tree + ropes, excluding removed frags
```

This is correct because `set_fragments()` rebuilds both ropes from the
old ropes via origin-interval-lookup. Fragments absent from the new vector
simply have their text omitted from the rebuilt ropes. No special rope
surgery is needed.

### 2.5 Bounded Undo

```
static constexpr size_t DEFAULT_MAX_UNDO_DEPTH = 1000;

fn apply_local_edit(...):
    // ... existing logic ...
    m_undo_stack.push_back(entry)
    m_undo_cursor = m_undo_stack.size()
    trim_undo_stack()

fn trim_undo_stack():
    if m_undo_stack.size() > m_max_undo_depth:
        excess = m_undo_stack.size() - m_max_undo_depth
        m_undo_stack.erase(first `excess` entries)
        m_undo_cursor = max(m_undo_cursor - excess, 0)
```

When entries are trimmed, their `deletion_id` leaves the protected set,
making their tombstones GC-eligible on the next `collect_garbage()` call.

### 2.6 GC Trigger Policy

`collect_garbage()` is an explicit method — the caller decides when to
invoke it. Recommended trigger points:

- After `trim_undo_stack()` discards entries (new tombstones became eligible)
- Periodically when fragment count exceeds a threshold
- On document save or sync checkpoint

GC is NOT called automatically on every edit — the sweep has O(n) cost
(iterating all fragments) and should be batched.

---

## 3. Bonus: Fragment Coalescing

### 3.1 Opportunity

Adjacent fragments from the same original insertion that were split by
a subsequent operation can be re-merged when the splitting content is
removed by GC. This further reduces fragment count.

### 3.2 Coalescing Conditions

Two adjacent fragments F1, F2 (F1 immediately before F2 in the tree) can
be merged when ALL of:

1. `F1.visible == F2.visible` (same visibility)
2. `F1.locator == F2.locator` (same fractional position)
3. `F1.origin.replica_id == F2.origin.replica_id` (same replica)
4. `F1.origin.value + F1.length == F2.origin.value` (contiguous timestamps)
5. `F1.deletions == F2.deletions` (same deletion history)

The merged fragment has:
- `origin = F1.origin`
- `byte_length = F1.byte_length + F2.byte_length`
- `length = F1.length + F2.length`
- All other fields copied from F1 (same as F2 by conditions above)

### 3.3 When Coalescing Helps

The primary case: normalization atomizes multi-character fragments at
shared locators (creating single-char fragments). After normalization is
no longer needed (the concurrent insertion has been fully integrated),
adjacent single-char fragments from the same replica can be re-merged.

### 3.4 Implementation

Run as a linear pass over the fragment vector inside `collect_garbage()`,
after tombstone removal:

```
fn coalesce_fragments(frags):
    i = 0
    while i + 1 < frags.size():
        if can_coalesce(frags[i], frags[i+1]):
            frags[i].byte_length += frags[i+1].byte_length
            frags[i].length += frags[i+1].length
            frags.erase(i+1)
        else:
            i++
```

### 3.5 Practical Impact

Limited for the tombstone problem (isolated tombstones between visible
fragments can't coalesce with visible neighbors). Main value is reducing
fragment count after multi-replica normalization cycles. Cheap O(n) pass
so we include it as a bonus optimization.

---

## 4. API

### 4.1 New Public Methods on Buffer

```cpp
/// Run garbage collection: remove tombstones whose deletions are no longer
/// in the undo stack. Also coalesces adjacent same-origin fragments.
/// Returns the number of tombstones removed.
size_t collect_garbage();

/// Get/set the maximum undo depth. When the stack exceeds this, the oldest
/// entries are discarded, making their tombstones GC-eligible.
size_t max_undo_depth() const;
void set_max_undo_depth(size_t depth);

/// Query: number of tombstone (invisible) fragments.
size_t tombstone_count() const;

/// Query: total fragment count (visible + tombstone).
size_t fragment_count() const;
```

### 4.2 New Private Methods on Buffer

```cpp
/// Build the set of protected deletion IDs from the undo stack.
std::unordered_set<uint64_t> build_gc_protected_set() const;

/// Check if a tombstone fragment is eligible for garbage collection.
bool is_gc_eligible(const Fragment& f,
                    const std::unordered_set<uint64_t>& protected_ids) const;

/// Merge adjacent fragments that meet coalescing conditions.
void coalesce_fragments(std::vector<Fragment>& frags) const;

/// Trim the undo stack to m_max_undo_depth.
void trim_undo_stack();
```

### 4.3 New Member Variables on Buffer

```cpp
size_t m_max_undo_depth = 1000;  // DEFAULT_MAX_UNDO_DEPTH
```

---

## 5. Correctness Invariants

All existing invariants (INV-1 through INV-9) must hold after GC:

- **INV-1:** visible_length == text().size()
- **INV-2:** visible fragment byte sum == visible_length
- **INV-4:** fragment ordering (locator, origin) strictly non-decreasing
- **INV-5:** every fragment has non-zero byte_length and length
- **INV-8:** rope byte lengths match fragment sums
- **INV-9:** byte_length sums match rope lengths

**New invariant (INV-10):** After `collect_garbage()`, no tombstone fragment
exists whose every deletion_id is absent from the undo stack's protected
set. (i.e., GC was thorough — no eligible tombstones were missed.)

**Convergence:** GC is a local operation. Two replicas that have observed
the same operations but run GC at different times will still have identical
`visible_text` (convergence depends only on visible fragments, which GC
does not touch).

---

## 6. Multi-Replica Considerations (Future: Option D)

### 6.1 The Distributed Problem

In a multi-replica scenario, a tombstone's deletion might be undoable by a
REMOTE replica (whose undo stack we don't see). Local-only GC is safe when:

- Single-user (no remote replicas)
- All replicas have synchronized and agreed on a GC watermark

### 6.2 Future Watermark API (Not Implemented Now)

```cpp
/// Remove all tombstones whose max_version <= watermark AND are not
/// undo-protected. Called by the application when it knows all replicas
/// have synced past the watermark.
size_t compact(const Global& watermark);
```

The infrastructure from `collect_garbage()` (sweep, removal, coalescing)
will be reused wholesale. The only difference is the eligibility condition:
instead of checking the local undo stack, check both the watermark AND
the undo stack.

`Global::meet()` (already implemented) computes the watermark as the
component-wise minimum of all replica version vectors.

### 6.3 Remote Edit After GC

If a remote edit references a GC'd fragment (via deletion_runs targeting
a Lamport range that no longer exists in the tree):

- **Deletion of GC'd fragment:** No-op. The fragment is already permanently
  deleted. The deletion_run targets don't match any fragment — the
  `apply_deletion_runs()` loop simply finds no match and moves on.

- **Insertion adjacent to GC'd fragment:** Works correctly. Locators are
  fractional position identifiers, not references to specific fragments.
  The new fragment's locator still sorts in the correct position in the
  tree even without the GC'd tombstone present.

---

## 7. What NOT To Change

- **set_fragments()** — Used as-is. GC works by filtering the fragment
  vector before passing to set_fragments().
- **SumTree implementation** — No changes needed.
- **UndoMap** — Entries are retained even after undo stack trimming.
  The UndoMap records permanent undo state; it is not trimmed by GC.
- **OperationQueue / deferred ops** — GC does not affect causal ordering.
- **Anchor system** — Anchors reference (replica_id, char_value, bias).
  GC'd tombstones have no visible position, so no anchor can be pointing
  at one. If an anchor references a character in a GC'd fragment, the
  anchor resolution falls through to the next fragment (existing behavior
  for deleted fragments).

---

## 8. Testing Strategy

### 8.1 Unit Tests (tst_gc.cpp)

1. **Basic GC removes tombstones:** Create tombstones, call
   `collect_garbage()`, verify fragment count decreased and text unchanged.

2. **GC respects undo protection:** Create tombstone, verify it's NOT
   removed while its deletion_id is in the undo stack. Clear the undo
   stack, verify it IS removed.

3. **GC + undo interaction:** Create tombstones, undo some deletions,
   verify GC only removes tombstones whose deletions are not in the stack.

4. **Bounded undo triggers GC eligibility:** Set max_undo_depth=5, perform
   10 edits with deletions, verify oldest tombstones become GC-eligible.

5. **Fragment coalescing:** Create fragments that meet coalescing
   conditions, verify they're merged. Verify non-coalesceable fragments
   are left alone.

6. **GC preserves all invariants:** Run `check_invariants()` after every
   GC call.

7. **GC + convergence:** Two replicas with tombstones, cross-apply,
   one runs GC. Verify visible text still matches.

### 8.2 Fuzz Test Integration

Add GC invocations to the existing fuzz test loop (tst_fuzz.cpp):
with some probability, call `collect_garbage()` on a random replica.
Verify convergence and all invariants still hold.

### 8.3 Benchmark Re-run

After implementation, re-run the tombstone degradation benchmark with
periodic GC calls to measure the improvement.
