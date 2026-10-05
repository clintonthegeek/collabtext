# Execution rules for one implementation task

This document is intended to let a less-capable executor faithfully implement a fully specified task. It does not supply missing CRDT algorithms or product policy. Read the phase task and referenced specifications as well.

## Workspace and authorization

Execute breaking work only in `/home/clinton/dev/collabtext/.worktrees/reliability-v2`, branch `roadmap/reliability-v2`. Verify `pwd`, `git branch --show-current`, and `git status --short` before editing. Build directories and logs belong in this worktree or task-specific temporary directories. Do not accidentally build against the original checkout's generated files.

The original checkout and master are the evaluated source baseline. The user confirmed there are no production stores/history and authorized freely redesigned APIs/formats; do not add migration or compatibility work. Do not merge/rebase onto master, publish packages, send consumer messages, or edit external consumer repositories as part of a task. A new explicit user instruction is required for external publication or master integration. Routine code changes and local test runs within the selected task do not need repeated permission.

These plans supersede old CLAUDE.md restrictions about the completed OpStream extraction **on this isolated branch**. They do not authorize unrelated product features or a backend substitution. There is no dependency on an unavailable Superpowers skill or parallel agent workflow. One executor can follow the serial task order.

## Choosing the next task

1. Load TASKS.json. Only tasks whose prerequisites have `status=complete` are eligible. A deferred prerequisite is not equivalent to complete; if the user chooses an alternate route, explicitly revise the dependency graph.
2. Check DECISIONS.md. All Q1–Q4 are resolved: clean API/formats, explicit offline-device retirement with advisory reminders, persistent undo/redo, and incremental improvement of the existing C++ engine. No backend question remains open. Do not ask resolved questions again or reintroduce Yrs adoption/Rust rewrites.
3. Check the task's design gate. S03 cannot begin merely because someone wrote an S02 title: the complete specification/model/evidence must exist. The same applies to S04a/S04b and G01/G02/G06. An early alpha changes sequencing, not the requirement for a complete independent boundary model.
4. Prefer registry order for a serial run. Work on one task at a time. If a task is too broad, split it into new well-defined subtask IDs with dependencies and acceptance criteria before coding; do not silently expand scope.
5. Mark the task `in_progress`, record the baseline revision and relevant failures, and read every existing file the task will touch. Paths named as new files are not missing prerequisites.

`validate-plan.py --ready` lists structurally ready tasks. It cannot establish technical review or user approval; the executor must still check the decision/design artifacts. V01–V08, D01, P01–P05, and L01 are complete. Continue unfinished P06a: both design gates (ordering and allocation) have landed and run, and the engine migration in steps 3–6 is pending; A01/W01/S01 are separately ready. After finishing P06a, use the preferred serial route in [00-consumer-integration.md](00-consumer-integration.md): package and baseline probes first, live-position specification/model and implementation next, then full ordering/recovery and final certification. Read the [current handoff](../../handoff/2026-10-04-reliability-review.md) and [external consumer requirements](../../specs/vnext-external-position-contract.md) before continuing. Acceptance must include the W-series cases assigned to each task; a test name in a spec is not implementation evidence.

## Implementing the selected task

1. Write the smallest meaningful failing regression first for an algorithm, interface, or lifecycle change. Existing known-failure tests use explicit expected-fail markers; remove only the markers the task actually fixes.
2. Confirm the test fails for the intended reason. Do not confuse a compiler failure with proof of a behavioral defect.
3. Implement the documented steps within the listed scope, reusing working components and correcting the identified invariant. Existing analogous code is a reference, not authority to repeat an unsafe behavior identified in the evaluation. The independent model is verification support, not a replacement production engine; do not add a backend substitution layer.
4. Keep logical operation identity, local history, durable coverage, and derived indexes distinct. Every mutation must honor the task's failure-atomicity and ownership contract.
5. Re-run the targeted checks, then the affected integration/model suites. Record seed, trace/fixture hash, compiler/configuration, pass counts, and any limits. Do not hide a red suite with a regex exclusion or unconditional skip.
6. Inspect the diff for unintended API exposure, regenerated artifacts, broad formatting, side effects outside this worktree, and test weakening. Update affected specs and the new API guide if public behavior changed.
7. Commit only the selected task's source, tests, and documentation after required checks pass. Use a conventional task-specific message, for example `fix(storage): replay own history on reopen (L05)`. Do not stage unrelated user edits. If Git metadata is sandbox-protected, use the normal narrowly scoped approval mechanism rather than manipulating .git through a different path.
8. Update TASKS.json to `complete` only when acceptance is met; add implementation commit(s), verification commands/results, and artifact paths. Write a concise entry in docs/reports/vnext-task-log.md. The Markdown task documents describe initial status; TASKS.json is authoritative.

