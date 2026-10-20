# Native performance work: measurement and acceptance contract

Plan date: 2026-10-19. This is an implementation/evaluation contract, not a claim
that the targets are achieved or a promise of latency on every machine.
Authoritative tasks: [07-performance.md](../roadmap/2026-10-reliability/07-performance.md)
and [TASKS.json](../roadmap/2026-10-reliability/TASKS.json).
X01, X02, X07a and X04a are complete; X07 is active. The complete serial order is in the phase file and registry.

## Baseline that must remain reproducible

Engine revision: **`d04f151b3a70b161b72c33a8de6895efbd1c13e2`**, merged `master`,
package alpha.5 / manifest v17. Read the
[evaluation](../reports/2026-10-19-performance-evaluation.md),
[recorded harness](../reports/2026-10-19-performance/public_api.cpp),
[raw evidence map](../reports/2026-10-19-performance/README.md),
[aggregates](../reports/2026-10-19-performance/summary.json), and
[environment](../reports/2026-10-19-performance/environment.json).
Keep those files unchanged. New harnesses/results live under `benchmarks/` and
new report directories; never overwrite evidence to make a comparison work.

The historical numbers use GCC 16.2.1, Release `-O3 -DNDEBUG`, i7-1185G7,
a shared/dynamically clocked host and small per-run samples. They are anchors for
planning. **Optimization decisions use new paired measurements on one host**;
a new machine's timing must not be divided by an old laptop timing to claim a win.
The one-insertion 1-MB public/byte cases use 256 MiB undo bytes; other standalone
cases use defaults. Default depth is 1,000 and byte budget 16 MiB. Prior edits
are not the count of active undo entries. Physical GC/log pruning stays disabled.

## Sequence, strategies and expected benefit

These are goals to try, with mandatory disposition rules below. Do not mark a
goal as achieved before measuring it. Values in the expected column illustrate
the historical environment, not cross-machine thresholds. Ratios compare the
same workload/state and statistic; never compare a mean to a p99.

| Stage | Strategy and metric | Recorded baseline | Improvement goal / expected historical-scale value |
|---|---|---:|---|
| X01 | Reproduce checked workloads; expand missing cases | 30 standalone + 43 native rows, five runs | Correct, attributable measurements; no promised speedup |
| X02 | Count/profile copies, allocations, scalar/run/tree work | No isolated allocation profile | Measured cost breakdown; no promised speedup |
| X07a, arc A | Run-aware boundary mint/resolve, no scalar expansion | 100-KB history 0: 2.391/2.370 ms mean; history 1,000: 2.896/2.945 ms | At least 10× lower mean on all four cases: roughly 0.24–0.30 ms; verify changing offsets/tokens too |
| X04a | Specify/model transactions, stable handles and rebuild rules | Existing scalar boundary/sequence models | Complete independent design gate; no promised speedup |
| X07 | Summary-guided UTF-16 rank/seek, bounded local conversion | 1-MB public mean 0.978 ms; byte control 0.238 ms | At least 2× lower public mean: roughly 0.49 ms, without slowing byte control >10% |
| X04 | Origin/deletion interval indexes | Whole-run lookups; no isolated count baseline | At least 90% fewer lookup run visits for a one-run target among 5,000 runs; separate mutation cost |
| X05 | Transactional tree splice / affected visibility, not vector rebuild | 10-KB after 3,000 replacements: mean 4.551 ms / p99 9.643 ms | At least 5× lower mean, roughly 0.91 ms; aim for p99 ≤2 ms on comparable hardware; ≥2× lower mixed 5-KB/50%-deletion mean (1.831 →0.916 ms) |
| X06 | Internal interval undo/deletion bookkeeping | Bulk allocation case not measured historically | Establish in X01, then ≥50% fewer temporary allocated bytes on 1-MB bulk delete/undo; unchanged parity/history/limits |
| X08, arc B | Indexed full list identities; incremental single-entry mutations | 5,000-entry full enumeration: mean 347.354 µs | Lookup ≥10× and aged single-entry mutation ≥5× faster versus new X01 baselines; full enumeration remains O(output), ≤10% mean regression |
| X03 | Conditional layout/sparse-frontier experiment after algorithm changes | No trustworthy bytes-per-engine baseline | ≥20% fewer live bytes in the chosen structure against preceding candidate; otherwise retain no speculative implementation |
| X11 | Reduce codec traversal/allocation, preserve exact bytes | 512 actors: encode/decode mean 435.558/304.851 µs; 28,100 bytes | ≥25% lower means: roughly 327/229 µs; wire size unchanged |
| X12, arc C | Installed/durable composition and stage measurements | Warm tmpfs checkpoint/replay: 512 ops 15.91/64.81 ms, 2,048 ops 66.15/940.37 ms | Separate real-storage baseline; no >10% mean regression in paired recovery/catch-up cases; no invented GUI/SSD guarantee |
| X10 | Publish all wins, misses, overhead and remaining limits | Immutable October 19 report | Auditable per-task/final comparison; no broad certification claim |

