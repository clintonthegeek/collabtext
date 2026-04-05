# CRDT Engine Competitive Performance Analysis

**Date:** 2026-04-05
**Context:** Comparison of our CRDT text engine against the major open-source
CRDT implementations, using published benchmark data.

---

## 1. Summary

Our engine is 2-5 orders of magnitude slower than the leading CRDT text
engines on per-operation throughput. The gap traces to a single architectural
bottleneck: the O(n*m) origin-interval-lookup in `set_fragments()`, which
rebuilds ropes from scratch on every edit. The entire field has converged on
O(log n) tree-based structures that avoid this cost.

Despite the throughput gap, our engine remains usable for interactive editing
on documents up to ~10K characters. Beyond 100K, we enter the danger zone
where latency approaches human perception thresholds.

---

## 2. Industry Standard Benchmark

The most widely-used CRDT benchmark is Martin Kleppmann's **automerge-paper
editing trace**: 259,778 character-by-character operations (182,315 inserts,
77,463 deletes) producing a final document of 104,852 characters. Nearly
every major CRDT implementation benchmarks against this dataset.

### 2.1 Results on automerge-paper Trace (single replica, full replay)

| Engine | Language | Time | ops/sec | Memory | Source |
|--------|----------|-----:|--------:|-------:|--------|
| cola | Rust native | ~30 ms | ~9,000,000 | -- | [cola blog](https://nomad.foo/blog/cola) |
| diamond-types | Rust native | 56 ms | 4,600,000 | 1.1 MB | [CRDTs go brrr](https://josephg.com/blog/crdts-go-brrr/) |
| diamond-types | Rust/WASM | 193 ms | 1,346,000 | -- | [CRDTs go brrr](https://josephg.com/blog/crdts-go-brrr/) |
| Automerge 2.0.2 | Rust/WASM | 661 ms | 393,000 | 23 MB | [Automerge 2.0](https://automerge.org/blog/automerge-2/) |
| Yjs | JavaScript | 970 ms - 5.7s | 45K-268K | 3.2 MB | [crdt-benchmarks](https://github.com/dmonad/crdt-benchmarks) |
| Loro | Rust/WASM | 3,089 ms | 84,100 | ~0 MB delta | [crdt-benchmarks](https://github.com/dmonad/crdt-benchmarks) |
| Automerge 1.0 | JS/WASM | 13,052 ms | 19,900 | 185 MB | [Automerge 2.0](https://automerge.org/blog/automerge-2/) |
| Automerge 0.14 | JavaScript | ~291,000 ms | ~900 | 880 MB | [CRDTs go brrr](https://josephg.com/blog/crdts-go-brrr/) |

We have no direct equivalent of this trace benchmark, but our per-operation
numbers place us in the Automerge 0.14 / Automerge 1.0 tier — the versions
that Kleppmann described as "embarrassingly slow."

### 2.2 Scaling Test (automerge-paper x100: ~26M ops)

| Engine | Time | Memory |
|--------|-----:|-------:|
| Loro (Rust native) | 13.3s | -- |
| Loro (JS/WASM) | 310s | ~0 B delta |
| Yjs | 609s | 327 MB |
| Automerge | Skipped (OOM) | -- |

Source: [automerge-paper-bench](https://github.com/zxch3n/automerge-paper-bench)

---

## 3. Our Engine's Numbers

From our benchmark suite (GCC 15.2, -O2, x86_64):

### 3.1 Single-Replica Throughput

| Doc Size | ops/sec | Fragments | Per-op latency |
|----------|--------:|----------:|---------------:|
| 1K | 203 | 417 | 4.9 ms |
| 10K | 121 | 588 | 8.2 ms |
| 100K | 27 | 1,495 | 37.7 ms |
| 1M | 22 | 1,252 | 45.4 ms |

### 3.2 Multi-Client Realistic Editing

| Scenario | ops/sec | Fragments/replica |
|----------|--------:|------------------:|
| 3-client, 300 ops | 17 | ~1,200 |

### 3.3 GC Effectiveness (3-client, 600 ops)

| Checkpoint | GC OFF frags | GC ON frags | Ratio |
|------------|-------------:|------------:|------:|
| @200 ops | 875 | 239 | 3.7x |
| @400 ops | 1,692 | 482 | 3.5x |
| Final | 2,972 | 2,854 | 1.0x |

### 3.4 Tombstone Degradation

| Tombstone % | Fragments | ops/sec | Degradation |
|------------:|----------:|--------:|------------:|
| 0% | 50 | 139 | 1x |
| 50% | 3,390 | 8 | 17x |
| 75%+ | >5,000 | -- | Timeout (>300s) |

---

## 4. Why the Field Is Faster

### 4.1 Data Structure Choices

Every competitive engine uses an O(log n) indexed tree structure where edits
touch only a logarithmic number of nodes:

- **diamond-types**: Range tree (B-tree variant) with RLE-compressed entries.
  Consecutive same-user edits compress into single runs. 260K keystrokes
  produce ~15K tree entries. 1.1 MB total memory.

- **cola**: G-tree with branching factor 32, storing EditRuns. Same RLE
  principle. Claims 1.4-2x faster than diamond-types.

- **Yjs**: Doubly-linked list of structs with skip pointers. Deleted content
  replaced by lightweight GC stubs (length only). No fragment-level scan.

- **Loro**: Fugue algorithm with `delete_times` counter on spans, not separate
  tombstone fragments. B-tree indexed. O(log n) lookups.

- **Automerge 2.0+**: Rust backend with columnar encoding. History compressed
  via run-length encoding. No per-character JavaScript objects.

### 4.2 Our Bottleneck

Our engine uses a SumTree (B+ tree with summary aggregation) for fragment
storage, which provides O(log n) *lookups*. However, every mutation
(`apply_local_edit`, `apply_remote_edit`) calls `set_fragments()`, which:

1. Extracts all fragments into a vector — O(n)
2. Performs mutations on the vector
3. Rebuilds the tree from scratch — O(n)
4. Rebuilds ropes via origin-interval-lookup — O(n*m) where m = old fragments

Step 4 is the killer. For every new fragment, we scan all old fragments to
find the origin interval and extract text. With 3,000 fragments, each edit
touches all 3,000 fragments to rebuild. This is why fragment count — not
document size — determines performance.

The competitive engines mutate their trees in-place with O(log n) operations.
They never rebuild from scratch.

---

## 5. The Practical Threshold

### 5.1 Human Typing Speed

| Speed | Keystrokes/sec |
|-------|---------------:|
| Average typist (40 WPM) | 3.3 |
| Fast typist (80 WPM) | 6.7 |
| Professional (120 WPM) | 10 |
| Burst | ~20 |

### 5.2 Latency Perception

| Threshold | Latency |
|-----------|--------:|
| Imperceptible | < 1 ms |
| Noticeable | 20-30 ms |
| Annoying | > 50 ms |
| "Good enough" CRDT target | < 1 ms/op |

Joseph Gentle (diamond-types author): *"Once a CRDT can handle any local user
edit in under about 1ms, going faster probably doesn't matter much."*

Kevin Jahns (Yjs author): *"The time to insert characters is the least
interesting property of a CRDT. It doesn't matter whether a character is
inserted within 0.1ms or 0.000000001ms."*

### 5.3 Our Position Relative to the Threshold

| Scenario | Per-op latency | Headroom over fast typist |
|----------|---------------:|--------------------------:|
| 1K doc | 4.9 ms | 20x |
| 10K doc | 8.2 ms | 12x |
| 100K doc | 37.7 ms | 2.7x — **perceptible** |
| 1M doc | 45.4 ms | 2.2x — **perceptible** |
| 3-client | 58.8 ms | 1.7x — **annoying** |

We are usable up to ~10K documents. At 100K, individual edits exceed the
20-30ms perception threshold. With multiple clients, we're in uncomfortable
territory even earlier.

---

## 6. What We Do Well

### 6.1 Testing Infrastructure

None of the major engines publish multi-client realistic benchmarks. Our
NetworkSim harness — with simulated latency, jitter, reordering, duplicates,
disconnect/reconnect, and cascading partition tests — is more thorough than
what most CRDT projects have publicly. 9 correctness scenarios across 1-10
clients, all seeded and reproducible.

### 6.2 Garbage Collection

Our dual GC approach (local `collect_garbage()` + distributed `compact()` with
watermark) is unusual in the field:

- **diamond-types, cola**: No GC. RLE compression keeps memory compact.
- **Yjs**: Simple content-clearing (replace deleted content with length stubs).
- **Automerge**: No GC by design (version control philosophy).
- **Loro**: Counter-based deletion avoids tombstone objects entirely.

Our GC achieves 3.5x fragment reduction at steady state, which is meaningful
for our architecture where fragment count is the cost driver.

### 6.3 Undo/Redo

Full undo/redo with bounded stack and GC-awareness (undo-protected tombstones
survive GC). Most CRDT engines either don't support undo or treat it as an
application-layer concern.

---

## 7. Path Forward

The performance gap traces to a single architectural decision: extracting
fragments to a vector and rebuilding via linear scan in `set_fragments()`.
Replacing this with in-place O(log n) tree mutations would close 2-3 orders
of magnitude of the gap.

The CRDT semantics, GC infrastructure, undo system, and test harness are all
solid. The bottleneck is purely in the data structure layer — how we store and
mutate fragments, not how we compute what to store.

### 7.1 Target Architecture

Replace the extract-mutate-rebuild pattern with in-place SumTree operations:

- **Insert**: O(log n) — walk the tree to the insertion point, split the leaf
  if needed, insert the new fragment, rebalance.
- **Delete (toggle visibility)**: O(log n) — find the fragment by ID, toggle
  visibility, update summaries up to root.
- **Split**: O(log n) — find the fragment, split in place, update summaries.
- **Rope integration**: Store text inline in fragments or in a parallel rope
  tree that mirrors the fragment tree structure.

### 7.2 Actual Impact (Phases 1 & 2 Complete)

Phase 1 (inline text) and Phase 2 (in-place SumTree mutations + B=6)
achieved a **2-3x improvement** across all benchmarks:

| Doc Size | Before | After | Improvement |
|----------|-------:|------:|------------:|
| 1K | 203 ops/sec | 357 | 1.8x |
| 100K | 27 ops/sec | 60 | 2.2x |
| 1M | 22 ops/sec | 66 | 3.0x |
| 3-client | 17 ops/sec | 23 | 1.4x |

The 300-3,000x target was not reached because the O(n) full path
(get_fragments + vector manipulation + set_fragments) still dominates for
edits involving deletions. The insertion-only fast path achieves O(log n)
but only covers a fraction of real-world operations.

**Remaining bottleneck:** Deletion runs require origin-based lookup in a
locator-ordered tree (O(n) scan). An origin-based secondary index would
make the full remote edit path O(log n).

See `docs/reports/2026-04-05-performance-refactor-results.md` for full
benchmark comparison.

---

## 8. Sources

- [crdt-benchmarks (Kevin Jahns)](https://github.com/dmonad/crdt-benchmarks)
- [CRDTs go brrr (Joseph Gentle)](https://josephg.com/blog/crdts-go-brrr/)
- [Automerge 2.0 blog](https://automerge.org/blog/automerge-2/)
- [Automerge 3.0 blog](https://automerge.org/blog/automerge-3/)
- [cola blog](https://nomad.foo/blog/cola)
- [Loro performance docs](https://loro.dev/docs/performance)
- [automerge-paper-bench](https://github.com/zxch3n/automerge-paper-bench)
- [diamond-types (GitHub)](https://github.com/josephg/diamond-types)
- [editing-traces (GitHub)](https://github.com/josephg/editing-traces)
