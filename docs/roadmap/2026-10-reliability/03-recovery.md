# 03. Durable storage and correct recovery

Goal: make ordinary edit/save/close/reopen and interrupted delivery correct before reclaiming history. Begin with full own-and-remote replay; acceleration comes later. Application state and delivery cursors must be recovered as one coverage boundary.

Read [execution rules](EXECUTION.md) and [decisions](DECISIONS.md) before starting. Task dependencies refer to the [master task registry](TASKS.json). New file paths below are intentional; do not assume they already exist.

## L01 — Implement checked durable filesystem primitives

**Status:** see TASKS.json (initially not started)  
**Depends on:** P01, V07  
**Coverage:** F9, R13  

**Files / scope:**

- `libs/collabtext/src/storage/DurableFile.h`
- `libs/collabtext/src/storage/DurableFile.cpp`
- `libs/collabtext/tests/tst_durable_file.cpp`

**Steps:**

1. Provide checked append/write-all, file sync, atomic replacement, parent-directory sync, close, and process lease primitives behind Linux/Windows/macOS adapters. Define supported durability guarantees and unsupported-operation errors.
2. Never delete the old destination before replacement. Check short writes, sync failures, rename failures, and cleanup failures; retain the last durable source and return StoreError.
3. Use injected filesystem hooks from V07; add real-platform subprocess smoke tests without claiming a kill test simulates power loss.

**Acceptance:**

- The documented write→file-sync→replace→directory-sync order is observable in fault tests.
- Failed writes cannot be reported as committed; atomic replacement preserves either old or new complete data.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_durable_file|tst_store_fault_harness'`; test the platform matrix established by A07.

## L02 — Make segment writer transactions durable and recoverable

**Status:** see TASKS.json (initially not started)  
**Depends on:** L01, P02, P08  
**Coverage:** F4, F9, R10  

**Files / scope:**

- `libs/collabtext/src/crdt/SegmentWriter.h`
- `libs/collabtext/src/crdt/SegmentWriter.cpp`
- `libs/collabtext/tests/tst_segment_writer.cpp`
- `libs/collabtext/CMakeLists.txt`
- `libs/collabtext/src/crdt/FileSync.h`
- `libs/collabtext/src/crdt/FileSync.cpp`
- `libs/collabtext/src/crdt/StreamSync.cpp`
- `libs/collabtext/tests/segment_writer_crash_worker.cpp`
- `libs/collabtext/tests/tst_filesync.cpp`
- `libs/collabtext/tests/tst_segment_reader.cpp`
- `libs/collabtext/tests/tst_evaluation_regressions.cpp`

**Steps:**

1. Implement checked append/flush/durable-commit/seal results. fsync_count counts actual successful durable-sync calls only; pending records are cleared only after the chosen durability boundary succeeds.
2. Seal using durable temporary contents, checked atomic publish, directory sync, then removal of the covered tail. Recover interrupted seals without silently dropping records; reconstruct header first/last counter ranges and seal deadlines from recovered records.
3. Define destructor policy as best-effort cleanup with explicit close required for reportable errors; use DocumentSession explicit close. Bound tail buffers and expose backpressure.

**Acceptance:**

- Every V07 seal/append crash point recovers each committed record exactly logically once; uncommitted records may survive and are deduplicated by identity.
- No failed write removes the only valid tail; recovered header metadata is correct.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_segment_writer|tst_durable_file'` plus seal subprocess crash cases in Debug and Release.

## L03 — Implement gap-aware transactional segment reads

**Status:** see TASKS.json (initially not started)  
**Depends on:** L02, P07  
**Coverage:** F6  

**Files / scope:**

- `libs/collabtext/src/crdt/SegmentReader.h`
- `libs/collabtext/src/crdt/SegmentReader.cpp`
- `libs/collabtext/tests/tst_segment_reader.cpp`
- `libs/collabtext/src/crdt/FileSync.cpp`
- `libs/collabtext/src/crdt/StreamSync.cpp`
- `libs/collabtext/tests/tst_evaluation_regressions.cpp`

**Steps:**

1. Replace one highest-segment cursor with an explicit contiguous committed frontier and bounded read state. Safest baseline: stop at missing/invalid predecessors; do not acknowledge later segments past a gap.
2. Treat open tails as provisional prefixes and sealed segments as authoritative equivalents. Store content identity/offset so an atomic tail replacement, truncation, repaired record, or seal transition cannot skip unseen records.
3. Return records plus delivery tokens without committing merely because they were read. Persist committed coverage only via L01 after recoverable consumer state exists; preserve unsupported/corrupt input and report the gap.

