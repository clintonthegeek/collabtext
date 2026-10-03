# 07. Measured performance and targeted optimization

Goal: improve the corrected algorithm without changing semantics. Measure against the same corrected state/trace; old buggy or differently ordered output is not a valid faster baseline. Keep each optimization independently reviewable and reversible.

Read [execution rules](EXECUTION.md) and [decisions](DECISIONS.md) before starting. Task dependencies refer to the [master task registry](TASKS.json). New file paths below are intentional; do not assume they already exist.

## X01 — Define workloads, budgets, and structured benchmark output

**Status:** see TASKS.json (initially not started)  
**Depends on:** V06, V04, S07, A04  
**Coverage:** R5  

**Files / scope:**

- `benchmarks/TraceFormat.md`
- `benchmarks/traces/`
- `benchmarks/BenchmarkRunner.cpp`
- `docs/specs/vnext-performance-targets.md`

**Steps:**

1. Create deterministic prose/typing, random mid-run, bulk paste/delete, Unicode, long-tombstone, retained-undo, 2/5/10-peer offline merge, stream restart, and high-actor-count workloads. Record original edit and remote apply counts.
2. Separate engine API, encode/decode, recovery, file transport, Qt mutation/layout, and paint scheduling measurements. Emit revision, seed/trace hash, hardware, flags, state/fragment counts, percentiles, allocation/RSS/disk/wire metrics, and unavailable fields.
3. Choose per-workload targets after a five-run corrected baseline. Treat a candidate 100 KB engine p99 below 8 ms and bounded GUI poll slices as goals to evaluate, not unmeasured release guarantees. Record all normalization units.

**Acceptance:**

- Every benchmark checks exact content/version/anchor expectations and is reproducible without private data.
- Targets reference one recorded environment; compiler/hardware changes cannot silently look like regressions or wins.

**Verification:**

Run a short Release workload matrix and five repeats of principal workloads; validate output schema and compare operation counts to trace metadata.

## X02 — Instrument algorithm paths, allocations, and retention costs

**Status:** see TASKS.json (initially not started)  
**Depends on:** X01  
**Coverage:** R5, R6, R7, R10  

**Files / scope:**

- `libs/collabtext/src/crdt/Diagnostics.h`
- `libs/collabtext/src/crdt/Buffer.cpp`
- `libs/collabtext/src/crdt/IdList.cpp`
- `libs/collabtext/src/crdt/SumTree.h`
- `benchmarks/BenchmarkRunner.cpp`

**Steps:**

1. Add opt-in counters for tree visits, clones, allocations, fragment splits, normalization/fallbacks, deferred queue bytes/retries, origin/deletion lookup, undo scans, delta scans, and retained pin/history/UndoMap bytes.
2. Keep diagnostics out of default hot paths or behind compile-time/runtime disabled settings. Distinguish allocated/live engine memory from process RSS and file cache.
3. Profile representative corrected traces and publish the largest measured costs. Do not assume fallback frequency remains 2.3 percent.

**Acceptance:**

- Counters agree with synthetic tiny cases and do not alter behavior.
- Profile report identifies target bottlenecks using data, including wire-size and catch-up paths.

**Verification:**

Run diagnostic correctness tests and Release trace profiles; compare diagnostics enabled/disabled logical output and overhead.

## X03 — Optimize summary layout and sparse vector ownership

**Status:** see TASKS.json (initially not started)  
**Depends on:** X02, P03  
**Coverage:** R7  

**Files / scope:**

- `libs/collabtext/src/crdt/Clock.h`
- `libs/collabtext/src/crdt/Clock.cpp`
- `libs/collabtext/src/crdt/Fragment.h`
- `libs/collabtext/src/crdt/IdList.h`
- `libs/collabtext/src/crdt/SumTree.h`

**Steps:**

1. Remove demonstrated unused summaries/index storage; retain only fields required by measured queries. Preserve summary monoid laws and checkpoint rebuild behavior.
2. Replace unchecked manual allocation with exception/error-consistent RAII small-buffer storage and checked growth. Test inline→heap, move/copy/self-assignment, empty vectors, and allocation failure before optimizing copies.
3. Measure node/item sizes, live allocations, cache behavior where available, and small/large-actor costs. Accept a layout change only with equivalent logical behavior.

**Acceptance:**

- Allocation failure leaves valid state; sparse-vector and summary law tests pass.
- Memory/latency results show actual improvement or the change is documented/reverted instead of retaining unproved complexity.

**Verification:**

Run clock/summary/tree/model suites under ASan; run the X01 fixed trace matrix and publish before/after node bytes and live allocation metrics.

