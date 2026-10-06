# State at handoff — 2026-10-05, corrected after independent review

Execute only in `.worktrees/reliability-v2`, branch `roadmap/reliability-v2`.
`TASKS.json` is authoritative. The earlier version of this handoff mistakenly
called W01/S01 complete and S04a ready. The correction below uses actual artifacts
and registry dependencies; a prose assertion does not complete a task.

## Completed work

V01–V08, D01, P01–P06b, L01, S01, W01, S04a and packaging A01/A02a/A06a.
P06a includes the final public operation-opacity step (`01614af`, `d52dceb`),
with immutable shared values and affected Debug/Release/leak-enabled sanitizers.
S04a supplies a full scoped-boundary contract, implementation map, compile-checked
target signatures and independent model. Exact W01 collapse/restoration rules are
resolved; corrected WP2/WP8 fixtures select the actual referent. F7/F8 production
fixes, public token bytes, durable undo/pins and store recovery remain pending.

## Next implementation work

1. **P06b is complete:** checked CTBO/CTLO/CTGV/CTGP version 1 codecs, fixed-width
   scalars, allocation/gap metadata and bounded readers/writers. Debug 72/72,
   Release/sanitizers 14/14, installed package passes. Failures retain and block
   decoder batches; full store recovery/gap integration remains P07/L02/L03/L08.
2. **Continue S04b:** implement vnext-boundary-semantics.md and the checked facade
   signatures using native fragment/entry storage. Golden bytes and a fresh-process
   replay establish public token codec evidence. Include scoped typed failures and
   clear-history invalidation. No backward API/format compatibility is needed.
3. **W02/S02** are independently ready. W02 establishes installed baseline probes;
   S02 must generate S04a's supplied orders and prove stable relative order of
   already integrated retained items before S03 can change integration.
4. W03/E07b and later recovery/order/pin tasks follow registry dependencies.
   No product-policy question is outstanding. No backend substitution, consumer
   edits/messages, publication or master integration is authorized by these tasks.

The model ran 21 token-bearing fixtures (50 minted gaps, 48 exact placements,
24 comparisons), 472 supplied orders and 75,328 visibility states, with two
identical fixed-seed traces. Debug/Release/ASan/UBSan/LSan pass. This is independent
boundary evidence over supplied orders; it is not production integration proof.
The current fixture validator rejects 37 corruption kinds / 362 applications.
Details and artifacts belong to TASKS.json / vnext-task-log.md.

## Verification cadence

```sh
tools/verify-changed.sh                          # affected bounded correctness
tools/verify-changed.sh --target '^tst_buffer$'   # narrow iteration
tools/verify-changed.sh --thorough                # include routed long simulation
tools/verify-changed.sh --all                     # every correctness test
tools/verify-roadmap.sh --fresh debug '.'         # fresh acceptance once per arc
```

Benchmarks are **absent from CTest**, including all/fresh runs. They are opt-in
standalone binaries, excluded from the default build even when enabled. The user
deferred engine optimization and performance comparisons. Do not tune the old
benchmark watchdog or run benchmarks as acceptance tests.

The runners use available CPU workers, automatic bounded local ccache and one
aggregate build traversal. The package gate builds enabled libraries rather than
unrelated tests/apps. Compilation cache does not cache test execution. Default
TMPDIR is inside the worktree; an explicit value is honored. Use EXECUTION.md
for environment controls and full verification requirements.

The reviewed broad bounded Debug gate passed **70/70** CTest tests plus a relocated
installed Core consumer and negative-component checks: 153.178 seconds including
configuration/build/package work; CTest itself took 25.62 seconds. A repeated warm
three-test check went from 12.833 to 4.421 seconds. Neither number measures a clean
full build. `tst_realistic` remains deliberately excluded from ordinary routing
and must run when its affected subject/acceptance gate requires it. No benchmark
ran in this review. See the throughput report for evidence and limits.

Unknown changed paths, invalid Git baselines, empty selections, stale routes and
build/test failures return nonzero. Regression tests exercise failure paths;
never weaken assertions to shorten verification.

## Remaining limitations and authorization

F7/F8 remain known defects: ignored affinity/unsafe text anchor fallback and
concurrent text interleaving. Current smoke convergence does not prove repaired
passage order. GC remains disabled; stores/session recovery and persistent history
remain later tasks. `ReplicaAllocator` is source-tree only; consumers supply their
own public `IdentityAllocator` and own durability. File transport `StreamSync`
is implemented but not advertised as an installed self-sufficient public class.

Publications, master integration, edits to Wave/Markoff and messages to consumers
are separate actions requiring explicit user instruction. Prepare reviewable
alpha artifacts locally. S04a changes no production integration algorithm.
