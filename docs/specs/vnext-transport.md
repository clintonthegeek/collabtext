# vNext transport receipt and commit contract

**Status:** normative P07 contract. This file defines the new `OpStream` surface;
`OpStream.h` and the installed API example must match it. The internal
`FileOpStream` implementation supplies this contract for shared-folder storage.
The legacy `StreamSync` interface remains a separate internal compatibility
adapter and does not publish its scalar observed-operation counter as a receipt.

`FileOpStream` stores producer records under actor, encoded stream name, and
epoch directories. Each record is a `CTDL` version 1 frame: magic/version,
length-prefixed ASCII stream name, epoch, 16 actor bytes, envelope sequence,
payload length, and exact opaque payload bytes. Receiver frontiers are separate
atomic `CTRC` version 1 files keyed by stream, epoch, and producer actor. A
commit stores the supplied recovery point in that file before exposing the new
frontier. Polling replays retained producer records and filters tokens covered
by those frontiers; this intentionally favors a simple recovery baseline over
incremental cursor acceleration. Sender receipt snapshots refresh during
`poll(scope)` by observing peer receipt files. `receipts(scope)` itself does no
I/O. This shared-folder adapter is internal and is not yet an installed public
transport component.

## 1. Separate identities and claims

An envelope delivery identity is:

```
DeliveryToken = (stream, epoch, producer ActorId, envelope_sequence)
```

`envelope_sequence` starts at 1 and is contiguous for a producer within one
(stream, epoch). It is allocated and persisted by the producer alongside its
outbound record, independently of the CRDT operation counter. A retry reuses the
same token and identical payload bytes. Reusing a token with different bytes is
an `actor_conflict`; gaps and regressions are errors. Sequence exhaustion is
`counter_exhausted`, never wraparound. The token is not an
operation identity, Lamport timestamp, or GC certificate.

Three separate milestones exist:

1. **Queued:** a transport accepted a payload in volatile memory. A process
   crash may lose it.
2. **Published durable:** the sender's recoverable transport log contains the
   payload. This does not mean any peer received or applied it.
3. **Receiver committed:** the receiving application durably incorporated the
   record, and the receiver transport durably recorded the commit together with
   a `RecoveryPoint`. Only this milestone advances a receipt frontier.

None of these milestones alone authorizes semantic garbage collection. Only a
separate validated GC certificate can do that (G01/G06).

## 2. Scope registration and push

A transport registers a `StreamScope {stream, epoch}` before use. Names are
1–128 ASCII bytes matching `[A-Za-z0-9_.:-]`, and epoch is nonzero. This matches
the current `StreamId` persistence contract. Invalid names or epoch zero return
`malformed`. Registering the same scope twice is idempotent. A different epoch
for an already registered name returns `wrong_epoch`; retirement/rejoin
transitions are outside P07. Well-formed but unregistered names return
`unknown_stream` from push, subscribe, poll and receipt queries. No operation
implicitly creates a stream.

The caller constructs `OutboundDelivery {token, payload}`. Payload is opaque
bytes; the transport preserves them exactly and does not infer application
validity from decoding. Push takes a `PushDurability`:

- `queued`: success means accepted into volatile sender-owned memory only.
- `durable`: success means the complete delivery can be recovered after the
  transport process restarts.

A failed call never reports success. `commit_uncertain` means the durability
barrier may have completed even though its result was not observed; the caller
must retry the same token and bytes. Repeating a successful token with identical
bytes is idempotent. Same token with different bytes is a conflict. The
transport must preserve increasing contiguous sequence order per producer and
scope; concurrent producers and streams may interleave. It cannot manufacture
or change producer identities, epochs, or sequence numbers.

## 3. Subscribe and bounded poll

There is at most one inbound handler per registered scope. `subscribe(scope,
handler)` returns a nonzero `SubscriptionId`; a second active handler for that
scope fails with `writer_conflict`. `unsubscribe(id)` is idempotent for an
already removed id. After it returns, no new handler invocation for that
subscription begins. It may be called from inside the handler; the current
invocation may finish. The handler is owned by the transport. Callback
exceptions are caught and treated as `retry`; they never escape `poll` and
never commit the record. Callbacks run without transport locks held. Read-only
queries and `unsubscribe` are safe from a callback. Other mutating reentry on
the same transport returns `writer_conflict` before changing state.
Subscription ids are nonzero checked `uint64_t` values; exhaustion returns
`counter_exhausted` without registering a handler.

`poll(scope, budget)` is synchronous and considers records in producer order.
`PollBudget` has positive `max_records` and `max_payload_bytes`. The byte count
is the sum of exact payload byte lengths offered during that call. If the next
record does not fit, polling stops before offering it and reports
`budget_limited`; a caller can raise the budget. A zero budget is
`limit_exceeded`. Other producers may make progress when one producer is
blocked. For one producer, later sequence values are not offered while an
earlier record is retrying or quarantined.

With no subscribed handler, poll is idle and does not consume or acknowledge
anything. A handler returns exactly one `DeliveryDecision`:

- `commit(recovery_point)`: the application has validated and applied the
  operation, or confirmed it as an identity-matching duplicate, and has durably
  written enough state to recover that result. The recovery point must cover
  this token. The transport then durably commits its cursor and publishes the
  receipt. If that transport commit fails, it reports an error and redelivers;
  replay must be safe because application state is already recoverable.