X09 transport/poll scheduling remains separately gated by G07. It is not on this
Core critical path. Removing G06/G08 from optimization/report prerequisites does
not complete them or authorize physical collection. Existing GC/certificate
fixtures still apply to every changed index and summary lifecycle.

X03/X11 may end in a measured no-change disposition. Other tasks must deliver
their specified algorithms/model/evidence to be complete, even if a speed target
needs a measured miss disposition. Missing correctness acceptance cannot be
reclassified as a mere performance miss.

## Measurement protocol for each implementation task

1. Record baseline engine SHA, immediately preceding candidate SHA, candidate
   SHA/diff, harness revision/hash, trace hash and build flags. Verify source is
   the intended revision. Materialize the pinned engine in a temporary source
   directory if needed (for example from `git archive`); do not reset the user's
   checkout, reuse stale library objects, or revive the retired worktree.
2. X01 provides opt-in Release executables for both sources. The original
   [reproduction commands](../reports/2026-10-19-performance/README.md#reproduce)
   are the starting point. New harnesses must run against both sources. If a
   private byte-control interface changes, adapt that control explicitly while
   retaining public trace, result and operation-byte equivalence; disclose it.
3. Run five complete baseline/candidate pairs, alternating order (B/C, C/B,
   B/C, C/B, B/C), fresh process per run. Finish all builds before timing. Record
   load, governor/power/affinity where available, CPU, compiler, filesystem,
   page/cache policy, and whether background load was controlled. No optimizing
   from a single best run. Preserve every run, including failures and contention.
4. For each pair compute candidate/baseline of the **same** per-run mean, p50,
   p95, p99, total time or allocation metric. Report median paired ratio and the
   five ratios/range, alongside absolute per-run values. Report cumulative
   versus interval measurements correctly. Do not pool disparate traces or call
   reciprocal batch throughput single-edit latency. Separate refused/no-op/
   accepted operations and remote applications from source edits.
5. Store raw call samples for the new harness. Match the historical lower order
   statistic (`floor(q*(N-1))`) when comparing historical percentiles. Keep the
   exact 200/50/100-call historical windows and report extra tail campaigns as
   separate cases. A 50-sample p99 is descriptive, not a service guarantee.
6. Keep GC, undo limits, seed, actor/fragment/history counts, initial seeding,
   measured region, offsets and timing statistic equal between paired cases.
   Check final content, identities, version/coverage, token outcomes and wire
   bytes as applicable; a changed result is a correctness failure, not a win.
   Count work inside the measured region. Avoid a fast cache-only artifact by
   including varying queries interleaved with mutations and invalidation checks.
7. Require ≤10% median mean regression on relevant non-target principal cases:
   fresh 1/10/100-KB text, explicit-budget 1-MB text, history 500/3,000, Unicode,
   boundary/marker, list enumeration, codec and recovery/catch-up. This is an
   investigation threshold rather than a flaky CTest time assertion. For noisy
   or regressed cases investigate first; rerun the **complete affected paired
   batch** under better-controlled conditions and retain both datasets. Never
   select only favorable repeats. Investigate large tail/max outliers too.
8. Compare against both d04f151 (cumulative result) and the preceding candidate
   (attribution / regression). X03's 20% layout target is against the preceding
   candidate; X06/X08 new cases use X01's new paired baseline. If diagnostic
   instrumentation is needed, profile separately and measure the ordinary
   diagnostics-disabled binary for published latency.

A target miss must produce an artifact with the counterexample/workload, actual
ratios, measured cost explanation, and a keep/revert/split decision. A speedup
smaller than the goal may be retained if it demonstrates a worthwhile benefit or
independent simplification with passing gates; state the miss and record a new
follow-up task if needed. Unexplained regressions, incorrect outputs, weakened
oracles or a missing required model leave the task incomplete. The executor may
make routine implementation choices; a missing algorithm must be settled in
X04a, not guessed by dependent tasks.

## Design and semantic boundaries

These contracts and models are prerequisites to changing their production paths:

| Concern | Required references |
|---|---|
| Ordering / item identity | [native algorithm](vnext-sequence-algorithm.md), [sequence semantics](vnext-sequence-semantics.md), [identity](vnext-identity.md), `libs/collabtext/tests/tst_fuguemax_model.cpp`, `tst_sequence_differential.cpp` |
| Boundaries / strict attachments | [boundary semantics](vnext-boundary-semantics.md), [external positions](vnext-external-position-contract.md), [retention](vnext-anchor-retention.md), `libs/collabtext/tests/tst_boundary_model.cpp`, `tst_positions.cpp` |
| Markers / composition / local undo | [Wave extension](wave-core-adoption.md), `tests/wave-core/marker-model.cpp`, `tests/consumer-smoke/wave_core.cpp` |
| Full list identities / terminal gaps | [Markoff extension](markoff-core-adoption.md), `tests/markoff-core/gap-model.cpp`, `tests/consumer-smoke/markoff_core.cpp` |
| Readers / restart / coverage | [codec](vnext-operation-codec.md), [checkpoint](vnext-checkpoint.md), [protocol](vnext-protocol.md), [installed manifest](../../libs/collabtext/include/collabtext/ALPHA_CAPABILITIES.md) |

Source map for the first inspection (function names are more durable than line
numbers):

- [CrdtEngine.cpp](../../libs/collabtext/src/CrdtEngine.cpp): `boundary_at`,
  `attachment_at`, `resolve` overloads; scope/history guards precede traversal.
- [PositionTraversal.cpp](../../libs/collabtext/src/crdt/PositionTraversal.cpp):
  existing scalar boundary/referent semantics; retain a test-only slow oracle.
- [Buffer.cpp](../../libs/collabtext/src/crdt/Buffer.cpp): `position_items`,
  `get_fragments`, `set_fragments`, `utf16_range`, `marker_offset`,
  `try_apply_validated_local_edit`, remote application, undo retirement.
- [SumTree.h](../../libs/collabtext/src/crdt/SumTree.h) and
  [Fragment.h](../../libs/collabtext/src/crdt/Fragment.h): summary aggregation,
  copy-on-write ownership, cursor invalidation, identity ranges, visibility.
- [IdListEngine.cpp](../../libs/collabtext/src/IdListEngine.cpp) and
  [IdList.cpp](../../libs/collabtext/src/crdt/IdList.cpp): `visible_entries`,
  `try_anchor_of(identity)`, retained-order mutation/rebuilds.
- [UndoMap.h](../../libs/collabtext/src/crdt/UndoMap.h) and
  [LocalHistoryBudget.h](../../libs/collabtext/src/crdt/LocalHistoryBudget.h):
  compensation parity, active/retired restoration accounting. Accounting bytes
  are not a measurement of allocator bytes; report both honestly.
- [OperationCodec.cpp](../../libs/collabtext/src/codec/OperationCodec.cpp):
  canonical bytes, causal frontier encoding and checked decode.

X04a must produce `docs/specs/vnext-incremental-index-design.md` and an independent
model before X07/X04/X05 code. Its minimum design contents are: data/ownership
schema; rank/identity/deletion lookup pseudocode; split/splice/coalesce and
visibility update pseudocode; every mutator's invalidation table; aggregate laws;
fallible preparation/commit sequencing; allocator and callback interactions;
restore/reset/discard/gap/collection rebuild policy; fallback conditions; test
oracle independence; complexity and memory costs. "Add an index" is not a
complete algorithm. The model must exercise failed updates and node/run splits,
not merely reproduce a lookup in a static map.

Do not change public signatures, operation/token bytes, supported archive readers,
undo policy or current writer formats as a shortcut. Current CTBO/0002–0003,
CTLO/0002, CTGP/0001 and CTPT/1–2 obligations remain. This is not permission to
restore experimental formats. If a proposed optimization needs a contract/format
change, stop that implementation path and specify a separate design/task with
reader duties first; continue independent eligible work. No new public API is
required by the current strategy. Internal representation changes are expected.

Markers stay in the same native text sequence. Do not replace boundary semantics
with markers, substitute another ordering engine, enable GC, evict deferred
causal evidence, silently acknowledge missing history, or mutate an engine from
multiple owners. Tree-delta and consumer-owned history APIs already exist; do not
implement a speculative replacement delta journal or force DocumentSession onto
Wave. Preserve callback reentrancy and checked allocation/refusal behavior.

## Correctness and package arcs

Follow [EXECUTION.md](../roadmap/2026-10-reliability/EXECUTION.md) throughout.
While iterating, use the affected warm checks and deterministic work-count tests.
Do not assert latency thresholds in correctness tests. New correctness models
must be registered in CTest; new benchmarks must never be registered there.
Unknown changed paths and zero selected tests fail.

Run these at the end of arc A (X07a), arc B (X08), and arc C (X12), after warm
checks pass. Set cache directory inside the checkout; adjust jobs to avoid host
contention. No timing benchmarks run concurrently with these checks.

```sh
export CCACHE_DIR="$PWD/.cache/ccache"
export COLLABTEXT_VERIFY_JOBS=4
./tools/verify-roadmap.sh --fresh debug '.'
./tools/verify-roadmap.sh --fresh release '.'
./tools/verify-roadmap.sh --fresh asan '.'
./tools/verify-consumer-package.sh --mode debug --gate live --components core
./tools/verify-consumer-package.sh --mode release --gate live --components core
./tools/verify-consumer-package.sh --mode asan --gate live --components core
python3 docs/roadmap/2026-10-reliability/validate-plan.py
python3 docs/roadmap/2026-10-reliability/check-scope.py --strict
```

Use the package script's documented relocated-prefix behavior; do not reuse an
older installed library. If FileTransport or Qt code changes, also run the
corresponding component package gates from `--help` and the routed adapter/
session suites. Keep LeakSanitizer enabled; distinguish environment restrictions
from sanitizer findings and obtain a valid run instead of suppressing either.
Every CTest invocation must select nonzero tests (`--no-tests=error` for direct
invocations). Record actual commands, revision, counts and artifact paths.

Existing especially relevant executables include `tst_boundary_model`,
`tst_positions`, `tst_position_token_reader`, `tst_text_markers`,
`tst_marker_model`, `tst_fuguemax_model`, `tst_sequence_differential`,
`tst_sequence_fast_paths`, `tst_summary_laws`, `tst_sumtree`, `tst_undomap`,
`tst_undo_retirement`, `tst_persistent_undo`, `tst_anchor_retention`,
`tst_checkpoint_codec`, `tst_markoff_core`, `tst_wave_core`,
`tst_counter_gap_model`, and the whole `tst_idlist*` family. New test names in
phase scopes are planned deliverables, not tests assumed already available.

If a failure occurs, preserve and reduce the trace according to the existing
seed/replay contract. A passing model that calls the optimized helper it is
supposed to validate is insufficient. Full gate evidence belongs to the arc,
while each intermediate task records its affected checks; do not claim final
certification before the arc is complete.

## Completion record for the next executor

For every task, update its registry entry and `docs/reports/vnext-task-log.md` with:

- baseline and implementation SHAs, source/harness hashes and exact scope;
- affected/model checks and arc evidence if this is an arc endpoint;
- five paired results, ratios, logical/work/allocation invariants, fallback rate;
- goal met/missed and keep/revert/split disposition, with any new task ID;
- memory/index overhead and remaining asymptotic or format constraints.

X10 assembles `docs/reports/vnext-performance-results.md`; it does not overwrite
this target contract's historical goals to make misses disappear. Keep installed
capabilities and handoff truthful if measured advice or behavior changes. Do not
bump package/manifest or tag a candidate merely for a benchmark gain. No consumer
edit, external message, push, publication or further merge is authorized by this
plan. Q06/M06 remain consumer-owned evidence, not an obstacle to local Core work
and not a status the executor can infer from producer-shaped fixtures.
