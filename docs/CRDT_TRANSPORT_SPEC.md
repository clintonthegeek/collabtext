# CollabText Transport Specification

This spec defines the public `OpStream` transport boundary and the `StreamSync` file-backed reference implementation. `OpStream` is the interface every transport implements; `StreamSync` is the only reference implementation shipped from collabtext, backed by a Syncthing-style shared folder. Other transport implementations (TCP, WebRTC) are a separate roadmap track per `docs/specs/transport-elevation-roadmap.md` and are not specified here.

The design was committed to in `docs/specs/2026-05-08-d5-negotiation-response.md` as the response to the Markoff D5 negotiation opener. Open design calls — the public type-surface form and the ack-frontier file format — were resolved in `docs/handoff/2026-05-08-d5-joint-design-outcomes.md`. This spec reflects both of those decisions.

---

## § Ack frontier

Each `StreamSync` replica publishes a per-peer observation record so that every participant in the sync folder can determine a safe garbage-collection horizon: the lowest Lamport counter that all enrolled peers have consumed from the local replica's op log.

### File path

```
replicas/{replica_id}/acks.json
```

`acks.json` is a sibling of `presence.json` inside each replica's directory. It is **not** an extension of `presence.json`. The two files have different lifetimes and different consumers — presence is ephemeral ("who is here now"); acks are durable ("what each peer has persisted"). Mixing them would either force ack-frontier values to be ephemeral (which blocks GC across a disconnect) or require special-casing inside the presence file. The cost of a separate file is one extra entry in the Syncthing delta per sync cycle, which is negligible.

### Schema

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

- `schema_version`: Always `1` for the format described here. Bumped additively if the format evolves.
- `acks`: A JSON object keyed by peer replica ID. Each entry describes what this replica has observed **from** that peer.
- `max_lamport_observed`: The highest Lamport counter (uint64) ever observed from the named peer's op log. Monotonic — must never decrease across writes (see Write semantics below).
- `last_observed_at`: ISO 8601 timestamp (UTC) of the most recent observation from this peer. Lets consumers detect a peer that has gone silent without updating its ack record (see Disconnect handling below).

### Write semantics

Each replica writes its own `acks.json`. The file describes what **this** replica has observed from every peer it has ever read from. It does not describe what peers have observed from this replica — each peer's `acks.json` carries that.

Write procedure on every poll cycle:

1. Compute fresh per-peer observations from the current read cursors.
2. If no per-peer value has changed since the last write, skip the write entirely.
3. For each peer, take `max(existing_max_lamport_observed, fresh_observation)` — monotonicity enforcement (see below).
4. Write atomically via temp-file-rename:
   ```
   write to replicas/{own_id}/acks.tmp
   fsync
   rename acks.tmp -> acks.json
   ```

The temp-file-rename pattern matches the atomic write convention used throughout `StreamSync` (see `docs/CRDT_SYNC_SPEC.md` §11.1).

### Read semantics

`StreamSync::lowest_peer_acked_lamport()` answers: "what is the lowest Lamport counter that any enrolled peer has confirmed observing from me?" This is the safe GC horizon for the local replica's own op log.

Read procedure:

1. Scan `replicas/*/acks.json` for every peer in the enrolled peer set.
2. For each peer's `acks.json`, look up `acks[own_replica_id]` — what that peer has observed **from us**.
3. Take the `min` of all `max_lamport_observed` values found in step 2.
4. Cache the result as a scalar. Recompute once per poll cycle.

The cached scalar is cheap to query between poll cycles. `set_on_ack_update(callback<uint64_t>)` fires whenever the cached value advances.

If a peer's `acks.json` does not contain an `acks[own_replica_id]` entry (the peer has not yet read from us), that peer contributes `0` to the minimum, effectively blocking GC advancement until it has caught up. This is correct: the peer hasn't confirmed any observations yet.

### Monotonicity