- `retry`: the operation is deferred, the application is not ready, or its
  durable write failed. Keep the original bytes, do not move the frontier, and
  stop this producer for the current poll. A later poll offers it again.
- `quarantine(error)`: the operation is permanently rejected by semantic
  validation. Retain the original bytes with the error, do not move the
  frontier, and stop this producer. Quarantining is visible and is never
  reported as a successful application.

No callback, a void callback, a thrown exception, `retry`, and `quarantine`
can advance durable coverage. Receipt publication happens only after both the
application recovery point and receiver cursor are durable. A receipt may be
replayed after restart; consumers must treat equal or lower frontiers as
idempotent and must never infer membership retirement from silence.

## 4. Recovery points, poll outcomes, receipts

`RecoveryPoint` is either `replay_from_beginning` or a nonempty opaque durable
checkpoint id. The former promises the application can reconstruct its state
from the retained source log. A checkpoint id must name a checkpoint durable
before the callback returns `commit` and containing this delivery. The
transport stores the point with its cursor; a cursor without its recovery point
is invalid. This API does not create or validate application checkpoints.

`PollReport` reports offered/committed/retried/quarantined counts, payload
bytes offered, and whether a budget stopped progress. These are transport
callback outcomes, not engine apply counts. Transport failures are returned as
typed `Error`; per-record retry and quarantine remain in the report.

`StreamReceipt` contains the acknowledging peer ActorId and the contiguous
`through` token. Its `(peer, stream, epoch, producer)` key is explicit. On a
sender, `receipts(scope)` reports remote peers' acknowledgements of this
transport's outbound producer tokens in that scope. It means that peer's
application state and receive cursor are durable through that exact producer
envelope sequence. Different streams, producers, epochs, and peers never share
a frontier, even when their numeric sequence values are equal. Receipt queries
return snapshots and perform no I/O. Receipt updates may be observed through a
subscription with the same `SubscriptionId` lifetime rules. Receipt observer
callbacks run after the receipt snapshot changes; an observer exception does
not roll back the receipt and is swallowed so it cannot make an already durable
commit appear to have failed. A receipt is transport evidence only; it is not a
membership decision, undo release, pin release, or destructive-GC authorization.

## 5. Failure and ordering rules

- A no-handler, deferred, rejected, malformed, unknown-version, or failed
  application record is never acknowledged as durably applied.
- The sender retains published bytes until its independent retention policy
  releases them. Receiver cursors cannot cause sender-side byte deletion unless
  a later storage contract makes that transaction safe.
- A transport receipt never changes engine state or an engine version vector.
- A fatal poll error leaves uncommitted records available for retry. A commit
  whose durability outcome is uncertain is reported as `commit_uncertain` and
  redelivery uses the same token and bytes.
- Implementations are single-owner unless they document stronger synchronization;
  mutation from a callback is rejected or deferred before state changes.

## 6. Adapter obligations and examples

A third-party transport must document what `queued` and `durable` mean for its
medium, how it persists producer envelope sequences, how it retains malformed
or rejected bytes, and how it orders/replays tokens. A network send success is
not `durable` unless the transport has a recoverable sender log. A peer's
receipt cannot be fabricated from a successful write or from an engine
Lamport/counter maximum.

An application wires its own durable apply step into the inbound handler. The
helper below is consumer-owned: it returns `applied` or `duplicate` only after
its recoverable state and recovery point are committed.

```cpp
auto inbound = stream.subscribe(scope, [&](const InboundDelivery& delivery) {
    auto result = application.apply_and_persist(delivery.token, delivery.payload);
    switch (result.kind) {
    case DurableApplyKind::applied:
    case DurableApplyKind::duplicate:
        return DeliveryDecision::commit(result.recovery_point);
    case DurableApplyKind::deferred:
        return DeliveryDecision::retry();
    case DurableApplyKind::rejected:
        return DeliveryDecision::quarantine(result.error);
    }
    return DeliveryDecision::retry();
});
if (!inbound) return inbound.error();

auto batch = stream.poll(scope, PollBudget{128, 256 * 1024});
if (!batch) report_transport_error(batch.error());
else record_poll_metrics(batch.value());

// Stops future calls after the current callback returns. A paused application
// can keep the registration or unsubscribe and later subscribe again.
auto removed = stream.unsubscribe(inbound.value());
if (!removed) report_transport_error(removed.error());
```

The sender chooses durability explicitly:

```cpp
OutboundDelivery delivery{token, encoded_operation};
auto queued = stream.push(delivery, PushDurability::queued);   // crash may lose it
auto stored = stream.push(delivery, PushDurability::durable); // retry same token on error
```

Production code chooses one push mode for a delivery. These two calls show the
signatures separately; they are not intended to publish one token twice with
different durability promises.

The in-memory reference adapter used by `tst_transport_commit_contract` models
these rules: it has separate stream/producer frontiers, withholds receipts
without a handler or after retry/reject, advances only after a commit decision
with a recovery point, enforces budgets, and treats token retries idempotently.
It is a contract test double, not a production transport or durability claim.