Documentation-only policy/spec tasks need review and consistency checks, not artificial implementation-mirroring tests. Executable models at design gates are substantive verification and must actually run.

## If the task cannot be completed

Leave it incomplete and record the concrete blocker in TASKS.json: missing user answer, incomplete algorithm/protocol specification, unavailable platform verification, or a failing required invariant. A difficult task is not license to choose missing semantics or mark acceptance complete.

Continue independent ready work when possible. Route a technical design gap to its existing design task with the exact counterexample and violated invariant; refine the native repair. Engine replacement and Rust/Yrs feasibility trials are outside the selected scope. Do not loosen GC eligibility, discard malformed records, or remove exact expected-output assertions to get green tests.

User-facing questions belong in ordinary conversation/final text for this user, who cannot use inline question controls. Ask only for missing policy, scope changes, or permission that is actually necessary. Routine implementation choices with documented authorization do not need another question.

## Verification tiers

The full sweep is the thing we trust and the thing we could not afford to run
often, so it is not the inner loop. Verification is layered, cheapest first, and
the layer a change actually needs is the one to run.

| Tier | Command | Cost | Use |
| --- | --- | --- | --- |
| 1 | `tools/verify-changed.sh` | seconds to ~1 min | every edit |
| 2 | `tools/verify-changed.sh --thorough` | minutes | before committing an engine change |
| 3 | `tools/verify-changed.sh --all` | minutes | when the route looks wrong |
| 4 | `tools/verify-roadmap.sh --fresh <mode> '.'` | the full sweep | once per arc, as the acceptance gate |

`tools/verify-changed.sh` reads the uncommitted diff (or `--since REV`), maps each
changed path to the test areas that can observe it through an explicit routing
table, builds only those targets in the warm `build-roadmap-{mode}` tree, and runs
them. It prints the table it used and warns about files nothing matched, because a
routing table that has quietly stopped matching a path is the failure mode that
would otherwise hide. Zero selected tests is an error, never a pass. Docs-only
changes route to nothing and exit successfully.

Routing is coarse on purpose. `src/crdt/Buffer*` pulls in the Buffer, undo, GC and
serialization tests; `src/identity/*` the identity suite; `src/transport/*` the
OpStream and file-floor suites; `src/ui/*` the widget tests; any change under
`include/`, `CMakeLists.txt`, `cmake/`, `tests/consumer-smoke/`, or
`tools/verify-consumer-package.sh` routes to the header-independence targets plus
the package gate, since those changes alter what a consumer sees without altering
engine behaviour. `--explain` prints the table and exits.

Slow property, fuzz and simulation tests are excluded from tier 1 and included by
`--thorough`; they dominate wall-clock and a fast subset already catches most
regressions. Acceptance is where they all run.

## Verification command contract

`tools/verify-roadmap.sh` supplies V01's verification entry point. The script interface is:

```sh
./tools/verify-roadmap.sh debug 'anchored-or-specific-ctest-regex'
./tools/verify-roadmap.sh release 'anchored-or-specific-ctest-regex'
./tools/verify-roadmap.sh asan 'anchored-or-specific-ctest-regex'
./tools/verify-roadmap.sh --fresh debug '.'   # from-scratch acceptance sweep
```

The script resolves its own repository/worktree root, configures `build-roadmap-{mode}`, builds the selected test targets, and runs CTest with `--output-on-failure`. Test names must match build target names, as they do in add_crdt_test. Zero matches and build/test failures return nonzero. Debug and Release use the appropriate CMake build type. ASan uses Debug with opt-in `COLLABTEXT_ENABLE_SANITIZERS=ON`, address/undefined-behavior sanitizer compile/link flags, and frame pointers on supported GCC/Clang non-Windows configurations; unsupported configurations fail explicitly. The script also checks that the core compile command contains instrumentation.

