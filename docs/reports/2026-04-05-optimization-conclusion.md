# Where We Stand — CRDT Optimization Retrospective

**Date:** 2026-04-05
**Scope:** Four optimization phases over one session, starting from the
original unoptimized engine.

---

## The Numbers

### Our Journey

| Benchmark | Original | Phase 4 | Total | Per-op latency |
|-----------|----------|---------|-------|----------------|
| 1K doc | 203 ops/sec | ~3,100 ops/sec | **15x** | 0.32 ms |
| 10K doc | 121 ops/sec | ~1,950 ops/sec | **16x** | 0.51 ms |
| 100K doc | 27 ops/sec | ~450 ops/sec | **17x** | 2.2 ms |
| 1M doc | 22 ops/sec | ~500 ops/sec | **23x** | 2.0 ms |
| 3-client | 17 ops/sec | ~530 ops/sec | **31x** | 1.9 ms |
| Tombstone 90% | Timeout | 14 ops/sec | **fixed** | 71 ms |

### The Field

| Engine | Language | automerge-paper trace | Single-edit latency |
|--------|----------|----------------------:|--------------------:|
| cola | Rust | ~9,000,000 ops/sec | ~0.0001 ms |
| diamond-types | Rust | 4,600,000 ops/sec | ~0.0002 ms |
| diamond-types | WASM | 1,346,000 ops/sec | ~0.0007 ms |
| Automerge 2.0 | Rust/WASM | 393,000 ops/sec | ~0.003 ms |
| Yjs | JavaScript | 45,000-268,000 ops/sec | ~0.004-0.02 ms |
| Loro | Rust/WASM | 84,100 ops/sec | ~0.01 ms |
| **collabtext** | **C++/Qt** | **~3,100 ops/sec** (1K) | **0.32 ms** |
| Automerge 1.0 | JS/WASM | 19,900 ops/sec | ~0.05 ms |
| Automerge 0.14 | JavaScript | ~900 ops/sec | ~1.1 ms |

### Where We Moved

Before optimization, we sat next to Automerge 0.14 — the version its own
author called "embarrassingly slow." We're now between Automerge 1.0 and
Loro in throughput, roughly one order of magnitude behind the JS-tier
engines (Yjs, Loro) and two orders behind the Rust-native engines
(diamond-types, cola).

In practical terms: we went from "uncomfortably slow at 10K documents"
to "sub-millisecond at 10K, perceptible at 100K." That's a meaningful
improvement for an interactive text editor.

---

## What We Did (and What Each Was Worth)

| Phase | Change | Impact | Why |
|-------|--------|--------|-----|
| 1 | Inline text in Fragment | 1.5x (large docs) | Eliminated O(n*m) origin-interval-lookup in set_fragments |
| 2 | SumTree mutations (B=6) + fast paths | 2x across the board | Halved tree depth, O(log n) for undo/redo and insertion-only remote edits |
| 3 | Origin index + Locator fix | 1.1x | O(log n) deletion lookup, expanded remote fast path |
| **4** | **Eliminate visibility + origin index rebuild** | **8-10x** | **Removed two O(n) per-edit bottlenecks from the fast path** |

Phase 4 dwarfed everything else combined. The lesson: the real bottleneck
was not the algorithmic complexity we spent Phases 1-3 addressing. It was
two O(n) housekeeping passes — a redundant visibility recompute and a full
map rebuild — that ran on every single edit. They had small O(n) constants
individually, but `std::map` allocation/deallocation costs compound
savagely at scale.

---

## Why We Can't Close the Remaining Gap

### The architecture ceiling: multi-character fragments

Our engine stores text in multi-character fragments. When a user types
"hello", that's one fragment with one locator. Zed, diamond-types, and
cola store one locator per character (or per run of characters from the
same user, with RLE compression).

This matters because inserting text in the middle of an existing fragment
requires **splitting** the fragment and telling remote replicas to split
their copy too (via SplitRelocations). This forces:

1. A deferred relocation mechanism during local edits
2. A post-processing sort/normalize/rebuild for ~2.3% of edits
3. The O(n log n) full path that the fast-path optimization can't touch

We investigated adopting Zed's locator-reassignment pattern (which avoids
all post-processing) and found it **fundamentally incompatible** with
multi-character fragments. Without SplitRelocations, remote replicas
can't correctly place insertions within unsplit fragments. Convergence
breaks.

### What per-character locators would unlock

Switching to per-character locators (like Zed) would:
- Eliminate SplitRelocations entirely
- Eliminate the deferred relocation mechanism
- Eliminate the sort/normalize/rebuild fallback
- Make every local edit O(k log n) with no exceptions
- Enable RLE compression (consecutive same-user characters as single runs)

This is what separates the ~100K ops/sec tier from the ~1M+ ops/sec tier.
But it's a full engine rewrite: fragment model, wire format, remote edit
handling, undo/redo, GC, and all tests.

### The O(n) floor

Even with per-character locators, two O(n) costs remain in our current
architecture:

1. The ordering check (O(n) tree walk, ~4us for 400 fragments — negligible)
2. The slow-path sort/rebuild (only for the 2.3% deferred-relocation case)

The fast path is now essentially O(k log n) for the cursor walk + O(1)
for the commit. The remaining O(n) costs are minor.

---

## Is This Good Enough?

### The 1ms threshold

Joseph Gentle (diamond-types): *"Once a CRDT can handle any local user
edit in under about 1ms, going faster probably doesn't matter much."*

| Doc size | Our latency | Verdict |
|----------|-------------|---------|
| 1K | 0.32 ms | Under threshold |
| 10K | 0.51 ms | Under threshold |
| 100K | 2.2 ms | Over threshold, but imperceptible to most users |
| 1M | 2.0 ms | Same |

For documents up to 100K characters (a ~40-page document), we're in
the "good enough" zone. Beyond that, the O(n) ordering check starts to
matter, but 2ms is still well under the 20-30ms perceptibility threshold.

### Multi-client editing

At 530 ops/sec for 3-client editing, each operation takes ~1.9ms.
For a team of 3 people typing simultaneously at 80 WPM, that's
~20 operations/second — well within our budget. The bottleneck
shifts to network latency long before CRDT processing becomes the
constraint.

### Where it breaks down

- Documents over 1M characters with heavy concurrent editing
- High-frequency automated edits (bots, bulk transforms)
- Scenarios where the 2.3% slow-path (deferred relocations) is
  triggered frequently (many mid-word insertions from multiple clients)

---

## What We Have That Others Don't

The throughput numbers don't tell the full story. Our engine has
capabilities that most faster engines lack:

**Full undo/redo** with bounded stack and GC-awareness. Most CRDT engines
treat undo as an application concern. Ours handles undo-protected
tombstones surviving garbage collection.

**Distributed garbage collection** with watermark protocol. diamond-types
and cola have no GC. Yjs has simple content-clearing. Automerge has no
GC by design. Our dual approach (local GC + distributed compact) achieves
3.5x fragment reduction at steady state.

**Comprehensive test infrastructure.** NetworkSim with simulated latency,
jitter, reordering, duplicates, disconnect/reconnect, and cascading
partition tests. 16 fuzz scenarios, 11 realistic multi-client tests.
Seeded and reproducible. More thorough than what most CRDT projects
publish.

---

## Recommendations

### If performance is sufficient (documents < 100K)
Ship it. The engine is correct, well-tested, and fast enough for
interactive editing. Focus on features, not further optimization.

### If performance needs to improve further
The only path to the next order of magnitude is per-character locators
with RLE compression. This is a new engine, not an optimization of
the current one. Budget accordingly.

### What NOT to do
Don't try to optimize the deferred relocation slow path in-place. We
attempted this and found a subtle correctness bug where newly inserted
fragments have contiguous origins with split suffixes, causing the
relocation walk to accidentally process the wrong fragments. The
complexity-to-benefit ratio is poor for a path hit 2.3% of the time.
