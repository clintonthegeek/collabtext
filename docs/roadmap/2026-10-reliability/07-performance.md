# 07. Performance implementation against the merged-master baseline

The user selected this lane on 2026-10-19. Read [DIRECTION.md](DIRECTION.md),
[DECISIONS.md](DECISIONS.md), [EXECUTION.md](EXECUTION.md), then the
[measurement and target contract](../../specs/vnext-performance-targets.md).
The [October 19 evaluation](../../reports/2026-10-19-performance-evaluation.md)
and its immutable artifacts are the evidence, not unmeasured expectations.
[TASKS.json](TASKS.json) owns statuses/dependencies; the blocks below mirror it.
Completed tasks and dispositions are recorded in TASKS.json and the linked
reports; the evaluation alone completes none.

## Start here

One executor; the active entry point is **X04**. Follow this serial order:

`X01 → X02 → X07a → X04a → X07 → X07b → X04 → X05 → X06 → X08 → X03 → X11 → X12 → X10`.

The IDs retain existing ownership where possible; numeric order is not execution
order. X07a is the early read optimization, X04a the mutation design/model gate,
and X07b followed up on X07's measured byte-control miss before X04. X07b's
compact-summary prototype did not meet the cumulative byte-control thresholds;
the exact-width X07 representation remains in production. See the
[X07b report](../../reports/2026-10-20-x07b/README.md).
X03 is now a conditional layout investigation after the larger algorithmic cuts.
X11 is byte-preserving codec work; X12 installed/durable measurement. X09 stays
a separate transport lane behind G07. Core optimization/reporting does not wait
for physical GC, log pruning, external acceptance or competitor comparisons.
This is an explicit revision of those old edges, not treating incomplete GC as
complete. Existing collection tests still protect index invalidation paths.

The old X05 premise about removing full-tree validation is superseded: normal
edits actually copy/rebuild fragments. The old X06 permission to discard current
formats is superseded by current durable-reader duties. The old X07 delta-journal
proposal is not an instruction to replace the already implemented tree-delta API.
No completed task or historical candidate is reopened by this plan.

Finish correctness/package arcs after X07a (A), X08 (B), and X12 (C), using the
exact commands and pass conditions in the target contract. Measure Release
separately; never add wall-clock benchmark assertions to CTest. For each task,
record baseline and preceding-candidate comparisons, outcome, limitations and
implementation commit in the registry and task log. Expected speedups are
investigation goals; model correctness, honest accounting and explained
regressions are mandatory. A miss requires the documented disposition procedure.

New paths in a task scope are intentional deliverables. Read every existing
source before editing; register each new correctness executable and changed-path
route. Do not stage unrelated edits, including these previously prepared baseline
artifacts if they are not part of your selected task.

## X01 — Freeze checked workloads and paired baseline measurement

**Status:** complete at `1701d35`; full results in `docs/reports/2026-10-19-x01-baseline/README.md` and TASKS.json.
**Depends on:** V06, V04, S07, A04, Q05, M05
**Coverage:** R5, R12

**Files / scope:**

- `benchmarks/TraceFormat.md`
- `benchmarks/README.md`
- `benchmarks/BenchmarkRunner.cpp`
- `benchmarks/traces/`
- `libs/collabtext/CMakeLists.txt`
- `tools/verify_changed.py`
- `tools/run_performance_matrix.py`
- `tools/test_verify_changed.py`
- `tools/test_run_performance_matrix.py`
- `docs/reports/2026-10-19-x01-baseline/README.md`
- `docs/reports/2026-10-19-x01-baseline/raw/`
- `docs/specs/vnext-performance-targets.md`
- `docs/roadmap/2026-10-reliability/07-performance.md`

**Steps:**

1. Read the October 19 report and its exact public_api.cpp, aggregate.py and environment.json artifacts. Preserve those artifacts unchanged. Build the pinned d04f151 baseline separately from the candidate; never reset the working source or move a tag to obtain a baseline.
2. Promote the October 19 public cases into an opt-in, non-CTest benchmark target. Preserve its seed-42, one-insertion fixtures, prior-edit counts, 200-call windows, matched byte controls and final-operation bytes. Add raw per-call samples, exact accepted/refused/no-op counts, checked state/position results and explicit sample/percentile definitions.
3. Add varying-position boundary and strict-attachment reads, marker resolution between edits, Unicode offset sweeps, bulk insert/delete/replace and undo, aged lists with exact-identity lookup, actor-frontier codec cases, and checkpoint reopen at two replay sizes. Record new baselines for cases absent from the October 19 report; never infer historical timings.
4. Emit versioned samples and a run manifest with revision/input hashes, compiler/flags, host/CPU, filesystem, available governor and per-command load, work units, history/actor/structure counts, and metric availability. Keep public engine, byte-control, codec, existing native benchmark and checkpoint-recovery cases identifiable. Run five alternating Release baseline/candidate pairs without competing builds and preserve every raw run.
5. Implement the ratio/target and regression rules in docs/specs/vnext-performance-targets.md, including unchanged default undo settings and a labeled 256-MiB override only where required. Register changed-path routes for new source; keep benchmarks opt-in and outside CTest.

**Acceptance:**

- The same trace yields exact logical results on d04f151 and the current candidate; malformed traces and refused work fail or are explicitly counted, never reported as successful throughput.
- The new harness reproduces the structural/wire facts of the recorded baseline and establishes paired timings and isolated cases for subsequent tasks; first measurement does not mark any optimization complete.

**Verification:**

Run workload-schema mutation tests, all sample/accounting and cross-build fingerprint checks, the opt-in/non-CTest build check, five Release baseline/candidate pairs, and plan/scope plus verifier-tooling checks. See vnext-performance-targets.md for the measurement protocol.

