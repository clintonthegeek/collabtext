# Wave Core adoption response

2026-10-11. Branch `feat/fuguemax-placement`; package `0.2.0-alpha.3`, capability
manifest v15. The new immutable candidate tag is
`collabtext-wave-core-2026-10-11` (resolve it with `git rev-parse TAG^{commit}`
in this checkout). The earlier
`collabtext-adoption-2026-10-11` at `0d21670896d02dda3dc44d18d37f9657935333ed`
is preserved. Local tagging does not distribute a package or move Wave's pin.
Exact revisions, commands and results are in the
[candidate report](../reports/2026-10-11-wave-core-candidate.md).

This answers Wave's `docs/handoff/collabtext-integration-feedback.md`. Wave
remains the canonical EventStore owner. Core adds capabilities, not another log.

## 1. Persistent zero-width marker: supported

Installed `<collabtext/CrdtEngine.h>` provides:

```cpp
auto inserted = engine.try_insert_marker(utf8_byte_offset);
if (!inserted) return inserted.error();
TextMarker reply_address = inserted.value().marker;
Crdt::Operation operation = inserted.value().operation;
auto operation_bytes = Crdt::encode_operation(operation); // CTBO/0003
auto position_bytes = encode_position(PositionToken{reply_address}); // CTPT/2
```

Both codecs are checked: handle their errors before accepting/publishing the
consumer transaction. Use one local publication source, either the returned
operation or the engine callback. Wave packages the operation and first reply's
manifest/thread creation in one authorized canonical event. If that transaction
fails after local mutation, rebuild the disposable projection from the accepted
prefix; do not acknowledge or separately publish a rejected event.

`try_remove_marker(reply_address)` produces an ordinary publishable operation.
`resolve(reply_address)` returns its current UTF-8 byte boundary. Markers have
zero UTF-8/UTF-16 width and are absent from `text()`/`length()`. Text-range
remove/replace, including whole-body replacement, preserves markers. Their
removal must be explicit by identity. Core does not choose Wave's product policy
for whether a whole-body replacement should also detach all reply threads.

At a gap containing multiple markers, numeric insertion precedes retained items
after the preceding visible character. Use exact gaps when attachment placement
matters:

```cpp
engine.try_insert_at_marker(reply_address, MarkerSide::before, "new text");
engine.try_insert_at_marker(reply_address, MarkerSide::after, "new text");
engine.try_insert_marker(reply_address, MarkerSide::after);
```

Text edit APIs still use UTF-16; numeric marker insertion uses UTF-8 scalar
boundaries. Centralize conversions in Wave. Persist binary CTPT without text
encoding assumptions and recover by `decode_position_as<TextMarker>()` plus
complete event replay into a fresh correctly scoped engine.

Removal makes the token `detached`; there is no fallback offset. A duplicate
original insertion does **not** restore a removed marker. Undoing the removal
restores its original identity; replay of a fresh engine's relevant prefix
reconstructs the same visibility. A token with unmet state reports
`awaiting_state`; missing retained history reports `history_unavailable`.
Ordinary character attachments never select markers.

Evidence: `tst_marker_model`, `tst_text_markers`, public-only `tst_wave_core` and
installed `consumer_wave_core`. They cover Unicode, adjacent deletion, two
concurrent delivery orders, exact marker gaps, causal deferral, detachment,
duplicate replay, undo/redo after metadata restoration and fresh replay.
Marker checkpoint engine section 3 retains type and identity. Initial adoption
keeps collection/pruning disabled; a token stored in EventStore is not a pin.

## 2. CTBO/0002 archive reader: supported indefinitely

`Crdt::decode_operation()` reads retained CTBO/0002 and marker CTBO/0003 into the
current opaque `Operation`, accepted by `apply_remote()`/`apply_ops()`. Keep old
event bytes, IDs and hashes unchanged. The promise covers identity, dependencies,
allocation intervals, FugueMax placement, deletion/compensation and position
referents. Future library releases must retain these readers and literal tests.
No consumer-side private payload builder or old engine linkage is necessary.
Unknown versions return `unsupported_version`; retain bytes for diagnosis.

