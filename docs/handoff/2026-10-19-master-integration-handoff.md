# Handoff: consume `collabtext-integration-2026-10-19` (master)

From: collabtext. To: **Wave**, **Corbomite**, **Markoff**. Date: 2026-10-19.

## The one thing to change

Consume the annotated tag **`collabtext-integration-2026-10-19`** on **`master`**:

```sh
git -C <collabtext-checkout> fetch --tags origin
git rev-parse collabtext-integration-2026-10-19^{commit}   # pin this SHA
```

Package label `0.2.0-alpha.5`, capability manifest v17. Components and targets are
unchanged: `core` -> `CollabText::Core`, plus optional `file-transport` ->
`CollabText::FileTransport` and `qt` -> `CollabText::QtAdapter`/`CollabText::Widgets`.
Earlier candidate tags are immutable and stay valid:
`collabtext-adoption-2026-10-11` (`0d21670896d02dda3dc44d18d37f9657935333ed`),
`collabtext-wave-core-2026-10-11` (`ffe9875ee35a525f422eff3c1aaee2841f55d962`),
`collabtext-markoff-core-2026-10-11` (`1399e9533c9cfc1006854487af4e71b8b79989fd`),
`collabtext-markoff-core-2026-10-13-alpha5` (`e8a8e7d1a95dd50e23f38d6727921b47b814e44b`).

## What actually changed, and what did not

**No library change.** The merge of `feat/fuguemax-placement` into `master` adds
the reliability engine line to trunk; the tree at the new tag is byte-identical to
`collabtext-markoff-core-2026-10-13-alpha5` for every library, test, fixture and
packaging path:

```sh
git diff --stat collabtext-markoff-core-2026-10-13-alpha5 collabtext-integration-2026-10-19 \
  -- libs app cmake CMakeLists.txt tools tests
```

is empty. The only differences in the merged tree are repository-level docs, the
CI workflow, `.gitignore`, `validate-plan.py`, and the installed manifest's
branch/status lines. So: **no Core API, behavior, wire format or CMake target
changed**; nothing you verified at alpha.5 needs re-verification beyond recording
your acceptance against the new pin, which the gate requires.

Two consequences worth knowing:

1. Every collabtext candidate tag was previously local-only. Tags are now on
   `origin`, so a fresh clone can fetch them. That closes the reproducibility gap.
2. `master` now carries both packaging lines: two previously unpublished W02
   consumer-probe commits (from local `master`) and the `STATIC -> SHARED` +
   `EXPORT CorbomiteTargets` commits (`origin/master`). The latter two described
   the retired monolithic `collabtext` target and are superseded by the component
   targets; see the merge review
   `docs/reports/2026-10-19-master-integration-review.md` in collabtext.

## Wave

Pinned today: tag `collabtext-wave-core-2026-10-11` = `ffe9875`, package
`0.2.0-alpha.3`, manifest v15, installed to `build-deps/collabtext`

Change:

- `scripts/build-collabtext-pinned.sh:4-6` — `expected=` and `tag=` become the new
  SHA and `collabtext-integration-2026-10-19`. The clone URL stays
  `https://github.com/clintonthegeek/collabtext.git`; this now works on a fresh
  machine because the tag is published.
- No `find_package` change: `src/CMakeLists.txt:16` already requests
  `collabtext 0.2.0 EXACT CONFIG REQUIRED COMPONENTS core`. Note for your records:
  CMake's numeric version is `0.2.0` for every alpha, so that constraint cannot
  distinguish snapshots — the tag/SHA in the script is the real pin.
- No adapter or header change: `src/collabtext/` keeps linking `CollabText::Core`.
- Acceptance to record (Q06): `tst_collabtext_adoption`, `tst_collabtext_adapter`,
  `tst_collabtext_document` (`src/tests/CMakeLists.txt:221-246`) and the standalone
  probe at `tests/collabtext-adoption/` configured with `-Dcollabtext_DIR=<prefix>/lib/cmake/collabtext`.
- Your ADR notes pinning alpha.3 (`docs/adr/0012-*`, `docs/specs/0003-*`,
  `docs/decisions-archive.md`) are prose records of that snapshot; update them only
  if Wave's pin moves.

## Corbomite