## X02 — Profile allocations and select concrete mutation mechanisms

**Status:** complete (see TASKS.json)
**Depends on:** X01
**Coverage:** R5, R6, R7, R10

**Files / scope:**

- `libs/collabtext/src/crdt/Diagnostics.h`
- `libs/collabtext/src/crdt/Buffer.cpp`
- `libs/collabtext/src/crdt/PositionTraversal.cpp`
- `libs/collabtext/src/crdt/Utf16.h`
- `libs/collabtext/src/IdListEngine.cpp`
- `libs/collabtext/tests/tst_perf_diagnostics.cpp`
- `benchmarks/TraceFormat.md`
- `libs/collabtext/src/crdt/IdList.cpp`
- `libs/collabtext/src/crdt/SumTree.h`
- `benchmarks/BenchmarkRunner.cpp`
- `docs/reports/vnext-performance-profile.md`
- `docs/specs/vnext-performance-targets.md`
- `docs/roadmap/2026-10-reliability/07-performance.md`

**Steps:**

1. Count fragment/item copies, scalar materializations, tree visits/rebuilds, split/visibility work, origin/deletion lookups, undo-retirement work and causal-frontier processing on the X01 traces. Use compile-time-disabled or genuinely opt-in diagnostics and validate tiny exact count fixtures.
2. Measure allocations and retained/peak bytes in isolated fresh processes. Keep allocator-reserved memory, live engine allocations, process RSS and filesystem cache distinct. Use an available sampling/allocation profiler; when unavailable, record that limitation and use operation/allocation counts without inventing CPU percentages.
3. Publish the leading measured costs, diagnostic overhead and hypotheses in docs/reports/vnext-performance-profile.md. Identify actual current paths in Buffer::get_fragments/set_fragments, utf16_range, PositionTraversal, IdListEngine and OperationCodec. Full debug consistency validation is not presumed to run per edit.
4. Record which representation changes are justified and which are conditional experiments. Supply evidence to X04a; do not change ordering, archive readers, undo limits, GC eligibility or public API in this profiling task.

**Acceptance:**

- Enabled/disabled diagnostics produce identical logical output and have measured overhead; ordinary shipped builds do not pay per-event logging or synchronization costs.
- The profile separates byte-size, retained-fragment, actor-count and active/retired undo costs and supplies a measured basis for implementation choices.

**Verification:**

Run diagnostic count tests and affected warm correctness checks; run isolated Release profiles and a disabled-diagnostics paired benchmark. Publish raw profiler/count evidence with limitations.

**Completion (2026-10-19):**

- Initial Release `perf` and Massif evidence plus five alternating diagnostics
  on/off quick-profile pairs are recorded in
  [`vnext-performance-profile.md`](../../reports/vnext-performance-profile.md);
  raw profiler and JSONL files are in
  [`2026-10-19-x02-profile/raw/`](../../reports/2026-10-19-x02-profile/raw/).
- Opt-in counters compile to no-ops by default. Exact snapshot, scalar
  projection, UTF-16, position traversal, list identity, and undo-state
  fixtures pass. The current counter set is
  `collabtext-perf-diagnostics/3`; all 54 quick-profile cases preserve logical
  and operation fingerprints with it enabled. Its apparent median overhead
  ratio was 0.887, but the run occurred under extreme load (20.02/16.93/13.52
  on an eight-CPU host); earlier counter sets reported 1.060 and 1.023. Treat
  timing overhead as unquantified and use diagnostics only for work counts.
- Counts show 9.71 million UTF-16 decoder steps across five 1-MB public edits;
  505,000 scalar items materialized/examined across five 100-KB boundary mints
  and five resolves; and 50,020 list items flattened across five aged 5,000-
  entry insertions. The history-3,000 text case separately observes active and
  retired undo budget charges. Source-path interpretation and X04a/X05/X07/
  X07a/X08 dispositions are recorded in the profile; algorithm changes remain
  behind the independent model gate.
- Byte-size, retained-fragment, active/retired undo, actor-count, UTF-16 and
  position/list paths are measured. Schema-3 counter fixtures pass and the
  final affected Debug verification passed all 100 selected tests at
  `build-roadmap-debug/verification/20261019T224402Z-changed-0o1o2fsh`.
  No algorithm or API change is selected by this profile; X04a retains the
  mutation/model gate.

## X07a — Replace scalar boundary projections with run-aware reads

**Status:** complete; implementation and evidence are recorded below and in TASKS.json.
**Depends on:** X02, S04a, S04b, S05
**Coverage:** F7, R6, R8

**Files / scope:**

- `libs/collabtext/src/CrdtEngine.cpp`
- `libs/collabtext/src/crdt/Buffer.h`
- `libs/collabtext/src/crdt/Buffer.cpp`
- `libs/collabtext/src/crdt/PositionTraversal.h`
- `libs/collabtext/src/crdt/PositionTraversal.cpp`
- `libs/collabtext/tests/tst_positions.cpp`
- `libs/collabtext/tests/tst_boundary_model.cpp`
- `libs/collabtext/tests/tst_position_token_reader.cpp`
- `libs/collabtext/tests/tst_text_markers.cpp`
- `libs/collabtext/tests/tst_marker_model.cpp`
- `libs/collabtext/tests/tst_anchor_retention.cpp`
- `libs/collabtext/tests/tst_perf_diagnostics.cpp`
- `tools/run_performance_matrix.py` (pinned-baseline harness compatibility)
- `tools/verify_changed.py`
- `docs/reports/2026-10-19-x07a/README.md`
- `docs/reports/2026-10-19-x07a/raw/`
- `docs/specs/vnext-performance-targets.md`
- `docs/roadmap/2026-10-reliability/07-performance.md`
- `docs/roadmap/2026-10-reliability/TASKS.json`

