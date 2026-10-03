# V01–V04 verification artifacts

These are portable excerpts of actual verification runs on 2026-10-03. See
[the task log](../vnext-task-log.md) and
[handoff](../../handoff/2026-10-03-reliability-kickoff.md) for conclusions and
limits. `manifest.json` records process exits and original full-log locations.

`*.ctest.log` retain the CTest execution and final runner status, omitting routine
configure/build output. `*.metadata.json` retain the exact compiler, build flags,
selected tests, regex, command arrays, and original worktree evidence paths.
Direct `*.log` files retain the full QtTest assertion output or tiny runner
negative-control output. Build-local evidence directories contain full logs,
selection JSON, target names, and exit-code files; they are intentionally ignored
by Git. Absolute paths in metadata describe this test environment, not portable
installation requirements.

| Evidence | Observed result |
| --- | --- |
| V01 Debug / Release fast | 51/51 CTest executables pass in each mode |
| V01 unrestricted sanitizer smoke | 3/3 pass; ASan/UBSan/LSan enabled |
| V01 restricted sanitizer attempt | Test assertions pass; LSan process inspection fails inside the sandbox |
| V01 runner negative controls | Empty selection exits 3; deliberately failing executable exits 8 |
| V02 default regression executable | 16 QtTest passes, 11 XFAIL assertions |
| V02 strict regression executable | Eleven actual failures; exits 11 |
| V02 Release / sanitizer | Regression executable passes in each mode |
| V03 restoration red run | Five actual failures before production containment; six unrelated XFAILs |
| V03 retention red run | Both revised GC executables fail against the old production implementation |
| V03 targeted Debug / sanitizer | 3/3 CTest executables pass in each mode |
| V03 strict restoration rows | Six data rows plus init/cleanup pass; exits zero |
| V03 default regression executable | 16 QtTest passes, six XFAIL assertions |
| V03 entire strict executable | Six remaining failures F3–F8; exits 6 |
| V03 final Debug / Release fast | 52/52 CTest executables pass in each mode |
| V04 targeted Debug / Release / sanitizer | 3/3 CTest executables pass in each mode |
| V04 final Debug / Release fast | 53/53 CTest executables pass in each mode |
| V04 final sanitizer fast selection | 53/53 pass with ASan, UBSan, and leak checking enabled |
| V04 seed-42 determinism | Two `tst_fuzz` runs produce byte-identical traces; a third run at seed 20261003 differs in every scenario |
| V04 injected failure | 42-event trace with one injected lost delivery diverges; reduction removes 26 events and the reduced trace still diverges |
| V04 strict regression executable | Unchanged: six remaining F3–F8 failures; exits 6 |

### V04 additions

`v04-*-targeted/` and `v04-*-fast/` hold the runner evidence directories for the
three verification modes. `v04-seed42-run-a/`, `v04-seed42-run-b/`, and
`v04-seed42-other-seed/` hold the recorded traces from two runs at seed 42 and
one at the default seed; `v04-seed42-identical.txt` and `v04-seed42-differs.txt`
are the corresponding `diff` results. `v04-injected-failure/` holds the
unreduced 42-event artifact, its 16-event reduction, and both the in-test and
CLI reduction logs. `v04-fuzz-seed42-*.log` are the two campaign runs.

Leak checking was active for every sanitizer row: this environment is not the
restricted sandbox V01 hit, which was confirmed by observing LeakSanitizer
report a deliberate leak from a throwaway program. No suppression was used.
`v04-realistic-compact-red` records a pre-existing failure found during the V04
migration rather than a passing run.

Sanitizer successes run outside the restricted sandbox with leak checking
enabled and no leak suppression. The restricted run is retained to explain the
required environment change, not counted as a successful sanitizer run.

Metadata's revision is the HEAD at execution time. The corresponding task's
source/tests were then uncommitted; final implementation hashes in TASKS.json
identify those changes. The V03 red runs used V03's new retention/assertion
expectations against V02's old production code. V03's final checks used the
complete containment change. No production C++ changes followed those checks.

The fast suites exclude only `tst_benchmark` and `tst_realistic`; their results
are not implied. V04 made the randomized suites seeded and traced, and
`tools/verify-roadmap.sh` did not run `tst_realistic` as part of these
selections: it is exercised separately and takes minutes in Debug. These runs
do not measure performance, certify other operating systems, prove CRDT
semantics exhaustively, or complete the remaining roadmap.
