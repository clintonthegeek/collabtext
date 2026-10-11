# Collabtext alpha capability manifest

**Version:** 15
**Package:** `0.2.0-alpha.3`
**Branch:** `feat/fuguemax-placement`
**Status:** local adoption candidate; not published. C++ API/ABI remains alpha.
Pin the exact candidate SHA and rebuild. CMake numeric package comparison uses
`0.2.0`; that alone does not distinguish adoption snapshots.

## Components and contracts

`CollabText::Core` is C++20, has no Qt/compression/crypto-provider dependency,
and exports installed public headers with opaque immutable operations and typed
positions. `CollabText::FileTransport` adds optional file-backed sessions;
`CollabText::QtAdapter` and `CollabText::Widgets` add optional Qt integration.
The Core-only installed consumer is relocated and cannot use private includes.

| Capability | Status and executable evidence |
|---|---|
| Native FugueMax text and structural IdList ordering | Available; `tst_sequence_differential`, `tst_sequence_semantics`, `tst_fuguemax_model` |
| Checked text insert/remove/replace, remote applied/duplicate/deferred/rejected outcomes, identity allocation and bounded deferral | Available; `tst_crdtengine_op_api`, `tst_operation_validation`, `tst_identity_allocation_engine` |
| Scoped boundaries, strict character attachments and list elements | Available; `tst_positions`, `tst_position_token_reader`, installed live consumer probes |
| Typed zero-width inline text markers with explicit before/after insertion, removal, detachment and undo restoration | Available through CrdtEngine; `tst_wave_core`, `tst_text_markers`, independent `tst_marker_model`, installed `consumer_wave_core` |
| Marker operations in ordinary causal stream, preserved by adjacent/whole-body text deletion | Available; Unicode and both delivery orders in `tst_wave_core`; seeded independent-tree histories in `tst_text_markers` |
| Checked shared undo/redo and bounded actor-local history | Available; `tst_persistent_undo`, `tst_idlist_undo`, `tst_wave_core` |
| Core text history snapshots/restore, coalescing, next groups and retirement acknowledgments | Available; `tst_wave_core` public replay/restore/compensation trace. Consumer owns durable metadata; replay alone never reconstructs the local stack |
| Deterministic disposable projection actor/allocator | Available through ProjectionAllocator; frozen derivation golden and 600 independently translated transitions in `tst_wave_core`/installed consumer |
| Permanent CTBO/0002 archive reader | Available; retained literal insert/undo plus identity, attachment and canonical-byte checks in installed `consumer_wave_core`; existing operation-codec goldens retained |
| File-backed session replay and persistent local undo | Available with FileTransport; `tst_session_recovery`, `tst_editor_recovery`, `tst_persistent_undo`. Does not certify a consumer's EventStore transactions |
| Public durable pin projection and source-level certified checkpoint/collection composition | Implemented with consumer/provider obligations; `tst_anchor_retention`, `tst_gc_certificate`, `tst_session_recovery`. Not turnkey installed GC; initial adoption disables physical collection and log pruning |
| Complete external Wave/Markoff production acceptance | Pending consumer-owned evidence; upstream-shaped probes do not certify production adapters |
| Broad measured performance certification | Pending X-series tasks. Edits still rebuild fragment vectors/tree; no logarithmic edit claim. This candidate removes full-text copies for UTF-16 conversion and scalar expansion for marker resolution |

## Reader obligations

The library owns these bytes, not C++ object layouts. Keep original input on
failure; decode never commits consumer coverage. Unknown versions return typed
`unsupported_version`. Supported-reader obligations are independent of alpha ABI.

| Format | Read/write versions | Promise |
|---|---|---|
| Text operation `CTBO` | 0002 text/undo; 0003 marker edits | Supported indefinitely for durable archives. Preserve item/operation identity, allocation, dependencies, FugueMax ordering, deletion and undo meaning. Decode into the current opaque operation accepted by Core; no consumer payload builder needed |
| Position `CTPT` (binary, includes NUL) | 1 existing four kinds; 2 TextMarker kind 5 | Supported persisted bytes; retain readers and semantic meaning. Existing version-1 goldens unchanged |
| List operation `CTLO` | 0002 | Checked alpha interchange; cross-version durable reader obligation not yet declared |
| Global `CTGV`, gap `CTGP` | 0001 | Checked alpha interchange; decoding a gap does not publish or certify it |
| Checkpoints/private history journals/store artifacts | Marker engine section 3, retained section readers 1/2 | Implemented checked formats, but no general cross-version store obligation. Validate upgrades against the exact candidate |

Pre-vNext CTBO/CTLO version 0001 is unsupported. Future writers must retain the
supported CTBO/CTPT readers and literal fixtures. Byte determinism of newly
translated edits across library versions is a separate, unpromised contract;
version and retain deterministic projection adapters.

## Consumer rules

- Text edits use UTF-16 code units; position and numeric marker insertion use
  UTF-8 byte offsets at Unicode scalar boundaries. No implicit normalization.
- A marker is one zero-width sequence identity, never a Unicode character or
  neighbor attachment. Text deletion preserves it; explicit marker deletion
  detaches its original token. Duplicate insertion does not restore it;
  compensating undo does.
- Tokens require matching document, stream, primitive, epoch and causal state.
  Hidden referents detach; missing retained history reports history_unavailable.
  Stored bytes create no pin. Initial adoption retains all history.
- Core returns local operations and emits synchronous callbacks. Choose one
  publication source. Checked mutation during callbacks is refused.
- Replay shared events first, then restore owner-local history with valid
  allocation authority. Fresh compensations consume fresh identities. Retirements
  need durable consumer acknowledgment before collection can become safe.
- ProjectionAllocator is only for disposable canonical-log replay, from the
  beginning in a fixed order. One stable actor per scope/adapter avoids growth
  past the 512-actor codec bound. Check namespace collisions; never use it as a
  user/device writer or publish synthetic projection operations as user edits.
- Wave owns authorization, accepted-event transactions, allocation durability,
  routing and reply/thread atomicity. Do not introduce DocumentSession as a
  second canonical conversation log.

The current handoff is `docs/handoff/wave-core-response.md`; the common adoption
and merge gates are in `docs/handoff/consumer-adoption.md`. Neither this manifest
nor a passing upstream fixture authorizes publication or a master merge.
