# Reliability roadmap implementation log

Worktree: `/home/clinton/dev/collabtext/.worktrees/reliability-v2`.
Branch: `roadmap/reliability-v2`. Source baseline: `046d375`; approved native
engine roadmap: `374f93c`. Task status and full implementation hashes are in
[TASKS.json](../roadmap/2026-10-reliability/TASKS.json).

## 2026-10-03 — V01: reproducible verification

Commit `9d5541b` adds an opt-in ASan/UBSan CMake configuration and
`tools/verify-roadmap.sh`. The runner configures the correct worktree, asks
CTest to select tests, builds their corresponding targets, executes them with
offscreen Qt, and retains commands/compiler/flags/counts/revision/logs/status.
Default build concurrency is two. Unsupported sanitizer setups fail explicitly;
an empty test selection cannot succeed.

Verification: the original fast suite passed **51/51** in both Debug and
Release. The Buffer/IdList/clock sanitizer smoke selection passed **3/3** with
ASan, UBSan, and LeakSanitizer enabled. An isolated temporary CMake fixture using
the actual runner returned exit **8** for an intentionally failing executable
and **3** for a zero-test selection. Restricted-sandbox sanitizer runs passed
test assertions but failed LeakSanitizer process inspection; the unrestricted
rerun passed with no suppressions. This is an environment restriction, not a
leak finding.

The runner now supplies the planned commands; Qt/zstd are still required until
the later packaging tasks. No public engine API changed in V01.

## 2026-10-03 — V02: evaluation regressions

Commit `cebabc3` registers `tst_evaluation_regressions`. Named tests encode
F1–F8, with both Buffer and IdList for insertion redo and remote deletion undo.
Ordinary remote undo/redo, fresh timestamps, fixture setup, decoding, and exact
content controls are normal passing assertions. No timing sleeps are required.
Expected-failure messages link each defect to its finding and fixing task.

Verification: Debug, Release, and ASan/UBSan/LeakSanitizer runs passed this
executable. The initial default Debug run reported **16 QtTest passes and 11
XFAIL assertions**. With `COLLABTEXT_STRICT_EVALUATION=1`, the same assertions
became **11 actual failures**, process exit **11**. The local IdList redo case
already passed; the remote deletion test clears A's incidental local insertion
pin while B retains deletion undo, exposing the actual compaction defect.

These are explicit known failures, not a claim that all lifecycle behavior is
correct. A fixing task removes its own marker; unexpected passes fail normal
QtTest execution. F9/F10 belong to later durability and validation work.

## 2026-10-03 — V03: contain unsafe reclamation

Commit `3f4a210` implements containment; its full hash is recorded in TASKS.json. `collect_garbage()` and
`compact(Global)` now retain every fragment/entry and return zero in both
primitives. The unsafe sweep/coalescing path was removed. Public comments
describe temporary retention and transport acknowledgements as receipt
information that cannot authorize deletion. Existing reclamation expectations
were replaced with retention checks; undo, convergence, and anchor-context
assertions remain, including remote undo after local history trimming.

Before the fix, removing F1/F2's five expected-failure markers caused exactly
**five actual restoration failures**; both revised GC executables also failed
their retention assertions against the old implementation. After the fix, all
six primitive restoration rows pass in strict mode (**8 QtTest passes** counting
initialization/cleanup). The full regression executable has **16 passes and six
remaining XFAIL assertions**, F3–F8; its entire strict run exits **6**.

Verification: the targeted Debug GC/regression selection passed **3/3**. The
final fast suite passed **52/52** in both Debug and Release, excluding only
`tst_benchmark` and `tst_realistic`. Targeted ASan/UBSan/LeakSanitizer GC and
regression checks passed **3/3**, unrestricted and without leak suppression.
These are Linux/GCC runs; historical randomized suites have not yet been made
replayable (V04), and no new performance claim follows from this work.

Physical reclamation is intentionally unavailable until G06. This fixes the
F1/F2 data-loss sequences by preserving state; it does not establish bounded
memory or implement persistent history, recovery, membership, retirement, or a
GC certificate. Engine state may grow in the meantime.

## Evidence and continuation

[Portable artifacts](2026-10-03-roadmap-kickoff-artifacts/README.md) retain CTest
output, direct QtTest traces, and configuration metadata. Full build logs remain
in ignored worktree verification directories. Metadata names the HEAD at test
time; task changes were then uncommitted, so TASKS.json's final implementation
commit identifies the tested code. The V03 red runs used the revised tests
against V02's production code.

The [handoff](../handoff/2026-10-03-reliability-kickoff.md) gives exact commands,
known failures, settled product decisions, and the next task. Continue with V04;
do not interpret this kickoff as completing the remaining roadmap.
