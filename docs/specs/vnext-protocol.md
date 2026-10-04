# vNext protocol contract

**Status:** binding design-gate artifact (P01). Paired with
[vnext-public-api.md](vnext-public-api.md), which carries the signatures this
document refers to.
**Authority:** [vnext-design-constraints.md](vnext-design-constraints.md) (D01)
sets the direction; this document fixes the protocol. Amendments update both,
and the API guide's compile-oriented examples are updated with them.

This document specifies the wire and lifecycle protocol. It does not specify the
merge algorithm (S02) or reclamation (G01); where those appear here, only the
protocol's obligations on them are stated.

Review evidence: F4 (sender restart drops a distinct operation), F5 (persisted
cursors outlive the state they cover), F10 (decoder success does not establish a
valid operation), plus the multi-stream independent-counter cases. Each is walked
at the end.

---

## 1. Identity and counters

### 1.1 Actor identity

```
ActorId = 128 bits, opaque to consumers
```

- Generated from OS cryptographic randomness through a narrow platform adapter.
  Never derived from presence, a display name, a document name, or a 16-bit
  random draw.
- Persisted **atomically before any operation is created**. A crash between
  "allocated" and "persisted" must not be able to reuse the id: allocation is
  either durable before use, or the id is discarded with its operations.
- Two actors with equal ids are a hard conflict. It is never resolved by
  "last writer wins" (R4).
- The same id appearing with different content in two stores is an *observable*
  conflict and is reported as such.

### 1.2 Counters are per (actor, stream)

```
OperationId { ActorId actor; uint64_t counter; }
```

- Counters are checked `uint64_t`. There is no wrapping and no reuse.
- **Each stream has its own counter space.** A `text` stream at counter 41 says
  nothing about a `comments` stream's coverage.
- Coverage is therefore a vector, keyed by (stream, actor) — never a scalar
  maximum. A single `lowest_peer_acked_lamport()`-style number cannot answer
  "have all peers seen everything on every stream", and this protocol does not
  define one (C5).
- Counter exhaustion (`counter == UINT64_MAX`) returns an error and mutates
  nothing. It is not a wrap and not a silent reset.

### 1.3 Operation context

Every operation carries its context. Nothing in the engine may act on an
operation whose context it does not know.

```
OperationContext {
    DocumentId document;   // stable per document, persisted
    StreamId   stream;     // "text", "comments", "chat", ...
    uint64_t   epoch;      // incremented on explicit retirement/rejoin
    ActorId    actor;
    uint64_t   counter;
}
```

An operation whose `epoch` is not the receiver's current epoch for that
(document, stream) is **rejected**, not applied and not deferred: it belongs to
an earlier or later membership era and the receiving engine has no obligation to
order it against current state. Retirement is the only thing that changes an
epoch (C2).

## 2. Formats and versions

Each artifact family has its own explicit version field. A version is *not* a
compatibility promise: it exists so that a reader can refuse a record it does not
understand rather than misread it (C6, C7).

| Artifact | Identifier | Baseline version | Notes |
|---|---|---|---|
| Operation payload (Buffer) | `"collabtext.buffer-op"` | 1 | Text edits and undo variants |
| Operation payload (IdList) | `"collabtext.idlist-op"` | 1 | Structural ops |
| Stream envelope | `"collabtext.envelope"` | 1 | Wraps context + payload + codec version |
| Store manifest | `"collabtext.manifest"` | 1 | Document/stream inventory + membership |
| Segment file | `"collabtext.segment"` | 1 | Sealed segment and open tail |
| Checkpoint | `"collabtext.checkpoint"` | 1 | Covered prefix + snapshot reference |
| Ack record | `"collabtext.ack"` | 1 | Per-stream receipt watermark |

Rules:

- A record carries its identifier and version in a form that can be read without
  parsing the rest (a fixed magic prefix plus a version word).
