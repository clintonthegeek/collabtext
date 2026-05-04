# collabtext — orientation

## Active project: `IdList` (β from the D-evolution response)

The current work is implementing a new CRDT primitive — `CollabText::Crdt::IdList`, a list CRDT over opaque `uint64` elements — alongside the existing `Buffer`. This was committed to in `docs/specs/2026-05-04-d-evolution-response.md` as the response to Markoff's D-evolution proposal.

**Implementation plan:** `docs/superpowers/plans/2026-05-04-idlist-implementation.md`. Execute it task-by-task using `superpowers:subagent-driven-development` or `superpowers:executing-plans`.

**Spec:** the response doc above is the binding scope statement. The "What we won't do" section is load-bearing — those are real constraints, not deferred decisions. Don't expand the API beyond what's listed there without an explicit conversation.

## Deferred (not dropped): Qt-based widget lab + `collabedit`

The widget lab (`app/`, `app/collabedit/`) and supporting collab-editor work are on hold while β ships. They're paused, not abandoned. When β is done, the widget lab roadmap (cursor lifecycle, comment threads, bookmarks, etc.) resumes.

**Don't touch `app/` for new feature work** during β. Bug fixes are fine; engine-level refactors that incidentally help β are fine; new widget-lab features wait.

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
  src/crdt/              # the CRDT engine itself — Buffer, SumTree, Locator, …
                         # IdList lands here, parallel to Buffer
  src/identity/          # presence, signing, identity store
  src/ui/                # Qt widgets — DEFERRED FROM β
  tests/                 # Qt Test units; tst_idlist*.cpp lands here

app/
  collabedit/            # the dogfood editor — DEFERRED FROM β
  testapp/               # small harness — DEFERRED FROM β

docs/
  ARCHITECTURE.md        # high-level system view
  CRDT_ENGINE_SPEC.md    # Buffer's spec; CRDT_IDLIST_SPEC.md will join it
  CRDT_SYNC_SPEC.md      # transport / sync model
  specs/                 # design specs (incl. the d-evolution response)
  research/              # research notes
  reports/               # post-implementation reports
  superpowers/plans/     # implementation plans
```

## Working conventions for β

- **TDD.** Every algorithmic primitive gets a failing test first. The plan is structured this way; don't skip it. Convergence/fuzz tests are a third of the work — schedule them up front.
- **Mirror `Buffer` shape.** `IdList` reuses `SumTree`, `Locator`, `Anchor`, `Clock`, `UndoMap`, the deferred-op pattern, GC primitives. When in doubt about an algorithmic detail, look at `Buffer`'s analogue first. Differences are simplifications, not novelties.
- **Don't share `Operation`.** `IdListOperation` is a separate variant. Keep `Buffer`'s ABI clean.
- **No moves in v1.** Express moves as remove + insert. The response doc explicitly defers `moveAfter`.
- **Schema bumps additively.** When wire format changes, bump `schema_version` (currently 2 — bump to 3). Existing files keep loading.
- **Frequent commits.** One commit per task in the plan, conventional-commit style (`feat(crdt):`, `test(idlist):`, etc.) matching recent history.

## What this project is not

- Not a general-purpose CRDT framework. `IdList` is the *only* new primitive. No `Map`, no `Counter`, no nested types — see `docs/specs/2026-05-04-d-evolution-response.md` §"What we won't do" for the full list.
- Not a `Buffer` rewrite. `Buffer` is unchanged except for additive serialization tags.
- Not a `CollabDocument` generalization. The structural composition of `IdList` + N `Buffer`s is Markoff's problem, built on collabtext's public primitives.
