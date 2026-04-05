# Performance Refactor Results — Phases 1 & 2

**Date:** 2026-04-05
**Build:** Release (GCC 15.2, -O2, Qt 6.11)
**Hardware:** x86_64, NVIDIA GPU system (Manjaro Linux)

---

## 1. Summary

Two refactoring phases reduced per-edit cost by 2-3x across all benchmarks
and fixed the tombstone degradation timeout. The remaining bottleneck is the
O(n) extract-mutate-rebuild pattern for edits involving deletions.

### Phase 1: Inline Fragment Text

**Commit:** `574e783` — Stored text directly in `Fragment.text`, eliminated
the O(n*m) origin-interval-lookup in `set_fragments()`, removed Rope
dependency from Buffer. Net -270 lines.

### Phase 2: In-Place SumTree Mutations

**Commits:** `889c650` — Added `edit_item`, `insert_item`, `remove_item` to
SumTree (O(log n) operations). Increased `FRAG_TREE_B` from 2 to 6
(MaxChildren 4 → 12). Added Buffer fast paths for undo/redo (in-place
visibility) and insertion-only remote edits (O(log n) `insert_item`).
19 new SumTree mutation tests.

---

## 2. Benchmark Comparison

### Single-Replica Throughput

| Doc Size | Pre-refactor | Phase 1 | Phase 2 | Total improvement |
|----------|-------------:|--------:|--------:|------------------:|
| 1K | 203 ops/sec | 192 | **357** | **1.8x** |
| 10K | 121 ops/sec | 117 | **232** | **1.9x** |
| 100K | 27 ops/sec | 30 | **60** | **2.2x** |
| 1M | 22 ops/sec | 33 | **66** | **3.0x** |

### Tombstone Degradation (5K doc)

| Tombstone % | Pre-refactor | Phase 1 | Phase 2 | Total |
|------------:|-------------:|--------:|--------:|------:|
| 0% | 139 ops/sec | 144 | **298** | **2.1x** |
| 50% (3,390 frags) | 8 ops/sec | 10 | **18** | **2.3x** |
| 75% | Timeout (300s) | Timeout | n/a | — |
| 90% (4,933 frags) | Timeout | Timeout | **14 ops/sec** | **fixed** |

The 90% tombstone case now completes — this was the primary usability fix.

### Multi-Client Realistic Editing

| Benchmark | Pre-refactor | Phase 1 | Phase 2 | Total |
|-----------|-------------:|--------:|--------:|------:|
| 3-client (300 ops) | 17 ops/sec | 17 | **23** | **1.4x** |
| GC sustained (600 ops) | 6 ops/sec | 6 | **9** | **1.5x** |

### Test Suite Performance

| Suite | Pre-refactor | Phase 2 | Improvement |
|-------|-------------:|--------:|------------:|
| Fuzz (16 tests) | 6.1s | **3.0s** | **2x** |
| Realistic (11 tests) | 180s | **101s** | **1.8x** |
| Fast suite (13 tests) | 15.5s | **8.3s** | **1.9x** |

---

## 3. What Improved and Why

### FRAG_TREE_B = 6 (biggest single change)

Increasing the branching factor from 2 to 6 reduced tree depth from ~6
levels to ~3 for typical fragment counts. This halved the cost of every
tree operation: push_item, cursor seek, slice, and the new mutations.
This accounts for the ~2x across-the-board improvement.

### Inline fragment text (Phase 1)

Eliminating the O(n*m) origin-interval-lookup was most impactful on large
documents. The 1M doc improved 50% (22 → 33 ops/sec) in Phase 1 alone.
Small docs saw no change because the lookup was fast with few fragments.

### In-place undo/redo (Phase 2)

Replacing get_fragments/set_fragments with for_each_mut for undo/redo
eliminates vector copy + tree rebuild. Undo-heavy workloads benefit.

### Insertion-only remote edit fast path (Phase 2)

Single-character remote insertions (the common typing case) use O(log n)
insert_item instead of O(n) get_fragments/set_fragments. Benefits
multi-client scenarios with light deletion workloads.

---

## 4. What Didn't Improve (and Why)

### 3-client realistic: 17 → 23 ops/sec (only 1.4x)

The spec targeted 500+ ops/sec. The gap is because most multi-client
operations involve deletions, which still use the O(n) full path:

1. `get_fragments()` — O(n) copies all fragments + strings
2. `apply_deletion_runs` — O(n) linear scan per deletion run
3. Vector manipulation (splits, relocations) — O(n)
4. `set_fragments()` — O(n) tree rebuild

The insertion-only fast path helps but only covers a fraction of operations
in realistic editing (typing includes deletions from backspace, etc.).

### The remaining O(n) bottleneck

Every edit involving deletions goes through the full path because deletion
runs identify fragments by origin timestamp (replica_id, value), but the
fragment tree is ordered by (locator, origin). Finding a fragment by origin
in a locator-ordered tree requires O(n) linear scan.

**Fix:** An origin-based secondary index that maps (replica_id, origin_value)
to tree position, enabling O(log n) deletion lookups.

### Locator::between edge case

The apply_local_edit fast path (skip extract-sort-rebuild) only covers
deletion-only edits. Insertions are excluded because `Locator::between(lo, hi)`
can return `hi` when the gap is 1 digit, violating the "strictly between"
contract. This is masked by the full path's sort but breaks the fast path.

**Fix:** Fix `biased_mid()` in Locator to always return a value strictly
less than `hi`.

---

## 5. Competitive Position Update

| Metric | Pre-refactor | Now | Yjs | diamond-types |
|--------|-------------:|----:|----:|-------------:|
| 1K doc | 203 ops/sec | 357 | ~100K+ | millions |
| 100K doc | 27 ops/sec | 60 | ~50K+ | millions |
| 1M doc | 22 ops/sec | 66 | ~10K+ | millions |

We've closed ~0.3 orders of magnitude of the 2-5 order gap. The fundamental
architecture (locator-ordered B+ tree with extract-mutate-rebuild) still
limits us. Full parity requires eliminating the O(n) full path for all
common operations.

---

## 6. Phase 3: Origin Index + Locator Fix (Complete)

**Commit:** `9371f31`

### Origin Index
Added `m_origin_index` — per-replica sorted map (`unordered_map<uint16_t,
map<uint32_t, Locator>>`) for O(log n) fragment lookup by origin timestamp.
Expanded `apply_remote_edit_fast` to handle whole-fragment deletions using
the origin index + edit_item. Falls back to full path for partial deletions
(which require fragment splitting).

### Locator::between Fix
Fixed `biased_mid()` to return `lo` (not `lo + 1 == hi`) when gap == 1.
Fixed `between()` to descend to the next digit level when gap == 1 at the
no-more-digits case. New stress tests verify strict ordering.

**Known regression:** Prepend operations grow locator depth ~1 level per 2
inserts (was ~1 level per 65536 inserts). This is a trade-off for
correctness — the old behavior could produce locators equal to neighbors.
Future work: reverse-biased allocation for prepend patterns.

**Task 4 (local edit fast path expansion) reverted:** The cursor-built tree
doesn't maintain correct locator ordering when insertions create new
locators. Investigation needed into why the ordering breaks despite the
Locator::between fix. The fast path remains deletion-only.

### Benchmark Results

| Benchmark | Phase 2 | Phase 3 | Improvement |
|-----------|--------:|--------:|------------:|
| 1K doc | 357 | **379** | +6% |
| 10K doc | 232 | **257** | +11% |
| 100K doc | 60 | **68** | +13% |
| 3-client | 23 | **25** | +9% |

### Cumulative Improvement (All Phases)

| Benchmark | Original | Now | Total |
|-----------|----------|-----|-------|
| 1K doc | 203 | **379** | **1.9x** |
| 10K doc | 121 | **257** | **2.1x** |
| 100K doc | 27 | **68** | **2.5x** |
| 1M doc | 22 | **66** | **3.0x** |
| 3-client | 17 | **25** | **1.5x** |
| Tombstone 50% | 8 | **18** | **2.3x** |
| Tombstone 90% | Timeout | **14** | **fixed** |

---

## 7. Next Steps

1. **Investigate local edit fast path ordering** — why does the cursor-built
   tree not maintain locator ordering for insertions, despite the between()
   fix? Likely an issue with how fragments are positioned relative to
   pending/suffix after cursor operations.

2. **Prepend-optimized Locator allocation** — reverse-biased `biased_mid`
   for patterns where lo is close to DMIN. Prevents locator depth explosion
   for prepend-heavy editing.

3. **Partial deletion in fast path** — currently falls back to full O(n)
   path for deletions that require splitting. Could use SumTree mutations
   to split + delete in O(log n).