**Acceptance:**

- Segment 2 before 1, repaired invalid 1, partial line, replaced tail, and open→sealed duplicate replay lose no logical operations.
- Reader restart returns uncommitted records again; commit cannot advance past an unvalidated record. F6 expected-fail marker is removed.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_segment_reader|tst_evaluation_regressions'`; run all arrival permutations of a three-segment fixture.

## L04 — Persist stream envelope identity and implement transport outcomes

**Status:** see TASKS.json (initially not started)  
**Depends on:** L03, P06b, P07  
**Coverage:** F4, F5, R9  

**Files / scope:**

- `libs/collabtext/src/crdt/StreamSerialization.cpp`
- `libs/collabtext/src/crdt/StreamSync.cpp`
- `libs/collabtext/include/collabtext/StreamSync.h`
- `libs/collabtext/tests/tst_opstream_replay.cpp`
- `libs/collabtext/tests/tst_transport_commit_contract.cpp`

**Steps:**

1. Generate persistent per-actor/per-stream envelope sequence numbers from the durable producer log; recover them under the writer lease before accepting push. ID allocation survives sender-only restart.
2. Handle unknown stream, invalid payload, callback absence, callback failure, deferred apply, and poll budget limits via explicit outcomes. Retain rejected records; do not overwrite progress to hide failures.
3. Use delivery token commits and per-stream coverage from P07. Deduplicate envelopes and operations separately. Cache ack writes and skip unchanged coverage; do not implement destructive GC here.

**Acceptance:**

- F4 sender-restart reproduction delivers b to the already-running receiver; no-op polling does not rewrite ack files.
- Buffer and IdList streams with reused counters have independent coverage; missing callbacks cannot ack.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_transport_commit_contract|tst_opstream_replay|tst_opstream_idempotent|tst_evaluation_regressions'` and multi-stream tests.

## L05 — Add a full-replay DocumentSession as the recovery baseline

**Status:** see TASKS.json (initially not started)  
**Depends on:** L04, V03  
**Coverage:** F5, R3  

**Files / scope:**

- `libs/collabtext/include/collabtext/DocumentSession.h`
- `libs/collabtext/src/DocumentSession.cpp`
- `libs/collabtext/src/crdt/FileSync.cpp`
- `libs/collabtext/tests/tst_session_recovery.cpp`
- `docs/specs/vnext-external-position-contract.md`
- `docs/specs/vnext-consumer-session.md`

**Steps:**

1. Own the engines, producer lease, log, and transport binding in one single-thread owner. Opening loads seed/context, replays ALL own and remote durable logs, and drains dependencies; do not use previously persisted incremental cursors to bootstrap empty state. This is an optional reference composition: direct headless Buffer/IdList operations, codecs and custom OpStream ownership remain public; a Wave/Markoff consumer is not required to route through this file-backed session.
2. Recover the own actor clock from accepted own operations before enabling new local edits. Missing causal data remains a visible recovery/incomplete state; never render it as complete and acknowledge it.
3. After baseline replay, derive incremental read state from the recovered coverage and keep it paired with the recoverable logs. Expose open/recover/apply/save/close results and logical visible deltas.
4. Define W5 headless embedding under a consumer-owned Document seam: engine operations/pins are idempotent projections of canonical events, local journals/outboxes are recoverable pending state, and accepted event history is never rewound. Specify server rejection and pending-outbox reconciliation without ID reuse. Support read-only historical projection without minting operations; after S05, recover external pins to a declared consumer frontier before any GC can run. Add the consumer_commit_pin_crash_matrix recovery hooks here; S05/G08 complete its pin and GC cases.

**Acceptance:**

- Own-only, mixed-peer, duplicate, reordered, copied, and restart fixtures recover exact CRDT state, not just visible text.
- F5 receiver-restart failure passes; stale cursors cannot suppress recovery. Opening does not manufacture fresh operations or callbacks for replayed local edits.
- Headless recovery tests cover canonical replay, pending outbox restart/reconciliation, and isolated historical playback without allocating new IDs. The session exposes the recovery frontier needed for subsequent S05 pin projection; per-session save is not advertised as a multi-block conversation transaction.
- The reference session is optional. A headless consumer can own its canonical log, per-block engines, block lifecycle, transport and cross-block history while using public primitive APIs.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_session_recovery|tst_evaluation_regressions|tst_opstream_replay'`; restart the same temp store twice and continue editing after each open. Verify the W-series named cases assigned above at this task's implementation layer and record their evidence; later dependent cases remain pending.

## L06 — Persist local undo policy and bounded history events

