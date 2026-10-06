# Current direction

**As of:** 2026-10-06. **Branch:** `feat/fuguemax-placement`, which contains
`roadmap/reliability-v2`. **Worktree:**
`/home/clinton/dev/collabtext/.worktrees/reliability-v2`.

This file is the instruction for the next agent. `TASKS.json` is the status
and dependency registry. Where an older spec, handoff, task step, or report
disagrees with this file, this file wins. Evidence for the ordering change is
the snapshot [2026-10-06-s03-progress.md](../../handoff/2026-10-06-s03-progress.md).
The algorithm is [vnext-sequence-algorithm.md](../../specs/vnext-sequence-algorithm.md) §0.

## What is true

Production `Buffer` and `IdList` place by FugueMax tree links
(`Placement.h`, `SequenceOrder`). `Locator`, `normalize_fragments`, and split
relocation are gone. `CTBO` and `CTLO` are version **0002**. Version 1 frames
are refused. `CTPT`, `CTGV`, and `CTGP` are version **0001**. `CTPT/1` is an
experimental position token, not a promise that Wave may store it.

S03 is complete (`0984d93`). Every S01 fixture passes on both engines, in every
arrival order. Concurrent pastes are `abcXYZ`. Concurrent typing and reverse
typing are `abcabcabc`. A peer inserting inside a typed run while its author
continues is `abcXYdefgh`. The installed WP6 baseline exception for `aXbYcZ`
is removed. WP6 is not a known failure.

WP8 is unimplemented. It is not cancelled and FugueMax does not forbid it.
`collect_garbage()` and `compact()` return zero and retain everything.

**E07a is complete.** The package alpha candidate is `0.2.0-alpha.1` at
`bdbae5c3d310f2388b1d445a254b4ac35474d215`, proposed tag `v0.2.0-alpha.1`,
**not created**. The installed package gate passed in Debug, Release and ASan at
that commit; the baseline probe matrix is 18 rows, 17 passing, one `unsupported`
row (WP8 durable pins). Nothing was published, pushed, tagged, merged or
messaged, and the preparation script performs no network access. Release notes
and pin instructions are in
[../../releases/integration-alpha.md](../../releases/integration-alpha.md); the
machine-readable record, checksums and the unexecuted publication checklist are
under `build-alpha/`.

**E07b is in progress, not complete.** Implementation commit `4b8d382` declares
`CTPT/1` a supported token version in `0.2.0-alpha.2` with 16 retained golden
cases and a reader fixture (`tst_position_token_reader`, 8/8 pass). The package
gate passed in Debug/Release/ASan at that commit. **The strict live-position gate
(`--gate live`) has NOT been run**, `prepare-integration-alpha.sh` has NOT been
run with `--gate live`, and the release-notes verification section is a
placeholder. The candidate SHA will change when the remaining work is committed.

## What to do next

One executor. Do not open a second implementation lane.

1. **Finish E07b.** Run `--gate live` in Debug, Release, ASan at the final
   implementation commit. Run `prepare-integration-alpha.sh --gate live` with
   the three live-gate evidence directories. Fill in the release-notes
   verification section. Commit bookkeeping and mark E07b complete.
2. **P07** is also unblocked. It is not next. Start it only after E07b, or when
   the user explicitly switches the executor to the recovery lane.
3. After that, follow registry order: P08, L02–L08, then S04–S07, then the
   G-series. S04 stays blocked on L08. S05 and the G tasks own pins and
   reclamation.

E07b must re-run the installed gates at its candidate revision in Debug, Release
and ASan, and it needs the `live` gate: E07a's runs cover `package` and
`baseline` only. Do not cite them as live-gate evidence. Do not cite W02 or W03
`verification_results` for current WP6 behavior: those rows record the
pre-S03 known failure.

## Do not do these things

- Do not re-implement placement. Do not put `Locator` back. Do not apply S02's
  R1/R2 transformation map as the production rule. Do not revive the
  locator-reuse spike (`a5ca463`, message says not for merge). It is an
  ancestor of this branch; the tip supersedes it.
- Do not treat `docs/CRDT_ENGINE_SPEC.md`, `docs/CRDT_IDLIST_SPEC.md`,
  `docs/ARCHITECTURE.md`, the superpowers plans, or §§1–7 and §5a of the
  sequence spec as the engine to build. They are the locator-era record, or
  the abandoned repair. §0 governs.
- Do not weaken an oracle, restore an F8 expected-fail, or put the WP6
  `aXbYcZ` baseline exception back to make a suite green.
- Do not optimize placement while doing E07 or a recovery task. Placement is
  O(n) on purpose. A later performance task may change that, and
  `tst_sequence_differential` is what makes the change safe.
- Do not merge this line into `master`, publish, or message a consumer unless
  the user says so in the current conversation. The histories diverged at
  `046d375`. `master` has two commits this line lacks: a partial copy of the
  roadmap scripts and a `.gitignore` for generated report artifacts. A future
  merge keeps both ignore intents. There is no compatibility shim and no
  migration of version 1 frames.
- Do not edit the Wave or Markoff checkouts.

## Contingencies the later tasks must keep

**Pins and reclamation (S05, G01, G06, G08).** A FugueMax link names a parent
and a right origin, and the right origin includes tombstones. Reclamation may
drop a deleted element's text only when no live pin, undo obligation, or
remaining link needs it. If the identity itself goes, a boundary stub or a
checked translation has to keep resolving the original token bytes. A pin miss
is an explicit error, never end-of-document. Storing token bytes in an
external log does not create a pin. Register, lookup, and release do not exist
yet. The WP8 probe expects `durable_pins.available == true` and today accepts
`unsupported` with those owners.

**Two primitives, one order, one difference.** `Buffer` attaches a new run to
the visible predecessor, as the paper does. `IdList` may attach to a tombstone
origin, as it always has. Do not make them match unless a task says to, and
record the choice in §0.7.

**Undo.** In-memory undo toggles visibility by item identity and does not move
a retained element in the tree. Undo that survives reopen is L06. It is not a
placement change.

**Tokens.** Operation fields changed, so the codec version moved to 0002.
Position token bytes did not. E07b decides whether any token version is
supported for consumer storage. Until that decision, the manifest says the
token is experimental. Do not remint stored bytes to hide a resolve failure.

**S02's interpreter.** `SequenceModel.h` stays. It agrees with FugueMax on
every S01 fixture and differs where S02 is not backward non-interleaving.
`tst_fuguemax_model::s02ModelIsSupersededByFugueMax` pins that history. The
production oracle is the FugueMax model, not `SequenceModel`.

**Identity order.** Equal-origin siblings use the engine's identity order,
counter first, then actor bytes. Theorem 10 leaves that tie free. Changing it
changes which of two concurrent runs leads, and nothing else.
