# Reliability workstream orientation

This worktree is the isolated breaking-change branch `roadmap/reliability-v2`.
The evaluated source checkout is `/home/clinton/dev/collabtext`, branch `master`.

Read `docs/roadmap/2026-10-reliability/README.md`, `DECISIONS.md`, and
`EXECUTION.md` before implementing a roadmap task. `TASKS.json` is the
authoritative dependency/status registry; the numbered Markdown files contain
the human-readable tasks. Implementation has started: V01–V03 are complete.
Read `docs/handoff/2026-10-03-reliability-kickoff.md` for the verified starting
point and next task, V04.

For implementation turns:

- Work only in this worktree. Confirm branch and status before editing.
- Follow a selected task's scope, dependencies, acceptance, and verification.
- All product-policy questions are resolved in DECISIONS.md. Improve the
  existing native C++20 engine incrementally: retain working components, repair
  invariants, and measure optimization. No Rust rewrite, Yrs adoption, backend
  trial, substitution layer, or replacement production engine is in scope.
- Complete the specifications and independent models at design gates before
  implementing dependent algorithms. Record a concrete gap if a gate cannot be
  satisfied; do not improvise ordering or reclamation semantics.
- Use one executor following registry order; this plan requires no subagents or
  unavailable skills. Break an oversized task into explicit dependent subtasks.
- Ask questions in ordinary conversation text; the user cannot use inline
  question controls. Routine authorized work does not require fresh permission.
- The user confirmed no production data/stores/history; freely redesign the
  API and formats without adding migration or compatibility tasks. Devices stay
  enrolled until explicitly retired; time-based reminders are advisory. Undo
  and redo persist across reopen. Rust/background comparisons are informational.
  Master integration, publication, external consumer edits, and messages are
  separate actions requiring the user's explicit instruction.

The roadmap supersedes the historical OpStream-project scope in `CLAUDE.md` on
this isolated branch. Preserve historical design material as context; follow
the current roadmap for intended new behavior and compatibility.