**Status:** see TASKS.json (initially not started)  
**Depends on:** L05, D01  
**Coverage:** F1, F2, R10  
**Design/decision gate:** Q3 resolved: persistent actor-local undo AND redo, bounded by step and byte limits.  

**Files / scope:**

- `libs/collabtext/src/storage/LocalHistoryLog.cpp`
- `libs/collabtext/src/crdt/Buffer.cpp`
- `libs/collabtext/src/crdt/IdList.cpp`
- `libs/collabtext/tests/tst_persistent_undo.cpp`
- `docs/specs/vnext-external-position-contract.md`

**Steps:**

1. Durably record local undo groups, coalescing boundaries, cursor metadata, redo truncation, and retirement/trim events with their operation context; replay into only the owning actor's local history.
2. Restore both undo and redo stacks after reopen/crash without resetting them at session close. Make history and operation commits a consistent transaction; a shared checkpoint must not replace actor-private history. Record retention obligations for G01/G03.
3. Bound history by steps AND retained bytes, including coalesced entries, inserted keys, and restoration content. Make trim atomic with its retirement record; never drop history silently on allocation/error.
4. Expose durable compensating operations and stable local undo group metadata to a consumer-owned append-only event log (W5). Specify event correlation and failure/retry ordering so undo is a new event and accepted canonical events are never deleted. Keep multi-block history orchestration in the consumer layer.

**Acceptance:**

- Undo and redo survive close/reopen/crash within the configured bounds; restored undo reverses only the owner's edits and preserves peer edits.
- A single huge coalesced edit respects the memory limit; retirement survives crash and never re-enables older undo.
- Reopened undo/redo emits idempotently correlated compensating operations while preserving canonical accepted history; engine history limits do not implicitly release external reply pins.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_persistent_undo|tst_idlist_undo|tst_undomap|tst_session_recovery'`; test undo→close→reopen→redo and trim crash points. Verify the W-series named cases assigned above at this task's implementation layer and record their evidence; later dependent cases remain pending.

## L07 — Wire the dogfood editor to the supported durable session

**Status:** see TASKS.json (initially not started)  
**Depends on:** L06  
**Coverage:** F5, F9, R1, R3  

**Files / scope:**

- `app/collabedit/CollabPane.h`
- `app/collabedit/CollabPane.cpp`
- `app/collabedit/Document.cpp`
- `app/collabedit/Document.h`
- `libs/collabtext/tests/tst_editor_recovery.cpp`

**Steps:**

1. Replace direct Buffer/FileSync startup ownership with DocumentSession; preserve surgical deltas, scroll behavior, presence, and side streams. Replace the 1–250 allocator with P02 actor state.
2. Define Save as durable operation/history commit followed by checked atomic materialized-view replacement; never report success when log commit fails. Expose errors and dirty state without engine internals in the user flow.
3. Use explicit session close with surfaced errors; ensure application shutdown invokes it once. Read-only recovery/incompatible stores must not accept accidental edits.

**Acceptance:**

- Actual editor open→type→save→close→open retains own/remote edits plus both local undo and redo stacks.
- Failed saves preserve old data and modified state; shutdown does not discard unflushed operations.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_editor_recovery|tst_scroll_stability|tst_collab_plain_text_edit'`; offscreen integration exercises actual Document/CollabPane, not only a fake wrapper.

## L08 — Prove the recovery milestone with restart/arrival matrices

**Status:** see TASKS.json (initially not started)  
**Depends on:** L07, V05  
**Coverage:** F4, F5, F6, F9, R4  

**Files / scope:**

- `libs/collabtext/tests/tst_store_lifecycle.cpp`
- `libs/collabtext/tests/tst_transport_commit_contract.cpp`
- `docs/reports/vnext-recovery-milestone.md`

**Steps:**

1. Enumerate sender restart, receiver restart, both restart, missing older segment, repaired corruption, blocked callback, failed durable commit, open→seal transition, and two competing writers for both operation kinds.
2. Run simulated sync between physically separate temporary directory trees; copy files in controlled orders, including manifest-before-seed and segment-delete-before-sealed-arrival. Do not assume shared-memory directory visibility models Syncthing.
3. Compare recovered versions, logical content, local history policy, and coverage. Record all outstanding defects explicitly; do not enable GC at this milestone.

**Acceptance:**

- All committed edits eventually appear exactly logically once after full recovery; unsupported/corrupt stores remain visibly incomplete rather than silently losing records.
- The recovery report cites seed/fixture hashes and Debug/Release/sanitizer results.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_store_lifecycle|tst_session_recovery|tst_transport_commit_contract'`, Release equivalents, and the bounded fault corpus under ASan.
