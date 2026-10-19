# Master integration review — 2026-10-19

Merge review prepared for the [`consumer-adoption.md`](../handoff/consumer-adoption.md)
master merge gate (condition 5: final candidate, consumer evidence records, known
limitations, deferred performance work).

## Candidate

| Item | Value |
|---|---|
| Final candidate tag | `collabtext-integration-2026-10-19` (annotated) |
| Branch | `master` |
| Package label | `0.2.0-alpha.5` (`COLLABTEXT_VERSION_STRING`) |
| Capability manifest | v17 |
| Merge commit | `cd4f4b90165c3127800939a7cc9c3c36494c2dd9` (parent 1: local `master`, parent 2: `feat/fuguemax-placement` at `e8a8e7d1a95dd50e23f38d6727921b47b814e44b`) |

## Branch lineage

All live branches descend from the same published base, `046d375`
(`codeberg/master`, the GPLv3 commit). None of them was an ancestor of `master`
before this merge.

| Branch | Tip | Relation to `master` before the merge |
|---|---|---|
| `feat/fuguemax-placement` | `e8a8e7d` (tag `collabtext-markoff-core-2026-10-13-alpha5`) | +329 commits; contains `roadmap/reliability-v2` and `spike/locator-reuse` entirely |
| `roadmap/reliability-v2` | `76842de` (local) / `9c5c0e6` (`origin`) | +74; ancestor of `feat/fuguemax-placement` |
| `spike/locator-reuse` | `a1f9218` | +77; ancestor of `roadmap/reliability-v2` |
| `master` (local) | `e477964` | +2 unpublished W02 consumer-probe commits |
| `origin/master` | `8b9e780` | +2 packaging commits (`STATIC -> SHARED`, `EXPORT CorbomiteTargets`) |
| `origin/feat/rope-integration`, `origin/feat/unified-deletion-tracking`, `origin/feat/offset-remote-edits` | — | already contained in `master`; no action |

Merge order: `origin/master` into local `master` (clean, one file), then
`feat/fuguemax-placement` (three conflicts, below).

## Conflict resolutions

| File | Resolution | Why |
|---|---|---|
| `libs/collabtext/CMakeLists.txt` | `feat/fuguemax-placement` side wins | The branch replaces the monolithic `collabtext` target with component targets (`CollabText::Core`, optional `FileTransport`, `QtAdapter`, `Widgets`) and an owning package config. `origin/master`'s `STATIC -> SHARED` flip and `install(TARGETS collabtext EXPORT CorbomiteTargets)` described the monolithic target, which no consumer builds in-tree any more: Corbomite consumes the installed package and its `AGENTS.md` forbids `add_subdirectory` of the vendored engine. Both entries are dead in the merged layout. |
| `.gitignore` | union of both sides | `master`'s generated-artifact patterns and the branch's `build-consumer-verify/` are unrelated. |
| `docs/roadmap/2026-10-reliability/validate-plan.py` | `feat/fuguemax-placement` side wins | Strict superset: adds the early-consumer-gate transitive checks and WP1–WP8 ownership validation. |

Tree identity: the merge commit's tree differs from `feat/fuguemax-placement` in
`.gitignore` only — `libs/`, `app/`, `cmake/`, `tools/`, `tests/` and
`CMakeLists.txt` are byte-identical, so **no library, test, fixture or packaging
content changed in the merge itself**.

Relative to the alpha.5 candidate tag `collabtext-markoff-core-2026-10-13-alpha5`,
the tagged revision differs only in repository-level documents, the CI workflow,
`.gitignore`, `validate-plan.py` and three `Branch:`/`Status:` lines of the
installed capability manifest:

| Path | Delta vs `collabtext-markoff-core-2026-10-13-alpha5` |
|---|---|
| `libs/**` | one file: `ALPHA_CAPABILITIES.md` (3 metadata lines; no capability, promise, format or target change) |
| `app/`, `cmake/`, `tools/`, `tests/`, `CMakeLists.txt` | none |
| `docs/**`, `README.md`, `CLAUDE.md`, `.gitignore`, `.github/workflows/consumer-alpha.yml` | status, candidate-pin and CI corrections |