**Steps:**

1. Read vnext-boundary-semantics.md, vnext-anchor-retention.md, PositionTraversal.cpp and the independent boundary model. Define a private retained-run traversal that computes the same boundary/strict-attachment results without constructing one PositionItem per scalar or copying all fragment text.
2. Implement read-only mint/resolve paths for boundaries and attachments. Preserve exact referents, affinity, hidden-neighbour collapse, scope/version checks, empty sequences and markers. Keep scalar expansion only as a test oracle or genuinely required non-hot-path fallback; do not route all calls through it.
3. Differentially compare old scalar oracle and new traversal over varying offsets, local/remote edits, marker removal/restoration, hidden runs, checkpoints, discarded views and existing certified-collection fixtures. Never validate a new algorithm by calling its own helper from the independent model.
4. Measure fixed and changing token/offset reads before and after mutations. Do not meet the target solely by caching the repeated fixture token or retaining stale projections across mutations. Leave byte offsets, token bytes and public signatures unchanged.

**Acceptance:**

- Boundary and attachment model/fixture results remain exact, including explicit detached/history-unavailable outcomes; marker resolution does not regress.
- Common reads allocate no O(retained-scalars) temporary vector. Demonstrate the X07a target of at least 10x lower mean boundary mint/resolve cost on the 100-KB history-0/1000 cases and report varied-position results.

**Verification:**

Run tst_boundary_model, tst_positions, tst_position_token_reader, tst_text_markers, tst_marker_model and tst_anchor_retention plus routed checks; finish arc A gates and five paired query measurements as defined in vnext-performance-targets.md.

**Completion (2026-10-20):** X07a is implemented at
`3e3d4c141afe90cafc4f25898559d478523c6555`. The paired-runner baseline
compatibility fix is `eb9176156ec83d5f1514094d630669d787add6d0`; benchmark
environment capture is `1a1cbe48d949433e4aa005b1477d7dd76e84492a`. The report
and raw five-pair results are in
[`2026-10-19-x07a/`](../../reports/2026-10-19-x07a/README.md). All eight
varied-offset/token boundary and attachment cases met the 10x target, with
63x–20,392x median speedups. The full 54-case public harness preserved result
and operation fingerprints. A pinned follow-up found no reproducible bulk
insert regression after one full-matrix outlier.

Focused position/model/marker/retention suites passed. Fresh Arc A Debug and
Release each passed 101/101 tests; ASan/UBSan with leak detection enabled passed
101/101 after running outside the sandbox's ptrace restriction. Relocated live
installed-Core package gates passed Debug, Release and ASan/UBSan (5/5 consumer
tests in each mode, Wave 16/16 strict cases, Markdown 4/4). Evidence paths and
environment limitations are recorded in the report and TASKS.json. The roadmap
validator and strict scope checker pass.
## X04a — Specify and model incremental tree and index transactions

**Status:** complete; design and independent model gate passed (see TASKS.json).
**Depends on:** X07a, S03, S07, P03
**Coverage:** R6, R7, R8, R10

**Files / scope:**

- `docs/specs/vnext-incremental-index-design.md`
- `tests/performance-model/incremental-model.cpp`
- `tests/performance-model/incremental-model.h`
- `libs/collabtext/tests/tst_incremental_model.cpp`
- `libs/collabtext/CMakeLists.txt`
- `tools/verify_changed.py`
- `tools/test_verify_changed.py`
- `docs/specs/vnext-performance-targets.md`
- `docs/roadmap/2026-10-reliability/07-performance.md`
- `docs/roadmap/2026-10-reliability/TASKS.json`
- `docs/reports/vnext-task-log.md`

**Steps:**

1. Use X02 evidence to write the exact private tree/index design before mutation code: ownership, stable handles, origin intervals, deletion incidence, summaries, split/splice/visibility algorithms, invalidation and complexity. Define how handles survive copy-on-write and node split; raw addresses into movable vector/tree storage are forbidden.
2. Specify one transaction boundary covering tree, identity/deletion indexes, visibility/Unicode summaries, undo state, causal coverage, allocator authority and callbacks. Classify every allocation/refusal point; preserve current checked-counter policy and failure-atomicity rather than promising a new allocator rollback capability.
3. Specify rebuild/invalidation for restore, reset/discard, gaps, marker edits, undo/redo, coalescing, retired history and existing certified collection. Derived indexes are not durable truth. Preserve existing readers/writers and ordering; define a fully correct fallback with an observable reason/count.
4. Build an independent small-state model of split/splice/identity lookup/visibility and transaction failure, with no production tree or index calls. Enumerate short traces and seeded longer traces including allocation/refusal injection. Specify aggregate monoid and Unicode-boundary laws.
5. Register the runtime-checked Release model and routing. Publish counterexamples or unresolved design details; do not start X07/X04/X05 until the design and model acceptance is complete.

**Acceptance:**

- The design gives executable pseudocode, ownership/failure rules, update tables for every mutator and explicit rebuild/fallback conditions; no unspecified pointer repair or summary invalidation remains.
- Independent model passes in Debug and Release with runtime checks and failure injection; it preserves the existing sequence/position contracts and exact refusal outcomes.

**Verification:**

Build/run tst_incremental_model in Debug and -DNDEBUG Release, replay fixed seeds twice, and run plan/scope/verifier checks. Review the model against the existing independent sequence and boundary models.

**Design/decision gate:** Complete the written algorithms and independent transaction/index model before dependent summary, index or mutation implementation.
## X07 — Seek UTF-16 ranges using maintained tree summaries

**Status:** complete with a measured byte-control miss; X07b completed the follow-up, and X04 is next (see TASKS.json).
**Depends on:** X04a, P03
**Coverage:** F7, R6, R8

