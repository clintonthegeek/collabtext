# CollabText Transport Specification

This spec defines the public `OpStream` transport boundary and the `StreamSync` file-backed reference implementation. `OpStream` is the interface every transport implements; `StreamSync` is the only reference implementation shipped from collabtext (file-backed via Syncthing). Other transport implementations (TCP, WebRTC) are a separate roadmap track per `docs/specs/transport-elevation-roadmap.md` and are not specified here.

The § Ack frontier section below is complete as of the joint-design outcomes doc (`docs/handoff/2026-05-08-d5-joint-design-outcomes.md`). The remaining sections — § OpStream interface contract, § StreamSync reference implementation, and § Consumer responsibilities — are stubs to be filled in Task 6.1.

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

*(Filled in Task 6.1.)*

---

## § StreamSync reference implementation

*(Filled in Task 6.1.)*

---

## § Consumer responsibilities

*(Filled in Task 6.1.)*