## Evidence

Library, at `e8a8e7d` (the alpha.5 candidate the merge imports) and re-run at the
merged `master` commit:

| Check | Command | Result |
|---|---|---|
| Debug correctness suite, alpha.5 candidate | `ctest --test-dir build-dev --output-on-failure -j8` | **101/101 passed** |
| Release package gate (relocated install; `core,file-transport,qt`) | `tools/verify-consumer-package.sh --mode release --gate package --components core,file-transport,qt` | **PASS** — 3 installed consumers passed; unknown-component request refused |
| Release live-position gate (`core`) | `tools/verify-consumer-package.sh --mode release --gate live --components core` | **PASS** — Wave 16/16 strict cases (18 rows, manifest v17), Markdown 4/4 |

The release gates above were run at the alpha.5 candidate. Because the merge
changed no library, test, fixture or packaging path, they were re-run at the
tagged revision; those results are recorded in the
`collabtext-integration-2026-10-19` tag annotation rather than here, so the
annotation and the tree it names stay consistent.

## Consumer evidence records

| Consumer | Recorded pin at review time | Recorded evidence | Gate status |
|---|---|---|---|
| Wave | tag `collabtext-wave-core-2026-10-11` = `ffe9875`, package `0.2.0-alpha.3`, manifest v15; installed under `build-deps/collabtext` from `scripts/build-collabtext-pinned.sh` | `tst_collabtext_adoption`, `tst_collabtext_adapter`, `tst_collabtext_document`, `tests/collabtext-adoption` probe | **Open — Q06.** Wave has not recorded acceptance of the alpha.5/integration candidate. |
| Markoff | tag `collabtext-markoff-core-2026-10-13-alpha5` = `e8a8e7d`, package `0.2.0-alpha.5`, manifest v17; prefix `build-collabtext-alpha5/install` | A13/A14/A15/A16 records; A15 94/94 on `281e3bc1`; installed `markoff-core-installed-consumer` 2/2 | **Open — M06.** Adoption arc closed except the publish gap; no acceptance record on the merged `master` commit. |
| Corbomite (via Markoff) | `find_package(collabtext CONFIG REQUIRED COMPONENTS core)` against `${CMAKE_SOURCE_DIR}/../Markoff/build-collabtext-alpha5/install` | builds through `Markoff::CollabTextVNext` | **No code change required**; depends on the prefix being recreatable from a published tag. |

Both upstream-shaped producer fixtures (Wave contract, Markoff/Markdown contract)
pass on the candidate; those fixtures are not the real consumer integrations and
do not close Q06/M06.

## Known limitations carried into `master`

- Q06 (Wave acceptance) and M06 (Markoff acceptance) are not started. They are
  consumer-owned records and are the remaining merge-gate items.
- G03 and G06 are in progress; G07 and G08 are not started. Initial consumer
  adoption keeps physical collection and log pruning disabled, so this is not a
  merge prerequisite — but no turnkey installed GC is claimed.
- X01–X10 (performance) are not started. Edits still rebuild fragment vectors and
  the tree; no logarithmic-edit claim, and benchmarks are excluded from the
  correctness suite.
- E03–E06 (fresh API consumers, architecture rewrite, coverage audit, release
  dossier) are not started. `README.md`, `CLAUDE.md` and the installed manifest
  were corrected for this integration; the E04 architecture rewrite remains open.
- CTGV/0001, checkpoint/private-history/store artifacts retain their stated alpha
  limits. Pre-vNext CTBO/CTLO 0001 remains unsupported.
- Publisher-side only: `tools/prepare-integration-alpha.sh` still defaults to
  branch `feat/fuguemax-placement` and gates a clean tree against it. Preparing a
  future candidate from `master` needs `--branch master`.

## Publish actions taken with this review

Assisted, user-instructed: merge into `master`, annotated tag
`collabtext-integration-2026-10-19`, push of `master`, `feat/fuguemax-placement`
and the candidate tags to `origin`. No GitHub release, package archive or
downstream repository commit is created here.