**Files / scope:**

- `libs/collabtext/src/crdt/Fragment.h`
- `libs/collabtext/src/crdt/SumTree.h`
- `libs/collabtext/src/crdt/Buffer.cpp`
- `libs/collabtext/tests/tst_summary_laws.cpp`
- `libs/collabtext/tests/tst_unicode_seek.cpp`
- `libs/collabtext/CMakeLists.txt`
- `tools/verify_changed.py`
- `docs/specs/vnext-performance-targets.md`
- `docs/roadmap/2026-10-reliability/07-performance.md`
- `docs/reports/2026-10-20-x07/README.md`
- `docs/reports/vnext-task-log.md`


**Steps:**

1. Implement the X04a visible-byte/UTF-16 aggregate design and summary-guided rank/seek. Add per-run scalar-boundary checkpoints or an equivalent bounded local scan where long Unicode runs require them; specify and measure their memory overhead. Do not add unused line/layout summaries.
2. Replace Buffer::utf16_length/utf16_range whole-visible-tree scans while preserving negative/range/overflow checks and surrogate-split refusal. Maintain aggregates through every existing vector rebuild, split, marker mutation, visibility change and restore before X05 replaces rebuilds.
3. Check ASCII, non-BMP, combining marks, newlines, empty/hidden text, marker adjacency and offsets at every run boundary against a straightforward independent UTF-8/UTF-16 conversion. Reuse aggregate helpers in the later incremental path; do not move policy into Qt.
4. Measure 1-KB to 1-MB public and matched byte cases plus changing Unicode positions. Separate coordinate-conversion visits from mutation cost and prove lookup does not scan all unrelated visible runs.

**Acceptance:**

- Summary laws and conversion/model outcomes are exact after all mutators, including checkpoint restore and failed edits.
- Target at least 2x lower mean public 1-MB replacement cost versus paired d04f151, with no more than 10% regression in byte controls; publish actual conversion visits and added bytes. No logarithmic claim may hide a whole large-run scan.

**Verification:**

Run tst_unicode_seek, tst_summary_laws, tst_sumtree, tst_positions, tst_crdtengine_op_api and Unicode/marker/restore routed checks; run five paired size/conversion workloads, then arc B gates after X08.

**Completion (2026-10-20):** The implementation and paired evidence are recorded
in [`2026-10-20-x07/`](../../reports/2026-10-20-x07/README.md). The 1-MB
public replacement median mean ratio is 0.322 (3.11× faster) and all 54 public
results/fingerprints/work counts match. The matched byte controls are 1.165 at
100 KB and 1.101 at 1 MB, exceeding the 1.10 guard; retain the useful UTF-16
seek and exact 64-bit overflow accounting, and resolve the representation cost
through X07b before X04. Memory is +8 bytes per FragmentSummary and +24 bytes
per Fragment on the measured 64-bit build, plus sparse checkpoint payload and
allocation overhead for long non-ASCII runs. Focused correctness and diagnostic
results, load caveats and raw samples are in the report.
## X07b — Reduce UTF-16 summary overhead in matched byte workloads

**Status:** complete; keep X07's exact-width summary after the compact prototype missed the cumulative byte-control thresholds (see TASKS.json and the [measured report](../../reports/2026-10-20-x07b/README.md)).
**Depends on:** X07, P03
**Coverage:** F7, R6, R8

**Files / scope:**

- `libs/collabtext/src/crdt/Fragment.h`
- `libs/collabtext/src/crdt/SumTree.h`
- `libs/collabtext/src/crdt/Buffer.h`
- `libs/collabtext/src/crdt/Buffer.cpp`
- `libs/collabtext/src/crdt/Diagnostics.h`
- `libs/collabtext/tests/tst_summary_laws.cpp`
- `libs/collabtext/tests/tst_unicode_seek.cpp`
- `libs/collabtext/tests/tst_perf_diagnostics.cpp`
- `docs/reports/2026-10-20-x07b/README.md`
- `docs/specs/vnext-performance-targets.md`
- `docs/roadmap/2026-10-reliability/07-performance.md`
- `docs/roadmap/2026-10-reliability/TASKS.json`
- `tools/run_performance_matrix.py`
- `tools/test_run_performance_matrix.py`

**Steps:**

1. Start from X07 commit eefcf9d and its paired evidence. Attribute the 100-KB (+16.5%) and 1-MB (+10.1%) matched byte-control costs using diagnostics-disabled Release runs and work counters; distinguish per-fragment footprint, tree-summary composition, and cache/index construction.
2. Prototype the smallest representation/update change that preserves exact visible-byte overflow refusal, UTF-16 rank/seek, summary associativity, checkpoint restore and split behavior. Keep public APIs and byte coordinates unchanged. Do not remove the independent oracle or use diagnostic builds for latency claims.
3. Run the affected summary, Unicode, SumTree, position, marker and restore checks. Compare five paired runs against both d04f151 and the X07 candidate, keeping all 54 public fingerprints and work counts exact. Preserve raw data and host-load records.
4. If a candidate cannot reduce the byte-control regression without weakening correctness or losing the large-Unicode benefit, retain X07 and record a measured no-change disposition rather than broadening scope.

**Acceptance:**

- Exact overflow refusal and UTF-16 boundary/model outcomes remain unchanged across all existing mutators, split, undo and restore.
- On the X07 paired workload, median mean ratios for matched byte controls at 100 KB and 1 MB are each <=1.10, and public 1-MB replacement remains at least 2x faster than d04f151. If this cannot be achieved, provide a measured keep/no-change disposition and identify any remaining material overhead.

**Verification:** Run tst_summary_laws, tst_unicode_seek, tst_sumtree, tst_positions, tst_text_markers, tst_perf_diagnostics and routed restore checks; run five paired Release comparisons against d04f151 and X07, then plan/scope/tooling validation.