## X04 — Index origins and deletion references for targeted updates

**Status:** see TASKS.json (initially not started)  
**Depends on:** X03, G06  
**Coverage:** R6, R7  

**Files / scope:**

- `libs/collabtext/src/crdt/OriginIndex.h`
- `libs/collabtext/src/crdt/DeletionIndex.h`
- `libs/collabtext/src/crdt/Buffer.cpp`
- `libs/collabtext/src/crdt/IdList.cpp`
- `libs/collabtext/tests/tst_crdt_indexes.cpp`

**Steps:**

1. Maintain origin-range→current run/boundary and deletion-id→affected runs mappings across split, insertion, coalescing, undo, GC, and checkpoint restoration. Do not keep raw pointers invalidated by tree mutation.
2. Use indexes for remote deletion lookup and undo/redo targeted visibility updates; remove redundant complete index rebuilds where tests establish incremental equivalence.
3. Provide diagnostic validation that compares indexed lookups to a full scan in test builds. Test origins absent because of safe reclamation distinctly from corrupt input.

**Acceptance:**

- Index rebuild and incremental maintenance agree on every seeded mutation trace.
- Undo/deletion visits scale with affected spans, with correct remote restoration and GC behavior.

**Verification:**

Run `tst_crdt_indexes`, GC/model/undo suites and ASan; profile 1, 100, and many affected runs at fixed total history size.

## X05 — Replace full-tree local ordering checks with checked splice boundaries

**Status:** see TASKS.json (initially not started)  
**Depends on:** X04, S07  
**Coverage:** R6  

**Files / scope:**

- `libs/collabtext/src/crdt/Buffer.cpp`
- `libs/collabtext/src/crdt/SumTree.h`
- `libs/collabtext/tests/tst_sequence_fast_paths.cpp`

**Steps:**

1. Derive local splice preconditions from S02 and validate adjacent boundaries touched by a splice; keep the full-tree ordering walk as a debug/test diagnostic.
2. Remove production O(all-fragments) validation only after differential traces demonstrate that untouched subtrees retain ordering invariants.
3. Retain an explicit slow path for cases outside those preconditions; record its frequency and avoid broad sort/rebuild unless required.

**Acceptance:**

- No ordering discrepancy against full validation/reference model, including adversarial split/undo/concurrent traces.
- Common local edits visit bounded tree paths/affected runs instead of a full history scan.

**Verification:**

Run sequence model, SumTree, seeded convergence, and fast/slow differential tests; publish visited-node counts at increasing historical fragment counts.

## X06 — Use run intervals for deletion and undo bookkeeping

**Status:** see TASKS.json (initially not started)  
**Depends on:** X04  
**Coverage:** R6, R7, R10  

**Files / scope:**

- `libs/collabtext/src/crdt/Buffer.cpp`
- `libs/collabtext/src/crdt/UndoMap.h`
- `libs/collabtext/src/storage/LocalHistoryLog.cpp`
- `libs/collabtext/tests/tst_run_bookkeeping.cpp`

**Steps:**

1. Replace temporary per-character deletion timestamp expansion and sorting with canonical actor/counter intervals; use interval references for undo groups where semantics permit.
2. Preserve origin identity and parity semantics through overlapping deletes, split runs, mixed Unicode, coalescing, local history persistence, and checkpoint/GcCertificate processing.
3. Version local-history/checkpoint encoding if intervals alter storage; explicitly reject obsolete development formats and recreate test stores. Do not silently reinterpret persisted records or add unnecessary compatibility decoders.

**Acceptance:**

- Large delete/paste replacement bookkeeping scales with affected runs and output intervals, not temporary character objects.
- Undo/redo and remote overlapping deletes match the slow model exactly.

**Verification:**

Run run-bookkeeping/model/undo/checkpoint suites; measure 1 KB/100 KB/1 MB bulk operations and retained history bytes.

## X07 — Add Unicode summaries and indexed anchor/delta queries

**Status:** see TASKS.json (initially not started)  
**Depends on:** X04, X05, S05  
**Coverage:** F7, R6, R8  

**Files / scope:**

- `libs/collabtext/src/crdt/Fragment.h`
- `libs/collabtext/src/crdt/SumTree.h`
- `libs/collabtext/src/CrdtEngine.cpp`
- `libs/collabtext/src/crdt/Buffer.cpp`
- `libs/collabtext/src/CollabDocument.cpp`
- `libs/collabtext/tests/tst_unicode_seek.cpp`

**Steps:**

