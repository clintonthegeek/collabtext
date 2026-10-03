# 05. Snapshots, membership, and safe reclamation

Goal: bound recoverable history only after proving that permitted future operations cannot require removed state. Durable receipt is necessary but insufficient. GC stays disabled until every certificate condition passes. Conservative blocking is preferable to guessed peer retirement.

Read [execution rules](EXECUTION.md) and [decisions](DECISIONS.md) before starting. Task dependencies refer to the [master task registry](TASKS.json). New file paths below are intentional; do not assume they already exist.

## G01 — Specify membership epochs and reclamation certificates

**Status:** see TASKS.json (initially not started)  
**Depends on:** D01, S06, L08, P07  
**Coverage:** F1, F2, F3, F7, R10  
**Design/decision gate:** Q2/Q3 resolved; protocol must pass independent technical review and executable-model checks. Incomplete certificates never unlock physical GC.  

**Files / scope:**

- `docs/specs/vnext-membership-gc.md`
- `libs/collabtext/tests/reference/GcModel.h`

**Steps:**

1. Define immutable epoch membership, explicit enrollment/retirement events, producer identities, and how new actors join without invalidating an already published common frontier. Q2 chooses the retirement policy; absence/presence timeout is never itself an acknowledgement.
2. Define a certificate binding stream, epoch, membership hash, common durable coverage, per-actor undo-retirement coverage, retained anchor translations, and durable checkpoint hash. Require all still-enrolled actors, including silent ones, to contribute; no scalar-max substitution.
3. Specify the full state machine for checkpoint proposal, coverage confirmation, epoch transition, interruption, competing proposals, and returning retired peers. Preserve a returning peer's unsynced branch for explicit reconciliation; do not pretend replay into a new epoch is automatically valid.
4. Implement a slow model and complete eligibility predicate. Include no available peers, unknown actor, missing ack, restored stale ack, remote redo, concurrent enrollment, retirement, and checkpoint corruption.

**Acceptance:**

- There is a complete implementable protocol; every physical deletion has a certificate and no permitted future undo/anchor request needs removed data.
- If a live actor retains a relevant undo group or is offline, GC blocks unless the selected explicit retirement/epoch policy safely removes that obligation.

**Verification:**

Run GcModel against all F1/F2/F3 histories and adversarial membership traces; publish the eligibility predicate and state transition table before G02 starts.

## G02 — Implement durable membership and undo-retirement publication

**Status:** see TASKS.json (initially not started)  
**Depends on:** G01, L06  
**Coverage:** F1, F2, R10  

**Files / scope:**

- `libs/collabtext/src/storage/MembershipStore.cpp`
- `libs/collabtext/src/storage/RetentionState.cpp`
- `libs/collabtext/tests/tst_membership.cpp`
- `libs/collabtext/tests/tst_undo_retirement.cpp`

**Steps:**

1. Persist enrollment/retirement/epoch events in per-producer immutable records using L01; validate them against G01 before changing eligible peer sets.
2. Publish per-stream undo-retirement state monotonically only after local trim is durable. A retained group containing old origins must block the corresponding floor; coalescing cannot conceal pins.
3. Require explicit confirmation for any product action retiring another device because that action changes later merge guarantees; show concrete affected store/actor and rescue instructions in the application, not an automatic heartbeat eviction.

**Acceptance:**

- Restart preserves membership and undo-retirement obligations; silence never removes a member on the recommended policy.
- Retired peers cannot silently resume old-epoch mutation, and their unmerged operations remain exportable.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_membership|tst_undo_retirement|tst_persistent_undo'`; inject crashes before/after each publication point.

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
- `libs/collabtext/src/crdt/Buffer.cpp`
- `libs/collabtext/src/crdt/IdList.cpp`
- `libs/collabtext/tests/tst_checkpoint_codec.cpp`

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

**Steps:**

1. Replace public compact(Global) as a destructive entry point with DocumentSession-managed certificate reclamation; retain a clearly deprecated harmless compatibility wrapper if useful.
2. Share eligibility policy across Buffer/IdList, not their entire storage implementations. Protect local and remote undo/redo, insertions without deletion IDs, and pinned anchors; retain boundary stubs/translation entries where the approved algorithm requires them.
3. Reclaim UndoMap entries only when no retained item/history/pin or supported historical query requires them. Maintain the documented edits_since floor. Count content, metadata, indexes, and retention bytes separately.

**Acceptance:**

- All local/remote restoration cases remain correct after GC; no missing anchor jumps to end; both primitives obey the same policy.
- Memory reclamation includes eligible undo metadata and never relies only on local stack protection.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_gc|tst_idlist_gc|tst_anchor_retention|tst_gc_certificate|tst_evaluation_regressions'`; differential-check permitted post-GC operations against unreclaimed engines.

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

**Steps:**

1. Exercise many edit/undo/redo/coalesce/trim cycles, durable comments, offline devices, explicit retirement, checkpointing, and reconnect with both CRDT primitives.
2. Measure engine allocations, UndoMap/history/pin bytes, checkpoint sizes, retained logs, and RSS separately. Demonstrate a plateau only in workloads meeting retirement/retention assumptions.
3. Publish final GC invariants, blocked-case diagnostics, seed/trace identities, and limits; do not promise history-free arbitrary offline return after retirement.

**Acceptance:**

- Logical state and permitted future operations match unreclaimed reference engines throughout the trace.
- Eligible workloads have bounded retained state; ineligible workloads preserve correctness and report unbounded obligations rather than deleting them.

**Verification:**

Run bounded long-lived Debug/Release campaigns and ASan; publish counts and actual byte metrics, with explicit campaign time/memory budgets.