**Completion:** The compact prototype reduced `FragmentSummary` from 16 to 12
bytes and measured SumTree nodes from 2,632 to 2,576 bytes. Five pinned public
comparison pairs preserved all 54 result/fingerprint/work-count records, but
the candidate's matched byte ratios versus d04f151 were 1.138 at 100 KB and
1.112 at 1 MB, both above the 1.10 guard. Against X07, its byte-control ratios
were 1.031 at both sizes. The prototype was reverted; X07 remains the selected
representation. Diagnostics and runner improvements are retained. X04 is next.
Detailed ratios, load caveats, and raw artifacts are in the linked report.

## X04 — Maintain origin and deletion indexes with checked updates

**Status:** in progress; see TASKS.json for baseline and current implementation slice.
**Depends on:** X07b, S05
**Coverage:** R6, R7, R10

**Files / scope:**

- `libs/collabtext/src/crdt/OriginIndex.h`
- `libs/collabtext/src/crdt/DeletionIndex.h`
- `libs/collabtext/src/crdt/RunDirectory.h`
- `libs/collabtext/src/crdt/PersistentAvlMap.h`
- `libs/collabtext/src/crdt/Buffer.cpp`
- `libs/collabtext/src/crdt/Buffer.h`
- `libs/collabtext/src/crdt/SumTree.h`
- `libs/collabtext/src/crdt/Fragment.h`
- `libs/collabtext/src/crdt/IdList.cpp`
- `libs/collabtext/tests/tst_crdt_indexes.cpp`
- `libs/collabtext/tests/tst_summary_laws.cpp`
- `libs/collabtext/tests/tst_perf_diagnostics.cpp`
- `libs/collabtext/CMakeLists.txt`
- `tools/verify_changed.py`
- `docs/specs/vnext-performance-targets.md`
- `docs/roadmap/2026-10-reliability/07-performance.md`

**Steps:**

1. Implement the modeled X04a origin-interval and deletion-incidence mappings with stable handles. Initially validate against existing rebuilding mutation paths; then maintain only affected entries where the model proves equivalence. Do not introduce per-scalar index objects for a compact retained run.
2. Route exact-identity lookup, remote deletion targeting and compensation affected-run lookup through the mappings. Preserve split-run identity ranges, overlapping deletions, hidden markers, duplicate list values and absent-versus-safely-collected outcomes.
3. Rebuild derived indexes on restore/reset where specified; invalidate or update them on every mutator from the design table. Compare indexed results with a full scan after each step in deterministic differential tests, including existing certificate-collection and counter-gap traces.
4. Use modeled transactional updates and fault injection so refusal cannot leave tree/index/version/history disagreement. Record affected-run visit counts at fixed history sizes and separate index setup costs from lookups.

**Acceptance:**

- Incremental/rebuilt indexes agree with scan results over all tested traces; current GC fixtures continue to pass without enabling physical collection for consumers.
- Target at least 90% fewer run visits for one-run origin/deletion/compensation lookup at 5,000 retained runs, excluding the later mutation work; disclose index memory and fallback frequency.

**Verification:**

Run tst_crdt_indexes, tst_incremental_model, sequence/undo/IdList/anchor/GC/gap/restore routed suites and sanitizer fault injection; compare fixed-size targeted lookup visit counts and latency.

**Current implementation slice:** Immutable path-copied AVL map storage and
origin/deletion interval indexes now update by changed-run deltas on ordinary
mutations; full sorted row rebuild remains a counted fallback when prior
indexes are invalid or a candidate delta fails validation. Remote deletion
resolves matching retained intervals from the origin index and applies splits
right-to-left. RunIds are monotonic and state-local, preserved when the run's
exact start identity survives; a RunDirectory resolves each ID to its current
ordinal. RunDirectory and SumTree are still rebuilt after structural mutations.
Stable SumTree NodeIds/parent directories, incremental RunDirectory maintenance,
and tree splices remain open.
Local undo/redo and remote undo use origin/deletion incidences to identify the
affected ordinals, then seek by the retained-run SumTree summary and edit only
those paths. A stale ordinal falls back to full visibility recomputation. The
opt-in diagnostics count SumTree leaf items inspected: a fixed 5,000-run case
compares the full scan against one ordinal edit and enforces the 90% work
reduction target. One-run undo diagnostics visit two leaf items in a three-run
tree. `set_fragments` now copies the persistent index roots, erases rows for
changed/removed runs, and inserts rows for changed/new/split runs. Untouched AVL
pages are shared. Full origin/deletion row vectors and sorted rebuilds are now
created only when the old index is invalid or a delta check fails. The opt-in
diagnostics schema 5 counts row edits and full-rebuild fallbacks; focused insert
and deletion cases each update only the affected run rows and assert zero full
index rebuilds. `RunDirectory` and `SumTree` are still rebuilt after every
structural mutation, and `set_fragments` still scans the old tree and staged
fragment vector to identify changes. The
summary adds one `uint64_t` per aggregate; its memory and byte-control cost have
been measured at 24 bytes per FragmentSummary and 2,736 bytes per tree node,
versus 16 bytes and 2,632 bytes for the exact-width X07 baseline (+104 bytes per
node, about 4%). A five-pair comparison against X07 matched all 54 public
fingerprints and work counts. Its median paired mean ratio for matched byte
replacement was 2.054 at 100 KB and 1.531 at 1 MB. The host load reached 17.57
on an 8-logical-CPU machine and used the `powersave` governor, so these figures
are a diagnostic warning only, not an acceptance claim or causal attribution.
See [the report](../../reports/2026-10-20-x04-summary-overhead/README.md) and
raw run metadata. Structural edits still flatten/rebuild the tree and
RunDirectory, and stable NodeId directories and incremental tree maintenance
remain open.
No end-to-end performance claim is made yet. Debug verification passed 104/104
on the index-delta implementation; final-source focused Debug and Release suites
passed 5/5 and 4/4. Leak-enabled ASan/UBSan cases completed with all test cases
passing, but CTest exits nonzero because LeakSanitizer cannot operate under this
runner's ptrace environment; that gate remains open.
See the current X04 entry in TASKS.json.

