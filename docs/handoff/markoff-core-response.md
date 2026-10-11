# Markoff Core adoption response

2026-10-11. This answers Markoff's
`docs/handoff/2026-10-11-collabtext-vnext-consumer-asks.md`.
New candidate: `collabtext-markoff-core-2026-10-11`, package `0.2.0-alpha.4`,
capability manifest v16. Resolve its exact SHA with
`git rev-parse collabtext-markoff-core-2026-10-11^{commit}`. Earlier adoption and
Wave tags remain immutable. Source/wire behavior for existing `ids()` and
CTLO/0002 operations remains unchanged. Rebuild for the alpha package.

## Ask 1: ordered full insertion identities — supported

Installed `<collabtext/IdListEngine.h>` provides:

```cpp
auto entries = list.visible_entries(); // Result<vector<VisibleIdListEntry>>
if (!entries) return entries.error();
for (const auto& entry : entries.value()) {
    // entry.application_value is caller-owned; it may repeat.
    // entry.identity is the full original insertion ActorId + counter.
    auto anchor = list.try_anchor_of(entry.identity);
    auto element = list.element_of(entry.identity);
}
```

Only visible entries are returned, in exactly `ids()` order. Concurrent inserts
with equal values stay distinct; remove hides an entry and compensating undo
restores the same identity. Fresh encoded replay preserves identity and order.
Invalid retained state fails the whole query with `history_unavailable`.
A discarded local view fails explicitly instead of returning a valid empty view.
Application-value overloads keep their first-match behavior; Markoff should use
the identity overloads when values repeat. Returned values contain no tree state.

Use the insertion identity as BlockId within a stable document namespace.
Persist a host document key; an ordinary document copy retains or explicitly
forks that namespace according to Markoff policy. Include the full actor and
counter in an injective stream name (for example `block:<32 lowercase actor hex>:<16 lowercase counter hex>`).
Keep the structural-list scope and epoch in the document's authority. Do not
hash/truncate/map identities to uint64_t or use application values as addresses.
Core cannot choose which two host files represent the same document.

## Ask 2: applicable terminal gaps — supported through Core

Both scoped facades provide:

```cpp
// Before this call: host has proved definitive absence and durably committed
// this exact scoped canonical record. The gap covers the entire reservation.
auto bytes = Crdt::encode_counter_gap(gap);
if (!bytes) return bytes.error();
// Host transaction/barrier here; publish no later same-stream operation yet.
auto outcome = engine.apply_counter_gap(scope, bytes.value());
```

Use the canonical-byte overload for durable host envelopes. The value overload
is available for checked composition. The scope binds document, stream,
primitive and epoch; CTGP alone does not encode those fields or authenticate
absence. The host record must bind them. Decoding a gap alone changes nothing.

A successful gap advances causal coverage without making an edit or an undo
entry. It raises allocator observation and releases previously deferred document
operations whose prerequisites are now satisfied. It emits no local-operation
publication callback; released document operations can emit change callbacks.
Exact retries return `duplicate`. Overlapping gaps, changed prior/interval,
already-covered intervals and conflicting deferred/quarantined allocations
return `actor_conflict`. Operations reusing a terminal allocation are rejected
before normal duplicate handling. Noncanonical byte spellings are rejected.
Missing prior returns `deferred`; gaps are not queued, so retry the durable host
record after its prerequisites arrive. Never advance an applied receipt on
deferral or rejection.

An uncertain append must inspect/recover its exact original bundle before any
gap decision. On definitive failure, discard/rebuild the tentative CRDT view
from the accepted prefix; do not apply a gap over the operation in that view.
Durably reserve the full interval, prove the bundle absent, commit the gap, apply
it to the rebuilt writer, then resume writes.
Capture `prior` from the accepted per-stream prefix before the tentative action.
The host can track it with Global::observe(op_identity) /
Global::observe(idlist_op_identity), and each accepted gap
`{actor, first + count - 1}`. Instrument successful reserve_run calls to retain
the exact `(first, count)`; do not infer unused intervals from a largest counter.
Uncertain reservation durability also requires recovery before a gap decision. Restart replays gaps and operations
in accepted order before writes or receipts. Never fabricate a placeholder edit.

Terminal evidence is bounded to 4096 records/8 MiB per stream; reaching the bound
stops additional gap acceptance rather than dropping conflict history. Section-4
engine checkpoints preserve terminal records; no-gap states retain sections 2/3.
Initial adoption keeps GC/pruning disabled. This is a Core host-log path;
DocumentSession does not publish Markoff gap envelopes for you.

## Ask 3: durable CTLO/0002 reader — supported indefinitely

`Crdt::decode_idlist_operation()` retains CTLO/0002 insert/remove/undo archive
semantics, including original insertion identities, causal dependencies,
FugueMax placement and shared compensation. Future releases retain the reader
and literal fixtures for durable bytes written under this adoption commitment.
Keep old host rows, event IDs and hashes unchanged. No independent consumer
archive decoder or private operation builder is required. Unknown versions
return `unsupported_version`; retain the original bytes.

CTGP/0001 also gains a retained-reader obligation for the committed terminal
metadata required by this host replay path. This commitment starts with the new
adoption cut. It creates no earlier experimental-store migration obligation,
alpha ABI promise, general private-checkpoint upgrade promise or guarantee that
future writers generate the same new operation bytes.

## Evidence and consumer acceptance

The smallest installed public-only test is
[markoff_core.cpp](../../tests/consumer-smoke/markoff_core.cpp), registered as
`consumer_markoff_core`. It covers repeated values, concurrent actors,
identity-addressed deletion/undo, fresh replay, independent literal CTLO
insert/remove/undo archives, text/list gap deferral and retry, conflicts, and a
synced host-log/private-reservation reopen boundary with two replay peers.
`consumer_wave_core` remains present for Wave's existing contract.
The independent gap model precedes engine integration; checkpoint tests cover
section 4, markers, truncation and retained terminal conflict detection.

The [normative contract](../specs/markoff-core-adoption.md) settles exact details.
The [candidate report](../reports/2026-10-11-markoff-core-candidate.md) records
commands, revisions, counts and available Markoff-owned probe results.
Markoff must integrate these APIs into its real BlockId/routing/persistence seams
and record exact-candidate production acceptance in M06. Wave's Q06 remains open;
final same-candidate checks and explicit merge authorization still govern master.
Nothing here changes Markoff's pin or authorizes a production cutover.

Terminal unused identities cannot be insertion parents/right origins, deletion
referents or compensation targets. Core rejects these document records with
actor_conflict and refuses abandoned tentative undo metadata with
history_unavailable. Rebuild from the accepted prefix and restore only accepted
owner history after a definitive append failure.