`--fresh` discards `build-roadmap-{mode}` first and is the acceptance-tier form. Run it once per arc rather than per task: a stale object file surviving a flag change is a real failure mode, and it is worth paying for at the arc boundary.

Set `COLLABTEXT_VERIFY_JOBS` to a positive integer to change the worker count, which defaults to the machine's core count. The runner sets `QT_QPA_PLATFORM=offscreen` only for CTest. Every invocation retains `run.log`, `metadata.json`, CTest selection JSON, target names, and exit status under `build-roadmap-{mode}/verification/`; metadata includes revision, compiler, flags, representative core compilation, selected test count, and commands. These generated artifacts are ignored by Git. Task reports retain portable evidence summaries and paths.

Leak checking stays enabled by default; the runner does not hide failures with global suppressions. A restricted sandbox may block LeakSanitizer's process inspection even when all test assertions pass. Rerun outside that restriction when authorized, and distinguish this environment failure from a leak report. Do not disable leak detection or suppress Qt allocations without recording the limitation and actual evidence. V01's successful unrestricted smoke run covers Buffer/IdList with Qt and requires no Qt leak suppression.

Before A01, the full project still needs Qt6 and zstd. A01 is now independently ready and is not gated by recovery or codec work. After A01, support these independent configurations:

```sh
cmake -S . -B build-roadmap-core \
  -DCMAKE_BUILD_TYPE=Release \
  -DCOLLABTEXT_BUILD_QT=OFF \
  -DCOLLABTEXT_BUILD_APPS=OFF \
  -DCOLLABTEXT_BUILD_TESTS=OFF \
  -DCOLLABTEXT_BUILD_FILE_TRANSPORT=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_Qt6=ON
cmake --build build-roadmap-core -j 2

cmake --install build-roadmap-release --prefix /tmp/collabtext-vnext-install
cmake -S tests/consumer-smoke -B build-roadmap-consumer \
  -DCMAKE_PREFIX_PATH=/tmp/collabtext-vnext-install
cmake --build build-roadmap-consumer -j 2
ctest --test-dir build-roadmap-consumer --output-on-failure
```

The exact options/paths become authoritative when A01/A06a implement them; A06 extends final certification rather than first creating installation. tools/verify-consumer-package.sh becomes the installed producer/consumer entry point in A06a and gains baseline/strict probe modes in W02/W03. Read its implemented --help for exact mode/option syntax; the roadmap does not pretend those scripts exist yet. The task registry names intended new tests; each creating task must register its executable so verification does not pass with “No tests were found.” The script must fail if a selected regex matches zero tests.

## Randomized test contract (V04)

Every randomized suite takes its entropy from two environment variables, and a
failure is expected to leave a replayable artifact behind.

```sh
COLLABTEXT_TEST_SEED=42 \
  build-roadmap-debug/libs/collabtext/tst_fuzz            # deterministic run

COLLABTEXT_TEST_SEED=random \
COLLABTEXT_TRACE_OUTPUT=/tmp/collabtext-traces \
  build-roadmap-debug/libs/collabtext/tst_fuzz            # opt-in campaign
```

- `COLLABTEXT_TEST_SEED` is a checked uint64 (decimal digits only, no sign or
  whitespace) or the word `random`. Unset, malformed, and out-of-range values
  fall back to the documented fixed default `20261003`, and a malformed value is
  reported once. This is what makes an ordinary `ctest` run a reproducible CI
  sample rather than an unrepeatable campaign.
- `COLLABTEXT_TRACE_OUTPUT` names a directory for trace artifacts. Unset, suites
  record nothing to disk. Set, each scenario writes one trace and a failing
  scenario writes its trace again with a `failed` suffix.
- Every migrated scenario prints its campaign seed, effective per-scenario seed,
  and whether the source was fixed or random.

A trace is versioned, records decisions rather than entropy, and replays without
rerolling anything. To investigate a failure:

```sh
build-roadmap-debug/reduce-trace /path/to/trace.trace --replay
build-roadmap-debug/reduce-trace /path/to/trace.trace \
  --predicate convergence --out /tmp/reduced.trace
```