`max_lamport_observed` per `(publisher, peer)` pair — meaning, the value written by `publisher` about its observations of `peer` — must never decrease across consecutive writes of `publisher`'s `acks.json`.

The implementation enforces this on the write path: before writing a new `acks.json`, read the current file (if it exists), compare stored and fresh values per peer, and write `max(stored, fresh)`. This guards against:

- Stale reads: a transient filesystem cache returns an older version of a peer's op log.
- Clock skew: a peer's Lamport counter appears to decrease due to a race between concurrent reads.

The enforcement is local to the writing replica. No coordination with peers is required.

### Disconnect handling

Ack-frontier values persist across peer disconnects. When a peer goes offline, its last-known `max_lamport_observed` remains in the local `acks.json`. That value continues to contribute to `lowest_peer_acked_lamport()` computation by other replicas scanning the folder.

This is correct for the GC case: a peer that goes offline has confirmed what it confirmed — GC should not advance past that frontier just because the peer is unreachable. The frontier is stable until the peer reconnects and updates its own `acks.json`.

`last_observed_at` gives consumers the timestamp of the last update from each peer. A peer that has been silent for an extended period can be identified via `last_observed_at` and handled according to the consumer's silent-peer eviction policy. The policy itself is outside `StreamSync`'s scope — it is consumer responsibility per Markoff D5 §3.5. `StreamSync` publishes the timestamp; the consumer decides what to do when a peer exceeds its silence threshold (e.g., exclude it from the GC watermark computation).

### Enrolled peer set

The enrolled peer set for `lowest_peer_acked_lamport()` aggregation is defined as: the set of replicas this `StreamSync` instance has successfully read from at least once during its lifetime.

There is no explicit registry file. Enrollment is implicit: when `StreamSync` reads ops from a replica for the first time, that replica is enrolled. Enrollment is tracked in local in-memory state (and can be reconstructed from the set of `replicas/*/acks.json` entries in the local `acks.json`).

The consumer-side policy for evicting a peer from the enrolled set (e.g., after extended silence) is documented in Markoff D5 §3.5 and is not specified here. `StreamSync` provides `last_observed_at` as the timestamp signal; the consumer drives eviction.

### Relationship to the four hard requirements

The format above satisfies Markoff's four hard requirements from the joint-design call. Full rationale in `docs/handoff/2026-05-08-d5-joint-design-outcomes.md` §2:

1. **Per-peer monotonic ack-frontier.** `max_lamport_observed` only increases per peer per writing replica. Enforced on the write path.
2. **Notification of advance.** `set_on_ack_update(callback<uint64_t>)` fires when the cached `lowest_peer_acked_lamport` scalar advances on a poll cycle.
3. **Aggregation visibility.** `lowest_peer_acked_lamport()` returns from a cached scalar; no linear scan on each call. Recomputed once per poll cycle.
4. **No silent staleness.** `last_observed_at` lets the consumer detect a peer that hasn't reported in N hours/days and decide to evict per its silent-peer policy.

---

## § OpStream interface contract

`OpStream` is the transport-agnostic op delivery boundary for collabtext. Any class that moves serialised CRDT operations between replicas implements this interface. `StreamSync` is the file-backed reference implementation; consumers that need direct-channel transports (TCP, WebRTC, in-memory test doubles) provide their own.

The interface mirrors Markoff's `ITransport` four-method shape from D5 §4.1, so a Markoff-side `ITransport` adapter over `CollabText::OpStream` is a thin forward, not a translation layer.

### Declaration

```cpp
namespace CollabText {

class OpStream {
public:
    virtual ~OpStream() = default;

    virtual void push(const std::string& stream_name,
                      const std::string& payload) = 0;

    virtual void set_on_inbound(
        std::function<void(const std::string& /*stream_name*/,
                           uint16_t          /*producer_replica_id*/,
                           const std::string& /*payload*/)> cb) = 0;

    virtual uint64_t lowest_peer_acked_lamport() const = 0;

    virtual void set_on_ack_update(
        std::function<void(uint64_t /*new_fence*/)> cb) = 0;
};

} // namespace CollabText
```

