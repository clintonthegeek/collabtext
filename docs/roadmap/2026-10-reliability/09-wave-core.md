# Wave Core adoption implementation tasks

Current lane selected by DIRECTION.md. TASKS.json owns status and evidence.

## Q01 — Specify and independently model Wave Core requirements

**Depends on:** S03, S04b, L06

**Coverage:** R9, R14

**Files:**

- `docs/specs/wave-core-adoption.md`
- `tests/wave-core/marker-model.cpp`

**Steps:**

1. Specify one identity-bearing zero-width marker in the existing FugueMax text sequence, explicit before/after gaps, text-deletion exclusion, detachment and compensating restoration; duplicate insertion must never revive removal.
2. Specify CTBO/0002 indefinite archival reading, marker CTBO/0003, marker CTPT/2, unchanged CTPT/1 readers, checkpoint engine section 3, local-history ownership and stable per-stream projection actors.
3. Run the independent explicit-tree model with an empty marker value, concurrent Unicode insertion in both delivery orders, adjacent text deletion and duplicate insertion after marker removal. Keep assertions active in Release.

**Acceptance:**

- The specification settles every Wave feedback question without consumer-private payload access.
- The independent model passes before sequence implementation.

**Verification:**

Build and run `tst_marker_model`; retain the direct C++ model gate result.

**Design/decision gate:** The marker uses existing ordering; specification and independent model must pass before Q02.

## Q02 — Implement typed markers through the native text lifecycle

**Depends on:** Q01

**Coverage:** R9, R14

**Files:**

- `libs/collabtext/include/collabtext/Anchor.h`
- `libs/collabtext/include/collabtext/CrdtEngine.h`
- `libs/collabtext/src/CrdtEngine.cpp`
- `libs/collabtext/src/crdt/Buffer.h`
- `libs/collabtext/src/crdt/Buffer.cpp`
- `libs/collabtext/src/crdt/Fragment.h`
- `libs/collabtext/src/crdt/OperationPayload.h`
- `libs/collabtext/src/crdt/OperationValidation.cpp`
- `libs/collabtext/src/crdt/PositionPayload.h`
- `libs/collabtext/src/crdt/PositionTraversal.h`
- `libs/collabtext/src/crdt/PositionTraversal.cpp`
- `libs/collabtext/src/codec/OperationCodec.cpp`
- `libs/collabtext/src/codec/AnchorCodec.cpp`
- `libs/collabtext/src/storage/CheckpointEngineCodec.cpp`
- `libs/collabtext/src/storage/CheckpointCodec.cpp`
- `libs/collabtext/tests/tst_text_markers.cpp`

**Steps:**

1. Add opaque TextMarker and MarkerSide before/after; insert by UTF-8 scalar boundary or exact marker side, remove by marker token, resolve to bytes with full scope/frontier checks. Return the marker token and publishable operation together.
2. Represent a marker as a length-one ordered fragment with empty text, zero bytes and an explicit kind. Exclude it from text range deletion and character attachments; share allocator, causality, undo, callbacks and ordering with text.
3. Reject malformed marker shapes and cross-type deletion before mutation. Preserve CTBO/0002 and CTPT/1 bytes; encode marker edits as CTBO/0003 and tokens as CTPT/2 kind 5; retain all supported readers. Checkpoint kind data uses engine section 3 only when markers exist.
4. Test Unicode, equal-offset exact gaps, wrong scope, deferral, adjacent and whole-body deletion, two delivery orders, removal, duplicate replay, restored undo/redo and fresh replay; compare retained order against the independent tree on seeded histories.
5. Remove the full-document text copy from UTF-16 edit conversion and per-scalar expansion from marker resolution, preserving boundary refusal and callback behavior. Do not claim changed asymptotic edit complexity.

**Acceptance:**

- Public marker operations round-trip and converge without altering text length.
- Malformed or refused operations do not change sequence, version or allocator.
- Checkpoint round-trip preserves marker type, original identity and resolution.
- Existing text/position/ordering/undo suites pass unchanged except future-version test inputs.

**Verification:**

Run Debug/Release/ASan `tst_text_markers`, `tst_wave_core`, `tst_positions`, `tst_sequence_differential`, `tst_checkpoint_codec` and existing codec/undo tests.

## Q03 — Expose consumer-owned undo and deterministic projection composition

**Depends on:** Q02

**Coverage:** R9, R14

**Files:**

- `libs/collabtext/include/collabtext/Projection.h`
- `libs/collabtext/src/Projection.cpp`
- `libs/collabtext/include/collabtext/CrdtEngine.h`
- `libs/collabtext/src/CrdtEngine.cpp`
- `libs/collabtext/tests/public-headers/only_Projection_h.cpp`
- `tests/consumer-smoke/wave_core.cpp`

