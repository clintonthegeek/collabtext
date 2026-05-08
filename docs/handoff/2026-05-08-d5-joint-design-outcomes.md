# D5 joint-design outcomes — collabtext side

**Date:** 2026-05-08
**Re:** the two design calls flagged in `docs/specs/2026-05-08-d5-negotiation-response.md`
**Markoff-side positions:** `~/dev/Markoff/.worktrees/foundation-exploration/docs/handoff/2026-05-08-collabtext-joint-design-positions.md`
**Drives:** `docs/superpowers/plans/2026-05-08-opstream-extraction.md` (the OpStream extraction plan)

This is the collabtext-side resolution doc Markoff requested in §4 of their positions paper. Brief — one decision per call, with reasoning where ours differs.

---

## 1. Public type surface for `Operation` / `IdListOperation`

**Decision: Form 2 — full struct types public, with a written narrow contract documented in the public header.**

### What's stable (the contract)

- `encode_operation(Operation) -> std::string` and `decode_operation(std::string_view) -> std::optional<Operation>` — the round-trip is stable across `schema_version` bumps.
- `encode_idlist_operation(IdListOperation) -> std::string` and `decode_idlist_operation(std::string_view) -> std::optional<IdListOperation>` — same.
- `Operation::lamport() const -> Lamport` and `IdListOperation::lamport() const -> Lamport`.
- `Lamport::counter() const -> uint64_t` and `Lamport::replica_id() const -> uint16_t`.
- `decode_*` returns `std::nullopt` on schema mismatch / parse failure (typed signal, not exception).

### What's reserved (subject to change without notice across `schema_version` bumps)

- Field layout of `Operation`, `IdListOperation`, and any internal types they reference (`Fragment`, `Anchor`, `Locator`, `Global`, op-variant discriminators, segment metadata, vector-clock state).
- Direct construction from individual fields. Consumers obtain `Operation` / `IdListOperation` only via `decode_*` or via `setOnLocalOp` callbacks. Public construction APIs are not provided.

### Why Form 2 over Markoff's preferred Form 1

Markoff prefers Form 1 (forward declarations + accessors only — i.e., opaque types with a pImpl interior). They explicitly accept Form 2 if Form 1 is materially harder on our side.

Form 1 is meaningfully harder. `Operation` is currently a `std::variant` over plain structs, constructed all over the internal CRDT engine. Making it pImpl requires routing all internal construction sites through a private interior, refactoring `Buffer`'s op production path, and inventing a friend / accessor protocol for the engine. Estimated 1–2 weeks for limited additional protection — Markoff already commits to not touching fields.

Form 2 achieves the same contract with documentation and discipline. The risk Markoff names — "field-level inspection by consumers turning into a de facto contract over time" — is real but mitigable through the doc-comment on the public header and the narrow `lamport()`-and-encode/decode contract written explicitly. We will write the contract loud and clear.

If field-level access by future consumers becomes a real drift problem, we'll revisit Form 1. Until then, Form 2 is right.

### Practical implication for the plan

- `libs/collabtext/include/collabtext/Operations.h` and `libs/collabtext/include/collabtext/IdListOperations.h` expose the full struct definitions. Public-header doc-comment names the narrow contract; everything else is marked evolution-reserved.
- `libs/collabtext/include/collabtext/Lamport.h` (or fold into `Operations.h`) exposes `Lamport` with stable `counter()` / `replica_id()` accessors.
- `libs/collabtext/include/collabtext/Serialization.h` exposes the four encode/decode functions with `std::optional` return on decode.
- No public constructors on `Operation` / `IdListOperation` beyond move/copy.

---

## 2. Ack-frontier file format

**Decision: sibling `acks.json` per replica, not extended presence.**

### Format

`replicas/{replica_id}/acks.json`:

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

Each replica writes its own observations of every peer it has read from. To answer "what's the lowest Lamport all peers have observed from me?" — read every peer's `acks.json`, look up `acks[me]`, take `min` over peers. Cache the result; recompute on poll cycle; fire `set_on_ack_update` callback when it advances.

### Why sibling, not extending presence

Both Markoff and we agree on this for the same reasons. Recapping for the file:

- **Different lifetimes.** Presence is ephemeral ("who's here now"); ack-frontier is durable ("what each peer has persisted"). Mixing them would either force ack-frontier to be ephemeral too (blocks GC on disconnect) or force special-casing inside the presence file.
- **Different consumers.** Presence drives UI; ack-frontier drives GC.
- **Future-proof.** Whatever evolution presence goes through (multi-cursor broadcast, expiring entries, richer metadata) doesn't touch ack-frontier semantics.

The cost is one extra file per replica per sync cycle. Negligible.

### Markoff's four hard requirements (file-format-independent)

The format above satisfies all four:

1. **Per-peer monotonic ack-frontier.** `max_lamport_observed` only increases per peer in a given replica's view. The implementation must enforce this on write — if a fresh observation produces a lower value (e.g., due to a clock skew or stale read), we keep the higher.
2. **Notification of advance.** `set_on_ack_update(callback<uint64_t>)` fires when our cached `lowest_peer_acked_lamport` advances on a poll cycle.
3. **Aggregation visibility.** `lowest_peer_acked_lamport()` returns from a cached scalar; no linear scan on each call. Recomputed once per poll cycle.
4. **No silent staleness.** `last_observed_at` lets the consumer detect a peer that hasn't reported in N hours/days and decide to evict per the silent-peer eviction policy in Markoff D5 §3.5. Eviction is consumer-side; we just publish the timestamp.

---

## 3. Other decisions

### 3.1 `OpStream` method names

Keep the names from the response doc: `push`, `set_on_inbound`, `lowest_peer_acked_lamport`, `set_on_ack_update`. Markoff has no strong preference; deferred to us; no shorter alternatives meaningfully improve clarity.

### 3.2 `Operation` move semantics

Move-only is the target. `Operation` and `IdListOperation` are move-constructible and move-assignable; copy is allowed where cheap (small structs) but consumers should not assume copy stability across schema_version bumps. The encode/decode round-trip is the supported way to copy across boundaries.

### 3.3 `applyRemoteOp` error path

`applyRemoteOp` returns `bool` matching `Buffer::apply_remote_edit`'s existing contract. `false` indicates the op could not be applied (e.g., dependency not yet satisfied — caller can retry later, or buffer for replay). Markoff's "warn-and-skip rather than crash" preference is satisfied: combined with `decode_operation` returning `std::optional`, malformed ops are caught at decode and never reach `applyRemoteOp`; in-domain-but-unappliable ops return `false` and the caller decides.

We do not throw from `applyRemoteOp`. We assert on programming errors (e.g., null engine, internal invariant violations) which are not consumer-recoverable and are bugs in collabtext.

---

## 4. What's not resolved here

These remain for the joint-design header-draft pass when Phase 1 / Phase 2 of the OpStream extraction begin:

- Exact spelling and ordering of method/field declarations in the public headers.
- Whether `Lamport` lives in its own `Lamport.h` or folded into `Operations.h`.
- Whether `Operation` and `IdListOperation` live in one public header (`Operations.h`) or are split (`Operations.h` + `IdListOperations.h` mirroring the internal split).
- Whether `StreamSync` keeps its name or is namespaced (e.g., `transport::FileBackedStream`) — current decision is "keep".

These are details. They land during implementation, not in advance.