### Method contracts

**`push(stream_name, payload)`**

Sends serialised op bytes onto a named stream. The `stream_name` identifies which CRDT stream the op belongs to (e.g. `"buffer:doc"`, `"idlist:structure"`). The `payload` is encoded op bytes produced by `encode_operation` or `encode_idlist_operation` from `Serialization.h`. The implementation is responsible for delivering the payload to all enrolled peers.

`push` does not block for acknowledgement. The caller proceeds immediately; delivery and ack tracking happen asynchronously on the implementation's own schedule.

**`set_on_inbound(cb)`**

Registers a callback that fires for each op received from a remote peer. The callback receives:

- `stream_name` — the stream the op arrived on (same namespace as `push`).
- `producer_replica_id` — the `uint16_t` replica ID of the peer that pushed this payload.
- `payload` — raw encoded op bytes; decode with `decode_operation` / `decode_idlist_operation`.

Only one callback is active at a time. Calling `set_on_inbound` again replaces the previous registration.

The callback is invoked synchronously by whatever thread or event-loop context the implementation uses to deliver inbound entries. Threading is implementation-defined; see the threading note below.

**`lowest_peer_acked_lamport() const`**

Returns the lowest Lamport counter that all enrolled peers have confirmed observing from this replica. This is the GC fence: ops with `lamport().counter() <= fence` are safe to pass to `Buffer::collect_garbage` / `IdList::collect_garbage`.

The value is cached from the most recent poll/update cycle. It never performs I/O on the call path. Returns `0` if no peers are enrolled — meaning no GC has been authorised by any external observer.

**`set_on_ack_update(cb)`**

Registers a callback that fires whenever `lowest_peer_acked_lamport()` advances to a higher value. The new fence value is passed as the argument.

Only one callback is active at a time. Calling `set_on_ack_update` again replaces the previous registration.

### Ordering contract

An implementation MUST deliver inbound entries within a single stream in the order they were pushed by a given producer. Entries from different streams MAY be reordered. Entries from different producers within the same stream MAY be reordered.

This is the only concurrency invariant the interface specifies. Per-stream, per-producer ordering is sufficient for the CRDT engine's causal dependency resolution: ops from the same producer within a stream arrive in the order they were emitted, so causal readiness checks succeed without reordering. Ops from different producers in the same stream may arrive in any interleaving; the engine handles this via its operation queue.

### Threading

The `OpStream` interface does not specify a threading model. Implementations may call the inbound callback from a background thread, from a poll-driven event loop on the caller's thread, or from any other context. Consumers that care about thread safety must synchronise access to their engine state externally.

---

## § StreamSync reference implementation

`StreamSync` is the file-backed `OpStream` reference implementation. It uses a Syncthing-style shared folder: each replica writes to its own subdirectory; peers read from each other's subdirectories. No network daemon is required for correctness — `StreamSync` is just a filesystem reader/writer; Syncthing (or any other folder-sync tool) handles replication.

### Declaration summary

```cpp
namespace CollabText::Crdt {

struct StreamEntry {
    std::string id;         // "<replica_name>-<seq>"
    uint16_t    replica_id; // numeric producer ID
    uint64_t    seq;        // monotonically increasing per-producer per-stream
    std::string timestamp;  // ISO 8601 UTC wall-clock at push time
    std::string payload;    // encoded op bytes
    bool        tombstone;  // true for AnchorKeyed deletions
};

class StreamSync : public CollabText::OpStream {
public:
    StreamSync(const std::filesystem::path& shared_folder,
               const std::string&           replica_name,
               uint16_t                     replica_id = 0,
               WriterConfig                 writer_cfg = WriterConfig{});

    void register_stream(const std::string& name, StreamType type);
    void start();

    // OpStream overrides
    void     push(const std::string& stream_name,
                  const std::string& payload) override;
    void     set_on_inbound(InboundCallback cb) override;
    uint64_t lowest_peer_acked_lamport() const override;
    void     set_on_ack_update(std::function<void(uint64_t)> cb) override;

    size_t poll();
    void   flush();

    std::vector<StreamEntry> entries(const std::string& stream) const;

    void set_on_new_entries(NewEntriesCallback cb);

    enum class StreamType { AppendOnly, AnchorKeyed };
};

} // namespace CollabText::Crdt
```

