# OpStream Extraction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract a transport-agnostic `OpStream` boundary on the public collabtext surface, with `StreamSync` adopting that boundary as the file-backed reference implementation. Surface CRDT op emission, op application, and op (de)serialisation through the public API so consumers (Markoff D5; future direct-channel transports) can wire collabtext under arbitrary transports.

**Architecture:**

- New public header `include/collabtext/OpStream.h` defines the four-method interface (`push` / `set_on_inbound` / `lowest_peer_acked_lamport` / `set_on_ack_update`).
- `StreamSync` is promoted from `src/crdt/` to `include/collabtext/`, implements `OpStream`, gains per-entry/producer-aware inbound callbacks and per-peer ack-frontier publication.
- `CrdtEngine` and `IdList` gain public `setOnLocalOp(callback<Operation>)` and `applyRemoteOp(Operation)` methods. Internals already exist inside `Buffer`; this is pImpl plumbing for `Buffer` and a new public surface for `IdList`.
- `Serialization.h` promotes from `src/crdt/` to `include/collabtext/`. `Operation`, `IdListOperation`, `Lamport` types become public **with a narrow written contract** (Form 2 per joint-design): only `lamport()` accessors and round-trip via `encode_*`/`decode_*` are stable. Field layout is evolution-reserved. Internal types (`Fragment`, `Anchor`, `Locator`, `Global`, op-variant tags) stay in `src/crdt/` and are not part of the public contract even where transitively included.
- Per-peer ack-frontier lives in `replicas/{replica}/acks.json` (sibling to presence, not extending it). Format and four hard requirements per joint-design outcomes §2.
- `SyncManager` is **not** modified beyond what's needed to keep its existing tests green. It continues to exist as a convenience facade.
- `app/collabedit/` and `app/testapp/` continue to compile via a typedef shim if needed; deeper migration deferred until widget lab resumes.

**Tech Stack:** C++20, Qt6 Test (test runner only — engine itself stays Qt-free), CMake 3.19+, the existing `nlohmann/json` for op serialisation.

**Binding spec:** `docs/specs/2026-05-08-d5-negotiation-response.md`. The "What we won't do" section is load-bearing — do not expand the work beyond what's listed there.

**Joint-design outcomes:** `docs/handoff/2026-05-08-d5-joint-design-outcomes.md`. The two design calls (public type-surface form, ack-frontier file format) are resolved there. **Read it before starting Phase 1.** The plan below references its decisions concretely; the outcomes doc carries the reasoning.

**Companion / consumer-side reference (in the Markoff repo, not this one):** `~/dev/Markoff/.worktrees/foundation-exploration/docs/specs/2026-05-07-d5-collab-activation-design.md` §4.1 (the `ITransport` shape we mirror) and §4.3 (the wiring diagram). Markoff's incoming positions on the joint-design calls: `~/dev/Markoff/.worktrees/foundation-exploration/docs/handoff/2026-05-08-collabtext-joint-design-positions.md`. Both are required reading for Phase 1; if the Markoff repo isn't available locally, request it before starting.

**Sequencing constraint:** This plan starts after IdList β (`docs/superpowers/plans/2026-05-04-idlist-implementation.md`) reaches its acceptance gate. **β was accepted on 2026-05-08** (commit `3ead04b`); this plan is now ready to execute.

---

## File Structure

### New files

| Path | Responsibility |
|---|---|
| `libs/collabtext/include/collabtext/OpStream.h` | The transport-agnostic interface. Pure abstract; no Qt dependency. |
| `libs/collabtext/include/collabtext/Serialization.h` | Promoted from `src/crdt/Serialization.h`; public encoders/decoders. |
| `libs/collabtext/include/collabtext/StreamSync.h` | Promoted from `src/crdt/StreamSync.h`; the file-backed `OpStream` reference impl. |
| `libs/collabtext/include/collabtext/Operations.h` | Promoted/distilled public view of `Operation`, `EditOperation`, `UndoOperation`, `Fragment`, `Lamport`, `Anchor`, `Global` declarations sufficient to round-trip. |
| `libs/collabtext/include/collabtext/IdListOperations.h` | Promoted public view of `IdListOperation` variants. |
| `libs/collabtext/tests/tst_opstream_interface.cpp` | Compliance fixture: any `OpStream` impl must pass these. |
| `libs/collabtext/tests/tst_opstream_streamsync.cpp` | `StreamSync`-as-`OpStream` two-replica convergence + ack-frontier. |
| `libs/collabtext/tests/tst_opstream_partition.cpp` | Ack-frontier under partition / reconnect / silent peer. |
| `docs/CRDT_TRANSPORT_SPEC.md` | Public-API spec for `OpStream` + `StreamSync` semantics, including ack-frontier file format. |