## X05 — Replace whole-fragment text rebuilds with transactional splices

**Status:** see TASKS.json (not started)
**Depends on:** X04, S07
**Coverage:** R6, R7

**Files / scope:**

- `libs/collabtext/src/crdt/Buffer.cpp`
- `libs/collabtext/src/crdt/SumTree.h`
- `libs/collabtext/src/crdt/SequenceOrder.cpp`
- `libs/collabtext/tests/tst_sequence_fast_paths.cpp`
- `libs/collabtext/tests/tst_sequence_differential.cpp`
- `libs/collabtext/tests/tst_sumtree.cpp`
- `docs/specs/vnext-performance-targets.md`
- `docs/roadmap/2026-10-reliability/07-performance.md`

**Steps:**

1. Implement X04a path-copy/splice and affected-visibility operations using X04 handles and X07 aggregates. First cover checked local inserts/replacements and markers, then remote insert/delete and compensation; keep each subchange reviewable and differential-tested.
2. Preserve native FugueMax integration and checked splice preconditions. Replace the measured get_fragments/vector edit/set_fragments rebuild path; do not claim a win from removing a debug consistency check that is absent from ordinary edits.
3. Keep the old reconstruction algorithm as a test oracle and a counted, justified production fallback for unsupported complex cases until those cases are proven. Preserve immutable operation data, exact identity/version/undo effects, callback timing and refusal atomicity.
4. Test concurrency, adversarial origins, Unicode splits, marker order, checkpoint recovery, gaps and undo/redo under allocation failure and seeded model traces. Assert common one-run edits do not enumerate/copy/rebuild the full fragment collection.
5. Measure fresh bulk-seeded, chunk-seeded, 500/3000-prior-edit, tombstone-rich, undo and catch-up cases. Publish fallback frequency and work counts; an optimization that merely shifts an O(history) rebuild into index maintenance does not satisfy acceptance.

**Acceptance:**

- All differential/model results and transactional refusal checks pass; common local and remote edits touch tree paths and affected runs rather than cloning the entire retained collection.
- Target at least 5x lower aged 10-KB/3000-prior-replacement mean and median per-run p99 at most 2 ms in a comparable paired environment; target at least 2x lower mixed 5-KB/50%-deletion mean, with no more than 10% regression in light-history cases.

**Verification:**

Run Buffer/SumTree/sequence/position/undo/GC/recovery/gap/marker routed suites and independent models with fixed replayable seeds; run five paired edit/tombstone/merge workloads. Finish arc B gates after X08.

## X06 — Compact in-memory undo and deletion bookkeeping by intervals

**Status:** see TASKS.json (not started)
**Depends on:** X05
**Coverage:** R6, R7, R10

**Files / scope:**

- `libs/collabtext/src/crdt/Buffer.cpp`
- `libs/collabtext/src/crdt/UndoMap.h`
- `libs/collabtext/src/crdt/UndoMap.cpp`
- `libs/collabtext/src/crdt/LocalHistoryBudget.h`
- `libs/collabtext/tests/tst_run_bookkeeping.cpp`
- `libs/collabtext/CMakeLists.txt`
- `tools/verify_changed.py`
- `docs/specs/vnext-performance-targets.md`
- `docs/roadmap/2026-10-reliability/07-performance.md`

**Steps:**

1. Use X02 measurements to identify temporary per-scalar identity expansion and sorting in delete/undo/retirement. Specify canonical actor/counter interval rules, splitting and parity updates in the X04a design before changing these representations.
2. Use compact internal intervals where equivalent; preserve origin identity, overlapping deletion parity, group coalescing/selective compensation, shared text/list undo and retirement acknowledgment semantics. Keep active undo, retired obligations and retained sequence history distinct.
3. Preserve current operation/token bytes, supported checkpoint/local-history reader behavior and default undo-depth/byte policy. Expand intervals only at existing encoding boundaries if necessary. Do not reinterpret old bytes, remove current readers or silently loosen the import limit to meet a benchmark.
4. Test interval and slow scalar bookkeeping equivalence for overlapping deletes, mixed Unicode, undo/redo, group retirement, reopened histories and existing collection certificates. Measure isolated allocations/peak live bytes and 1-KB/100-KB/1-MB insert/delete/replace under explicitly labeled budgets.

**Acceptance:**

- Independent parity/interval and existing undo/recovery models agree; current archive goldens and default 1-MB refusal probe remain unchanged unless a separately documented policy task is authorized.
- Target at least 50% fewer temporary allocated bytes in the X01 1-MB bulk-delete/undo case relative to its newly established baseline; scalar count alone must not allocate one temporary object per item when the result is one interval.

**Verification:**

Run tst_run_bookkeeping, tst_undomap, tst_undo_retirement, tst_persistent_undo, text/list undo, checkpoint and reader suites; run paired isolated bulk allocation/latency cases.

## X08 — Make IdList identity lookup and mutation incremental

**Status:** see TASKS.json (not started)
**Depends on:** X06, M01
**Coverage:** R6, R7, R14

**Files / scope:**