### Construction

```cpp
StreamSync sync(shared_folder, replica_name, replica_id, writer_cfg);
```

- `shared_folder` — path to the Syncthing-shared directory (or any directory shared by another means). Must exist before `start()` is called.
- `replica_name` — human-readable string identifying this replica (e.g. `"alice-laptop"`). Used as the subdirectory name and as the prefix in `StreamEntry::id`. Must be unique across all replicas sharing the folder.
- `replica_id` — `uint16_t` numeric identifier. Used as the producer ID in `StreamEntry::replica_id` and as the key in ack tracking. Passed through as the `producer_replica_id` argument to inbound callbacks. Must be unique across replicas.
- `writer_cfg` — segment writer tuning (flush policy, segment rotation size). Defaults are appropriate for most uses.

### `register_stream(name, type)`

Registers a named stream before operations are exchanged on it. Must be called before `start()`, or immediately after `start()` before the first `poll()`.

```cpp
sync.register_stream("buffer:doc", StreamSync::StreamType::AppendOnly);
sync.register_stream("idlist:structure", StreamSync::StreamType::AppendOnly);
```

`StreamType::AppendOnly` — every pushed entry is retained in order. Suitable for CRDT op logs where all entries are meaningful.

`StreamType::AnchorKeyed` — entries are keyed by `StreamEntry::id`; a tombstone entry with the same key supersedes the live entry. Suitable for presence or other last-write-wins records where only the current value matters.

### `start()`

Creates the directory structure under `shared_folder` and marks the instance ready for `push`/`poll`. Must be called after all streams are registered.

Directory structure created by `start()`:

```
shared_folder/
  replicas/
    {replica_name}/
      log/
        streams/
          {stream_name}/    # segment files written here (one per push batch)
      acks.json             # per-peer ack observations, written on each poll
```

Peer replicas are discovered by scanning `replicas/` on each `poll()` call. No explicit registration of peer names is needed.

### `push(stream_name, payload)` — OpStream override

The `OpStream::push` override. Constructs a `StreamEntry` internally (managing sequence numbers per stream) and delegates to the low-level `push(stream, entry)` overload:

```
entry.id         = replica_name + "-" + next_seq
entry.replica_id = replica_id
entry.seq        = next_seq++   (per-stream, starts at 1)
entry.timestamp  = current ISO 8601 UTC
entry.payload    = payload
entry.tombstone  = false
```

The low-level `push(const std::string&, const StreamEntry&)` overload is also public; it accepts a fully populated `StreamEntry` and is used by non-op consumers (e.g. presence, chat).

### `poll()`

Reads new entries from all peer streams, fires callbacks, updates ack state, and returns the total number of new entries processed.

On each call:

1. Scan `replicas/` for peer replica subdirectories not yet known to this instance.
2. For each registered stream, read new segment data from each peer's `replicas/{peer}/log/streams/{stream}/` directory, advancing the per-peer read cursor.
3. For each new entry:
   - If `set_on_inbound` is registered: fire the callback with `(stream_name, entry.replica_id, entry.payload)`.
   - If `set_on_new_entries` is registered: accumulate a per-stream count, then fire the batch callback once per stream after all entries for that stream are processed.
