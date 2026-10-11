# Markoff Core adoption contract

This extension preserves native FugueMax ordering and CTLO/0002 operation bytes.
The host owns document identity, canonical bundles, Markdown, cross-stream
transactions and receipt durability. No earlier experimental store migration is
required. Initial adoption retains history and disables physical GC/log pruning.

## Ordered list identity projection

`IdListEngine::visible_entries()` returns owned values `{application_value,
identity}` for visible entries in the same order as `ids()`. `identity` is the
full original insertion ActorId/counter, never an application value or hash.
Equal application values remain distinct. Removal hides an entry; compensating
undo restores the same identity. Invalid retained identities fail the entire
query with `history_unavailable`. A discarded local view also fails explicitly.

An identity is unique within its document/stream/epoch. Markoff owns a stable
document key and a canonical injective per-block stream naming scheme containing
the full actor and counter. Never truncate/hash/map it into uint64_t. Use
`element_of(identity)` and `try_anchor_of(identity)` for exact entry addressing;
the application-value overloads retain their existing first-match semantics.

## Terminal counter gaps

`apply_counter_gap(scope, committed_bytes)` on scoped text and list facades applies an
already durably committed CTGP/0001 record bound by the host envelope to the
exact document, stream, primitive and epoch. CTGP bytes themselves contain
actor, half-open interval `[first, first+count)` and causal prior, not scope or
proof of durable absence. Core validates scope and shape but cannot prove host
transaction absence or authorization. Decoding alone changes nothing.

The host must establish definitive absence of every reserved identity in the
interval, then atomically commit the scoped gap before publishing any later
same-stream operation. An uncertain append must recover its original exact
bundle; it is never evidence of absence. Actor allocation intervals may span
multiple counters for text, including the edit identity; close the entire
unused reservation, not just its final operation counter. Do not gap counters
used by any other action in the same stream.

Application is fail-closed and atomic on refusal. Durable envelopes must use the canonical-byte overload; noncanonical spellings
are rejected even if they decode to equal fields. The value overload implicitly
uses canonical encoding. Exact canonical repeats return
`duplicate`, even after later coverage. Reusing an interval with another prior,
overlapping another gap, or overlapping already observed state returns
`actor_conflict`. A gap intersecting an offered deferred/quarantined operation
also conflicts: that input must first be reconciled by the host. Missing prior
returns `deferred` without retaining the gap; retry its durable record after
prerequisites arrive. The host must not advance an applied receipt for deferral.
Malformed, wrapped, zero or exhaustion-sentinel intervals fail checked decoding.

Successful application records the terminal interval, advances actor causal
coverage to its last counter, raises allocator observation, and retries existing
causal waits. It creates no text/list item, no local undo entry and no local-op
publication callback. Change callbacks may run for queued document operations
released by the gap. Offered document operations whose allocation overlaps a
terminal gap are rejected before ordinary duplicate handling.

The terminal registry is bounded to 4096 records and 8 MiB canonical CTGP data;
exhaustion fails with `limit_exceeded`, never forgets conflict evidence. Hosts
must retain gaps in replay order with canonical operations. Fresh engines replay
that committed sequence before writes or receipt publication. Checkpoint engine
section 4 preserves terminal records when present, including marker kind data;
sections 2/3 remain current for states without gaps. Recovery validates terminal
records against causal coverage and retained identities. A checkpoint does not
prove a gap's host authorization. DocumentSession's own durable log does not
publish consumer gap envelopes: use the Core host-owned replay path.

## Durable CTLO reader

Starting with this candidate, CTLO/0002 has the same indefinite archive-reader
obligation as the current CTBO formats: `decode_idlist_operation` must preserve
insert/remove/undo identities, dependencies, ordering and compensation semantics
for durable bytes consumers retain. Future releases keep the reader and literal
fixtures. New writer versions do not authorize rewriting old host rows/hashes.
Unknown versions return `unsupported_version`; keep original bytes. Alpha ABI,
general private-store migration and cross-version newly generated operation
byte determinism are separate and are not promised.