### Modified files

| Path | Reason |
|---|---|
| `libs/collabtext/CMakeLists.txt` | Install new public headers; add new test targets. |
| `libs/collabtext/src/crdt/Serialization.{h,cpp}` | Header becomes a thin re-include of the public version (or moves entirely); .cpp updated for any namespacing/include changes. |
| `libs/collabtext/src/crdt/StreamSync.cpp` | Header path adjusts; per-entry callback added; ack-frontier publish/read added. |
| `libs/collabtext/include/collabtext/CrdtEngine.h` | Add `setOnLocalOp`, `applyRemoteOp` methods. |
| `libs/collabtext/src/CrdtEngine.cpp` | Plumb the new methods through pImpl to the underlying `Buffer`. |
| `libs/collabtext/src/crdt/IdList.h` (or `.cpp`) | Add `setOnLocalOp`, `applyRemoteOp` to `IdList`'s public surface. |
| `libs/collabtext/src/crdt/Operations.h` | Either moves to public or stays as internal-detail header included by the public one. |
| `libs/collabtext/src/crdt/IdListOperations.h` | Same as above. |
| `libs/collabtext/tests/tst_stream_sync.cpp` | Adopt the new per-entry callback signature. |
| `app/collabedit/CollabPane.{h,cpp}` | Update include path for `StreamSync.h`; otherwise no change. |
| `app/testapp/main.cpp` | Same. |
| `docs/CRDT_SYNC_SPEC.md` | Cross-reference new transport spec; mark sections superseded. |
| `docs/ARCHITECTURE.md` | Note `OpStream` as the public transport boundary. |
| `README.md` | One paragraph noting the public op-stream surface (post-ship). |

### Untouched

`Buffer.{h,cpp}` — internal already correct; only public exposure changes upstream of it. `CollabDocument` — Buffer-bound by D-evolution scope; not touched. `Identity::*` — already opt-in by going around `SyncManager`; no changes here. `NetworkSim` — stays as our internal convergence test rig; no public elevation. `direct-channel-interface-design.md` — separate roadmap track; this plan does not implement direct channels.

---

## Phase 1 — Public op surface on engines

Goal: `CrdtEngine` (Buffer-side) and `IdList` emit local ops to a callback and accept remote ops via a method, with a public encode/decode round-trip. No transport involvement yet.

### Task 1.1: Promote `Serialization.h` and op types to public

**Files:**
- New: `libs/collabtext/include/collabtext/Serialization.h`
- New: `libs/collabtext/include/collabtext/Operations.h`
- New: `libs/collabtext/include/collabtext/IdListOperations.h`
- Modified: `libs/collabtext/src/crdt/Serialization.{h,cpp}`
- Modified: `libs/collabtext/src/crdt/Operations.h`
- Modified: `libs/collabtext/src/crdt/IdListOperations.h`
- Modified: `libs/collabtext/CMakeLists.txt`

**Form 2 contract (per joint-design outcomes §1):** `Operation`, `IdListOperation`, and `Lamport` types become publicly visible as full structs. **Stable contract = `lamport()` accessors + round-trip via `encode_*`/`decode_*` only.** Field layout, op-variant tags, and any transitively-included internal types (`Fragment`, `Anchor`, `Locator`, `Global`) are evolution-reserved and must NOT be relied on by consumers. The narrow contract is enforced by documentation, not by language-level access control.

- [ ] **Step 1: Decide the header partitioning.** One of: (a) `Operations.h` exposes `Operation` + `Lamport`, `IdListOperations.h` exposes `IdListOperation`; (b) all in `Operations.h`; (c) `Lamport.h` separate, op headers separate. Choose during the joint-design header-draft pass with Markoff. Default: (a), mirroring the existing internal split. Capture the decision in the header doc-comment.

