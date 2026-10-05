# State at handoff — 2026-10-05, corrected after independent review

Execute only in `.worktrees/reliability-v2`, branch `roadmap/reliability-v2`.
`TASKS.json` is authoritative. The earlier version of this handoff mistakenly
called W01/S01 complete and S04a ready. The correction below uses actual artifacts
and registry dependencies; a prose assertion does not complete a task.

## Completed work

- V01–V08, D01, P01–P05 and L01: validation, independent models, primitive
  foundations and checked filesystem primitives.
- A01/A02a/A06a: independent Core/FileTransport/QtAdapter/Widgets build components,
  installed public headers/capability manifest, relocation and consumer checks.
  Core-only consumers do not discover Qt. Read `docs/USING_VNEXT.md`.
- P06a **steps 1–5**, not the whole task: 128-bit actors, uint64 counters,
  checked run allocation, caller-supplied allocators and invalid-identity refusal.
- S01: `docs/specs/vnext-sequence-semantics.md`, 17 versioned JSON histories,
  46 operations and 29 arrival permutations, plus a schema/history validator.
  Exact desired content, boundary offsets and later token comparisons are
  specified. This does not implement F8 ordering or F7 position tokens.

## Next implementation work

1. **W01:** follow `00-consumer-integration.md`. Create the eight-probe fixture
   corpus, provenance and schema validator named by its task. The external
   position contract exists; its executable probe corpus does not. S01's sequence
   fixtures are useful inputs, not a substitute for WP1–WP8.
2. **P06a step 6:** finish opaque operation values through internal construction
   and accessors. P01's public API specification already chooses opacity. This is
   unfinished implementation, not a missing user-policy answer. Do not silently
   declare the visible variants an accepted permanent interface.
3. **S04a**, only after W01/P06a/S01 are complete in the registry: scoped
   boundary specification, exact outcomes, bounded canonical token codec contract
   and an independent mint/resolve/compare model. Trace ignored bias, missing
   text anchors resolving to end, and unscoped end sentinels. The model must
   detect ignored affinity, erased scope and missing IDs mapped to end; include
   both legacy and desired repaired item orders. Neighbors alone are insufficient
   without model evidence. This is the gate for S04b.
4. Continue S04b, W02/W03, S02/S03 and early alpha preparation following actual
   dependency edges, not just this abbreviated lane. Keep durable undo/pins,
   recovery and certified GC obligations explicit.

The registry currently depends on **all of P06a** for S04a. No partial-step
prerequisite was added. Do not invent one in a handoff or start dependent code.
The user has resolved all product questions in DECISIONS.md. Work on the existing
native plaintext/IdList engine, retaining transport independence.

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
alpha artifacts locally. This turn changes no production integration algorithm.