**Steps:**

1. Expose bounded text local_history_snapshot, restore_local_history, next undo/redo group, coalesce and retirement acknowledgments by delegating to the existing Buffer contracts. Guard callback reentrancy; restore only after complete shared replay.
2. Implement ProjectionAllocator with frozen v1 domain-separated SHA-256 derivation over length-prefixed document/stream/adapter, primitive, epoch and adapter version. Use one stable actor per stream, a checked counter starting at 1 and observed-counter raises; prohibit use as a device writer in the contract.
3. Test restored compensation uses fresh operation identities and replays on fresh replicas; test two independent projections produce identical bytes across 600 canonical transitions and an independently computed actor golden. Require consumer namespace collision checks and versioned translators.

**Acceptance:**

- Wave can own local undo metadata without DocumentSession or a second canonical log.
- Synthetic projection needs no durable device allocator and does not create one actor per event.
- Public headers compile alone and Core retains no new external dependency.

**Verification:**

Run `tst_wave_core` and public-header compilation in Debug/Release/ASan and relocated `consumer_wave_core`.

## Q04 — Retain archival readers and installed Wave acceptance traces

**Depends on:** Q03

**Coverage:** R9, R14

**Files:**

- `tests/consumer-smoke/wave_core.cpp`
- `tests/consumer-smoke/CMakeLists.txt`
- `tests/consumer-smoke/main.cpp`
- `libs/collabtext/tests/tst_serialization_public.cpp`
- `libs/collabtext/tests/tst_anchor_codec.cpp`
- `libs/collabtext/CMakeLists.txt`
- `tools/verify_changed.py`

- `libs/collabtext/tests/tst_position_token_reader.cpp`
- `tests/consumer-smoke/wave-contract/probes.cpp`

- `libs/collabtext/tests/tst_filesync.cpp`
- `libs/collabtext/tests/tst_stream_sync.cpp`

- `tests/consumer-smoke/markdown-contract/cases.json`
- `tests/consumer-smoke/markdown-contract/run.py`
- `tests/consumer-smoke/wave-contract/run.py`

**Steps:**

1. Keep literal CTBO/0002 insertion and undo archives independent of the current writer. Decode/apply through installed opaque operations and verify identity, visible text, attachment detachment and canonical encoding.
2. Register public-header-only Wave trace in the producer and relocated installed consumer; cover marker replay, explicit marker gaps, shared compensation, history restoration and deterministic projection.
3. Move unknown-version rejection inputs to future versions without loosening known-version validation. Route new test paths fail-closed in changed-path verification.

**Acceptance:**

- A newer package reads the retained archive through decode_operation without consumer payload builders.
- Unknown versions return unsupported_version; original bytes remain caller-owned.
- Installed tests use no source/build/private includes and run a nonempty CTest selection.

**Verification:**

Run codec goldens and `tools/verify-consumer-package.sh --mode MODE --gate live --components core` for Debug/Release/ASan; run the actual Wave-owned standalone probe against the relocated Release prefix.

## Q04a — Remove obsolete compatibility paths after proving current replacements

**Depends on:** Q04, G04

**Coverage:** R9, R14

**Files:**

- `libs/collabtext/src/storage/MembershipStore.cpp`
- `libs/collabtext/src/storage/CheckpointCodec.cpp`
- `libs/collabtext/src/storage/CheckpointEngineCodec.cpp`
- `libs/collabtext/tests/tst_membership.cpp`
- `libs/collabtext/tests/tst_checkpoint_codec.cpp`
- `libs/collabtext/tests/fuzz_decode.cpp`
- `libs/collabtext/include/collabtext/SyncManager.h`
- `libs/collabtext/src/SyncManager.cpp`
- `libs/collabtext/tests/tst_sync_manager_presence.cpp`
- `docs/specs/wave-core-adoption.md`
- `libs/collabtext/include/collabtext/ALPHA_CAPABILITIES.md`

**Steps:**

1. Use passing current membership, certified checkpoint and presence gates as replacement evidence. Remove CME1 membership journal decoding, unbound checkpoint engine section 1 decoding, and the ignored CrdtEngine constructor parameter from presence-only SyncManager.
2. Replace old-read acceptance tests with explicit unsupported-version rejection; retain current CME2 and engine section 2/3 round-trip, recovery and certified binding checks. Keep fuzz inputs addressed to current engine section versions.
3. State the clean adoption cut explicitly: no earlier alpha store/token upgrade obligation is assumed. CTBO/0002–0003 and CTPT/1–2 are current writer formats; their archive reader commitment starts with this candidate for Wave durable adoption, not obsolete experimental stores.
4. Run affected Debug/Release/leak-enabled ASan and installed current-package checks before candidate tagging; consumers must rebuild for the SyncManager constructor cleanup.