Existing text/undo encoding stays CTBO/0002. Marker edits use CTBO/0003. CTPT/1
bytes stay unchanged; TextMarker uses CTPT/2. These reader promises do not
promise alpha ABI stability, newly generated projection byte determinism across
versions, or general checkpoint/private-store upgrade compatibility. Rebuild
against the exact package and retain/version Wave's legacy translator.

Evidence: literal CTBO/0002 insertion and undo frames in installed
`consumer_wave_core`, checked identities/attachments/visible output, retained
existing operation and position goldens. Wave must additionally verify its own
reopened EventStore hashes/IDs; upstream tests cannot assert those values.

## 3. Shared compensation and restart-local undo: supported

Publish each nonempty `try_undo()`/`try_redo()` result once as an ordinary new
operation. Replay does not populate local undo history. The text facade now
provides the same composition as IdList:

- `local_history_snapshot()` / `restore_local_history(snapshot)`;
- `next_undo_group()` / `next_redo_group()`;
- `coalesce_last_undo()`;
- `acknowledge_undo_retirements(group_ids)` and bounded depth/byte limits.

Persist this actor-local metadata in Wave's own versioned schema beside its
canonical log. Restore only after replaying all referenced shared events, with
allocation authority that cannot reuse issued identities. Persist undo cursor
changes consistently with accepted compensation events. A snapshot is a value,
not a portable C++ memory layout; use explicit fields and checked integer sizes.
Restore validates ownership/references/budgets atomically. Retirement records
remain pending until Wave durably handles them; no retirement permits unilateral
physical collection. There is no public arbitrary historical-operation undo.

Evidence: installed `consumer_wave_core` replays on restart, restores metadata,
emits distinct fresh undo/redo identities and replays them on another replica.
Use returned operations or callback, never both, as the publisher.

## 4. Legacy projection identity: supported with a stable stream actor

Use `<collabtext/Projection.h>`:

```cpp
auto allocator = ProjectionAllocator::create(
    block_scope, "wave/legacy-body", 1);
if (!allocator) return allocator.error();
auto engine = CrdtEngine::create(allocator.value(), block_scope);
```

This derives one actor from a versioned library domain, conversation/document,
block stream, primitive, epoch and adapter name/version; it starts a checked
in-memory counter at 1 and observes incoming counters. The derivation is frozen
as v1 in [the contract](../specs/wave-core-adoption.md). Independently replay the
complete accepted prefix in its canonical order with the same edit shape to
obtain identical operations on the same candidate.

Do **not** use one actor per canonical event: that grows causal vectors beyond
the 512-actor format limit. Event IDs identify and order Wave transitions; they
are not projection actors. Check namespace collisions and keep derived actors
disjoint from device writers. A projection allocator is disposable replay only,
never a writable device or independent branch allocator. Do not publish its
synthetic operations as user-authored deltas. Do not resume with a fresh counter
at an arbitrary prefix unless every earlier allocation has been reconstructed.
Retain/version the legacy translation algorithm. The archive reader promise
preserves old bytes; it does not freeze future edit-generation byte choices.

Evidence: independently computed derivation golden and two independent
translators across 600 replacements in installed `consumer_wave_core`.

## Candidate acceptance and merge

The smallest executable consumer test is
[wave_core.cpp](../../tests/consumer-smoke/wave_core.cpp), public headers only,
registered as `consumer_wave_core` by the relocated package gate. Wave should
adapt its marker trace into its own adapter/EventStore test and rerun both
existing Wave-owned probes against the exact new SHA. Record package/prefix,
Wave revision, commands, outcomes and stage-4/T-406 approval in Q06.

Upstream checks do not move Wave's adoption pin or approve production cutover.
Markoff has not started; its later real acceptance remains a merge gate. The
common [handoff](consumer-adoption.md) defines the final same-candidate checks and
explicit master merge authorization. This work creates no push, publication,
consumer message or master merge.


2026-10-11 clean-cut amendment: the user requires no compatibility for earlier
alpha stores. Superseded readers and source-compatibility parameters are removed
once current replacements pass. CME1 and unbound checkpoint engine section 1 are
unsupported; presence-only SyncManager no longer takes an ignored engine pointer.
CTBO/0002–0003 and CTPT/1–2 remain current writer formats; the reader obligation
above starts with this adoption candidate for newly durable Wave data. This is
not an obligation to upgrade earlier experimental stores.
