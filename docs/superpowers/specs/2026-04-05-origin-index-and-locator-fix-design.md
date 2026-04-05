# Origin Index + Locator::between Fix — Design Spec

**Date:** 2026-04-05
**Status:** Design ready
**Prereqs:** Phase 2 (in-place mutations) complete

---

## 1. Motivation

Two independent issues prevent the engine from achieving O(log n) for all
common edit operations:

1. **Deletion runs require O(n) linear scan.** The fragment tree is ordered
   by (locator, origin), but deletion runs target characters by
   (replica_id, origin_value). Finding the target fragment requires scanning
   all fragments because origin order doesn't correspond to locator order.
   This keeps the full remote edit path at O(n).

2. **Locator::between can return hi.** `biased_mid(lo, hi)` returns
   `lo + 1 == hi` when the gap is 1, violating the strict-between contract.
   This prevents the apply_local_edit fast path from covering insertions
   (the new locator could equal the next fragment's locator, breaking tree
   ordering).

Fixing both makes the common-case remote edit AND local edit paths O(log n).

---

## 2. Feature A: Origin Index

### 2.1 The Problem

`apply_deletion_runs` scans all fragments linearly for each deletion run:

```
for each deletion_run:
    for each fragment in frags:
        if fragment.origin covers the target range:
            split/delete
```

With n=1,200 fragments and k deletion runs, this is O(n*k) per remote edit.

### 2.2 The Solution

A per-replica sorted map from origin_value to locator:

```cpp
std::unordered_map<uint16_t, std::map<uint32_t, Locator>> m_origin_index;
```

Lookup: given (replica_id, target_value), find the fragment:
1. `auto it = m_origin_index[replica_id].upper_bound(target_value);`
2. Decrement `it` to find the entry with largest origin_value <= target_value
3. That entry's locator + the origin give the FragmentOrderDim target
4. Seek the main tree in O(log n)

Total: O(log m + log n) where m = fragments from that replica.

### 2.3 Maintenance

The index is maintained alongside the main fragment tree:

**On fragment insertion** (fast path insert_item):
```cpp
m_origin_index[frag.origin.replica_id][frag.origin.value] = frag.locator;
```

**On fragment split** (split_fragment_at): the original entry still covers
the first half (same origin, same locator). Add entry for second half:
```cpp
m_origin_index[second.origin.replica_id][second.origin.value] = second.locator;
```

**On full rebuild** (set_fragments): clear and rebuild:
```cpp
void Buffer::rebuild_origin_index() {
    m_origin_index.clear();
    m_fragment_tree.for_each([&](const Fragment& f) {
        m_origin_index[f.origin.replica_id][f.origin.value] = f.locator;
    });
}
```

**On GC** (sweep_and_coalesce): rebuilding after set_fragments handles it.

### 2.4 Fast Path for Deletion Runs

With the origin index, `apply_deletion_runs` becomes:

```
for each deletion_run(replica_id, start_value, count):
    locator = origin_index_lookup(replica_id, start_value)
    cursor_target = FragmentOrderDim{locator, Lamport(replica_id, start_value)}
    seek main tree to cursor_target
    split/mark deleted using edit_item/insert_item
```

This is O(log n) per deletion run instead of O(n).

### 2.5 Expanded apply_remote_edit Fast Path

The fast path currently only handles insertion-only edits. With the origin
index, it expands to handle:
- Deletion runs (via origin index lookup + edit_item)
- Single-character insertions (via insert_item)
- Still falls back for split_relocations and multi-char insertions

This covers the vast majority of real-world remote edits (typing + backspace).

---

## 3. Feature B: Locator::between Fix

### 3.1 The Bug

In `Locator.cpp`, `biased_mid(lo, hi)`:

```cpp
static uint64_t biased_mid(uint64_t lo, uint64_t hi) {
    assert(lo < hi);
    uint64_t gap = hi - lo;
    uint64_t step = gap >> 48;
    if (step == 0) step = 1;
    return lo + step;  // BUG: when gap == 1, returns lo + 1 == hi
}
```

When `gap == 1`, `step = 1`, return value = `lo + 1 = hi`. This violates
the `lo < result < hi` contract.

### 3.2 The Fix

Two changes:

**A. Fix biased_mid to never return hi:**

```cpp
static uint64_t biased_mid(uint64_t lo, uint64_t hi) {
    assert(lo < hi);
    uint64_t gap = hi - lo;
    if (gap == 1) return lo;  // No integer strictly between; caller descends
    uint64_t step = gap >> 48;
    if (step == 0) step = 1;
    return lo + step;
    // Proof: gap >= 2, step = max(1, gap >> 48).
    // If gap < 2^48: step = 1, lo + 1 < hi (since gap >= 2). ✓
    // If gap >= 2^48: step = gap >> 48 < gap, lo + step < hi. ✓
}
```

**B. Fix the caller at line 112 (no-more-digits case):**

Currently:
```cpp
uint64_t mid = biased_mid(ld, hd);
result.push_back(mid);
return Locator(result);
```

After fix:
```cpp
if (ld + 1 < hd) {
    uint64_t mid = biased_mid(ld, hd);
    result.push_back(mid);
    return Locator(result);
}
// ld + 1 == hd: no room at this digit level. Descend below ld.
// Since lo has no more digits, lo = (..., ld). Picking (..., ld, X)
// where X > DMIN gives a result strictly > lo and strictly < hi
// (since hi's prefix at this level has hd > ld).
result.push_back(ld);
result.push_back(biased_mid(DMIN, DMAX));
return Locator(result);
```

### 3.3 Impact on apply_local_edit Fast Path

After this fix, `Locator::between(lo, hi)` always returns a value strictly
between lo and hi. The apply_local_edit fast path can be expanded from
deletion-only to all edits without relocations (including insertions),
because the new locator is guaranteed to maintain tree ordering.

---

## 4. File Map

| File | Action | Change |
|------|--------|--------|
| `libs/collabtext/src/crdt/Locator.cpp` | Modify | Fix biased_mid + between caller |
| `libs/collabtext/src/crdt/Buffer.h` | Modify | Add m_origin_index member |
| `libs/collabtext/src/crdt/Buffer.cpp` | Modify | Add origin index maintenance, expand fast paths |
| `libs/collabtext/tests/tst_locator.cpp` | Modify | Add tests for between() edge cases |

---

## 5. What NOT to Change

- **SumTree.h** — no changes
- **Fragment.h** — no changes
- **Existing tests** — all must pass unchanged
- **The full path** — kept as fallback, still used for relocations

---

## 6. Testing

### Locator::between tests

- between() with adjacent digits (gap == 1) at various levels
- between() with gap == 1 at the last digit of lo
- Verify result is always strictly lo < result < hi
- Stress test: generate 1000 sequential between() calls, verify ordering

### Origin index tests

- Build index from fragment tree, verify lookups
- Verify after insert_item, index contains new entry
- Verify after split, index contains both halves
- Full path rebuild consistency

### Integration

- tst_fuzz 5 runs (random seeds)
- tst_realistic full suite
- All 13 fast tests

### Benchmarks

- 3-client realistic: target >50 ops/sec (from 23)
- tombstone_degradation: target improvement at 50% case

---

## 7. Success Criteria

- Locator::between never returns lo or hi (verified by new tests)
- apply_remote_edit fast path covers deletion + insertion edits
- apply_local_edit fast path covers all non-relocation edits
- All 14 test targets pass
- 3-client realistic benchmark improves measurably
