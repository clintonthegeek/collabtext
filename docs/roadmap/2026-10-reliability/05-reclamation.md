# 05. Snapshots, membership, and safe reclamation

Goal: bound recoverable history only after proving that permitted future operations cannot require removed state. Durable receipt is necessary but insufficient. GC stays disabled until every certificate condition passes. Conservative blocking is preferable to guessed peer retirement.

Placement is FugueMax (`docs/specs/vnext-sequence-algorithm.md` §0). A link's parent and right origin are element identities, and the right origin includes tombstones. Reclamation may drop deleted text only when no live pin, undo obligation, or remaining link needs that identity. Otherwise retain a boundary stub or a checked translation. A pin miss is an explicit error, never end-of-document. WP8 is unimplemented and still required; these tasks own it. `Buffer` anchors on the visible predecessor and `IdList` may anchor on a tombstone. Do not collapse that difference inside a GC task.

Read [execution rules](EXECUTION.md) and [decisions](DECISIONS.md) before starting. Task dependencies refer to the [master task registry](TASKS.json). New file paths below are intentional; do not assume they already exist.

## G01 — Specify membership epochs and reclamation certificates

**Status:** see TASKS.json (initially not started)  
**Depends on:** D01, S06, L08, P07  
**Coverage:** F1, F2, F3, F7, R10  
**Design/decision gate:** Q2/Q3 resolved: explicit retirement and persistent undo/redo. The protocol still requires complete independent review/model checks; incomplete certificates never unlock physical GC.  

**Files / scope:**

- `docs/specs/vnext-membership-gc.md`
- `libs/collabtext/tests/reference/GcModel.h`
- `libs/collabtext/tests/tst_gc_model.cpp`
- `docs/specs/vnext-external-position-contract.md`

**Steps:**

1. Define immutable epoch membership, explicit enrollment/retirement events, producer identities, and how new actors join without invalidating an already published common frontier. Devices stay enrolled indefinitely until explicitly retired; elapsed time, reminders, and presence are advisory and never acknowledgements.
2. Define a certificate binding stream, epoch, membership hash, common durable coverage, per-actor undo-retirement coverage, retained anchor translations, and durable checkpoint hash. Require all still-enrolled actors, including silent ones, to contribute; no scalar-max substitution.
3. Specify the full state machine for checkpoint proposal, coverage confirmation, epoch transition, interruption, competing proposals, and returning retired peers. Preserve a returning peer's unsynced branch for explicit reconciliation; do not pretend replay into a new epoch is automatically valid.
4. Implement a slow model and complete eligibility predicate. Include no available peers, unknown actor, missing ack, restored stale ack, remote redo, concurrent enrollment, retirement, and checkpoint corruption.
5. Model W3 external pin obligations, consumer projection frontiers, crash/orphan retention, pin release, original token-version readers, and epoch translations. Explicit device retirement removes device obligations only; it cannot release another live document object. Include consumer_commit_pin_crash_matrix and retire_device_preserves_live_reply_pin before implementing certificates.

**Acceptance:**

- There is a complete implementable protocol; every physical deletion has a certificate and no permitted future undo/anchor request needs removed data.
- If an enrolled actor retains a relevant undo group or has insufficient coverage, GC blocks until a validated explicit retirement/epoch transition removes that obligation. A stale-device flag does not alter membership.
- Eligibility refuses reclamation with an incomplete external-pin projection or missing translation. Live reply pins survive author retirement; unlimited live references have an explicitly measured metadata obligation rather than an unconditional bounded-memory claim.

**Verification:**

Run GcModel against all F1/F2/F3 histories and adversarial membership traces; publish the eligibility predicate and state transition table before G02 starts. Verify the W-series named cases assigned above at this task's implementation layer and record their evidence; later dependent cases remain pending.

## G02 — Implement durable membership and undo-retirement publication

**Status:** see TASKS.json (initially not started)  
**Depends on:** G01, L06  
**Coverage:** F1, F2, R10  

**Files / scope:**

- `libs/collabtext/src/storage/MembershipStore.cpp`
- `libs/collabtext/src/storage/MembershipStore.h`
- `libs/collabtext/src/storage/RetentionState.cpp`
- `libs/collabtext/src/storage/RetentionState.h`
- `libs/collabtext/src/DocumentSession.cpp`
- `libs/collabtext/src/transport/StreamSync.h`
- `libs/collabtext/src/crdt/StreamSync.cpp`
- `libs/collabtext/include/collabtext/DocumentSession.h`
- `libs/collabtext/include/collabtext/ALPHA_CAPABILITIES.md`
- `docs/specs/vnext-public-api.md`
- `docs/specs/vnext-consumer-session.md`
- `libs/collabtext/tests/tst_membership.cpp`
- `libs/collabtext/tests/tst_undo_retirement.cpp`
- `libs/collabtext/tests/tst_session_recovery.cpp`
- `tests/consumer-smoke/CMakeLists.txt`
- `tests/consumer-smoke/main.cpp`
- `libs/collabtext/CMakeLists.txt`

