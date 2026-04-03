# CRDT Engine Benchmark Suite — Design Spec

**Date:** 2026-04-02
**Status:** Design ready
**Goal:** Establish performance baselines across all dimensions and identify bottlenecks before optimizing.

---

## 1. Motivation

The engine has 200+ correctness tests but zero performance data. We don't know:
- How throughput scales with document size
- Whether the O(n) vector bridge pattern is the bottleneck
- How tombstone accumulation affects edit speed
- The cost of multi-replica convergence
- Whether the origin-interval-lookup in set_fragments degrades with fragment count

Without numbers, optimization decisions are guesses.

## 2. Dimensions

Six independent variables, each tested at extreme values:

| Dimension | Values | Rationale |
|-----------|--------|-----------|
| **Document size** | 1K, 10K, 100K, 1M chars | O(n) operations should show scaling |
| **Fragment count** | natural vs forced (10K+ tombstones) | Origin-interval-lookup is O(n*m) |
| **Replica count** | 1, 2, 5, 10 | Normalization/atomization cost |
| **Edit pattern** | sequential, random, hotspot | Hotspot = worst case for splitting |
| **Edit size** | 1, 10, 100, 1000 chars | Rope chunking, fragment byte_length distribution |
| **Undo depth** | 0, 100, 1000 pending undos | UndoMap scan cost |

### Edit Patterns

- **Sequential:** Edits at the end of the document (append-like). Best case for cursor seeking.
- **Random:** Edits at uniformly random positions. Average case.
- **Hotspot:** All edits within a 100-byte window. Worst case for fragment splitting and shared-locator normalization.

## 3. Cross-Dimensional Combinations

These probe second-order effects where dimensions interact:

### 3.1 Tombstone Degradation (doc_size x fragment_count)

Build a document, then delete 90% of it character-by-character (creating one tombstone per deletion). Measure edit throughput on the remaining 10% visible text. The set_fragments origin-interval-lookup scans ALL fragments (visible + tombstones), so tombstone accumulation should directly degrade edit speed.

**Sizes:** 10K, 100K visible chars (with 9x tombstones = 90K, 900K total fragments)

### 3.2 Multi-Replica Hotspot (replica_count x edit_pattern)

K replicas all edit within the same 100-byte region concurrently. This forces normalize_fragments to atomize multi-character fragments at shared locators — the most expensive normalization path. Measures whether normalization becomes a bottleneck with many concurrent editors.

**Replicas:** 2, 5, 10; 100 edits each in the same region

### 3.3 Fragment Proliferation (doc_size x edit_size)

Insert a large document, then perform N single-character edits at random positions. Each edit splits a fragment, creating 2 fragments from 1. After N edits on an M-fragment document, fragment count approaches M + 2N. Measures how fragment count growth affects throughput over time.

**Sizes:** 100K chars base, 1000/5000/10000 single-char edits

### 3.4 Undo Stack Pressure (undo_depth x doc_size)

Perform N edits, then undo all N, then redo all N. Measures UndoMap growth cost and visibility recomputation overhead. The UndoMap is a SumTree that grows with each undo — scan cost for is_undone() increases.

**Sizes:** 100, 500, 1000 edits on 10K and 100K documents

### 3.5 Convergence Scaling (replica_count x doc_size)

K replicas each perform M independent edits, then cross-apply all operations. Measures total wall-clock time to reach convergence. The apply_ops path includes causal ordering, deferred queue management, and normalization.

**Replicas:** 2, 5, 10; 100 edits each on 10K and 100K documents

### 3.6 Tombstone + Undo Interaction (fragment_count x undo_depth)

Build tombstone-heavy state (delete 90% char by char), then undo the deletions. The undo path recomputes visibility for ALL fragments. With 90K tombstones, each with a deletions vector entry, compute_visible iterates every fragment's deletions list.

**Sizes:** 10K, 50K total fragments; full undo cycle

## 4. Metrics

Each benchmark records:

| Metric | Type | Description |
|--------|------|-------------|
| `ns_per_op` | uint64 | Wall-clock nanoseconds per operation |
| `ops_per_sec` | double | Throughput |
| `total_fragments` | uint32 | Fragment count after benchmark |
| `visible_bytes` | uint32 | Visible text bytes |
| `tombstone_fragments` | uint32 | Invisible fragment count |
| `elapsed_ms` | double | Total wall-clock time |
| `memory_kb` | int64 | RSS delta from baseline (via /proc/self/statm) |

## 5. Implementation

### 5.1 File structure

- `libs/collabtext/tests/tst_benchmark.cpp` — All benchmarks in one file
- Output: human-readable summary to stdout, CSV to file if `--csv` flag

### 5.2 Benchmark harness

Each benchmark is a QTest slot. Use `std::chrono::high_resolution_clock` for timing. Warm-up phase (10% of iterations) excluded from measurement. Each benchmark runs enough iterations to accumulate at least 100ms of wall time (auto-calibrated).

### 5.3 Helper functions

Reuse patterns from tst_fuzz.cpp:
- `random_text(rng, maxChars)` — random UTF-8 string
- `random_byte_offset(rng, text)` — valid UTF-8 boundary
- `build_document(buf, size)` — insert text to reach target size
- `create_tombstones(buf, fraction)` — delete fraction of chars one-by-one
- `cross_apply(buffers, ops_per_buffer)` — full convergence round

### 5.4 Memory measurement

Read `/proc/self/statm` (RSS in pages) before and after each benchmark. Linux-specific but sufficient for development. Report delta in KB.

## 6. Output Format

```
=== CollabText CRDT Benchmark Suite ===

--- Single-Replica Throughput ---
  1K doc, sequential insert:     1,234 ns/op   (810,372 ops/sec)   frags=152
  10K doc, sequential insert:    2,456 ns/op   (407,166 ops/sec)   frags=1,523
  ...

--- Tombstone Degradation ---
  10K visible + 90K tombstones:  12,345 ns/op  (81,004 ops/sec)   frags=100,234
  ...

--- Multi-Replica Hotspot ---
  5 replicas, hotspot:           45,678 ns/op  (21,892 ops/sec)   frags=2,345
  ...
```

## 7. Success Criteria

- All benchmarks run to completion without crashes
- Baseline numbers established for all 6 dimensions
- At least one bottleneck identified (operation that scales worse than expected)
- Results reproducible (< 15% variance across 3 runs)
- CSV output parseable for future regression tracking

## 8. What We Expect to Find

Predictions to validate or falsify:

1. **set_fragments dominates.** The extract-to-vector-modify-rebuild cycle should show up as the hot path, especially at large document sizes.
2. **Tombstone accumulation is linear.** Edit throughput should degrade linearly with tombstone count due to the O(n*m) origin-interval-lookup.
3. **Normalization is expensive but rare.** Multi-replica hotspot should show high per-op cost but only triggers for shared-locator groups.
4. **Undo cost is proportional to undo_map size.** The SumTree-backed UndoMap scan should be O(log n) but with a high constant factor.
5. **Memory growth is fragment-driven.** RSS should correlate with fragment count, not visible text size.
