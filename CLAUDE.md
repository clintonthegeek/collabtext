# collabtext — orientation

## Reliability roadmap: merged into `master`

The reliability engine line (329 commits) merged into `master` on 2026-10-19 and
is tagged `collabtext-integration-2026-10-19` (package `0.2.0-alpha.5`, manifest
v17). Read `AGENTS.md` and `docs/roadmap/2026-10-reliability/README.md` first;
`TASKS.json` owns status and `DIRECTION.md` carries the current next steps
(E07a/E07b complete; Q06 Wave acceptance and M06 Markoff acceptance are next,
then the G03/G06/G07/G08 reclamation series and the X-series performance work).

The engine branches `feat/fuguemax-placement` and `roadmap/reliability-v2`, and
the `.worktrees/reliability-v2` checkout, were merged and deleted on 2026-10-19;
their commits are reachable from `master` and the candidate tags. That roadmap
supersedes the historical OpStream scope below.
W1–W6 external consumer requirements are in `docs/specs/vnext-external-position-contract.md`. All product-policy questions are resolved
in its `DECISIONS.md`: improve the existing C++ engine incrementally. No Rust
rewrite, Yrs adoption, or replacement-engine trial is in scope. No Superpowers
skill or subagent workflow is required.

The rest of this file preserves the previous project orientation as historical
context. It remains the legacy checkout's orientation until its work is updated
separately.

## Active project: `OpStream` extraction (post-IdList β)

The current work is extracting a transport-agnostic `OpStream` boundary on the public collabtext surface, with `StreamSync` adopting that boundary as the file-backed reference implementation. This was committed to in `docs/specs/2026-05-08-d5-negotiation-response.md` as the response to Markoff's D5 negotiation opener.

**Implementation plan:** `docs/superpowers/plans/2026-05-08-opstream-extraction.md`. Execute it task-by-task using `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans`. **Read the joint-design outcomes doc before starting Phase 1:** `docs/handoff/2026-05-08-d5-joint-design-outcomes.md`.

**Spec:** the response doc above is the binding scope statement. The "What we won't do" section is load-bearing — those are real constraints, not deferred decisions. Don't expand the work beyond what's listed there without an explicit conversation.

**Joint-design outcomes:** the two design calls flagged in the response doc (public type-surface form, ack-frontier file format) are resolved in `docs/handoff/2026-05-08-d5-joint-design-outcomes.md`. Do not re-litigate them; the plan references those decisions concretely.

## Completed: `IdList` (β from the D-evolution response)

**β accepted on 2026-05-08** (commit `3ead04b`). Plan: `docs/superpowers/plans/2026-05-04-idlist-implementation.md` (with acceptance footer). All 42/42 tests passing. Spec: `docs/CRDT_IDLIST_SPEC.md`. Wire-format `schema_version` advanced 2 → 3 in this work.

`IdList` is now the second engine primitive alongside `Buffer`. Public API surface: `insert_after` / `remove_at` / `ids` / `anchor_of` / `apply_ops` / `set_on_change` / `undo` / `redo` / `collect_garbage` / `compact`. Deliberately small per the binding spec.

## Deferred (not dropped): Qt-based widget lab + `collabedit`

The widget lab (`app/`, `app/collabedit/`) and supporting collab-editor work are on hold while β-arc ships. They're paused, not abandoned. When the OpStream extraction lands, the widget lab roadmap (cursor lifecycle, comment threads, bookmarks, etc.) resumes.

**Don't touch `app/` for new feature work** during the current OpStream extraction either. Include-path fixes for header relocations are fine; bug fixes are fine; engine-level refactors that incidentally help are fine; new widget-lab features wait.

## Build

Preset project. Build dir is `build-dev/`.

```bash
cmake --preset dev          # configure
cmake --build build-dev -j  # build
ctest --test-dir build-dev --output-on-failure  # run tests
```

`compile_commands.json` is symlinked from `build-dev/`. `.clangd` points there. If clangd is unhappy, regenerate the build dir with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`.

## Repo layout

```
libs/collabtext/
  include/collabtext/    # public-facing wrappers (CrdtEngine, CollabDocument, …)
                         # OpStream.h, StreamSync.h, Operations.h, IdListOperations.h,
                         # Serialization.h land here under the OpStream extraction
  src/crdt/              # the CRDT engine itself — Buffer, IdList, SumTree, Locator, …
                         # internal-only after the extraction: Fragment, Anchor, Locator,
                         # Global, op-variant tags
  src/identity/          # presence, signing, identity store
  src/ui/                # Qt widgets — DEFERRED
  tests/                 # Qt Test units; tst_opstream*.cpp lands here

app/
  collabedit/            # the dogfood editor — DEFERRED
  testapp/               # small harness — DEFERRED

docs/
  ARCHITECTURE.md        # high-level system view
  CRDT_ENGINE_SPEC.md    # Buffer's spec
  CRDT_IDLIST_SPEC.md    # IdList's spec
  CRDT_SYNC_SPEC.md      # current file-floor sync spec (file-backed transport)
  CRDT_TRANSPORT_SPEC.md # public OpStream contract — created in Phase 6 of the plan
  specs/                 # design specs (D-evolution response, D5 negotiation response)
  handoff/               # cross-team handoff docs (joint-design outcomes)
  research/              # research notes
  reports/               # post-implementation reports
  superpowers/plans/     # implementation plans
```

## Working conventions

- **TDD.** Every algorithmic / interface change gets a failing test first. Convergence/fuzz tests are a third of the work — schedule them up front.
- **Mirror existing shapes.** `OpStream` mirrors Markoff's `ITransport` four-method contract; new public types follow `Buffer`/`IdList`'s style. When in doubt about an implementation detail, the rule is: look at the existing analogue first.
- **Don't share `Operation`.** `IdListOperation` is a separate variant. Keep `Buffer`'s ABI clean. Both become public types under the OpStream extraction with a written narrow contract (only `lamport()` + round-trip via encode/decode are stable).
- **Schema bumps additively.** When wire format changes, bump `schema_version` (currently 3 — next bump goes to 4). Existing files keep loading where possible; if not, the bump is documented as a hard cut.
- **Frequent commits.** One commit per task in the plan, conventional-commit style (`feat(transport):`, `feat(streamsync):`, `feat(crdt):`, `test(transport):`, `docs(transport):`, `refactor(crdt):`) matching recent history.

## What this project is not

- Not a general-purpose CRDT framework. `IdList` is the *only* new CRDT primitive ever; OpStream is a transport boundary, not a primitive. No `Map`, no `Counter`, no nested types — see `docs/specs/2026-05-04-d-evolution-response.md` §"What we won't do" for the full list.
- Not a `Buffer` rewrite. `Buffer` is unchanged except for additive serialization tags and public surface adjustments under the OpStream extraction.
- Not a `CollabDocument` generalization. The structural composition of `IdList` + N `Buffer`s is the consumer's problem, built on collabtext's public primitives + `OpStream`.
- Not a transport implementation menagerie. `StreamSync` (file-backed via Syncthing) is the only reference impl shipped from collabtext. Direct-channel transports (TCP, WebRTC) are a separate roadmap track per `docs/specs/transport-elevation-roadmap.md`.