4. Write `replicas/{own_name}/acks.json` with updated per-peer observations (see § Ack frontier).
5. Recompute `lowest_peer_acked_lamport()` by scanning peer `acks.json` files. If the fence advances, fire the `set_on_ack_update` callback.

Read cursor positions are persisted in `local/{replica_name}/read-cursors/` to survive process restarts.

### `flush()`

Force-fsyncs all open segment file tails. Call on save or shutdown to ensure all pushed entries are durable before the process exits. Not required for correctness under normal operation (the OS page cache provides eventual durability), but recommended on controlled shutdown paths.

### `entries(stream)`

Returns all merged entries for a named stream, including entries from all peers, in the order they were received. Useful for non-op consumers (e.g. reading all chat messages across replicas). For op streams, consumers should use the `set_on_inbound` callback path instead.

### Two callback styles

`StreamSync` exposes two inbound notification styles:

**`set_on_inbound(cb)` (from `OpStream`)** — per-entry callback, fires for every new entry with `(stream_name, producer_replica_id, payload)`. This is the right path for CRDT op consumers that need to decode and apply each operation immediately.

**`set_on_new_entries(cb)`** — batch notification, fires once per stream per poll cycle with `(stream_name, count)` after all new entries for that stream are processed. No payload is delivered; the consumer calls `entries(stream)` to retrieve them. This is the right path for non-op consumers (presence, chat, comment streams) that process entries in bulk.

Only one of each callback type is active at a time.

### `StreamEntry` fields

| Field | Type | Meaning |
|---|---|---|
| `id` | `std::string` | `"{replica_name}-{seq}"`. Unique per producer per stream within a single process lifetime. |
| `replica_id` | `uint16_t` | Numeric producer ID. Matches the `replica_id` passed to the constructor. Delivered as `producer_replica_id` in the inbound callback. |
| `seq` | `uint64_t` | Monotonically increasing per-producer per-stream counter, starting at 1. Used by readers to detect gaps and determine read cursor position. |
| `timestamp` | `std::string` | ISO 8601 UTC wall-clock time at push. Informational; not used for CRDT ordering (Lamport timestamps in the payload handle that). |
| `payload` | `std::string` | Encoded op bytes. For CRDT streams: UTF-8 JSON from `encode_operation` / `encode_idlist_operation`. For non-op streams: consumer-defined. |
| `tombstone` | `bool` | For `AnchorKeyed` streams only: `true` means this entry deletes the record with the same `id`. Always `false` for `AppendOnly` streams. |

### Known limitation: seq reset on restart

`StreamEntry::id` is `replica_name + "-" + seq`, and `seq` resets to 1 on process restart. If the same `replica_name` pushes entries in two separate processes (e.g., after a crash-restart without persisting `next_seq`), `id` values can collide across sessions.

This is a known limitation. Peers that read entries from both sessions may observe entries with duplicate `id` values. For `AppendOnly` streams used as op logs, the CRDT engine's idempotence check (`version.observed(op.timestamp)`) prevents double-apply, but the log file will contain duplicates. A future phase will address this by persisting `next_seq` to disk on each write, so restarts resume from the last known sequence number rather than resetting to 1.

---

## § Consumer responsibilities

This section describes what consumers of `OpStream` and `StreamSync` are expected to do — decisions that are intentionally outside the transport layer.

### Stream naming convention

Stream names are chosen by the consumer. The recommended convention, used in tests and documentation throughout this codebase:

- `"buffer:{name}"` — Buffer op stream for a document or resource named `{name}`.
- `"idlist:{name}"` — IdList structural op stream for a list named `{name}`.

Examples: `"buffer:doc"`, `"idlist:structure"`, `"buffer:comments"`.

The convention is advisory. `StreamSync` treats stream names as opaque strings; no naming enforcement is applied. Consumers that deviate from the convention will interoperate correctly as long as the producer and consumer agree on names out of band.

### Peer enrolment

