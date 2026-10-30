# M06 — Markoff's acceptance wording and evidence

**From:** Markoff maintainers
**To:** collabtext maintainers (same machine, `~/dev/collabtext`)
**Date:** 2026-10-31
**Re:** M06 — "Record production Markoff acceptance on exact candidate"
(`docs/roadmap/2026-10-reliability/10-markoff-core.md`). M06's note says the
status stays `not_started` until Markoff states acceptance of the exact
candidate for this row. This is that statement, plus the evidence behind it.

## Paste-ready wording for M06

> **Markoff accepts `collabtext-perf-checkpoint-2026-10-21`
> (`16cc48f4969abc2940a7fb332b72af018075dab7`, package `0.2.0-alpha.5`,
> capability manifest v17) as the engine of its production document path**,
> recorded at Markoff revision `78cac549` — the commit that carries this
> record; its tree is the acceptance build's tree (code-bearing revision
> `4e470f0c` plus the build-configuration fix in `78cac549`; the
> documentation-only revisions between them do not affect the result) —
> on 2026-10-31.
>
> The acceptance is Markoff's own. Its provider seams ran against the
> installed package at `build-collabtext-perf-checkpoint/install` (rebuilt
> from the tag) and, independently, as a standalone consumer project
> configured against a **relocated copy** of that prefix
> (`/tmp/collabtext-16cc48f-relocated`, `collabtext_DIR` recorded in the
> build cache): the relocated consumer suite passed **4/4**, and Markoff's
> full Debug suite passed **101/101** offscreen, including the
> installed-package consumer tests `markoff_collabtext_vnext_probe`,
> `markoff_document_storage_provider_test`, `markoff_provider_scope_test`,
> `markoff_provider_interface_conformance` and
> `markoff_provider_backed_document_test`.
>
> Coverage against M06's named obligations:
> - **Document keys and canonical per-block stream names** — exercised by
>   the provider scope and interface-conformance tests and the storage-
>   provider test: stable `DocumentStorageScope` keys, canonical injective
>   stream names carrying the full actor and counter, checked epoques.
> - **Full insertion identities** — the composite-block-identity and
>   scoped-router cases: `element_of`/`try_anchor_of` addressing, identities
>   bounded to document/stream/epoch and never truncated or hashed into
>   `uint64_t`.
> - **Committed terminal gap recovery** — the provider-backed document's
>   gap/rebuild/finalize cases (`in-session recovery … finalize gaps`,
>   `fresh MarkoffDocument open … rebuild accepted state and finalize gaps`)
>   and the storage-provider terminal-coverage barrier and revision
>   validators.
> - **Retained CTLO/0002 and CTGP/0001 archives** — the literal-archive
>   round-trip cases in the storage-provider test (operation identity,
>   canonical bytes, scoped identity interval) and the installed probe's
>   typed-value codec contract.
>
> **Scope of the acceptance, and what it is not.** It is acceptance of this
> candidate for Markoff's production path. It is not a certification of the
> producer's open X05 gates — the tag's own words keep X05 in progress
> (mixed-tombstone acceptance ≈1.18× against a 2× target, the routed
> Release sweep and the relocated package gate) — and it does not touch
> Wave's Q06, which stays open and unimplied. No upstream-shaped fixture and
> no unchanged older probe is substituted: every result above comes from
> Markoff's own harness binaries and provider seams. Merge authorization
> remains the user's.

## The evidence

| Item | Value |
|---|---|
| Consumer revision | Markoff `78cac549` (acceptance tree: code-bearing `4e470f0c` + build-configuration fix in `78cac549`) |
| Producer | tag `collabtext-perf-checkpoint-2026-10-21` = `16cc48f4969abc2940a7fb332b72af018075dab7` |
| Package | `0.2.0-alpha.5`, capability manifest v17, `CollabText::Core` component |
| Prefix | `build-collabtext-perf-checkpoint/install` (rebuilt from the tag) |
| Relocated prefix | `/tmp/collabtext-16cc48f-relocated` (a copy of the prefix above) |
| Full Debug suite | **101/101** `ctest`, offscreen |
| Relocated standalone consumer suite | **4/4** `ctest`, offscreen |
| Producer's own suite at the tag | 104/104 Debug (recorded in the tag's acceptance) |

Commands, in order:

```sh
cd ~/dev/Markoff

# 1. Full tree against the installed candidate prefix (a fresh configure:
#    MARKOFF_BUILD_TESTS and MARKOFF_BUILD_COLLABTEXT_VNEXT_TESTS default ON
#    for a standalone tree).
cmake -S . -B build-m06 -DCMAKE_BUILD_TYPE=Debug \
  -Dcollabtext_DIR=$PWD/build-collabtext-perf-checkpoint/install/lib/cmake/collabtext
cmake --build build-m06 -j 4
MARKOFF_BUILD_DIR=build-m06 scripts/run-tests.sh      # offscreen

# 2. Relocated standalone consumer, as tests/collabtext-vnext/CMakeLists.txt
#    asks: configure with CMAKE_PREFIX_PATH at a relocated installation.
cp -a build-collabtext-perf-checkpoint/install /tmp/collabtext-16cc48f-relocated
cmake -S tests/collabtext-vnext -B build-m06-consumer -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=/tmp/collabtext-16cc48f-relocated
cmake --build build-m06-consumer -j 4
QT_QPA_PLATFORM=offscreen ctest --test-dir build-m06-consumer -j 8 --output-on-failure
```

Notes on the numbers, for honesty:

- Today's run re-establishes the **Debug** result at the current consumer
  revision. The three-configuration matrix on this same candidate — Debug
  100/100, Release 100/100, ASan+UBSan 100/100 (LeakSanitizer off; this
  runner's ptrace restriction) — was run when the pin moved on 2026-10-21
  and is recorded in Markoff's `docs/STATUS.md` ("Acceptance for the new
  pin"). It has not been re-run today.
- The count is 101 rather than 100 because ASK-M31 added
  `tst_markoff_parser_session` after that record; nothing failed.
- The standalone relocated consumer project registers four tests by design
  — `markoff_provider_backed_document_test` needs `Markoff::Core` and only
  exists inside the full tree, where it ran against the installed prefix.
- The acceptance run caught one build-configuration defect on our side and
  it is fixed in the same change: the demo-app option was declared **after**
  the leaves that read it, so a fresh configure silently skipped the canvas
  app and its smoke test (a pre-existing dev cache hid it). The option is
  now declared above the leaves, and a fresh configure reproduces the dev
  tree's full count — hence 101/101 above.

## What M06 still needs from upstream

Nothing on the consumer side. Recording the wording above in
`docs/handoff/markoff-core-response.md` and the M06 row of `TASKS.json`
(consumer revision, producer SHA, relocated prefix, commands, explicit
acceptance) is yours; we have not edited the collabtext checkout.

The separate gates are unaffected: Q06 remains open, X05 remains in
progress at the producer, and master merge authorization remains the user's.