- An **unknown identifier or version is rejected and the record is retained or
  quarantined**. It is never skipped, and skipping it may never advance durable
  coverage (F10, R8).
- Retention means the bytes stay available for a retry or an upgrade;
  quarantine means they are moved aside with a reason and are not counted as
  read. Both are observable through `PollOutcome`.

## 3. The envelope

```
Envelope {
    magic:    fixed prefix
    version:  envelope version (1)
    context:  OperationContext
    payload_format: "collabtext.buffer-op" | "collabtext.idlist-op"
    payload_version: 1
    payload:  bytes
    checksum: covers context + payload (integrity, not authenticity)
}
```

Decoding is three separate steps, and conflating them is exactly finding F10:

1. **Syntactic parse.** Complete input consumption is required. `{"t":"u"}garbage`
   is a syntax error. Bounded lengths and counts; no unbounded array indexing.
2. **Schema and version resolution.** Identifier known, version supported, and
   the payload routed to the right decoder. Failure here is *retained*, because a
   newer writer may still be understood after an upgrade.
3. **Semantic validation.** Identity present and well-formed, counter in range,
   ranges well-ordered and inside the payload, replacement text valid UTF-8 and
   paired one-to-one with ranges, insertion metadata self-consistent, locator
   valid, all arithmetic checked. Failure here is *quarantined*: the bytes are
   retained, but the operation will never be applied.

A decoder that returns successfully has established **only** that these three
steps passed. It has not established that the operation is applicable to the
current document, which is what `apply` reports separately.

Unicode: `\u` escapes are decoded as surrogate *pairs*; a lone surrogate is a
validation error. Raw input is validated as UTF-8, and truncated sequences are
errors rather than shorter strings.

## 4. Applying an operation

```
ApplyOutcome apply(BufferOperation, OperationContext) noexcept
```

| Outcome | Meaning | Engine state |
|---|---|---|
| `applied` | integrated | advanced |
| `duplicate` | already seen; identity matches | unchanged (idempotent) |
| `deferred` | causal dependencies missing; queued | unchanged |
| `rejected(Error)` | cannot ever be applied as addressed | unchanged |

Rules:

- `deferred` requires an explicit reason (which dependency is missing) and a
  bounded queue. When the bound is reached, the oldest or newest entry is
  **quarantined with a reason** — never silently dropped, because a dropped
  operation is lost work (R1).
- `rejected` is terminal and distinguishable from `deferred` by type: wrong
  epoch, unknown stream, counter regression, malformed (should not reach here),
  unsupported version, actor conflict.
- Failure atomicity: a rejected or deferred operation mutates nothing — not the
  document, not the version vector, not the undo stack, not the coverage
  watermark.
- Applying is single-owner and non-reentrant. A callback fired by an apply may
  not call back into the same engine.

## 5. Coverage and durable progress

Coverage answers "what has been received and applied", per (stream, actor).