- `libs/collabtext/src/IdListEngine.cpp`
- `libs/collabtext/src/crdt/IdList.cpp`
- `libs/collabtext/src/crdt/SumTree.h`
- `libs/collabtext/tests/tst_idlist_mutation_paths.cpp`
- `tests/consumer-smoke/markoff_core.cpp`
- `libs/collabtext/CMakeLists.txt`
- `tools/verify_changed.py`
- `docs/specs/vnext-performance-targets.md`
- `docs/roadmap/2026-10-reliability/07-performance.md`

**Steps:**

1. Route full insertion-identity anchor/element lookup through X04 indexes without calling visible_entries for each lookup. Keep application values distinct from insertion identities; duplicate values remain legal.
2. Reuse the X04a/X05 transaction/splice mechanisms for list insert/remove/visibility changes instead of get_entries/vector edit/set_entries. Preserve list-specific semantics rather than coercing values into text or creating a second ordering engine.
3. Keep visible_entries as a complete ordered O(output entries) enumeration; eliminate avoidable intermediate projections/copies but do not promise constant-time full enumeration. Add differential tests for equal values, concurrent insertion, hidden anchors, undo/redo, reset, checkpoint and existing collection paths.
4. Measure 100/1000/5000 entries plus aged/deleted lists and an opt-in 100000-entry stress case when feasible. Establish and compare exact-identity lookup and mutation baselines added in X01; keep Markoff public-header fixture coverage.

**Acceptance:**

- Whole IdList family and full-identity Markoff traces pass; common single-entry mutation avoids copying the entire list and indexed lookup agrees with enumeration.
- Target at least 10x lower exact-identity lookup mean at 5000 entries and at least 5x lower aged-list single-entry mutation mean versus X01 baselines; keep visible_entries mean within 10% of its paired baseline and disclose output-allocation cost.

**Verification:**

Run all tst_idlist* plus tst_markoff_core, list model/index/transaction suites and routed checks; run five paired list workloads and finish arc B Debug/Release/leak-enabled ASan and relocated installed gates.

## X03 — Evaluate measured summary and sparse-frontier layout costs

**Status:** see TASKS.json (not started)
**Depends on:** X08, P03
**Coverage:** R7

**Files / scope:**

- `libs/collabtext/src/crdt/Clock.h`
- `libs/collabtext/src/crdt/Clock.cpp`
- `libs/collabtext/src/crdt/Fragment.h`
- `libs/collabtext/src/crdt/IdList.h`
- `libs/collabtext/src/crdt/SumTree.h`
- `libs/collabtext/tests/tst_clock.cpp`
- `libs/collabtext/tests/tst_summary_laws.cpp`
- `docs/specs/vnext-performance-targets.md`
- `docs/roadmap/2026-10-reliability/07-performance.md`

**Steps:**

1. Reprofile after incremental mutation; use actual node/item/summary sizes and live allocation counts to select an experiment, not the obsolete assumption that summary layout is a prerequisite for all optimization.
2. Remove only demonstrated unused summaries or improve sparse-frontier ownership with checked RAII storage where measured costs justify it. Preserve aggregation versus coverage-intersection semantics, inline/heap transitions, copy/move/self-assignment and failure safety.
3. Compare memory and latency over small/large actor counts and aged streams. Keep an experiment only for a measured target improvement or an explicit independent simplification; otherwise revert its production code and record the negative result.

**Acceptance:**

- Target at least 20% lower live allocated bytes for the selected dominant structure with no more than 10% target-latency regression, measured against the immediately preceding candidate.
- A justified no-change disposition with before/after evidence may complete this investigation; it must not be described as a delivered memory improvement. Summary/clock laws and sanitizer checks remain exact.

**Verification:**

Run tst_clock, tst_summary_laws, tst_sumtree and affected model suites; publish isolated allocation and paired actor/history latency results with an explicit keep/revert disposition.

## X11 — Reduce codec CPU while preserving current operation bytes

**Status:** see TASKS.json (not started)
**Depends on:** X03
**Coverage:** R5, R7, R9

**Files / scope:**

- `libs/collabtext/src/codec/OperationCodec.cpp`
- `libs/collabtext/src/crdt/Clock.h`
- `libs/collabtext/tests/tst_serialization.cpp`
- `libs/collabtext/tests/tst_decode_validation.cpp`
- `libs/collabtext/tests/tst_operation_validation.cpp`
- `docs/specs/vnext-performance-targets.md`
- `docs/roadmap/2026-10-reliability/07-performance.md`

**Steps:**

1. Profile actor-frontier traversal, allocation, formatting and validation using X01 1/32/128/512-actor cases. Preserve canonical bytes and all checked decode/refusal behavior; optimize traversal/buffering/ownership only where evidence identifies avoidable work.
2. Run exact encode/decode goldens and malformed/future-version inputs alongside the supported CTBO/CTLO/CTGP/CTPT readers. Keep encoded scope, identities and causal prior intact; do not omit remembered actors or replace full frontiers with unsafe scalar watermarks.
3. Report wire bytes separately from CPU. Current 512-actor fixture remains 28100 bytes unless a future explicit format/reader design task is approved. Document transport batching/compression options as follow-up evidence, without modifying consumer logs or adding a durable format in this task.

**Acceptance:**

- Target at least 25% lower encode and decode mean at 512 actors relative to paired d04f151 (recorded 435.558/304.851 microseconds); canonical bytes and rejection semantics remain identical.
- If profiling establishes an irreducible cost under the current encoding, record a measured no-change disposition rather than retain unproved complexity or claim a wire-size improvement.

**Verification:**

Run codec/decode/operation validation, scope/gap and retained-reader suites; run five paired codec cases with exact byte checks, then arc C gates after X12.

## X12 — Measure durable composition and deliver installed performance evidence

**Status:** see TASKS.json (not started)
**Depends on:** X11, A04, M04
**Coverage:** R5, R9, R10, R12, R14

**Files / scope:**