**Steps:**

1. Persist enrollment/retirement/epoch events in per-producer immutable records using L01; validate them against G01 before changing eligible peer sets.
2. Publish per-stream undo-retirement state monotonically only after local trim is durable. A retained group containing old origins must block the corresponding floor; coalescing cannot conceal pins.
3. Expose a typed retirement-preview and explicit commit API identifying the affected device, documents/epochs, and merge consequences. Persist retirement only after an intentional commit call. Returning-device reconciliation preserves its local unsynced branch; A08 implements the reminder and confirmation UI.

**Acceptance:**

- Restart preserves membership and undo-retirement obligations; silence, advisory stale flags, and dismissal of reminders never remove a member.
- Retired peers cannot silently resume old-epoch mutation, and their unmerged operations remain exportable.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_membership|tst_undo_retirement|tst_persistent_undo|tst_session_recovery'`, `./tools/check-public-headers.sh`, and the relocated package/live consumer gate with `--components file-transport`; inject crashes before/after each publication point.

**Current implementation note:** Producer-scoped checksummed membership journals and per-stream monotonic undo-retirement journals hold process writer leases and have restart, torn-tail, cross-process contention, and uncertain-fsync tests. Membership journal append and recovery require contiguous transition epochs after the first recorded transition; append/reopen and checksummed skipped-epoch recovery regressions passed in Debug (20261010T164716Z-2), Release (20261010T164729Z-12), and ASan/UBSan with leak detection disabled for the ptrace-limited runner (20261010T164738Z-2). Membership descriptors now carry parent and transition hashes, base checkpoint hash, per-stream operation/delivery frontiers for all current members plus historical producers, sorted members, supported token versions, and translation-manifest hash; canonical content-hash/tamper tests passed `tst_membership` in Debug (20261010T171921Z-12), Release (20261010T171933Z-182), and ASan/UBSan (20261010T171948Z-2). This proves field integrity only; verifying the named transition and checkpoint against durable journal/checkpoint artifacts remains open. Enrollment and retirement commits reconcile exact uncertain publications by syncing the existing journal again before acknowledging success. Retirement publication failure injection covers write, file-sync, and directory-sync boundaries; identical retries become durable and conflicting retries are refused. These cases passed `tst_membership` in Debug (20261010T165551Z-2), Release (20261010T165605Z-12), and ASan/UBSan (20261010T165615Z-2). The integrated G02 gate (`tst_membership|tst_undo_retirement|tst_persistent_undo|tst_session_recovery`) passed 4/4 in Debug (20261010T165207Z-12), Release (20261010T165224Z-214), and ASan/UBSan (20261010T165236Z-2). The full 87-test CTest set passed after these changes in Debug (20261010T165917Z-2), Release (20261010T170025Z-2), and ASan/UBSan (20261010T170125Z-2, leak detection disabled due to the runner's ptrace restriction). Typed enrollment and retirement preview/commit APIs bind the current parent descriptor; enrollment requires all current members plus the joining device, while retirement binds consequence data, requires explicit confirmation, and requires all remaining members. A caller-supplied verifier validates durable acknowledgement evidence. A deterministic 48-epoch churn test exercises membership changes and restarts. Session/history integration has a crash regression: the session saves pending trim metadata before publication, retains its obligation through two failed sync barriers, and clears it only after an idempotent retry confirms durability. `DocumentSession::export_local_branch()` streams only the producer's original-scope retained deliveries, includes a durable pending operation when no segment was written, and rejects gaps or conflicting duplicate bytes; visitor errors and thrown exceptions stop export without consuming source state, verified by retry in Debug (20261010T164450Z-2), Release (20261010T164526Z-12), and ASan/UBSan (20261010T164549Z-2). It never rebases into a later epoch. The session suite passed in Debug (20261010T163904Z-2), Release (20261010T163923Z-12), and ASan/UBSan (20261010T163942Z-2). Installed consumers were revalidated against revision `aafb159`: relocated FileTransport package gate `debug-package-20261010T165735Z-2`; live Wave 16/16 strict plus Markdown/Markoff 4/4 gate `debug-live-20261010T165746Z-2`; both used manifest v11. Installed package and live consumers were revalidated against revision `93b2255`: FileTransport package gate `debug-package-20261010T172207Z-2`; Wave 16/16 strict plus Markdown/Markoff 4/4 live gate `debug-live-20261010T172222Z-2`, manifest v11. The public header inventory is synchronized. G02 remains open for linking descriptors to their actual transition records and validated checkpoint artifacts, production session/history retirement orchestration, old-epoch mutation refusal, and explicit returning-branch reconciliation in a later epoch.

## G03 — Implement per-stream durable coverage and certificate construction

**Status:** see TASKS.json (initially not started)  
**Depends on:** G02, L04, P03  
**Coverage:** F2, F3, F5  

**Files / scope:**

- `libs/collabtext/src/storage/CoverageStore.cpp`
- `libs/collabtext/src/crdt/StreamSync.cpp`
- `libs/collabtext/src/storage/GcCertificate.cpp`
- `libs/collabtext/tests/tst_gc_certificate.cpp`

**Steps:**

1. Publish only delivery-token coverage tied to durable local state; enforce contiguous producer envelope coverage separately from engine operation vectors. Include IdList streams.
2. Compute common coverage over the current certified epoch membership using true intersection. Missing/stale/unsupported peer data constrains eligibility; cache by membership hash and invalidate on epoch changes.
3. Construct an immutable internal certificate only after all G01 requirements are satisfied. Public callers cannot forge a certificate from a Global or integer.

**Acceptance:**

- Cross-stream counter reuse, silent members, deferred ops, callback absence, and stale checkpoint recovery cannot advance eligibility.
- A certified frontier references exact coverage and checkpoint artifacts and can be independently revalidated after restart.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_gc_certificate|tst_transport_commit_contract|tst_membership'`; compare with GcModel on adversarial traces.

## G04 — Implement full engine checkpoint serialization

**Status:** see TASKS.json (initially not started)  
**Depends on:** S06, L06, P08  
**Coverage:** F5, F7, R10  

**Files / scope:**

- `libs/collabtext/src/storage/CheckpointCodec.cpp`
- `libs/collabtext/src/storage/CheckpointCoverageCodec.cpp`
- `libs/collabtext/src/storage/CheckpointCodec.h`
- `libs/collabtext/src/storage/CheckpointDeferredCodec.cpp`
- `libs/collabtext/src/storage/CheckpointEngineCodec.cpp`
- `libs/collabtext/src/storage/CheckpointPinsCodec.cpp`
- `libs/collabtext/src/storage/CheckpointTranslationCodec.cpp`
- `libs/collabtext/src/storage/CheckpointLocalCodec.cpp`
- `libs/collabtext/src/storage/CheckpointMembershipCodec.cpp`
- `libs/collabtext/src/DocumentSession.cpp`
- `libs/collabtext/include/collabtext/DocumentSession.h`
- `docs/specs/vnext-checkpoint.md`
- `libs/collabtext/CMakeLists.txt`
- `libs/collabtext/src/crdt/Buffer.cpp`
- `libs/collabtext/src/crdt/IdList.cpp`
- `libs/collabtext/tests/fuzz_decode.cpp`
- `libs/collabtext/tests/tst_checkpoint_codec.cpp`
- `libs/collabtext/tests/tst_session_recovery.cpp`
- `tests/consumer-smoke/main.cpp`

**Steps:**

1. Specify local versus shareable checkpoint sections. Capture logical tree runs/entries, origin/boundary state, visibility derivation, UndoMap, causal/version/counter state, deferred input, pins, and coverage. Local actor undo/redo groups and private writer state belong in local sections.
2. Rebuild derived indexes rather than serializing raw tree pointers or object layouts. Validate snapshot invariants and limits before exposing restored engines.
3. Reject incomplete/corrupt/wrong-epoch checkpoints and fall back to retained replay data with an explicit diagnostic. Include checkpoint format version and integrity hash.

**Acceptance:**

- Round-trip engines produce identical future edits, undo/redo, anchors, and convergence after receiving more remote operations.
- Malformed checkpoints never partially replace live state; shared import does not adopt another actor's local undo stack or writer identity.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_checkpoint_codec|tst_sequence_model|tst_persistent_undo'`; fuzz bounded checkpoint input under ASan.

## G05 — Publish checkpoints atomically and accelerate recovery

**Status:** see TASKS.json (initially not started)  
**Depends on:** G04, L08  
**Coverage:** F5, R5, R10  

**Files / scope:**

- `libs/collabtext/src/storage/CheckpointStore.cpp`
- `libs/collabtext/src/DocumentSession.cpp`
- `libs/collabtext/tests/tst_checkpoint_recovery.cpp`

**Steps:**

1. Write/check durable checkpoint contents, then atomically publish its pointer and coverage using L01. Never advance a reader cursor ahead of a recoverable local checkpoint/log set.
2. On startup choose a validated checkpoint, replay uncovered own/remote records, restore Q3 local history, and rebuild counters/indexes. Verify full replay and checkpoint replay give equivalent future behavior.
3. Retain previous checkpoints and covered logs until G06 certifies deletion. Support threshold configuration and manual creation without requiring SQLite.

**Acceptance:**

- Crash at each publication point recovers old or new complete state; missing/corrupt newest checkpoint falls back safely.
- Recovery time reduction is measured separately from correctness; reader cursors cannot cause lost operations after rollback.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_checkpoint_recovery|tst_store_lifecycle|tst_session_recovery'`; compare full replay and checkpoint bootstrap fixtures.

## G06 — Implement certificate-based tombstone and UndoMap reclamation

**Status:** see TASKS.json (initially not started)  
**Depends on:** G03, G05, S05, V03  
**Coverage:** F1, F2, F3, F7, R7, R10  

**Files / scope:**

- `libs/collabtext/src/crdt/ReclamationPolicy.h`
- `libs/collabtext/src/crdt/Buffer.cpp`
- `libs/collabtext/src/crdt/IdList.cpp`
- `libs/collabtext/src/crdt/UndoMap.cpp`
- `libs/collabtext/tests/tst_gc.cpp`
- `libs/collabtext/tests/tst_idlist_gc.cpp`
- `docs/specs/vnext-external-position-contract.md`

**Steps:**

1. Remove public compact(Global) as a destructive entry point and expose DocumentSession-managed certificate reclamation only; no compatibility wrapper is required.
2. Share eligibility policy across Buffer/IdList, not their entire storage implementations. Protect local and remote undo/redo, insertions without deletion IDs, and pinned anchors; retain boundary stubs/translation entries where the approved algorithm requires them.
3. Reclaim UndoMap entries only when no retained item/history/pin or supported historical query requires them. Maintain the documented edits_since floor. Count content, metadata, indexes, and retention bytes separately.
4. Enforce W3 pinned_token_survives_checkpoint_epoch_translation using the original encoded bytes across certified reclamation. Without necessary history/translation return typed unavailable/wrong-epoch/unresolvable results, never fabricated end. Test released and unregistered tokens where reclamation actually removes their context; absence of registration alone does not invalidate context that still exists.

**Acceptance:**

- All local/remote restoration cases remain correct after GC; no missing anchor jumps to end; both primitives obey the same policy.
- Memory reclamation includes eligible undo metadata and never relies only on local stack protection.
- Pinned external tokens remain resolvable through approved GC/epoch transitions; unsupported unprotected historical tokens fail explicitly after their required context is removed.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_gc|tst_idlist_gc|tst_anchor_retention|tst_gc_certificate|tst_evaluation_regressions'`; differential-check permitted post-GC operations against unreclaimed engines. Verify the W-series named cases assigned above at this task's implementation layer and record their evidence; later dependent cases remain pending.

## G07 — Reclaim covered log segments and old checkpoints safely

**Status:** see TASKS.json (initially not started)  
**Depends on:** G06  
**Coverage:** F5, F6, R10  

**Files / scope:**

- `libs/collabtext/src/storage/StoreCompactor.cpp`
- `libs/collabtext/src/storage/CheckpointStore.cpp`
- `libs/collabtext/tests/tst_store_compaction.cpp`

**Steps:**

1. Delete only sealed segments fully covered by the validated shareable/local checkpoint and G01 certificate; never delete live tails or logs needed by an enrolled peer's future operations.
2. Retain required checkpoint chains, epoch translation records, retirement data, and unsupported/quarantined records. Use durable deletion bookkeeping so interrupted cleanup is idempotent.
3. Add dry-run inventory listing eligible and protected artifacts with reasons. Returning stale peers follow G01 rescue/bootstrap semantics instead of silently omitting their unsynced edits.

**Acceptance:**

- Repeated compaction/restart produces the same recoverable logical state; no checkpoint pointer references deleted data.
- Disk usage plateaus in an eligible workload, while an offline/pinned workload reports exactly why reclamation is blocked.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_store_compaction|tst_checkpoint_recovery|tst_store_lifecycle'`; inject interruption at every deletion/publish boundary.

## G08 — Validate long-lived editing and reclaimable memory/disk bounds

**Status:** see TASKS.json (initially not started)  
**Depends on:** G07, S07, V05  
**Coverage:** F1, F2, F3, F7, R4, R7, R10  

**Files / scope:**

- `libs/collabtext/tests/tst_long_lived_store.cpp`
- `docs/reports/vnext-reclamation-milestone.md`
- `docs/specs/vnext-external-position-contract.md`

**Steps:**

1. Exercise many edit/undo/redo/coalesce/trim cycles, durable comments, offline devices, explicit retirement, checkpointing, and reconnect with both CRDT primitives.
2. Measure engine allocations, UndoMap/history/pin bytes, checkpoint sizes, retained logs, and RSS separately. Demonstrate a plateau only in workloads meeting retirement/retention assumptions.
3. Publish final GC invariants, blocked-case diagnostics, seed/trace identities, and limits; do not promise history-free arbitrary offline return after retirement.
4. Run the W3 consumer_commit_pin_crash_matrix, external_pin_recovery_before_gc, retire_device_preserves_live_reply_pin, pinned_token_survives_checkpoint_epoch_translation, and unregistered_or_released_token_is_explicit campaigns against both primitives. Measure the retention cost of indefinitely live reply pins and separate it from reclaimable text and bounded undo.

**Acceptance:**

- Logical state and permitted future operations match unreclaimed reference engines throughout the trace.
- Eligible workloads have bounded retained state; ineligible workloads preserve correctness and report unbounded obligations rather than deleting them.
- Every external-pin crash prefix either recovers a protected live reference or blocks GC explicitly. Retiring an author does not discard live reply references; only eligible workloads claim a plateau.

**Verification:**

Run bounded long-lived Debug/Release campaigns and ASan; publish counts and actual byte metrics, with explicit campaign time/memory budgets. Verify the W-series named cases assigned above at this task's implementation layer and record their evidence; later dependent cases remain pending.


**G02 follow-up (2026-10-10):** CME2 transition records now bind a parent descriptor hash, and `MembershipStore::validate_transition_link()` matches the exact durable event and membership delta. `validate_transition()` is the fail-closed path: it requires caller-supplied persisted-evidence and durable-checkpoint verifiers after event linkage. Tests prove callback ordering and refusal/exception handling only; there is still no persisted-evidence decoder or shareable checkpoint codec/store to verify those artifacts and represented frontiers. A `DocumentSession` mutation permit can hold a consumer coordinator lock through durable local publication; without a configured callback the session remains uncoordinated. The mutex-backed retirement/write race test passed Debug, Release, and ASan/UBSan, including 20 consecutive ASan session-suite repetitions. A real consumer coordinator, checkpoint artifact validator, retirement/history orchestration, and returning-branch reconciliation remain open.

**G02 follow-up (2026-10-11):** Recovery now applies the same live membership authorization to a durable pending local edit before publishing it into its original epoch. If authorization is denied, the session opens read-only, does not append the old-epoch delivery, and keeps its exact pending payload available through `export_local_branch()`; a later authorized `recover()` retries publication and restores the local undo snapshot. Regression `retired_actorDoesNotPublishPendingCommitIntoOldEpoch` verified that a second session sees no old-epoch delivery while authorization is denied, then verified successful retry after authorization. `DocumentSessionConfig::authorize_inbound_delivery` now lets consumers validate every inbound delivery during startup replay and polling, returning a nonnull lease that keeps membership stable through application. Denied envelopes are quarantined without being applied, and later authorized recovery can replay the retained log. Regression `inboundRetiredEpochDeliveryIsQuarantinedBeforeApply` verified rejection, byte-preserving quarantine, and replay after reopening with authorization. The full `tst_session_recovery` target passed in Debug (`20261011T043755Z-2`), Release (`20261011T043822Z-2`), and ASan/UBSan with `ASAN_OPTIONS=detect_leaks=0` (`20261011T043853Z-2`). The standalone public-header object target compiled successfully with `CCACHE_DISABLE=1`; LeakSanitizer without that setting ran all 31 cases but failed afterward because the runner does not support LeakSanitizer under ptrace. Old-epoch write prevention now covers live local mutations, crash-recovery publication, and inbound deliveries when the consumer configures its membership callbacks; descriptor/checkpoint artifact verification, consumer-owned retirement/history orchestration, and protocol-level returning-branch reconciliation remain open.