The CLI exits nonzero and explains itself when the trace is unreadable, does not
fail the predicate, or when a reduction stops reproducing. Record the seed, the
trace path, and the reduced path in the task's evidence. When changing a
randomized suite, migrate it onto `TestSeed.h`/`TraceHarness.h` rather than
reaching for `std::random_device`, and run it twice with the same seed to confirm
the traces are identical.

Useful existing baseline commands before V01:

```sh
cmake -S . -B build-roadmap-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-roadmap-debug -j 2
QT_QPA_PLATFORM=offscreen ctest --test-dir build-roadmap-debug \
  --output-on-failure -E 'tst_benchmark|tst_realistic'
```

Early package/probe gates run only implemented capability sets and explicitly record unsupported/known-failure cases. W03 requires strict WP1–WP5/WP7; E05 requires all promised final cases. No unsupported or skipped case is passing feature evidence. The full acceptance run at E05 includes all correctness executables, bounded model/fuzz tests, sanitizer corpus, installed-consumer checks, and selected corrected benchmarks. Expensive benchmark campaigns are not mandatory for every small code change; targeted performance work uses the frozen trace matrix.

## Invariants that must remain visible

- Context identity is document + epoch + stream + actor + counter; independent streams do not share scalar coverage.
- An operation is applied, duplicate, deferred, or rejected. Receipt is not application, and application is not durable coverage.
- Writer identity survives restart; concurrent/cloned ownership requires explicit fork/conflict handling. Presence does not allocate operation IDs.
- Recovery reconstructs own and remote state before accepting fresh writes. Persisted cursors never bootstrap an empty engine by themselves.
- Durable-write success follows checked platform sync ordering; counters named fsync count actual successful calls.
- Unsupported/corrupt input remains recoverable/quarantined and blocks advancing coverage; it is not silently skipped and forgotten.
- GC remains disabled until a validated certificate covers membership, durable state, retired undo, and anchor/checkpoint obligations.
- An offline member contributes missing/old coverage; absence does not automatically remove it from eligibility.
- A stale-device flag/reminder is advisory. Keep/snooze/dismiss and clock passage never call retirement commit or authorize GC.
- Actor-local undo AND redo survive reopen within configured step and byte limits.
- Anchors are boundaries with affinity and context. Unresolvable/wrong-epoch anchors are explicit outcomes.
- A reference model must not call the production helper whose behavior it is validating.
- Performance output includes units and correct work counts; content mismatches fail the run.
- SQLite, additional CRDT primitives, and concurrent mutation of the same engine are outside this roadmap.

## Evidence and status template

Add the following after each task in the implementation task log:

```text
Task: L05
Status: complete
Baseline: <commit>
Implementation commits: <commit(s)>
Scope: <actual files and behavior>
Regression: <test that failed before the fix>
Verification: <exact commands, modes, counts, seeds>
Artifacts: <logs / traces / reports>
API/store changes: <new usage guide/spec sections or none>
Known limits: <material limits, not generic disclaimers>
Next ready tasks: <IDs whose prerequisites now pass>
```

Do not write elapsed estimates as deadlines. Small routine tasks should fit one focused implementation/review cycle; S02/G01 and large lifecycle tasks can span several cycles and must retain a written state/checklist between sessions.

## Early integration and alpha handoff

Read [00-consumer-integration.md](00-consumer-integration.md) before executing
W/A/S/E alpha tasks. The task registry order is topological and deliberately pulls
consumer work before the full lifecycle milestone; do not follow phase numbers
as a serial schedule or reinstate L08 as a blanket prerequisite. The validator
checks transitive required/excluded ancestors and all eight probe owners.

When completing an alpha task, report a concrete candidate SHA, tag/version
proposal, build/install/probe commands, capability and supported-format manifests,
artifact checksums, named verification evidence and pending external availability.
The task completes preparation only. Follow the explicit publication procedure in
the consumer lane after a separate user publication instruction; do not substitute
waiting for approval for preparation work, and do not claim a local candidate is
already fetchable. Existing full Git SHAs are valid pins when available remotely.

Later ordering/recovery/checkpoint work reruns the earlier installed position
probes and retains supported published token golden fixtures. C++ API instability
does not permit silently breaking an approved persisted token contract. Tests
must reuse originally minted bytes, not remint positions after merging/reopening.