- [ ] **Step 2: Move type declarations.** Promote the existing `src/crdt/Operations.h` and `src/crdt/IdListOperations.h` to `include/collabtext/`. Keep internal-only helper structs (anything not directly named in `Operation` / `IdListOperation` member types that the encoders touch) in `src/crdt/` and have the public headers `#include` them as needed. Goal: a consumer can include `<collabtext/Operations.h>` + `<collabtext/IdListOperations.h>` + `<collabtext/Serialization.h>` and round-trip ops without seeing internals beyond what the type definitions transitively pull in.

- [ ] **Step 3: Write the contract doc-comment.** At the top of the public `Operations.h`, write a contract block with this content, in this order:
  - **Stable across `schema_version` bumps:** `lamport()`, `Lamport::counter()`, `Lamport::replica_id()`, round-trip via `encode_*` / `decode_*`.
  - **Evolution-reserved (DO NOT depend on):** field layout, member ordering, op-variant discriminator values, internal types (`Fragment`, `Anchor`, `Locator`, `Global`) where transitively visible, public construction APIs (none provided).
  - **`decode_*` returns `std::nullopt` on schema mismatch; consumers warn-and-skip.**
  - Cross-link to `docs/handoff/2026-05-08-d5-joint-design-outcomes.md`.

- [ ] **Step 4: Add `Lamport::counter()` and `Lamport::replica_id()` accessors** if not already present. These are part of the stable contract per Markoff's bundle-meta requirement.

- [ ] **Step 5: Suppress public construction APIs.** No public constructors on `Operation` / `IdListOperation` beyond move/copy/default. Mark explicit-from-fields constructors `private` (with `friend` for the engine internals that need to construct them); consumers obtain them only via decode or via callbacks.

- [ ] **Step 6: Update CMakeLists.** Add `install(FILES ... DESTINATION include/collabtext)` for the new public headers.

- [ ] **Step 7: Write `tst_serialization_public.cpp`** that includes ONLY `<collabtext/Serialization.h>` + `<collabtext/Operations.h>` + `<collabtext/IdListOperations.h>` and verifies round-trip on representative ops AND that `Operation::lamport()` returns the expected value. This proves the public surface is sufficient for Markoff's bundle-meta use case.

- [ ] **Step 8: `cmake --build build-dev -j` + `ctest --test-dir build-dev --output-on-failure` clean.** Commit: `refactor(crdt): promote op serialization to public API with narrow contract`.

### Task 1.2: Add `setOnLocalOp` and `applyRemoteOp` to `CrdtEngine`

**Files:**
- Modified: `libs/collabtext/include/collabtext/CrdtEngine.h`
- Modified: `libs/collabtext/src/CrdtEngine.cpp`

- [ ] **Step 1: Write the failing test.** Create `tst_crdtengine_op_api.cpp`. Two engines, replica IDs 1 and 2. Engine 1 inserts text; the `setOnLocalOp` callback fires with an `Operation`; pass that to engine 2's `applyRemoteOp`; assert engine 2's `text()` equals engine 1's `text()`. Also test undo round-tripping.

