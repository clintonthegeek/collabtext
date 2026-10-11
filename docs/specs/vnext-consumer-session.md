# Consumer document and recovery seam

**Status:** normative embedding guidance for W5. `DocumentSession` is an
optional one-document reference composition. A consumer may own its own event
log, block map, operation routing and transport while using the same engine
primitives.

## Canonical events and pending work

The consumer's canonical event log remains authoritative. Give every local
attempt a stable event identifier and retain the exact encoded operation bytes
and delivery token in a recoverable outbox before sending it for acceptance.
On restart, replay accepted canonical events in their original order, then
reconcile pending outbox entries against the server's canonical frontier.
Retries reuse the same event identifier, token and bytes.

An accepted event is append-only. Server rejection marks that attempt rejected;
it does not erase an accepted event or rewind the canonical sequence. A user
correction or undo is a new event with a newly allocated operation identity.
Never recycle the rejected attempt's identity. A consumer that uses
`DocumentSession` may recover its operation projection from the shared-folder
log, but that projection does not replace the consumer's canonical record or
outbox.

## Blocks, pins and history

Keep block identity and lifecycle in the consumer. Use one text engine per
independently edited block and, when suitable, an IdList for structural order.
The parent block identifier stays outside text operations and position tokens.
One `DocumentSession` commit covers only its own stream; it does not make a
multi-block conversation transaction.

Rebuild engine state and external position pins as idempotent projections of
canonical events. During historical playback, use a disposable read-only
projection with no operation allocator; navigation must not mint operations.
Recover pins to the consumer's declared frontier before enabling any future
reclamation. Engine undo limits do not retire consumer pins. Wave owns
formatting, authorization, canonical sequencing, and cross-block undo policy.

## Reference session behavior

The native `DocumentSession` opens a private `ReplicaState`, acquires a
document/stream writer lease, creates a Buffer, and replays every retained
envelope in that stream before enabling local edits. It ignores saved transport
receipts during this baseline replay. A valid operation with missing causal
data stays queued and makes the session incomplete; local edits remain disabled
until the missing history arrives and the Buffer drains its dependency queue.
Malformed operation payloads and rejected operations remain in their source
logs and keep the session incomplete. Corrupt envelope framing stops open with
a typed recovery error.

Membership enforcement remains consumer-owned. Configure
`DocumentSessionConfig::authorize_local_mutation` to return a nonnull lease
which holds the membership coordinator stable through durable local
publication. The same check gates recovery of a durable pending local edit; if
the actor is no longer authorized in that epoch, the session stays read-only
and leaves the original delivery available through `export_local_branch()`.
Configure `authorize_inbound_delivery` to return a nonnull lease which keeps
producer membership stable while replay or poll applies each delivery.
Rejected envelopes are retained in the bounded quarantine snapshot and do not
advance receipts. Without these callbacks, `DocumentSession` is deliberately
uncoordinated and must not be treated as enforcing epoch membership itself.

`DocumentSessionConfig::initial_text` supplies deterministic seed content for
sidecar formats that reconstruct their seed locally instead of storing it as a
normal operation. The session applies that content-derived seed operation
before replaying durable envelopes, so positions and editor deltas are based
on the same initial state on every device. The seed operation does not consume
the device's persistent identity or enter the actor-local undo stack.

Local apply reserves operation identity durably, appends the resulting
operation as a durable envelope, and returns success only after the log write
reports durable publication. If publication fails after the in-memory edit,
the session becomes unusable for further local edits and reports that the
caller must reopen to reconcile. Save flushes the operation log and private
identity state. Close is explicit so write failures can be returned.

The reference session currently supports a single text stream and persists the
owning actor's bounded Buffer undo/redo history alongside operation commits.
`next_undo_group()` and `next_redo_group()` expose a stable group identity and
the local operation identities represented by that group. Coalescing keeps the
older group's identity and appends the newer operations; reopening preserves
that metadata. `coalesce_last_undo()` checkpoints the boundary before reporting
success. Consumers can correlate those identities with their own event log,
while undo remains a new compensating event. The consumer owns engine and
session lifetime and may instead construct its own headless document seam
around public primitives and a custom `OpStream`. `pending_undo_retirements()`
exposes groups removed by history trimming or redo-branch truncation. Publish
each retirement idempotently under its stable group ID, then call
`acknowledge_undo_retirements()`; that call checkpoints the acknowledgment. A
crash before the acknowledgment leaves the intent available for retry.
`IdListEngine` exposes an identity-only history snapshot/restore API, including
pending retirements, for consumer-owned persistence. The consumer controls when
that snapshot is checkpointed beside canonical events and owns any multi-block
group policy.