- `benchmarks/BenchmarkRunner.cpp`
- `tests/consumer-smoke/wave_core.cpp`
- `tests/consumer-smoke/markoff_core.cpp`
- `docs/reports/vnext-performance-results.md`
- `docs/handoff/wave-core-response.md`
- `docs/handoff/markoff-core-response.md`
- `docs/specs/vnext-performance-targets.md`
- `docs/roadmap/2026-10-reliability/07-performance.md`

**Steps:**

1. Add producer-owned benchmark compositions for actual durable identity reservation, operation encode/append/reopen, checkpoints, callbacks and repeated position/list reads using installed public headers. Wave owns its log; DocumentSession is optional and is not a substitute for Wave transaction acceptance.
2. Run warm recovery on tmpfs and a named real filesystem separately when available; record sync policy, storage/mount and cache conditions. Never label a warm/tmpfs open as cold SSD durability. Establish stage baselines against d04f151 before optimizing these previously unisolated cases.
3. Re-run 512/2048-operation checkpoint/replay and 2/5/10-peer catch-up fixtures, exact content/identity/version and current tree-delta composition tests. Check callbacks/reentrancy and local history restore. Compare installed public-header results to in-tree timings without private benchmark shortcuts.
4. Document consumer adoption advice, measured limits and any missing real adapter traces. Do not edit consumer repositories, send messages, push, publish, move tags, merge, or mark Q06/M06 approved. No additional GUI or transport API is introduced by this task.

**Acceptance:**

- Stage costs and producer-shaped versus consumer-owned evidence are explicit; target no more than 10% mean regression in paired checkpoint/replay/catch-up cases, or isolate and resolve the regression before completing the arc.
- All modified behavior remains available from a relocated installed Core package; real filesystem/GUI/consumer metrics absent from the environment are marked unavailable, not invented or treated as approvals.

**Verification:**

Run five paired Release compositions and recovery/catch-up cases; finish arc C Debug/Release/leak-enabled ASan and relocated installed gates in the verification protocol; preserve raw evidence and current reader fixtures.

## X10 — Publish paired results and dispose of every performance target

**Status:** see TASKS.json (not started)
**Depends on:** X12
**Coverage:** R5, R12

**Files / scope:**

- `docs/reports/vnext-performance-results.md`
- `docs/specs/vnext-performance-targets.md`
- `docs/roadmap/2026-10-reliability/TASKS.json`
- `docs/roadmap/2026-10-reliability/DIRECTION.md`
- `docs/roadmap/2026-10-reliability/07-performance.md`
- `docs/README.md`
- `docs/handoff/wave-core-response.md`
- `docs/handoff/markoff-core-response.md`

**Steps:**

1. Publish d04f151 versus final candidate and per-task comparisons with all five runs, raw samples, schema/trace hashes, environment and exact correctness/package evidence. Recompute tables from artifacts and keep the October 19 baseline immutable.
2. Classify each target as met, missed with measured limitation, or not applicable with evidence. Failed correctness or an unexplained regression leaves its implementation task incomplete; a missed aspirational speed target requires a documented disposition, not a fabricated pass.
3. Report retained-history slope, query work/allocation counts, UTF-16 conversion, list identity lookup, undo memory, actor-frontier CPU/wire and warm recovery separately. Preserve default import-limit advice. Do not claim end-to-end consumer responsiveness or broad memory bounds from engine-only measurements.
4. Record remaining optional X09 transport/GC prerequisites and any new performance tasks with explicit dependencies. External engine comparisons are optional separately scoped benchmark work, not a prerequisite for this native-engine report. Update current docs and handoff without changing historical candidate evidence or claiming consumer approval.

**Acceptance:**

- Every implemented optimization is attributable to a task/revision, demonstrates an accepted benefit or justified simplification, and has passing correctness and installed evidence.
- The report is complete without enabling physical GC, finishing external consumer acceptance, or running external-engine comparisons; remaining limits and deferred opportunities are explicit.

**Verification:**

Run the complete paired Release matrix, artifact/schema/table validation, validate-plan.py and check-scope.py --strict; cross-check all three arc gate records. Documentation edits alone do not require another engine test sweep.

## X09 — Bound catch-up work and eliminate repeated idle scanning

**Status:** see TASKS.json (not started)
**Depends on:** X01, A04, G07
**Coverage:** F5, F6, R5, R9, R10

**Files / scope:**

- `libs/collabtext/src/crdt/StreamSync.cpp`
- `libs/collabtext/src/crdt/SegmentReader.cpp`
- `libs/collabtext/src/DocumentSession.cpp`
- `libs/collabtext/src/CollabDocument.cpp`
- `libs/collabtext/tests/tst_poll_budget.cpp`

**Steps:**

1. This is a separate transport/session lane after G07, not a prerequisite for the October 19 Core optimization report. Establish X01-compatible baselines for idle polling, resume positions and event-loop delay before changing scheduling.
2. Add record/byte/time budgets per poll with exact resume state. Batch engine application and Qt notifications without acknowledging beyond committed coverage. Use resumable validated inventories and periodic complete reconciliation; filesystem watchers are hints.
3. Do not evict payloads, prune logs or infer reclaimability merely because recoverable storage exists. Follow G07 certificates and retention obligations. Keep one engine owner; if measured worker needs arise, specify immutable messages and ownership before adding concurrency.

**Acceptance:**

- Catch-up progresses across bounded slices without coverage loss or event-loop starvation; idle work and notification counts are measured against a new paired baseline.
- G07 correctness/retention gates are complete before any storage-pruning interaction; the Core performance lane and X10 do not wait for this task.

**Verification:**

Run tst_poll_budget, transport/session recovery and Qt adapter tests; establish and compare event-loop/idle baselines separately from engine timings. Never register timing benchmarks in CTest.