`StreamSync` auto-discovers peer replicas by scanning `replicas/` on each `poll()` call. There is no explicit registration step for peers. A new peer becomes enrolled the first time `poll()` finds its replica directory. Once enrolled, the peer contributes to `lowest_peer_acked_lamport()` aggregation.

### Presence and identity

`StreamSync` has no concept of presence or user identity. `replica_id` is a `uint16_t` integer; `replica_name` is a string chosen by the consumer. `StreamSync` does not validate names, does not assign IDs, and does not enforce uniqueness. Consumers are responsible for:

- Assigning unique `replica_id` values across all replicas sharing a folder.
- Associating `replica_id` with user identity if needed (e.g., for display purposes in a collaborative editor UI).
- Maintaining presence signals through a separate channel (e.g., the `presence.json` file described in `docs/CRDT_SYNC_SPEC.md`).

### GC fence usage

`lowest_peer_acked_lamport()` returns the safe GC threshold. Ops with `lamport().counter() <= fence` have been observed by all enrolled peers and can be collected:

```cpp
uint64_t fence = stream.lowest_peer_acked_lamport();
if (fence > 0) {
    engine.collect_garbage(fence);      // Buffer
    id_list.collect_garbage(fence);     // IdList
}
```

`fence == 0` means no peers have enrolled yet, or no peer has confirmed observing any ops. Do not call GC in this case.

The consumer decides when to call GC. `set_on_ack_update` provides a callback for when the fence advances; a reasonable policy is to schedule a GC pass on each advance. Aggressive GC reduces memory and disk use; deferring GC is always safe.

### Silent-peer policy

A peer that stops polling holds its ack at the last-observed value. Its `acks.json` persists across disconnects (by design — see § Ack frontier §"Disconnect handling"). A permanently-gone peer will pin the GC fence at its last-acked value indefinitely.

`StreamSync` publishes `last_observed_at` per peer in `acks.json`. Consumers should monitor this timestamp and implement their own eviction policy for peers that have been silent beyond an acceptable threshold. Eviction mechanics (removing the peer from the enrolled set so it no longer contributes to the fence minimum) are the consumer's responsibility; `StreamSync` provides the timestamp signal but does not act on it. See Markoff D5 §3.5 for the consumer-side eviction policy used by the Markoff testapp.

### What this project does not provide

These items are explicitly outside collabtext's scope. See `docs/specs/2026-05-08-d5-negotiation-response.md` §"What we won't do" for the full list and rationale.

- **No `MemoryOpStream`** shipped from collabtext. Markoff's testapp owns the in-memory `ITransport` mock; collabtext uses `NetworkSim` for internal convergence tests at the algorithmic layer.
- **No `SyncManager` generalisation.** `SyncManager` continues to exist as an all-in-one convenience facade (presence + identity + ops + Syncthing floor). Consumers that need direct `OpStream` access bypass it; that is the intended path.
- **No direct-channel transports.** TCP, WebRTC, mDNS, and other real-time transports are a separate roadmap track. See `docs/specs/transport-elevation-roadmap.md`.
- **No `CollabDocument` generalisation.** Structural composition of `IdList` + N `Buffer`s is the consumer's concern, built on collabtext's public primitives and `OpStream`.

---

## Cross-references

- `docs/handoff/2026-05-08-d5-joint-design-outcomes.md` — resolved design calls: public type-surface form (§1), ack-frontier file format (§2), error paths (§3).
- `docs/specs/2026-05-08-d5-negotiation-response.md` — binding scope statement; "What we won't do" section is load-bearing.
- Markoff D5 §4.1 — the `ITransport` four-method shape that `OpStream` mirrors.
- Markoff D5 §3.5 — consumer-side silent-peer eviction policy.
- `docs/specs/transport-elevation-roadmap.md` — future direct-channel transport track (TCP, WebRTC).
- `docs/CRDT_SYNC_SPEC.md` — file-backed sync mechanics, `presence.json`, snapshot protocol.
