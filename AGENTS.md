# Reliability workstream orientation

This worktree is the isolated breaking-change branch `roadmap/reliability-v2`.
The evaluated source checkout is `/home/clinton/dev/collabtext`, branch `master`.

Read `docs/roadmap/2026-10-reliability/README.md`, `DECISIONS.md`, and
`EXECUTION.md` before implementing a roadmap task. `TASKS.json` is the
authoritative dependency/status registry; the numbered Markdown files contain
the human-readable tasks. Phase numbers group concerns; do not reinstate the old
serial recovery-first route. Read `docs/handoff/2026-10-04-reliability-review.md`
for current evidence and `docs/specs/vnext-external-position-contract.md` for
W1–W6 consumer cases.

## Where the work is, as of 2026-10-05

**Done:** V01–V08, D01, P01–P05, L01, W01, S01, and the whole packaging arc —
A01, A02a, A06a. The registry has 72 tasks.

**Do next: S04a**, the design gate for scoped boundary identities
(`TASKS.json` id `S04a`, doc `docs/roadmap/2026-10-reliability/`). Its
dependencies (S01, P06a steps 1–5, W01) are met. S04a is on Wave's critical
path: it specifies affinity, typed unresolvable anchors and the position-token
codec, which are the three things the manifest records as *not implemented* and
which Wave's R3 calls a data-corruption risk. S04b implements it.

**Then:** S04b → W02 (installed baseline probes) → W03 (strict live-position
probes) → S02/S03 → E07a/E07b. Follow
`docs/roadmap/2026-10-reliability/00-consumer-integration.md` for the early
package/probe/position ordering.

**P06a remains open** at step 6 (opaque operation values). It is blocked on a
recorded A02 boundary decision, not on anything S04a needs. Do not let it
reorder the critical path.

## Read this before trusting a green run

Three failures here all had the same shape — a check that reported success after
failing — so verify the verifier:

- A verification script that cannot exit nonzero is worse than no script. The
  `finish()` in `tools/verify-consumer-package.sh` once took `$?` instead of its
  argument and exited 0 on every explicit failure path.
- CTest exits 0 when a regex selects zero tests. Every script here passes
  `--no-tests=error`, and `tools/verify-changed.sh` refuses an empty selection
  and reports any changed file that matched no route. A routing table that stops
  matching a path is the regression a fast subset would otherwise hide.
- `tst_benchmark::memory_growth` trips the QtTest watchdog at ~307s under a
  parallel Debug sweep. That is a Debug timing limit on an uninstrumented
  measurement, not an assertion failure; it does not block acceptance. The user
  has asked for it to be skipped rather than tuned.

## Verification tiers

The full sweep is trusted and is not the inner loop. Use
`tools/verify-changed.sh` (routes on the diff, ~30s), `--thorough` (adds fuzz
and property tests), or `--all`. `tools/verify-roadmap.sh --fresh <mode> '.'` is
the acceptance tier — once per arc, not once per task. Details in `EXECUTION.md`
under "Verification tiers".

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
- S04a gates live boundaries independently of full S02/S03 ordering and L08
  recovery. W03 requires installed strict live-position probes; S04 certifies
  their later order/recovery composition. S05/G-series durable pin/GC gates stay.
  Keep Core plaintext/IdList and transport-agnostic; DocumentSession is optional.
  E07a/E07b prepare reviewable alpha candidates, not external publication.
- A consumer-facing statement of what the alpha can and cannot do lives in
  `libs/collabtext/include/collabtext/ALPHA_CAPABILITIES.md`, versioned, and
  installed beside the headers. `docs/USING_VNEXT.md` is the consumer how-to. If
  behaviour changes, the manifest changes in the same commit — a capability with
  no executable test behind it says `not implemented`, not `probably works`.
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