- [ ] **Step 2: Add the methods to `CrdtEngine.h`.** Signatures:
  ```cpp
  using LocalOpCallback = std::function<void(const Crdt::Operation&)>;
  void setOnLocalOp(LocalOpCallback cb);
  bool applyRemoteOp(const Crdt::Operation& op);
  ```
  - The callback receives the raw `Operation`; the caller decides whether to encode (consistent with consumers like Markoff's `MarkoffSerializer` wrapping the encode call themselves).
  - `applyRemoteOp` returns `bool` per joint-design outcomes §3.3: `true` on success, `false` if the op cannot be applied (dependency unmet — caller may retry / buffer). Never throws on in-domain ops. Asserts only on programming errors (e.g., null engine).
  - Document both points in the header doc-comment, cross-linking to the outcomes doc.

- [ ] **Step 3: Plumb through pImpl.** `Impl` already has `Buffer m_buffer`; wire `apply_local_edit` to fire the callback after committing local edits, and route `applyRemoteOp` to `m_buffer.apply_remote_edit` / `apply_remote_undo` based on op variant.

- [ ] **Step 4: Test passes; existing tests still green.** Commit: `feat(engine): public setOnLocalOp/applyRemoteOp`.

### Task 1.3: Add `setOnLocalOp` and `applyRemoteOp` to `IdList`

**Files:**
- Modified: `libs/collabtext/src/crdt/IdList.h`
- Modified: `libs/collabtext/src/crdt/IdList.cpp`

- [ ] **Step 1: Confirm IdList β state.** β was accepted on 2026-05-08 (`docs/superpowers/plans/2026-05-04-idlist-implementation.md` acceptance footer). `IdList::apply_ops(const std::vector<IdListOperation>&)` exists and is tested. Local-op emission is via the existing `set_on_change` (callback fires after local ops; ops are returned from `insert_after` / `remove_at` / `undo` / `redo` directly). For Markoff's path we need a `set_on_local_op` analogue that hands back the produced `IdListOperation`. Surface that without breaking existing `set_on_change`.

- [ ] **Step 2: Write the failing convergence test.** Two `IdList`s, divergent local insertions, route ops between them via the public callback/method, assert `ids()` converge.

- [ ] **Step 3: Add the methods.** Mirror `CrdtEngine`'s shape exactly — same callback typedef pattern, same return-bool-on-apply contract.

- [ ] **Step 4: Test passes.** Commit: `feat(idlist): public setOnLocalOp/applyRemoteOp`.

---

## Phase 2 — `OpStream` interface

Goal: define the four-method interface. No implementation yet.

### Task 2.1: Define `OpStream` in a public header

**Files:**
- New: `libs/collabtext/include/collabtext/OpStream.h`
- New: `libs/collabtext/tests/tst_opstream_interface.cpp`
- Modified: `libs/collabtext/CMakeLists.txt`

- [ ] **Step 1: Write `OpStream.h`.** Pure abstract class. Four methods exactly as in the response doc: `push`, `set_on_inbound`, `lowest_peer_acked_lamport`, `set_on_ack_update`. Doc-comment each method with the contract — invariants, threading expectations, ordering guarantees per stream, what implementations MAY vs MUST do.

- [ ] **Step 2: Define the per-stream order contract in the header doc.** "An implementation MUST deliver inbound entries within a single stream in the order they were pushed by a given producer. Entries from different streams MAY be reordered. Entries from different producers within the same stream MAY be reordered." Reference Markoff D5 §3.2 for the rationale.

- [ ] **Step 3: Define a no-op test fixture.** A `TestOpStream` subclass in `tst_opstream_interface.cpp` that does nothing in `push` / etc. Instantiate it. Confirms the interface is implementable in isolation (no surprise pure-virtuals, no hidden Qt deps). This is a smoke test, not a real impl.

- [ ] **Step 4: Build clean; install header.** Commit: `feat(transport): OpStream public interface`.

---

## Phase 3 — `StreamSync` adopts `OpStream`

Goal: `StreamSync` becomes the file-backed reference implementation of `OpStream`.

### Task 3.1: Promote `StreamSync.h` to public

**Files:**
- New: `libs/collabtext/include/collabtext/StreamSync.h`
- Modified: `libs/collabtext/src/crdt/StreamSync.h` (becomes a thin re-include or is deleted in favor of the public version)
- Modified: `libs/collabtext/src/crdt/StreamSync.cpp`
- Modified: `libs/collabtext/CMakeLists.txt`
- Modified: `app/collabedit/CollabPane.h` (include path)
- Modified: `app/testapp/main.cpp` (include path)
- Modified: `libs/collabtext/tests/tst_stream_sync.cpp` (include path)

- [ ] **Step 1: Move the header.** `git mv libs/collabtext/src/crdt/StreamSync.h libs/collabtext/include/collabtext/StreamSync.h`. Adjust `#include` paths inside (`crdt/SegmentReader.h` → keep relative since `SegmentReader` stays internal; expose just enough through the public header to satisfy callers).

- [ ] **Step 2: Decide on `StreamEntry` visibility.** The struct is currently in `StreamSync.h`. It needs to be public if callers see it. Options: (a) keep `StreamEntry` in the public header as-is; (b) replace per-entry fields with opaque `std::string` blobs at the public boundary. Recommendation: (a) — it's already simple and Markoff's `ITransport` shape uses similar metadata.

- [ ] **Step 3: Update all `#include "crdt/StreamSync.h"` to `#include <collabtext/StreamSync.h>`** in tests and `app/`. The internal-include style for `SegmentReader/Writer` stays.

- [ ] **Step 4: Build clean; existing `tst_stream_sync.cpp` passes unchanged.** Commit: `refactor(crdt): promote StreamSync to public API`.

### Task 3.2: Add per-entry inbound callback

**Files:**
- Modified: `libs/collabtext/include/collabtext/StreamSync.h`
- Modified: `libs/collabtext/src/crdt/StreamSync.cpp`
- Modified: `libs/collabtext/tests/tst_stream_sync.cpp`

- [ ] **Step 1: Write the failing test.** Two `StreamSync`s in temp dirs, one pushes a `StreamEntry`, the other's per-entry callback receives it with `(stream_name, producer_replica, payload)`. Confirm the producer_replica field equals the pushing replica's id.

- [ ] **Step 2: Add the per-entry callback method.** Keep `set_on_new_entries(callback<stream_name, count>)` as the existing batch-style callback for backward-compat with chat/comments code paths. Add `set_on_inbound(callback<stream_name, producer, bytes>)` matching `OpStream`. Both fire from `read_remote_stream_`; the per-entry callback fires once per new entry, the batch callback fires once per stream after all entries land.

- [ ] **Step 3: Test passes; existing chat/comments behavior unchanged.** Commit: `feat(streamsync): per-entry inbound callback`.

### Task 3.3: Make `StreamSync` implement `OpStream`

**Files:**
- Modified: `libs/collabtext/include/collabtext/StreamSync.h`
- Modified: `libs/collabtext/src/crdt/StreamSync.cpp`
- New: `libs/collabtext/tests/tst_opstream_streamsync.cpp`

- [ ] **Step 1: Write the failing convergence test.** `tst_opstream_streamsync.cpp`: two `StreamSync` instances in temp dirs, treated as `OpStream*` via the public interface. Replica 1 pushes encoded `Buffer` ops on stream "buf:doc"; replica 2 receives and applies them. Final `text()` matches.

- [ ] **Step 2: Inherit + implement.** `class StreamSync : public OpStream`. Wrap existing `push(stream, StreamEntry)` with the `OpStream::push(stream, bytes)` override (constructs a `StreamEntry` internally with current replica id and a fresh seq). The `set_on_inbound` override delegates to Task 3.2's per-entry callback.

- [ ] **Step 3: Decide on the constructor signature.** `OpStream::push` doesn't take a producer; the producer is implicit in `StreamSync`'s constructor. Document this.

- [ ] **Step 4: Test passes.** Commit: `feat(streamsync): implement OpStream`.

---

## Phase 4 — Per-peer ack-frontier

Goal: `lowest_peer_acked_lamport()` returns a real number; `set_on_ack_update` fires when it advances.

### Task 4.1: Document the ack-frontier file format

**Decision settled (joint-design outcomes §2):** sibling `acks.json`, not extending presence. Format and four hard requirements specified in the outcomes doc; this task writes them into our spec.

**Files:**
- Modified or new: `docs/CRDT_TRANSPORT_SPEC.md`

- [ ] **Step 1: Write the spec section.** Concrete content:
  - **File path:** `replicas/{replica_id}/acks.json` (sibling to `presence.json`, not extending it).
  - **Schema:**
    ```json
    {
      "schema_version": 1,
      "acks": {
        "<peer_replica_id>": {
          "max_lamport_observed": <uint64>,
          "last_observed_at": "<ISO 8601 UTC>"
        }
      }
    }
    ```
  - **Write semantics:** each replica writes its own `acks.json` describing its observations of every peer it has read from. Atomic write via temp-file-rename. Update on every poll cycle, skip if no changes.
  - **Read semantics:** `lowest_peer_acked_lamport()` scans `replicas/*/acks.json`, looks up `acks[me]` in each (i.e., what each peer has observed FROM us), takes `min`, caches the result. Recomputed once per poll cycle; cheap to query.
  - **Monotonicity:** `max_lamport_observed` per (publisher, peer) pair must never decrease across writes. Enforce on the write path: read the current file, take `max(existing, fresh_observation)`, then write.
  - **Disconnect handling:** ack-frontier values persist across peer disconnects — a peer that goes offline keeps its last-observed frontier in the publisher's `acks.json`. The consumer drives eviction by inspecting `last_observed_at` (silent-peer policy per Markoff D5 §3.5).
  - **Cross-link:** reference `docs/handoff/2026-05-08-d5-joint-design-outcomes.md` §2 for the four hard requirements rationale.

- [ ] **Step 2: Define the enrolled-peer-set boundary.** The "enrolled peer set" for aggregation is the set of replicas we've successfully read from at least once (i.e., peers that have an `acks.json` we can read). We do not maintain an explicit registry — consumer policy per Markoff D5 §3.5. Document this in the same spec section.

- [ ] **Step 3: Commit.** `docs(transport): ack-frontier file format spec`.

### Task 4.2: Implement ack publication

**Files:**
- Modified: `libs/collabtext/include/collabtext/StreamSync.h`
- Modified: `libs/collabtext/src/crdt/StreamSync.cpp`
- New: `libs/collabtext/tests/tst_opstream_acks.cpp`

- [ ] **Step 1: Write the failing test.** Two `StreamSync` instances. Replica 1 pushes 5 ops on a stream with monotonic Lamports. Replica 2 polls; its `acks.json` is written with `acks[1].max_lamport_observed == 5` and a non-empty `last_observed_at`. Replica 1 then reads replica 2's `acks.json` directly (filesystem) to confirm the format.

- [ ] **Step 2: Track max-observed-Lamport per peer.** When `read_remote_stream_` reads new entries, update an in-memory `max_lamport_seen_per_peer` map. The Lamport value is the per-op `Lamport::counter()` from the decoded op payload — NOT the `StreamEntry::seq`. Reason: Markoff's bundle-meta + GC contract is in Lamport space; using `seq` would mean per-stream local sequence numbers, which don't compose across streams. If a stream's entries are not all decodable as `Operation` (e.g., chat messages), skip ack-tracking for that stream — ack-frontier is meaningful only for op streams.

- [ ] **Step 3: Enforce monotonicity on write.** Read existing `acks.json` (if present); for each peer, take `max(existing.max_lamport_observed, in_memory.max_lamport_seen)`; write the result. This survives concurrent-reader scenarios where in-memory state was reset (e.g., process restart) but the file has higher values.

- [ ] **Step 4: Write `acks.json` on every poll cycle.** Atomic write via temp-file-rename. Skip if no changes since last write. Include `last_observed_at` ISO 8601 UTC timestamp per peer (refreshed only when `max_lamport_observed` advances — not on every poll cycle, to avoid spurious updates).

- [ ] **Step 5: Test passes.** Commit: `feat(streamsync): publish per-peer ack frontier (acks.json)`.

### Task 4.3: Implement aggregate read and `lowest_peer_acked_lamport`

**Files:**
- Modified: `libs/collabtext/src/crdt/StreamSync.cpp`
- Modified: `libs/collabtext/tests/tst_opstream_acks.cpp`

- [ ] **Step 1: Extend the test.** Three replicas, R1 pushes ops, R2 and R3 poll and write acks. R1 calls `lowest_peer_acked_lamport()` and gets `min(R2.acks[R1], R3.acks[R1])`. Add a partition scenario: R3 stops polling; R1's `lowest_peer_acked_lamport` should not advance until R3 catches up.

- [ ] **Step 2: Implement the aggregate.** On each poll cycle, scan `replicas/*/acks.json` and compute the `min` of `acks[me]` across all peers we discover. Cache the result; expose via `lowest_peer_acked_lamport()`.

- [ ] **Step 3: Wire `set_on_ack_update`.** Fire the callback when the cached value advances.

- [ ] **Step 4: Test passes.** Commit: `feat(streamsync): aggregate ack frontier and update callback`.

### Task 4.4: Partition / silent-peer scenarios

**Files:**
- New: `libs/collabtext/tests/tst_opstream_partition.cpp`

- [ ] **Step 1: Write tests.** (a) Silent peer never writes acks → `lowest_peer_acked_lamport` stays at 0. (b) Peer leaves (acks file deleted) → behavior is documented (likely: peer is dropped from aggregate). (c) Peer reconnects → ack frontier advances on next sync. (d) Three-peer partition: 2 peers stay connected; the third lags; aggregate is bounded by the laggard.

- [ ] **Step 2: Pass.** Commit: `test(transport): ack frontier under partition and silent-peer scenarios`.

---

## Phase 5 — End-to-end: convergence over `OpStream`

Goal: prove the whole stack works end-to-end without `SyncManager` on the path.

### Task 5.1: Two-`Buffer` convergence over `StreamSync`-as-`OpStream`

**Files:**
- New: `libs/collabtext/tests/tst_opstream_convergence_buffer.cpp`

- [ ] **Step 1: Write the test.** Two `CrdtEngine`s wired to two `StreamSync`s as `OpStream*`. Use `CrdtEngine::setOnLocalOp` → encode → `OpStream::push`. Use `OpStream::set_on_inbound` → decode → `CrdtEngine::applyRemoteOp`. Run divergent edit scripts; poll; assert `text()` converges.

- [ ] **Step 2: Pass.** Commit: `test(transport): two-replica Buffer convergence over OpStream`.

### Task 5.2: Two-`IdList` convergence over `StreamSync`-as-`OpStream`

**Files:**
- New: `libs/collabtext/tests/tst_opstream_convergence_idlist.cpp`

- [ ] **Step 1: Write the test.** Same shape as 5.1 but with `IdList`. Insert / remove / undo divergently; assert `ids()` converge.

- [ ] **Step 2: Pass.** Commit: `test(transport): two-replica IdList convergence over OpStream`.

### Task 5.3: Mixed Buffer + IdList convergence (Markoff-shaped)

**Files:**
- New: `libs/collabtext/tests/tst_opstream_convergence_mixed.cpp`

- [ ] **Step 1: Write the test.** Two replicas, each with one `IdList` (structural) + N `Buffer`s (per-block text). Use `OpStream` streams named `"idlist:structure"` and `"buffer:block-{id}"`. Run a script that inserts blocks, edits text in them, removes a block, undoes. Final state converges.

- [ ] **Step 2: Pass.** Commit: `test(transport): mixed Buffer + IdList convergence over OpStream`.

### Task 5.4: Catch-up replay

**Files:**
- New: `libs/collabtext/tests/tst_opstream_replay.cpp`

- [ ] **Step 1: Write the test.** Replica 1 runs for many ops. Replica 2 attaches fresh (its `StreamSync` constructed against the same shared folder, or its segment readers reset). On first poll, replica 2 receives all of replica 1's history and applies it. Final state matches.

- [ ] **Step 2: Pass.** Commit: `test(transport): catch-up replay on first attach`.

### Task 5.5: Idempotent re-delivery

**Files:**
- New: `libs/collabtext/tests/tst_opstream_idempotent.cpp`

- [ ] **Step 1: Write the test.** Replica 2 receives replica 1's ops, applies them, and then somehow re-applies the same ops (simulate by manually invoking `applyRemoteOp` with the same op twice). State is unchanged; CRDT identity guarantees.

- [ ] **Step 2: Pass.** Commit: `test(transport): idempotent op re-delivery`.

---

## Phase 6 — Documentation

Goal: external docs reflect the new public surface.

### Task 6.1: New `CRDT_TRANSPORT_SPEC.md`

**Files:**
- New: `docs/CRDT_TRANSPORT_SPEC.md`

- [ ] **Step 1: Write the spec.** Sections: purpose; `OpStream` interface contract; per-stream ordering invariants; ack-frontier semantics + file format (from Task 4.1); `StreamSync` as the file-backed reference implementation; consumer responsibilities (peer-set membership, peer enrolment, presence, identity — all opt-in / consumer-side). Mirror the depth of `CRDT_ENGINE_SPEC.md`.

- [ ] **Step 2: Cross-link.** Reference Markoff D5 §4.1 + §4.3 as the consumer-side perspective; reference `direct-channel-interface-design.md` and `transport-elevation-roadmap.md` as separate-track follow-ons.

- [ ] **Step 3: Commit.** `docs(transport): CRDT_TRANSPORT_SPEC for OpStream and StreamSync`.

### Task 6.2: Update `CRDT_SYNC_SPEC.md`

**Files:**
- Modified: `docs/CRDT_SYNC_SPEC.md`

- [ ] **Step 1: Mark superseded sections.** Anything that prescribed `StreamSync` as the only path now points to `CRDT_TRANSPORT_SPEC.md` for the boundary; `CRDT_SYNC_SPEC.md` becomes the file-floor-specific spec.

- [ ] **Step 2: Commit.** `docs(sync): defer transport boundary to CRDT_TRANSPORT_SPEC`.

### Task 6.3: Update `ARCHITECTURE.md`

**Files:**
- Modified: `docs/ARCHITECTURE.md`

- [ ] **Step 1: Update the high-level diagram.** Show `OpStream` as the public boundary; `StreamSync` as the reference impl beside it; consumers (Markoff, future direct-channel) as `OpStream` users.

- [ ] **Step 2: Commit.** `docs(architecture): show OpStream as public transport boundary`.

### Task 6.4: README paragraph (post-acceptance)

**Files:**
- Modified: `README.md`

- [ ] **Step 1: Add a one-paragraph note** to the "What it is" section noting the public op-stream surface, the four-method interface, and the file-backed reference impl. Cross-link to `CRDT_TRANSPORT_SPEC.md`.

- [ ] **Step 2: Commit.** `docs(readme): mention public OpStream boundary`.

---

## Phase 7 — Joint-design header-draft pass + acceptance

Goal: hand the public headers to Markoff for a header-draft review (narrower than the original "joint-design pass" — the two big design calls are already settled in `docs/handoff/2026-05-08-d5-joint-design-outcomes.md`), incorporate feedback, then call it done.

**Reduced scope:** the two pre-resolved questions (Form 2 type contract, sibling `acks.json`) are NOT on the table for re-litigation. This pass covers:
- Header partitioning details (one `Operations.h` vs split, `Lamport.h` separate or folded)
- Naming spelling / doc-comment precision
- Constructor visibility and friend-list correctness
- `decode_*` `optional` semantics (parse failure vs schema mismatch differentiation, if any)

### Task 7.1: Hand off public headers for review

- [ ] **Step 1: Notify Markoff** at the end of Phase 6 with a tag or branch reference and a short note enumerating the four new public headers (`OpStream.h`, `Serialization.h`, `StreamSync.h`, `Operations.h` + `IdListOperations.h`), the new spec docs (`CRDT_TRANSPORT_SPEC.md`), and the joint-design outcomes doc. Cross-link Markoff's positions doc.

- [ ] **Step 2: Reserve up to 1 week for back-and-forth.** Mirroring the IdList β header pass. Adjustments land as small follow-up commits, not as new phases. Markoff's expected output (per their §4): a brief outcomes doc on their side recording any decisions that differ from this plan.

### Task 7.2: Acceptance gate

- [ ] **Step 1: Acceptance criteria.**
  - All Phase 5 tests green.
  - All existing tests still green (`ctest --test-dir build-dev` is fully passing).
  - Markoff-side has signed off on the public header shape.
  - `app/collabedit/` and `app/testapp/` compile without modification beyond include-path fixes.
  - Public headers documented with contract-bearing doc-comments cross-linking the outcomes doc.
  - `Operation::lamport()` and `IdListOperation::lamport()` exposed and tested via `tst_serialization_public.cpp`.
  - `lowest_peer_acked_lamport()` cached and recomputed once per poll (not on every call).
  - `last_observed_at` ISO 8601 timestamp present in `acks.json` per peer.

- [ ] **Step 2: Tag.** `opstream-v1`. Note in `docs/specs/2026-05-08-d5-negotiation-response.md` that this commitment is fulfilled.

---

## Out of scope (documented, deferred)

- **Direct-channel `OpStream` impl** (TCP, WebRTC, etc.) — separate track per `transport-elevation-roadmap.md`.
- **`SyncManager` split** for fully opt-in identity — a separate quality refactor; not on this work's critical path.
- **`MemoryOpStream` shipped from collabtext** — Markoff's testapp owns this.
- **`app/collabedit/` and `app/testapp/` migration** — deferred to widget-lab resumption per project posture.
- **`Operation` ABI deep stability** — fields are subject to evolution; consumers round-trip via encode/decode.

---

## Working conventions

- **TDD.** Every algorithmic change gets a failing test first. The convergence + ack-frontier tests are roughly a third of the work; do not try to ship them late.
- **One commit per task.** Conventional commits: `feat(transport):`, `feat(streamsync):`, `feat(engine):`, `test(transport):`, `docs(transport):`, `refactor(crdt):`.
- **Keep `Buffer.{h,cpp}` and `IdList.{h,cpp}` algorithm code untouched.** This work is purely about surfacing existing behaviour publicly and adding the transport boundary above it.
- **Frequent build + test runs.** `cmake --build build-dev -j && ctest --test-dir build-dev --output-on-failure` after every step.

---

## Estimated effort

Roughly 4 weeks of focused work, sequenced after IdList β. Distribution:
- Phase 1: ~1 week
- Phase 2: ~2 days
- Phase 3: ~1 week
- Phase 4: ~1 week
- Phase 5: ~3-4 days
- Phase 6 + 7: ~3-4 days

Variance budget: +1 week for unknown-unknowns in ack-frontier file format and per-stream ordering edge cases.
