# Reliability workstream orientation

This worktree is the isolated breaking-change line. As of 2026-10-06 the
checked-out branch is `feat/fuguemax-placement`, which contains
`roadmap/reliability-v2`. The evaluated source checkout is
`/home/clinton/dev/collabtext`, branch `master`.

Read `docs/roadmap/2026-10-reliability/DIRECTION.md` first. Then
`README.md`, `DECISIONS.md`, and `EXECUTION.md`. `TASKS.json` is the
authoritative dependency/status registry; the numbered Markdown files contain
the human-readable tasks, including steps that describe the original procedure
of a finished task. Phase numbers group concerns; do not reinstate the old
serial recovery-first route. `docs/specs/vnext-external-position-contract.md`
is the W1–W6 consumer contract. Older handoffs and the pre-vNext engine specs
are provenance.

## Where the work is, as of 2026-10-06

**Done:** V01–V08, D01, P01–P06b, L01, S01, S02, W01–W03, S04a/S04b and
packaging A01/A02a/A06a. Registry count: 72. S01/W01 are validated desired histories and
probe fixtures, not production F7/F8 fixes. P06a's final operation-opacity step
has passed affected Debug/Release/sanitizer and installed-consumer checks.

**S03 is complete** at `0984d93`. Direction and contingencies are in
`docs/roadmap/2026-10-reliability/DIRECTION.md`. The evidence snapshot is
`docs/handoff/2026-10-06-s03-progress.md`. Checked-out
branch is `feat/fuguemax-placement` (it contains `roadmap/reliability-v2`).
Production placement is FugueMax (`docs/specs/vnext-sequence-algorithm.md` §0).
`Locator` is gone. `CTBO`/`CTLO` are version 0002. ASan 18/18 and Release 30/30
targeted sweeps passed at `cc7d8cc`. The Debug installed live gate passed with
WP6 observing `abcXYZ`. WP6's baseline exception for `aXbYcZ` is removed.
WP8 durable pins stay unsupported.

**E07b is in progress, not complete.** Implementation commit `4b8d382` declares
`CTPT/1` a supported token version in `0.2.0-alpha.2` with 16 retained golden
cases and a reader fixture. The package gate passed in Debug/Release/ASan at
that commit. **The strict live-position gate (`--gate live`) has NOT been run**,
`prepare-integration-alpha.sh` has NOT been run with `--gate live`, and the
release-notes verification section is a placeholder. The candidate SHA will
change when the remaining work is committed.

**E07a is complete.** The package alpha candidate is `0.2.0-alpha.1` at
`bdbae5c3d310f2388b1d445a254b4ac35474d215`; proposed tag `v0.2.0-alpha.1`, not
created. Installed package gate passed in Debug, Release and ASan there, and the
baseline probe matrix is 18 rows with 17 passing and WP8 durable pins
`unsupported`. Nothing was published. Notes and pinning are in
`docs/releases/integration-alpha.md`; the preparation script is
`tools/prepare-integration-alpha.sh`, which refuses a dirty tree, the wrong
branch, and evidence that does not name the candidate SHA.

P07 is also ready but is not next; start it after E07b or on an explicit switch.

P06b and S04b are complete: checked versioned operation/gap codecs and native
scoped position/CTPT v1 APIs pass Debug/Release/leak-enabled sanitizer and
installed-package checks. S04b has 1,050 text/550 list native/model resolutions
and original-token fresh-process replay. No old API/format compatibility required.

W02 and W03 were complete before S03. Their earlier "WP6 known failure" record
is historical: the 2026-10-06 Debug baseline and live gates both observed WP6
`abcXYZ`. Store recovery, persistent undo, and durable pins remain pending.
Token bytes are experimental and unpublished. Follow actual registry dependencies.

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
- Benchmarks are opt-in standalone binaries, absent from CTest even with the
  build option enabled. Never run them with tests. The user deferred engine
  optimization and performance comparisons; focus on consumer correctness.

## Verification tiers

Use `tools/verify-changed.sh` for affected warm correctness checks. Bounded fuzz
and regression tests stay included. `--thorough` includes the long realistic
simulation where routed; `--all` includes every correctness test. A fresh
acceptance sweep is once per arc, not per edit. Compilation uses local bounded
ccache when available and one aggregate build traversal. Unknown routes and
empty selections fail. Details and separate benchmark commands are in EXECUTION.md.

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
