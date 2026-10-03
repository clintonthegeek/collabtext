# Reliability implementation kickoff and handoff

Continue in `/home/clinton/dev/collabtext/.worktrees/reliability-v2`, branch
`roadmap/reliability-v2`. The original checkout remains on `master`; its new
documentation is a discovery copy. Do not implement against that checkout or
its build directories.

## Completed starting point

V01–V03 are complete. The authoritative task registry is
[TASKS.json](../roadmap/2026-10-reliability/TASKS.json). Read the worktree's
`AGENTS.md`, then the roadmap's [README](../roadmap/2026-10-reliability/README.md),
[DECISIONS](../roadmap/2026-10-reliability/DECISIONS.md), and
[EXECUTION](../roadmap/2026-10-reliability/EXECUTION.md).

- **V01:** `9d5541b` adds Debug, Release, and ASan/UBSan verification through
  `tools/verify-roadmap.sh`. It records compiler/configuration, selected tests,
  commands, revision, logs, and exit status. A failing selected test and an empty
  selection both return failure.
- **V02:** `cebabc3` adds named F1–F8 evaluation regressions, including both
  primitives for restoration and passing ordinary remote undo/redo controls.
  Its baseline had eleven expected-failure assertions; strict mode reproduced
  eleven actual failures.
- **V03:** `3f4a210` implements containment; its full hash is recorded in
  TASKS.json. Both primitives'
  `collect_garbage()` and `compact(Global)` now retain all state and return zero.
  No unsafe opt-in or coalescing path remains. The five failing F1/F2 restoration
  assertions now pass without expected-failure markers; all six Buffer/IdList
  restoration rows pass. Public comments explain why transport receipt counters
  cannot authorize reclamation.

This is conservative containment. It does **not** implement certified GC,
bounded engine memory, durable engine recovery, persistent undo/redo, or the
remaining protocol/ordering repairs. Tombstones and associated restoration
state can grow until safe reclamation is implemented through G06. Keep this
containment in place throughout intermediate tasks.

## Verification state

The final Debug and Release fast suites each pass 52 CTest executables. The
sanitizer selection (`tst_gc`, `tst_idlist_gc`, `tst_evaluation_regressions`)
passes all three with ASan, UBSan, and LeakSanitizer enabled. These are Linux/GCC
checks, not cross-platform certification or performance measurements.

The evaluation regression executable still records **six known expected failures**:

| Finding | Remaining behavior | Fixing tasks named by the test |
| --- | --- | --- |
| F3 | Missing actor does not constrain common coverage to zero | P03 |
| F4 | Restarted sender reuses envelope identity | P02, L04 |
| F5 | Receiver's persisted read progress cannot restore a fresh engine | L03, L05 |
| F6 | Late earlier segment is skipped | L03 |
| F7 | Left boundary affinity follows insertion | S04 |
| F8 | Concurrent passages interleave character by character | S02, S03 |

Each marker wraps one confirmed assertion, rather than setup or an entire test.
Remove a marker when its fixing task makes the assertion pass. F9/F10 need the
later durability/validation tasks; V02's declared scope is F1–F8.

```sh
cd /home/clinton/dev/collabtext/.worktrees/reliability-v2
git branch --show-current
git status --short
python3 docs/roadmap/2026-10-reliability/validate-plan.py --ready
./tools/verify-roadmap.sh debug '^(tst_gc|tst_idlist_gc|tst_evaluation_regressions)$'
./tools/verify-roadmap.sh release '^(tst_gc|tst_idlist_gc|tst_evaluation_regressions)$'
./tools/verify-roadmap.sh asan '^(tst_gc|tst_idlist_gc|tst_evaluation_regressions)$'
```

The sanitizer command needs execution outside this environment's restricted
sandbox: LeakSanitizer process inspection is blocked inside it. The unrestricted
run passed with leak checking enabled and no suppressions. Do not convert that
environment limitation into a disabled leak check.

Leave `COLLABTEXT_STRICT_EVALUATION` unset for ordinary verification. Setting it
disables the expected-failure markers: the entire regression executable currently
exits six. Its already-fixed restoration cases must succeed even in strict mode:

```sh
COLLABTEXT_STRICT_EVALUATION=1 \
  build-roadmap-debug/libs/collabtext/tst_evaluation_regressions \
  f1_local_gc_preserves_insertion_redo \
  f1_remote_gc_preserves_insertion_redo \
  f2_compact_preserves_remote_delete_undo
```

For a full fast suite, obtain CTest's names and construct an anchored selection
excluding only `tst_benchmark` and `tst_realistic`. The committed evidence
metadata contains the exact 52-name regex and command arrays. Both excluded
executables are longer-running suites, with their own later verification tasks;
the fast result makes no claim about them. Build-local full logs live under
`build-roadmap-{mode}/verification/`. Portable test output and configuration are
in [kickoff artifacts](../reports/2026-10-03-roadmap-kickoff-artifacts/README.md);
the [task log](../reports/vnext-task-log.md) records task results.

## Next task

Start **V04 — Make randomized failures replayable and reducible**, following
[01-validation.md](../roadmap/2026-10-reliability/01-validation.md). V04 is the
next task in registry order. V06, V07, and V08 are also structurally ready; do
not start their dependents early. Work on one task at a time.

V04's concrete deliverables are a checked 64-bit seed with a documented fixed
default; versioned event traces capturing delivery/undo/restarts/GC requests;
replay that never rerolls decisions; and causal delta reduction with an injected
failure proving replay and reduction. Save the full failing trace first. The
current fuzz suites still use their historical randomness; passing kickoff
runs are samples, not deterministically replayable campaigns. If V04 is too
large for one execution cycle, first split it into explicit dependent subtasks
in JSON and Markdown, retaining its acceptance requirements. Do not settle for
adding a seed alone and mark the whole task complete.

Mark the selected task in progress, read every file in its scope, establish the
failure/control, implement it, verify acceptance, and record its commit and
evidence before marking it complete. Run the plan validator after registry
edits. Design gates later in the roadmap require full specifications and
independent executable models before algorithm implementation.

## Product decisions already settled

Improve the existing C++20 engine and Qt/C++ integration incrementally. No Rust
rewrite, Yrs adoption, backend trial, or replacement production engine is in
scope. There are no existing production stores/history to migrate; API/ABI and
formats may change on this branch. Undo **and** redo must survive reopen with
defined step/byte limits. Offline devices remain enrolled until explicitly
retired; a configurable reminder after a few months is advisory, never automatic
retirement. These are resolved decisions, not questions to ask again.

The user will arrange the agent handoff. No parallel agents or unavailable
skills are needed. Ask genuinely new questions at the end of ordinary response
text; this user's terminal cannot use inline question controls. Master
integration, publication, and external consumer changes require a separate
explicit instruction.