**Acceptance:**

- No obsolete membership or unbound checkpoint reader remains; old inputs fail with a typed unsupported-version error.
- Current replacements preserve bound recovery and presence behavior; Core adoption remains transport-independent.
- No compatibility layer is introduced for earlier alpha data.

**Verification:**

Run `tst_membership`, `tst_checkpoint_codec`, `tst_checkpoint_recovery`, `tst_gc_certificate`, `tst_session_recovery`, `tst_sync_manager_presence` in Debug/Release/ASan and installed header checks.

## Q05 — Align adoption documents and prepare a new immutable candidate

**Depends on:** Q04a

**Coverage:** R9, R14

**Files:**

- `AGENTS.md`
- `README.md`
- `docs/USING_VNEXT.md`
- `docs/handoff/consumer-adoption.md`
- `docs/handoff/wave-core-response.md`
- `docs/roadmap/2026-10-reliability/DIRECTION.md`
- `docs/roadmap/2026-10-reliability/README.md`
- `docs/roadmap/2026-10-reliability/DECISIONS.md`
- `docs/roadmap/2026-10-reliability/EXECUTION.md`
- `docs/roadmap/2026-10-reliability/TASKS.json`
- `docs/roadmap/2026-10-reliability/09-wave-core.md`
- `docs/specs/vnext-public-api.md`
- `docs/specs/vnext-protocol.md`
- `docs/specs/vnext-design-constraints.md`
- `docs/specs/vnext-external-position-contract.md`
- `docs/specs/wave-core-adoption.md`
- `libs/collabtext/include/collabtext/ALPHA_CAPABILITIES.md`
- `libs/collabtext/include/collabtext/Serialization.h`
- `CMakeLists.txt`
- `libs/collabtext/CMakeLists.txt`

- `docs/specs/vnext-operation-codec.md`
- `docs/roadmap/2026-10-reliability/00-consumer-integration.md`

- `docs/README.md`
- `tests/consumer-smoke/README.md`
- `docs/releases/integration-alpha.md`
- `docs/reports/2026-10-11-wave-core-candidate.md`

- `docs/reports/vnext-task-log.md`

**Steps:**

1. Update package to alpha.3 and capability manifest in the same change. Replace stale next-task and unimplemented-recovery claims; distinguish implemented source capability, installed evidence and external consumer approval.
2. Give Wave exact public entry points, smallest installed test, reader obligations, coordinate/gap semantics, atomic marker/thread event composition, undo persistence and projection identity invariants.
3. Run roadmap/scope/verifier checks, full appropriate Debug/Release/sanitizer correctness and relocated live package gates; record exact implementation SHA, logs, counts and limitations. Do not claim unexecuted Wave EventStore or production-adapter acceptance.
4. Commit implementation and evidence; create a separate annotated candidate tag without moving collabtext-adoption-2026-10-11. Keep external Q06 and the later Markoff/merge gate open; do not push, publish, message or merge.

**Acceptance:**

- Roadmap JSON and Markdown agree exactly and all claimed capabilities have executable evidence.
- The handoff names an immutable candidate and a precise remaining consumer/merge gate.

**Verification:**

Run validate-plan.py, check-scope.py --strict, verification tooling tests, full correctness gates and relocated installed live gates at the implementation commit.

## Q06 — Record real Wave acceptance of the new candidate

**Depends on:** Q05

**Coverage:** R9, R14

**Files:**

- `docs/handoff/wave-core-response.md`
- `docs/roadmap/2026-10-reliability/TASKS.json`

**Steps:**

1. Obtain Wave-owned adapter and reopened EventStore evidence against the exact new candidate. Require the typed marker/first-thread atomic event trace, retained archive bytes and unchanged canonical event IDs/history hashes.
2. Record exact Wave revision, collabtext SHA, package/prefix, commands and outcomes. Do not substitute the upstream-shaped consumer_wave_core fixture for production consumer acceptance.
3. After Wave acceptance, proceed to Markoff adoption under the existing handoff. Merge only after both real consumers and final engine gates pass on the same candidate and the user explicitly instructs merging.

**Acceptance:**

- Wave explicitly accepts its stage-4/T-406 protocol against the new candidate.
- No consumer acceptance or master merge is inferred from upstream test success.

**Verification:**

Wave runs its owned installed adoption and EventStore probes and supplies production adapter acceptance evidence.

