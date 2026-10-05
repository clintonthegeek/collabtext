# 08. Fresh release, documentation, and acceptance gates

Goal: deliver an installable new package and complete usage examples. The user confirmed no existing production data, stores, or history; legacy inspection and migration tasks E01/E02 were removed, with their IDs retained in the change record.

Read [execution rules](EXECUTION.md) and [decisions](DECISIONS.md) before starting. Task dependencies refer to the [master task registry](TASKS.json). New file paths below are intentional; do not assume they already exist.

## E03 — Compile fresh API consumers and custom transport examples

**Status:** see TASKS.json (initially not started)  
**Depends on:** A06  
**Coverage:** R2, R13, R14  

**Files / scope:**

- `tests/consumer-smoke/`
- `examples/core-editor/`
- `examples/custom-opstream/`
- `docs/USING_VNEXT.md`

**Steps:**

1. Create independently built new samples for text, IdList composition, anchors, reset, error handling, subscriptions, and transport receipt commits from the V08 use-case inventory.
2. Compile a minimal custom OpStream producer/consumer to demonstrate durable receipt versus semantic GC. Show how an external store pairs application recovery with delivery tokens.
3. Provide supported API/build/link/offset examples without accessing real external consumers. Old/new symbols may be listed as release changes, but no porting shim or data migration is required.

**Acceptance:**

- All examples compile against the installed package and exercise restart/duplicate delivery correctly.
- Every supported public behavior has a concrete example; samples use only the new package and formats.

**Verification:**

Install a clean build and run all independent consumer examples; use no internal include paths or direct fragment access.

## E04 — Replace obsolete status claims and publish actual architecture

**Status:** see TASKS.json (initially not started)  
**Depends on:** L08, S07, G08, A07, E03, X10, A08  
**Coverage:** R11, R12, R13  

**Files / scope:**

- `README.md`
- `CLAUDE.md`
- `docs/ARCHITECTURE.md`
- `docs/CRDT_ENGINE_SPEC.md`
- `docs/CRDT_IDLIST_SPEC.md`
- `docs/CRDT_SYNC_SPEC.md`
- `docs/CRDT_TRANSPORT_SPEC.md`
- `docs/LOW_LATENCY_INPUT.md`

**Steps:**

1. Rewrite current architecture around implemented targets/session/transport/checkpoints/GC policy and exact status. Preserve historical design reports with a superseded notice rather than editing past measurements to look current.
2. Describe guarantees conditionally: durable commits, eventual valid delivery, epoch membership, configured undo retention, and supported anchor lifetime. Remove absolute no-lost-work and unsupported uniqueness/performance claims.
3. Document trusted-folder identity scope; replace signing-success stubs with explicit Unsupported/no-authentication results, or hide them from the supported API. Cryptographic authentication is a separate feature; do not silently install a signing system.
4. Update low-latency guidance using measured editor paths, and list tested platforms, unsupported schemas, resource limits, and deferred features.

**Acceptance:**

- Docs match installed public APIs and actual behavior; build instructions reproduce optional-core/Qt/package configurations.
- No unsupported signing, WebSocket requirement, proven non-interleaving, GC, or input-to-photon claim remains.

**Verification:**

Validate local links and API examples; compare each advertised guarantee to a named passing test and retained report.

## E05 — Audit task coverage and close every known failure marker

**Status:** see TASKS.json (initially not started)  
**Depends on:** E04  
**Coverage:** F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, R4, R11  

**Files / scope:**

- `docs/roadmap/2026-10-reliability/TASKS.json`
- `docs/reports/vnext-acceptance.md`
- `libs/collabtext/tests/tst_evaluation_regressions.cpp`
- `docs/specs/vnext-external-position-contract.md`
- `tests/consumer-smoke/wave-contract/`

**Steps:**

1. Map F1–F10 and R1–R14 to commits, exact passing tests, and any explicitly user-approved deferrals. Verify no expected-fail marker remains for a promised release feature.
2. Review model independence, fresh-format rejection, error paths, single-owner threading, GC certificates, and recovery fallback invariants. Recheck original report probes against new facades.
3. Document residual risks and make incomplete tasks visible; a green test runner is insufficient if tasks or policy decisions remain unresolved.
4. Map W1-W6 and every named external-position case to implementation commits and executable evidence. Rerun installed_headless_wave_contract with completed G-series reclamation and supported token-version readers. Report formatting as explicitly consumer-owned outside this plain-text release; a requirement document or passing historical probe cannot substitute for the new public-API gate.

**Acceptance:**

- Every recommendation is implemented/validated or visibly deferred by a recorded user decision.
- No evaluation defect is claimed fixed solely through disabling its test or narrowing an advertised guarantee without documenting the limitation.
- The final acceptance dossier contains exact installed token/pin/reopen/compensation/GC/epoch tests for W1-W3/W5-W6 and an explicit W4 formatting scope statement; no Wave readiness claim is based solely on task checkboxes.

**Verification:**

Run the complete Debug/Release correctness suite, bounded model/fuzz campaigns, ASan/UBSan, installed-consumer checks, and selected performance suite; retain acceptance logs. Verify the W-series named cases assigned above at this task's implementation layer and record their evidence; later dependent cases remain pending.

## E06 — Prepare the fresh release dossier without merging master

**Status:** see TASKS.json (initially not started)  
**Depends on:** E05  
**Coverage:** R11, R13  

**Files / scope:**

- `CMakeLists.txt`
- `docs/USING_VNEXT.md`
- `docs/releases/vnext.md`
- `docs/reports/vnext-acceptance.md`

**Steps:**

1. Choose the release version explicitly from the current 0.1.0 baseline and compatibility policy; keep the architecture workstream name v2 distinct from a promise of package version 2.0.
2. Create reviewable release notes, package manifests, new API examples, format identifiers, known limits, device-retirement instructions, and persistent-undo guarantees. Explicitly state that old prototype formats are unsupported and no production migration is needed.
3. Keep the breaking branch/worktree for review and API experiments. Prepare any tag/PR/package locally; publishing, external communications, and merging to master require a separate explicit user instruction.

**Acceptance:**

- The new package installs independently with complete examples and acceptance evidence.
- A clean-store start is documented; no promise of downgrade or old-format replay accompanies the fresh design.

**Verification:**

Validate release metadata against installed-package and acceptance evidence; no external publishing or messaging is performed by this task.
