# External anchor retention and history

**Status:** implementation contract for S05 and inputs to G01. GC remains
disabled until G01/G06 provide a validated reclamation certificate.

## Position lifetime classes

Position tokens are values; decoding one never creates a retention obligation.
An ephemeral cursor may be discarded at any time and is never replayed as a
durable pin. A durable internal reference (comment, bookmark, or undo selection)
and a durable external reference (consumer reply or annotation) each need an
explicit retention registration. Undo history and external references are
independent obligations. Device enrollment and presence do not own these
references: retiring an author device cannot remove another live object's pin.

The stable pin key is a consumer supplied, nonempty opaque reference ID, scoped
by document, stream, primitive and epoch. IDs are immutable and never reused.
Recreating a reference after release uses a fresh ID. A pin stores the original
encoded token bytes and their validated scope, not a reminted token or C++ object
layout. The token must decode as a supported CTPT version and match the stated
scope before registration is accepted.

## Durable pin journal and projection

The consumer's canonical event log is authoritative. Its pin projection has
these events:

* `Register(ref, scope, token_bytes)` creates a live obligation.
* `Release(ref)` permanently retires that reference ID.

Both events are append-only and carry the consumer's canonical sequence and
event identity. Replaying an identical event is idempotent. Reusing an event ID
with different bytes, or a reference ID with different registration bytes, is a
typed conflict and leaves the projection unchanged. A release for an unknown
reference is retained as a tombstone; a later registration for that ID is a
conflict. Concurrent releases converge to released. There is no undo of
release: an application undo that restores the attachment publishes a new
consumer event and registers a fresh reference ID. This makes an old release
safe to replay after a later user action.

Consumers persist canonical events through their own durability mechanism and
project them into engine pin state through a caller supplied persistence hook.
The projection advances only after the hook reports durable success. Recovery
replays canonical pin events idempotently and reports the highest contiguous
consumer sequence projected. GC is forbidden while this frontier is behind the
consumer's declared canonical frontier, while a pin event is malformed or
conflicted, or while recovery has not completed. The library must not infer
completion from a high sequence number with a gap.

For an attachment whose canonical event is not yet committed, persist its pin
projection before publishing the attachment. If a crash leaves a pin with no
canonical event, retain it as an orphan until an authoritative consumer
reconciliation explicitly releases it. An offline replica's lack of the event
is not evidence that the pin is orphaned. If a consumer instead projects
canonical pin events, the certified consumer frontier must include every event
through the frontier before collection is eligible.

Release is also ordered: commit the consumer release event, durably project the
release, then permit reclamation. A crash at either boundary leaves either the
old pin live or the release replayable. Neither outcome loses a live reference.
The pin registry is usable with any public engine composition; it does not
require FileOpStream, DocumentSession, a library-owned block tree, or a
cross-block undo manager.

## Retained context, epochs, and history

A live pin obligates the collector to preserve enough checked context to resolve
its original bytes: the referenced item/boundary, a boundary stub, or a validated
translation. It does not imply retaining all deleted text or arbitrary past
states. Every translation binds source and destination scope/epoch, preserves
the token's boundary meaning, and is validated before publication. G01/G06 may
not reclaim an identity required by a live pin unless such a translation is
available. A missing or invalid translation returns `wrong_epoch` or
`history_unavailable`; it never resolves to an edge sentinel or document end.
Author retirement changes none of these obligations.

`edits_since(version)` is a current-state delta query. It is supported only
when the engine still has the operation and visibility history needed to derive
the delta from that version. Once a checkpoint or future certified collection
advances the history floor past the requested version, the API returns
`history_unavailable`. It must not return an empty vector, because that would
claim that no edits occurred. The current implementation retains all needed
history and has no advancing floor; its floor is the initial version. This
statement does not authorize G06 to delete history.

Historical playback is different from current-state reconstruction. A consumer
rebuilds an isolated, read-only projection from its canonical operations and
matching checkpoint/epoch. Tokens resolve against that projection only. A
current pin preserves current resolution, not every earlier document state.
Playback below the consumer or engine's available floor returns
`history_unavailable`, unless the consumer can rebuild from its own archive.

## Comment and chat records

The existing internal `Crdt::Anchor` comment/chat fields are legacy
actor/counter/bias values. They have no document or epoch scope and therefore
cannot prove a durable pin. They remain usable only inside the existing
single-stream projection where the owning document/epoch is supplied by the
caller. New durable external records store CTPT bytes plus explicit document,
stream, primitive and epoch scope. Ingestion validates that scope before
resolution. A well-formed but causally unresolved supported token is preserved
byte-for-byte for retry after dependencies arrive; wrong-document, wrong-epoch,
malformed and unsupported-version input is rejected with a typed error.

The legacy comment/chat codecs do not currently carry this scope or an unresolved
token representation. They must not be presented as the durable external
anchor API; that adapter work remains gated on the public pin registry and
consumer envelope contract.

## Memory and collection accounting

Each live pin retains at least its reference ID, validated scope, original
token bytes, and any required stub/translation. Accounting reports pin bytes,
translation/stub bytes, retained text, undo metadata and operation history
separately. An indefinitely live pin set can grow without bound; only workloads
with finite pin lifetimes can claim a bounded retention plateau. G01 models
live pins and incomplete consumer projections as certificate blockers. G06
implements physical reclamation only after those inputs are independently
validated.
