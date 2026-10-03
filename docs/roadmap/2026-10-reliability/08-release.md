# 08. Migration, documentation, and release gates

Goal: give consumers a concrete porting path and preserve existing data. Original stores and legacy history remain untouched; migration cannot reconstruct edits that were never written or already lost by the old implementation.

Read [execution rules](EXECUTION.md) and [decisions](DECISIONS.md) before starting. Task dependencies refer to the [master task registry](TASKS.json). New file paths below are intentional; do not assume they already exist.

## E01 — Implement a read-only legacy-store inspector

**Status:** see TASKS.json (initially not started)  
**Depends on:** V08, P06, L08  
**Coverage:** F4, F5, F6, R13  

**Files / scope:**

- `tools/collabtext-inspect-legacy.cpp`
- `libs/collabtext/src/codec/LegacyCodec.cpp`
- `libs/collabtext/tests/tst_legacy_inspection.cpp`
- `docs/MIGRATING_TO_VNEXT.md`

**Steps:**

1. Scan all own/remote legacy segments and seeds independently of persisted read cursors. Deduplicate by actual operation identity, not colliding stream entry IDs.
2. Use the frozen legacy semantics to reconstruct legacy visible content. Detect reused operation IDs with different payloads, missing dependencies, corrupt records, manifest conflicts, invalid seeds, and unresolved anchors.
3. Produce a machine-readable inventory with recoverable/unrecoverable ambiguities; never guess a winner or claim dropped/unwritten content is recovered. Input filesystem is opened read-only.

**Acceptance:**

- Inspection does not modify input bytes; colliding envelope IDs with distinct valid operations are not suppressed.
- Ambiguous legacy clock reuse or missing history yields an explicit rescue report instead of a silent plausible document.

**Verification:**

Run all V08 legacy fixtures and compare input hashes before/after inspection; verify reconstructed content against the frozen legacy engine.

## E02 — Implement migration into a new store and translate persistent anchors

**Status:** see TASKS.json (initially not started)  
**Depends on:** E01, S05, G05, A06, D01  
**Coverage:** F7, R13  
**Design/decision gate:** Q1 selected; legacy undo migration exception must be explicit in DECISIONS.md.  

**Files / scope:**

- `tools/collabtext-migrate.cpp`
- `libs/collabtext/tests/tst_store_migration.cpp`
- `docs/MIGRATING_TO_VNEXT.md`

**Steps:**

1. For the recommended Q1 route, materialize validated legacy content into a new document/epoch/store using new identities and a deterministic seed/checkpoint. Preserve old histories as immutable archive/provenance; do not replay old interleaving rules as vNext operations.
2. Translate durable comments/anchors by resolved legacy boundary/range to new stable anchors; preserve IDs/lifecycle state and report every unresolved translation. Ephemeral presence is reset.
3. Migration creates a dry-run report first and never overwrites the input. State explicitly whether old undo groups can be ported: the safe baseline starts a new active undo history and archives old history, while Q3 persistence applies to vNext sessions. Require explicit opt-in to any lossy translation.
4. Prevent old/new writers from opening the same store; validate epoch/schema. If Q1 instead requires live mixed-version collaboration, this task stays blocked until a separate bridge plan is added and tested.

**Acceptance:**

- Migration retains exact legacy visible content and resolvable comments in the new store; source hashes are unchanged.
- Unsupported/ambiguous data cannot silently become a successful migration. Porting and store migration are documented as separate actions.

**Verification:**

Run migration fixtures including Unicode, comments on deleted text, restart-colliding envelopes, and IdList+Buffer composition; reopen the new store and continue edits/undo across restart.

## E03 — Compile consumer ports and custom transport examples

**Status:** see TASKS.json (initially not started)  
**Depends on:** A06, E02  
**Coverage:** R2, R13, R14  

**Files / scope:**

- `tests/consumer-smoke/`
- `examples/core-editor/`
- `examples/custom-opstream/`
- `docs/MIGRATING_TO_VNEXT.md`

**Steps:**

1. Port the V08 old consumer samples into independently built new samples for text, IdList composition, anchors, local_clear replacement, error handling, subscriptions, and transport receipt commits.
2. Compile a minimal custom OpStream producer/consumer to demonstrate durable receipt versus semantic GC. Show how an external store pairs application recovery with delivery tokens.
3. Provide a checklist of old→new symbols/behavior without changing real consumers outside this repository. Include minimum build/link/offset requirements.

**Acceptance:**

- All examples compile against the installed package and exercise restart/duplicate delivery correctly.
- Every public breaking change has a concrete example or migration entry; source compilation success does not imply wire/store compatibility.

**Verification:**

Install a clean build and run all independent consumer examples; use no internal include paths or direct fragment access.

## E04 — Replace obsolete status claims and publish actual architecture

**Status:** see TASKS.json (initially not started)  
**Depends on:** L08, S07, G08, A07, E03, X10  
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

**Steps:**

1. Map F1–F10 and R1–R14 to commits, exact passing tests, and any explicitly user-approved deferrals. Verify no expected-fail marker remains for a promised release feature.
2. Review model independence, compatibility exceptions, error paths, single-owner threading, GC certificates, and recovery fallback invariants. Recheck original report probes against new facades.
3. Document residual risks and make incomplete tasks visible; a green test runner is insufficient if tasks or policy decisions remain unresolved.

**Acceptance:**

- Every recommendation is implemented/validated or visibly deferred by a recorded user decision.
- No evaluation defect is claimed fixed solely through disabling its test or narrowing an advertised guarantee without documenting the limitation.

**Verification:**

Run the complete Debug/Release correctness suite, bounded model/fuzz campaigns, ASan/UBSan, installed-consumer checks, and selected performance suite; retain acceptance logs.

## E06 — Prepare a consumer migration release without merging master

**Status:** see TASKS.json (initially not started)  
**Depends on:** E05  
**Coverage:** R11, R13  

**Files / scope:**

- `CMakeLists.txt`
- `docs/MIGRATING_TO_VNEXT.md`
- `docs/releases/vnext.md`
- `docs/reports/vnext-acceptance.md`

**Steps:**

1. Choose the release version explicitly from the current 0.1.0 baseline and compatibility policy; keep the architecture workstream name v2 distinct from a promise of package version 2.0.
2. Create reviewable release notes, package manifests, migration examples, old/new store isolation rules, known limits, and rollback instructions that preserve original stores.
3. Keep the breaking branch/worktree available for consumer porting. Prepare any tag/PR/package locally; publishing, external communications, and merging to master require a separate explicit user instruction.

**Acceptance:**

- Consumers can evaluate/install the new package while the original master API remains available.
- Rollback restores the old untouched store and binary; new edits are exported/reconciled explicitly rather than silently replayed into v1.

**Verification:**

Validate release metadata against installed-package and acceptance evidence; no external publishing or messaging is performed by this task.
