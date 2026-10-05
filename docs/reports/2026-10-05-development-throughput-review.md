# Development throughput and progress review — 2026-10-05

Reviewed baseline `8496194`, branch `roadmap/reliability-v2`, against its task
registry and executable checks. All work stays in the isolated worktree.

## Assessment

The recent packaging and identity work is substantive. Independent components,
installed headers, relocated package consumption, wide identities and checked
caller-supplied allocation address real prerequisites for consumers. The inspected
bounded correctness suite and installed Core package pass. This supports those
specific capabilities; it does not establish completed ordering, live boundaries,
durable sessions or safe reclamation.

Development was paying too much verification overhead. Recorded Debug timings
put the realistic simulation near 450 seconds; benchmark measurements added
separate long work. Most correctness cases take substantially less time. Repeated
Make target traversals, unconditional configuration and rebuilding apps/tests for
package installation added time unrelated to the selected checks. An hour-long
fresh build/sweep after each edit is the wrong cadence. Use affected warm checks,
then required broader acceptance once an implementation arc is complete.

The fast verifier also had correctness gaps: mixed unknown code/documentation
could become a docs-only success, routing missed affected families, invalid Git
baselines could be treated as empty changes, and missing option values could loop.
Speed requires coverage and truthful failures, not just a shorter test regex.
The package `finish()` also needed to return a recorded failing step's status
when called with zero. These paths were corrected and covered by verifier tests.

Finally, orientation documents claimed W01/S01 were complete and S04a ready.
The registry and absent fixture artifacts contradicted this. S04a depends on the
whole unfinished P06a task and W01. A prose reference to P06a steps 1–5 was not a
registry amendment. Those claims could send another executor past design gates.
The handoff, AGENTS and roadmap now reflect actual state. The P01 specification
already chooses opaque operations; finishing P06a step 6 does not need another
user-policy decision.

## Implemented in this turn

- Removed benchmarks from CTest altogether. `COLLABTEXT_BUILD_BENCHMARKS`
  defaults OFF; ON exposes only an explicitly built standalone binary. Even ON
  excludes that binary from the default build. All/fresh correctness runs never
  execute benchmarks. No benchmark was built or run in this review.
- Added one selected-target aggregate build, reused warm configuration where
  possible, enabled available ccache with a local 512 MB compilation cache, and
  kept compiler temporaries in the worktree by default. The package gate builds
  enabled library components while retaining relocation/header/dependency checks.
- Made changed-path routing fail for every unknown file and invalid/empty
  selection. Buffer routes include anchors and bounded fuzz/regression checks;
  IdList routes include its complete test family. Only the long realistic suite
  is excluded by default; explicit thorough/all selections remain available.
- Added twelve verifier regression tests, including nonzero failure status and
  machine-readable evidence. Missing-value arguments terminate promptly.
- Completed S01's desired semantic specification and corpus: seventeen histories,
  forty-six operations, twenty-nine complete arrival permutations, exact content,
  stable identities, scoped mint directives, affinity/collapse offsets and later
  token comparisons. Its Python validator and eleven parser regression tests run
  as `tst_sequence_fixtures`, without compiling a Qt parser just to validate JSON.

S01 is fixture/specification work. The validator checks declared histories and
oracles for consistency; it is neither the S02 ordering interpreter nor the S04a
boundary model. Production F7/F8 behavior and expected-failure markers are
unchanged. Cases include whole paste, forward/reverse typing, nested post-merge
edits, concurrent replacement, local undo/redo, empty/start/end boundaries,
concurrent same-token insertions, deleted neighbors, all UTF-8 scalar boundaries,
list ties and lookup misses. The finite branch tie rule is a required example,
not a proof of maximal non-interleaving for arbitrary histories.

## Evidence and practical limits

| Check | Result |
| --- | --- |
| Same warm three-test selection, old runner | 12.833 s; CTest about 0.42 s |
| Same selection, aggregate/reused configuration | 4.421 s; CTest about 0.36 s |
| Broad bounded Debug correctness | 70/70 CTest tests; CTest 25.62 s |
| Broad changed-path gate plus installed Core consumer | PASS; total 153.178 s |
| Verifier failure/coverage regressions | 12/12 pass |
| Sequence validator regressions | 11/11 pass; 17 histories validate |
| Benchmarks ON, configure/CTest metadata only | 71 correctness tests, no benchmark registration |
| Core-only tests/Qt/apps/transport/benchmarks OFF | Configures without finding Qt or Python |

The warm comparison selects `tst_buffer`, `tst_anchor`, `tst_idlist_anchor`, Debug,
eight workers, same checkout and already built objects. This measures verification
orchestration, not engine throughput or clean compilation. A changed selection
still requires configuration. Compiler cache was exercised by fourteen actual
consumer compilation misses; no clean-build cache speedup is claimed. No fresh
full build, Release/ASan resweep or realistic simulation was run for this
infrastructure/specification change. Existing C++ behavioral checks remained
unchanged; the later parser/comparison additions received targeted verification.

Local evidence:

- `build-roadmap-debug/verification/20261005T215421Z-changed-ylk7ei8r/results.json`
  records the 70-test gate and 153.178 s total.
- `build-consumer-verify/debug-package-20261005T215611Z-1970/` contains relocated
  installed-consumer evidence and one passing consumer CTest case.
- `build-roadmap-debug/verification/20261005T215920Z-2/` records the acceptance
  runner selecting just fixture/verifier checks and the actual aggregate commands.
- `/tmp/collabtext-fast-before.log` and `/tmp/collabtext-fast-after-warm2.log`
  retain local warm comparison output. They are temporary, not portable artifacts.
- `docs/reports/2026-10-05-development-throughput-evidence.json` retains portable
  results and the fixture corpus digest; task registry/log record implementation
  commits and final targeted evidence.

## Recommended execution from here

Complete W01's eight-probe contract/corpus and P06a step 6. Then finish S04a's
independent boundary gate, S04b and installed probes in actual registry order.
Repair sequence ordering through S02's independent specification/interpreter and
S03's production transformation; never try to fix it with an unreviewed comparator.
Keep persistent undo, recovery and GC obligations attached to their existing tasks.

The user has explicitly deferred engine optimization and performance comparisons.
Keep performance tasks as later work; do not let benchmarking or speculative
optimization consume the consumer delivery lane. Run bounded correctness checks
while coding and the required acceptance gates at completed arcs. No additional
product-policy answer is needed to proceed with the ready implementation work.
