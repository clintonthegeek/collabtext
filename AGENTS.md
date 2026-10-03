# Reliability workstream orientation

This worktree is the isolated breaking-change branch `roadmap/reliability-v2`.
The legacy consumer checkout is `/home/clinton/dev/collabtext`, branch `master`.

Read `docs/roadmap/2026-10-reliability/README.md`, `DECISIONS.md`, and
`EXECUTION.md` before implementing a roadmap task. `TASKS.json` is the
authoritative dependency/status registry; the numbered Markdown files contain
the human-readable tasks. The initial deliverable is planning only: production
implementation has not started. Begin implementation when the user requests it.

For implementation turns:

- Work only in this worktree. Confirm branch and status before editing.
- Follow a selected task's scope, dependencies, acceptance, and verification.
- Do not infer answers to pending product-policy questions from plan defaults.
- Complete the specifications and independent models at design gates before
  implementing dependent algorithms. Record a concrete gap if a gate cannot be
  satisfied; do not improvise ordering or reclamation semantics.
- Use one executor following registry order; this plan requires no subagents or
  unavailable skills. Break an oversized task into explicit dependent subtasks.
- Ask questions in ordinary conversation text; the user cannot use inline
  question controls. Routine authorized work does not require fresh permission.
- Keep the original stores and checkout available for consumers. Master
  integration, package publication, external consumer edits, and messages are
  separate actions requiring the user's explicit instruction.

The roadmap supersedes the historical OpStream-project scope in `CLAUDE.md` on
this isolated branch. Preserve historical design material as context; follow
the current roadmap for intended new behavior and compatibility.