1. Add UTF-16/scalar/line summaries only for supported query needs; make byte↔UTF-16 conversion seek by tree summary with local run scanning.
2. Resolve anchors through maintained origin/boundary indexes and retain correct affinity/retention behavior. For deltas, add an applied-batch change journal or safe summary pruning within the documented historical window.
3. Avoid whole text() reconstruction for each keystroke, remote caret, or viewport conversion. Cache viewport conversion/layout data at the adapter boundary where valid.

**Acceptance:**

- Multibyte, non-BMP, combining-mark, newline, and invalid-offset tests match exact expected Qt/text positions.
- Anchor/conversion visits are logarithmic plus touched-run work; delta queries never return plausible but incorrect edits outside their retention window.

**Verification:**

Run unicode-seek/anchor/delta/document-adapter/model suites; measure 1/10/100 remote cursors on fixed large Unicode documents.

## X08 — Implement incremental IdList mutations

**Status:** see TASKS.json (initially not started)  
**Depends on:** X04, S07  
**Coverage:** R6, R7, R14  

**Files / scope:**

- `libs/collabtext/src/crdt/IdList.cpp`
- `libs/collabtext/src/crdt/SumTree.h`
- `libs/collabtext/tests/tst_idlist_mutation_paths.cpp`

**Steps:**

1. Replace ordinary get_entries→vector edit→set_entries with tree-path insert/remove/visibility updates under the S02 ordering rules.
2. Keep a rebuild/reference path for diagnostics, checkpoint import, and complex fallback. Share policy/index helpers with Buffer without changing element-vs-text semantics.
3. Cover duplicate element values, equal-position concurrent inserts, removed anchors, undo/redo, reset, and GC certificates.

**Acceptance:**

- Tree path and reference vector path remain equivalent; individual edits do not copy the whole list.
- Costs are measured at 10/1,000/100,000 structural entries with controlled history and correct output.

**Verification:**

Run all IdList suites, index/model tests, and ASan; publish fixed-trace before/after allocations and latency.

## X09 — Bound catch-up work and eliminate repeated idle scanning

**Status:** see TASKS.json (initially not started)  
**Depends on:** X01, A04, G07  
**Coverage:** F5, F6, R5, R9, R10  

**Files / scope:**

- `libs/collabtext/src/crdt/StreamSync.cpp`
- `libs/collabtext/src/crdt/SegmentReader.cpp`
- `libs/collabtext/src/DocumentSession.cpp`
- `libs/collabtext/src/CollabDocument.cpp`
- `libs/collabtext/tests/tst_poll_budget.cpp`

**Steps:**

1. Add record/byte/time budgets per poll and preserve exact resume state. Batch engine application and Qt notifications without acknowledging beyond committed coverage.
2. Use resumable/cached validated segment inventories plus periodic complete reconciliation; filesystem watchers are hints, not the source of truth. Skip unchanged acks and avoid indefinitely retaining every merged payload when recoverable storage already owns it.
3. Start with bounded work on the owner thread. If profiling warrants workers, specify one engine owner and immutable messages before adding threading; never mutate one Buffer concurrently.

**Acceptance:**

- Large catch-up progresses across bounded slices without losing/reordering producer coverage; repeated idle polls do bounded work.
- GUI remains responsive in an instrumented offline-merge workload; notification count scales with batches, not inbound records.

**Verification:**

Run `tst_poll_budget`, transport/session recovery and Qt adapter tests; instrument event-loop delays during the X01 10-peer catch-up trace.

## X10 — Publish matched comparisons and the optimization result

**Status:** see TASKS.json (initially not started)  
**Depends on:** X03, X05, X06, X07, X08, X09, G08  
**Coverage:** R5, R12  

**Files / scope:**

- `benchmarks/adapters/`
- `docs/reports/vnext-performance-results.md`
- `docs/reports/vnext-backend-decision.md`

**Steps:**

1. Run five repeats of the frozen corrected traces before/after each accepted optimization; report per-stage and tail latency, history memory, recovery time, and disk/wire costs.
2. Run at least one native-accessible comparison adapter if the S02 backend decision calls for it; pin revision/licenses/build flags, normalize offsets and operation counts, and verify exact content/semantics. Feature mismatch is explicitly described.
3. If an optimization does not help the measured target, revert or justify its independent simplification benefit. Do not use trace throughput reciprocal as single-edit latency.

**Acceptance:**

- Results are attributable to specific changes and comparable states; all correctness/retention gates still pass.
- Remaining performance limits and any backend experiment recommendation are explicit; no backend substitution is performed merely to improve headline numbers.

**Verification:**

Run the Release trace matrix and publish machine-readable/raw results with metadata; cross-check narrative tables against measured units.