Pinned today: `CMakeLists.txt:83-95` resolves a prefix from `COLLABTEXT_ROOT` or
the hard-coded sibling `${CMAKE_SOURCE_DIR}/../Markoff/build-collabtext-alpha5/install`,
probes `lib/cmake/collabtext/collabtext-config.cmake`, then
`find_package(collabtext CONFIG REQUIRED COMPONENTS core)`. Corbomite sources
include no `collabtext/` header and name no `CollabText::` symbol; the only direct
user is Markoff's `markoff_collabtext_vnext`, which `markoff_core` PUBLIC-links.

Required work: **none in CMake.** The prefix is the only coupling, and it is
recreatable once the tag is published. Two things to be aware of:

- Do not expect an in-tree `collabtext` target. The merged tree no longer installs
  the monolithic `collabtext` target into `CorbomiteTargets`; that install only
  ever applied when the retired engine tree was `add_subdirectory`d, which your
  `AGENTS.md:35,48` already forbids.
- Found while auditing, **not caused by this merge**: `cmake/CorbomiteConfig.cmake.in:5-10`
  finds Qt6/KF6 dependencies and then includes `CorbomiteTargets.cmake`, but never
  `find_dependency(collabtext)`, even though the exported set contains
  `Markoff::CollabTextVNext` (PUBLIC `CollabText::Core`). A third-party
  `find_package(Corbomite)` in a clean prefix will fail to resolve the imported
  target. Your call whether that is in scope; it is a packaging fix on the
  Corbomite side.
- Also stale on your side: `README.md:325` still says `libs/collabtext` is an
  uninitialized submodule.

## Markoff

Pinned today by prose plus a machine-local prefix: tag
`collabtext-markoff-core-2026-10-13-alpha5`, commit `e8a8e7d1a95dd50e23f38d6727921b47b814e44b`,
package `0.2.0-alpha.5`, manifest v17, prefix `build-collabtext-alpha5/install`
(`docs/STATUS.md:31-43`, `CLAUDE.md:27-29`).

The publish gap you recorded is now closed: the branch, that tag, and the new
integration tag are on `origin`, so the prefix is recreatable on the laptop or a
fresh clone.

Your choice, both supported:

- Keep installing `e8a8e7d1a95dd50e23f38d6727921b47b814e44b` — the tree is
  identical; the reference is now fetchable.
- Move the pin to `collabtext-integration-2026-10-19` and record a new acceptance
  record, per your own rule in
  `docs/handoff/2026-10-17-to-corbomite-ready-for-collabtext-transition.md:22-23`
  ("A Core behavior change needs a new tag and a new Markoff acceptance record").

No CMake change either way: `CMakeLists.txt:89` and
`libs/markoff-collabtext-vnext/CMakeLists.txt:3-5` request `COMPONENTS core`
without a version, and link `CollabText::Core`. Update `docs/STATUS.md:31-43`,
`CLAUDE.md:27-29` and the two handoff docs only if the pin moves; also drop the
"Publishing gap (found 2026-10-19)" paragraph, which no longer holds.

Latent debt you already filed, unaffected by this merge: a `<collabtext/...>`
include in a PUBLIC Markoff header (triage M27 item 4).

## Evidence behind this handoff

In collabtext, at the alpha.5 candidate `e8a8e7d` and re-run on merged `master`:

| Check | Result |
|---|---|
| Debug correctness suite | 101/101 passed |
| `tools/verify-consumer-package.sh --mode release --gate package --components core,file-transport,qt` | PASS (relocated install; 3 installed consumers; unknown component refused) |
| `tools/verify-consumer-package.sh --mode release --gate live --components core` | PASS (Wave-shaped 16/16 strict, 18 rows, manifest v17; Markoff/Markdown 4/4) |

Full record: `docs/reports/2026-10-19-master-integration-review.md`. Adoption gate
and per-consumer responsibilities: `docs/handoff/consumer-adoption.md`.

## Still open, by design

Q06 (Wave acceptance) and M06 (Markoff acceptance) are the remaining items on the
merge gate and are consumer-owned. Reclamation (`G03`, `G06`, `G07`, `G08`) and
performance (`X01`–`X10`) are unfinished: keep physical collection and log pruning
disabled, and do not expect logarithmic-edit performance.
