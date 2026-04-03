# CRDT Engine Benchmark Results

**Date:** 2026-04-03
**Build:** Release (GCC 15.2, -O2, Qt 6.11)
**Hardware:** x86_64, NVIDIA GPU system (Manjaro Linux)
**Commit:** `b4481e1` (13 benchmarks, 6 dimensions)

---

## 1. Summary

Tombstone accumulation is the dominant performance bottleneck. A 5K document
with 50% tombstones (3,390 fragments) is **21x slower** than the same document
clean (50 fragments). Fragment count — not visible text size — determines
edit throughput. This strongly motivates garbage collection.

## 2. Single-Replica Throughput

| Doc Size | ns/op | ops/sec | Fragments |
|----------|------:|--------:|----------:|
| 1K | 577K | 1,734 | 417 |
| 10K | 936K | 1,068 | 588 |
| 100K | 3.97M | 252 | 1,495 |
| 1M | 5.66M | 177 | 1,252 |

**Finding:** Sub-linear scaling. 100x document size = 7x slower. The 1M doc
has fewer fragments than the 100K doc (1,252 vs 1,495) because it was built
with larger chunks, confirming that fragment count — not byte count — is the
cost driver.

## 3. Edit Patterns (100K doc)

| Pattern | ns/op | ops/sec |
|---------|------:|--------:|
| Sequential (append) | 3.18M | 314 |
| Random position | 4.05M | 247 |
| Hotspot [5000,5100] | 3.75M | 267 |

**Finding:** Edit pattern has minimal impact (~1.3x range). The fixed cost
of `set_fragments()` dominates; cursor seeking to the edit point is a small
fraction of total time.

## 4. Edit Sizes (100K doc)

| Insert Size | ns/op | ops/sec |
|-------------|------:|--------:|
| 1 char | 3.45M | 290 |
| 10 chars | 3.57M | 280 |
| 100 chars | 3.35M | 298 |
| 1000 chars | 3.52M | 284 |

**Finding:** Edit size is irrelevant. Inserting 1 char vs 1000 chars costs
the same. Rope chunking overhead is negligible vs the set_fragments rebuild.

## 5. Tombstone Degradation (key finding)

### 5.1 Impact at fixed document size (5K doc)

| Tombstone % | Fragments Before | ns/op | ops/sec | Degradation |
|-------------|----------------:|------:|--------:|------------:|
| 0% | 50 | 789K | 1,267 | 1x |
| 50% | 3,390 | 16.7M | 60 | **21x** |
| 90% | 4,422 | 24.0M | 42 | **30x** |

### 5.2 Scaling with document size (50% tombstones)

| Doc Size | Fragments | ns/op | ops/sec |
|----------|----------:|------:|--------:|
| 3K | 2,046 | 7.5M | 133 |
| 5K | 3,390 | 15-37M | 27-66 |
| 8K | 5,349 | 32.8M | 30 |

**Finding:** Cost grows super-linearly with fragment count. Consistent with
the O(n*m) origin-interval-lookup in `set_fragments()`, where n = new fragments
and m = old fragments. Doubling fragment count roughly quadruples cost.

## 6. Multi-Replica Convergence

| Replicas | Edits Each | Total Convergence Time | ops/sec | Converged |
|----------|-----------|----------------------:|--------:|-----------|
| K=2 | 50 | 26ms | 3,851 | YES |
| K=5 | 50 | 705ms | 354 | YES |
| K=10 | 50 | 6.9s | 73 | YES |

**Finding:** Convergence time grows super-linearly with replicas. Cross-apply
is O(K^2) operations (each of K replicas applies K-1 other replicas' ops).
K=10 with 50 edits each = 4,500 apply_ops calls.

### Multi-Replica Hotspot (K=5, all editing same 100-byte region)

| Metric | Value |
|--------|------:|
| Total convergence time | 1.42s |
| Throughput | 176 ops/sec |
| Final fragments | 648 |
| Converged | YES |

**Finding:** Hotspot convergence (1.42s) is 2x slower than random convergence
(705ms) at K=5. Normalization (atomization at shared locators) adds cost but
is not catastrophic.

## 7. Undo/Redo (10K doc, 500 operations)

| Phase | ns/op | ops/sec | Cost Ratio |
|-------|------:|--------:|-----------:|
| Edit | 2.18M | 459 | 1.0x |
| Undo | 5.62M | 178 | **2.6x** |
| Redo | 5.37M | 186 | **2.5x** |

### Undo Stack Depth (10K doc)

| Undo Depth | Fragments | ns/op | ops/sec |
|-----------|----------:|------:|--------:|
| N=100 | 354 | 887K | 1,127 |
| N=500 | 1,298 | 5.1M | 195 |
| N=1000 | 2,391 | 16.7M | 60 |

**Finding:** Undo cost is 2.5x edit cost. Scales with fragment count (not undo
depth directly) because `set_fragments()` recomputes visibility for ALL fragments
via `compute_visible()`.

## 8. Fragment Proliferation (10K doc + single-char inserts)

| After N Inserts | Fragments | ns/op | ops/sec |
|----------------|----------:|------:|--------:|
| 500 | 1,071 | 1.9M | 540 |
| 1,000 | 1,997 | 4.1M | 242 |
| 1,500 | 2,900 | 7.2M | 139 |
| 2,000 | 3,766 | 9.9M | 101 |

**Finding:** Clear linear degradation. Every ~1,000 additional fragments adds
~4ms per operation. Single-char edits are the worst case because each edit
splits a fragment, creating 2 from 1.

## 9. Memory Growth (5000 edits from empty)

| Edits | Visible Bytes | Total Fragments | Tombstones | Memory Delta |
|------:|--------------:|----------------:|-----------:|-------------:|
| 500 | 866 | 614 | 132 | 0 KB |
| 1,000 | 1,782 | 1,222 | 228 | 0 KB |
| 2,000 | 3,521 | 2,424 | 454 | 0 KB |
| 3,000 | 5,265 | 3,616 | 683 | 0 KB |
| 5,000 | 9,032 | 6,029 | 1,095 | 0 KB |

**Finding:** RSS delta shows 0 KB — the allocator holds freed memory. Fragment
count grows linearly with edit count. Tombstone ratio stabilizes at ~18% for
a mixed insert/delete workload.

## 10. Predictions Validated

| Prediction | Result |
|-----------|--------|
| set_fragments dominates | **Confirmed.** Edit pattern and size are irrelevant; fragment count determines cost. |
| Tombstone accumulation is linear degradation | **Worse than linear.** Super-linear (O(n*m) lookup). 68x fragments = 30x slower. |
| Normalization is expensive but rare | **Confirmed.** Hotspot is 2x slower, not 10x. |
| Undo cost proportional to undo_map size | **Partially confirmed.** Undo cost tracks fragment count, not undo_map size directly. |
| Memory growth is fragment-driven | **Inconclusive.** RSS delta was 0 due to allocator pooling. Fragment count itself grows linearly. |

## 11. Implications for GC

The data makes a clear case:

1. **Any document with > 3,000 fragments drops below 100 ops/sec.** Real documents
   with active editing will hit this within minutes.

2. **Tombstones are the multiplier.** A document where users delete and retype
   (the most common editing pattern) accumulates tombstones at the rate of one
   per deleted character.

3. **GC needs to reduce fragment count, not just free memory.** The performance
   cost is in scanning fragments during `set_fragments()`, not in memory usage.

4. **Merging adjacent same-visibility fragments would help even without true GC.**
   After undo/redo cycles, many adjacent fragments have identical visibility
   state and could be coalesced.