- **Receipt is not application.** A transport ack says bytes were received. It
  never authorizes reclamation (V03's rule, now protocol-level).
- **Durable coverage advances only over a validated, contiguous prefix.** A
  gap, a quarantine, or a retained-unknown-version record **blocks** advancement
  across it. This is the direct answer to F6: a late earlier segment is not
  skipped; the frontier does not move past the gap until the gap is filled or
  explicitly retired.
- A read cursor may only be committed for state the application can durably
  recover. Committing a cursor over state that exists only in memory is exactly
  finding F5, and the transport is required to refuse that pairing: the
  application hands the transport a *recovery point* (checkpoint id or "replay
  from the beginning"), and the cursor is meaningless without one.
- A contiguous frontier, not a high-water mark. Segment 2 arriving before
  segment 1 leaves the frontier at 0.

## 6. Ordering

- Causal order: an operation is never applied before the operations it depends
  on; it is deferred instead (R3).
- Per-actor order on a stream is expected to be preserved by the transport, and
  the engine still tolerates reordering by deferral.
- Cross-actor and cross-stream order is unspecified and must not be relied upon.

## 7. Lifecycle sequences

### 7.1 Sender restart (F4)

1. On start, the sender loads its persisted identity and its next counter per
   stream from its own durable log or checkpoint.
2. It **never** reuses a durable `OperationId`. The counter resumes at or above
   the highest value durably recorded for that (actor, stream).
3. A peer's dedupe index is keyed by (actor, counter), so a restart cannot make
   a distinct operation look like a duplicate.
4. Test obligation: A pushes an operation, the peer polls and applies it, A
   restarts from its persisted state, A produces a *different* new operation,
   the peer polls and applies it. Before the fix this second operation was
   suppressed by a reused transport entry id.

### 7.2 Receiver restart (F5)

1. The receiver rebuilds state *before* accepting new writes: own-log replay or
   a checkpoint, plus remote replay from its durable coverage frontier.
2. Durable coverage is restored from the checkpoint, not from a bare cursor.
3. If the recovery point cannot cover the persisted cursor, the store is
   quarantined and reported rather than silently resuming.

### 7.3 Partition, gap, and heal

- A disconnected peer's operations are queued by the transport and released on
  reconnect; they are not lost and not applied early.
- Missing segments hold the frontier (§5).
- Retirement (§8) is the only thing that resolves a gap that can never be filled.

## 8. Membership

- Enrollment is explicit and durable. An enrolled peer stays enrolled
  indefinitely, however silent (C2).
- Coverage includes silent members: a missing member's operations keep the
  frontier from advancing past them.
- Retirement is an explicit transition that bumps the stream epoch. Retired
  members' retained undo, redo, anchor, and checkpoint obligations are released
  only at that point (R7).
- Advisory staleness reminders (proposed default 90 days, configurable) are a
  *display* concern. Keep, snooze, and dismiss never change membership, and
  clock passage never calls retirement (R9).
- A retired device preserves its local branch and enters
  export/reconciliation/rejoin before new-epoch editing. No claim is made about
  lossless merging of arbitrary old-epoch edits after reclamation.

## 9. Writer ownership

- A writer lease is held per (document, stream) across processes.
- Private actor state lives outside shared document stores, so an ordinary copy
  of a shared folder produces a *new* actor rather than a duplicate one. Copying
  private device state requires an explicit fork-identity operation that mints a
  fresh actor.
- Two identical disconnected clones cannot always be detected before they
  exchange operations. This limit is documented rather than papered over; when
  they do meet, the conflict is detected and reported as an identity conflict
  (§1.1).

## 10. Review against the findings and the multi-stream cases

**F4 — sender restart drops a distinct operation.** §1.2 makes the counter
per-(actor, stream) and durable; §7.1 requires resuming above the highest durable
value and keys dedupe on `(actor, counter)`. The failure mode needs a transport
that mints entry ids from a counter it fails to persist; the protocol removes
that counter's authority.

**F5 — persisted cursors outlive the state they cover.** §5 makes coverage a
frontier over validated, contiguous state, ties cursors to a recovery point, and
requires §7.2's rebuild-before-write. A cursor alone is not a recovery point.

**F10 — decoder success is not validity.** §3 separates syntax, schema/version,
and semantics, requires complete input consumption and bounded/checked
arithmetic, and refuses to advance coverage past retained or quarantined records.
§4 keeps "well-formed" and "applicable" as different answers.

**Multi-stream independent counters.** A `text` stream at counter 41 and a
`comments` stream at counter 3 are unrelated. Coverage is keyed by (stream,
actor) throughout; there is no scalar in this protocol that could be mistaken for
cross-stream coverage — which is precisely the failure mode of
`lowest_peer_acked_lamport()`.

**Two of these are also frozen as executable evidence:** the F8 fixture
(`buffer_concurrent_passages_stay_contiguous`) and the store fixtures in
`tst_store_fault_harness` that pin the F6 skip behaviour. Their red tests exist;
this protocol says what must change to make them green.
