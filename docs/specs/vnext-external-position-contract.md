# External position tokens and consumer integration

**Date:** 2026-10-04. **Status:** accepted roadmap requirements, updated for Wave
ask2 sequencing. S04a/S04b provide the earlier boundary model/live implementation;
S04 certifies their later composition with repaired ordering and actual recovery.
S05/G-series retain pin and reclamation gates. These are planned requirements,
not a declaration that current anchors are stable. Read the
[early consumer lane](../roadmap/2026-10-reliability/00-consumer-integration.md).

The initiating consumer is Wave, whose requests are in
`/home/clinton/dev/wave/docs/handoff/2026-10-04-collabtext-devs-our-asks.md`.
Its sequencing follow-up is
`/home/clinton/dev/wave/docs/handoff/2026-10-04-collabtext-ask2-sequencing-and-probes.md`.
Its accepted ADR 0004 keeps the engine behind Wave's own `Document` interface;
the canonical conversation log, block identity, permissions, and formatting
remain Wave responsibilities. This document uses **W1–W6** to avoid confusing
their six requests with the evaluation's R1–R14 recommendation IDs.

## W1 — A token suitable for an external canonical log

S04a specifies and S04b supplies an opaque, self-contained value with a library-owned,
independently versioned encode/decode API. S04 later certifies actual durable recovery.
A consumer may store those bytes inside its own
versioned envelope, alongside its block/conversation identity. It must not copy
the C++ object layout or serialize internal `Lamport`, `Locator`, `Global`, or
fragment fields. Defining a consumer envelope does not make an internal engine
layout stable.

The codec binds document, stream, primitive kind, epoch, boundary identity
context, and checked affinity. Boundary representation and canonical bytes are
specified at the S04a/S04b gates, not invented here. S02/S03 must preserve the same
meaning through the ordering repair. Readers reject unknown versions,
malformed fields, unsupported affinity, and foreign context explicitly. Decode
must not create a pin or mutate document state.

The published vNext token format becomes a persisted-data contract. Future
supported releases must keep reading published token versions and retain the
translation needed for live registered pins; changing C++ layout or introducing
a new encoding version does not invalidate existing bytes. This future contract
does not require conversion of today's experimental anchors or pre-vNext stores.
Unknown future formats remain explicit unsupported-version errors.
E07b reviews and records supported token versions before an approved position
alpha advertises persistence. Alpha C++ API/ABI changes do not waive supported
published token reader obligations. The earlier package alpha E07a may list
positions as unavailable and prototype encodings as experimental; it does not
certify those encodings merely by packaging them.

The same stored bytes resolve after engine destruction, reopen, remote editing,
and replay on another replica that has the required causal state. A decoder alone
cannot recreate missing document history. A supported token awaiting dependencies
is distinguishable from malformed, foreign, retired/untranslated, or unavailable
history. Callers can retain the token and display the outcome without relocating
it to a convenient offset.

## W2 — Boundaries, affinity, and meaningful resolution results

Mint scoped tokens at every valid Unicode scalar boundary, including offset zero,
end, and the empty document. Start/end context is part of a real document-scoped
boundary value; a global `Anchor::max()` cannot stand in for every document's end.
Resolution yields a typed position **with an offset**, or a typed error. It is not
an operation-application outcome without a position.

S04a specifies and S04b implements left/right affinity for text boundaries separately from IdList
element identity. Text offsets are UTF-8 bytes on scalar boundaries; list positions
are element indices. Adapters must distinguish these from Qt UTF-16 positions.
Resolving a boundary collapsed by deletion must report that condition when it
affects placement, rather than imply the original passage still exists. Exact
collapse rules come from the boundary specification and sequence fixtures.
Deletion of one neighboring character does not necessarily destroy a caret
boundary. Strict content attachments must surface loss/collapse rather than a
bare relocated integer; consumers may map that information to detached/stale UX.
The precise referent/undo rules are a design gate, not a universal fail-on-delete
assumption. Wrong context, malformed encoding and off-end minting always reject
explicitly. A consumer checks containing block existence; the engine independently
checks token scope before looking up any item and must not assert or abort.

Convergence compares the resolution of the **same minted token**, not separately
minted tokens from possibly different causal histories. Test every delivery order,
duplicate delivery, and restart around concurrent inserts at the boundary. Tokens
minted later must also compare consistently in the converged state.

An anchor preserves a content boundary under specified operations. It does not
recognize semantic intent such as "the second paragraph" after arbitrary passage
replacement. The consumer decides how to render a collapsed/deleted attachment.
An IdList lookup miss must return an explicit not-found/unresolvable result;
it must never fabricate an end sentinel that another API treats as an element.

## W3 — External pins, compaction, and epoch changes

Storing bytes in an external log is **not** notification that the engine must
retain their referents. S05 provides explicit durable pin registration, lookup,
and release using stable consumer reference IDs. Pins belong to the lifetime of
the referenced document object, such as a live reply, rather than to the presence
of the device that authored it. Retiring that device does not release a live
reply's pin. Undo limits and ephemeral cursor timeouts do not release it either.

S05/G01 specify the pin state machine, including idempotent registration,
replay/recovery, concurrent release, permitted undo of release, and incomplete
external-log projections. An adapter must establish durable pin protection before
publishing an attachment that depends on it, and recover pins before enabling GC.
Two unrelated successful writes do not establish atomicity across the external
event log and the engine store. Define the actual protocol: for example, a durable
pin before an external append with conservative orphan retention, or canonical
pin events projected before a certified consumer frontier. Do not release an
orphan just because an offline replica has not seen its corresponding event.

Indefinitely retained live pins require retained boundary context or durable
translations. They do not necessarily require retaining all deleted text, but
the metadata has a cost which must be measured. The G-series model/certificate
accounts for this cost and for supported historical queries. Every enrolled
participant's necessary coverage, undo/redo, pin, and checkpoint obligations
still matter. A scalar receipt never overrides them.

For an approved epoch transition, existing supported token bytes continue to
resolve through a validated translation while their pin is live. Without the
required translation/history, return an explicit unavailable/wrong-epoch outcome.
Physical reclamation must never convert a pin miss into end-of-document.

External replay has two distinct cases:

- **Current-state reconstruction:** replay canonical events and re-establish pins
  before current-state reclamation can proceed.
- **Playback at an earlier event sequence:** use an isolated historical projection
  with the required operation history/checkpoint and matching epoch. Resolve
  against that projection, not today's live document. A current-state pin does
  not preserve all historical text, and `edits_since` is not unlimited playback.

A consumer needing unlimited playback must retain the necessary canonical
operations/checkpoints itself, or explicitly retain an engine archive under a
defined policy. Requests below the supported engine history floor return
`HistoryUnavailable`; the adapter may rebuild from its archive. The engine does
not promise unlimited history from a bounded current-state checkpoint.

## W4 — Formatting belongs above this plain-text engine

The active roadmap deliberately excludes a built-in rich-text/attribute CRDT.
This is considered and excluded from the current release scope, rather than
unconsidered or implicitly scheduled. Wave should own formatting and annotation
events above its `Document` seam, using stable anchored spans when available.
Anchor support alone does not define concurrent attribute merges, overlapping
spans, insertion inheritance, deletion, or formatting undo. Those require Wave's
own specification and tests. Installed consumer examples can demonstrate storing
and resolving span endpoints; they must not masquerade as a formatting engine.

## W5 — Durable embedding without competing canonical logs

A02a exposes the implemented primitive facade early; A02 later certifies the complete
supported surface. L05's native `DocumentSession` is an optional reference composition,
not the only way to embed primitives or own recovery. Consumers may use their own
log, per-block engines, operation routing, history coordination and transport.
Keep Wave's `Document` interface; adapt underneath it. The existing
`CollabDocument` stub and private source headers are not the supported seam.
One text Buffer per content block and an optional IdList for structural ordering
remain valid compositions. Map each independently edited block to an explicit
document/stream identity and keep the parent block ID outside its text token.
IdList payloads are uint64 values, while Wave block IDs may be strings: use the
consumer structure directly or a collision-checked canonical mapping; do not
truncate or blindly hash IDs into the list.

L05/L06 specify append-only compensation, persistent local undo/redo groups,
idempotent operation/pin replay, and a read-only historical projection that does
not mint new operation IDs. Wave owns user authorization and its canonical event
sequence; neither a CRDT timestamp nor successful decode grants permission.
Engine checkpoints and local journals are recoverable projections/outboxes, not
a second authority that can silently contradict accepted Wave events.

The consumer adapter must specify optimistic local edits, server acceptance or
rejection, restart with pending outbox records, and reconciliation with the
canonical frontier. A compensating undo is a new event; removing an accepted
event or reusing a rejected attempt's operation ID is not a valid shortcut.
Multi-block transaction/undo semantics and Wave's server protocol remain consumer
work; the library's per-session durability does not imply a conversation-wide
transaction. Whole-block replacement is Wave's interim fallback, not an engine
guarantee of character-level merge fidelity.

L01 supplies filesystem primitives. L02–L08 must still connect them to segments,
cursors, session save/reopen, history, and actual editor behavior. A directory
barrier failure after publication is an uncertain commit requiring reconciliation,
not proof that nothing changed. Linux subprocess tests do not certify power-loss
behavior or Windows/macOS support.

## W6 — Installed boundary and consumer gates

A01/A02a/A06a provide self-contained implemented public headers and installed CMake
targets early, without waiting for complete recovery/anchors. W02 provides the
installed baseline and W03 requires usable live positions; A02/A06 certify the
complete supported API and consumer scenarios later.
Headless consumers must build with only the selected Core/session/transport
components and without mandatory Qt Widgets/Test. The consumer owns its outer
log encoding; the library owns the persisted anchor codec. No source-tree include
paths or object-layout assumptions are permitted in the smoke project.

Use self-contained engine fixtures modeled on Wave's requirements. Do not make
tests depend on a sibling Wave checkout or implement Wave's application for it.
Its original evaluation probes are useful historical evidence, but the new
release gate uses current public APIs and exact contract expectations.

## Required named cases and implementation ownership

| Case | Exact acceptance | Tasks |
| --- | --- | --- |
| `external_token_roundtrip_reopen` | Same encoded token resolves after reopen and replica replay | S04, S05, L05, A06 |
| `boundary_affinity_ab_insert_X` | Old left/right boundary at 1 resolves to 1/2 in `aXb` | S01, S02, S04 |
| `scoped_empty_start_end_tokens` | Empty/start/end values have scope, affinity, and defined later comparison | S02, S04 |
| `shared_token_concurrent_boundary_inserts` | Every replica resolves the same token identically in every delivery order | S02, S04, S06 |
| `deleted_neighbors_explicit_collapse` | Retained deleted neighbors use specified placement and report collapse | S02, S04, S06 |
| `foreign_or_unsupported_token_is_explicit` | Wrong context/version never resolves accidentally or changes state | S04, A02 |
| `missing_list_id_is_not_an_end_anchor` | Lookup miss is typed; insert/remove cannot mutate using it | S04, A02 |
| `external_pin_recovery_before_gc` | Crash/reopen restores pin obligations before reclamation is allowed | S05, L05, G01, G08 |
| `unregistered_or_released_token_is_explicit` | Missing protection/history has a visible failure, never fabricated end | S05, G06, G08 |
| `retire_device_preserves_live_reply_pin` | Device retirement does not discard a live document reference | S05, G01, G06, G08 |
| `pinned_token_survives_checkpoint_epoch_translation` | Original bytes resolve after permitted GC and epoch translation | S05, G01, G06, G08 |
| `historical_projection_resolves_its_own_tokens` | Earlier playback uses matching historical state; below-floor live query errors | L05, S05, A06 |
| `consumer_commit_pin_crash_matrix` | Partial external-log/pin/outbox progress cannot authorize premature GC or lose accepted work | L05, S05, G01, G08 |
| `installed_headless_wave_contract` | Installed-only consumer covers tokens, pins, compensation, reopen, and explicit errors | A02, A06, E05 |

The named cases above retain their final-certification owners. Early ownership
is additionally explicit: S04a/S04b/W03 cover affinity, scope, empty/end, collapse,
wrong-context and missing-list cases; S04b/W03 cover token codec and fresh-process
operation replay; S04 supplies actual store/session reopen certification. W02
reports all eight WP probes, W03 requires WP1–WP5/WP7, and E05 requires the full
promised set. The registry and consumer lane contain the exact staged ownership.
An unsupported/known-failure baseline result is not passing feature evidence.

S01/S04a review boundary semantics before S04b implements them; S02/S03 later
repair ordering against that contract. S05 makes retention
explicit; G01's independent model proves GC obligations before G06 enables physical
reclamation. A06a/W03 prove the early installed seam; A06 extends it and E05 reruns it with the completed
reclamation implementation. Writing this requirement document alone completes none
of those tasks and supplies no delivery date.

W01 turns W1–W6 into checked-in cases: the eight consumer probes have permanent
ids, typed expectations, per-expectation provenance, and a validator in
[vnext-consumer-probes.md](vnext-consumer-probes.md), with fixtures under
`libs/collabtext/tests/fixtures/external-position/` and an honest record of what
could not be verified in
[the provenance report](../reports/vnext-consumer-probe-provenance.md). W01
specifies cases; it supplies no evidence that the engine passes them, and its
expectations derived from this document rather than from the consumer are tagged
as such.


S04a resolves the pending deletion rules in
[vnext-boundary-semantics.md](vnext-boundary-semantics.md): a gap before b in abc
collapses as a caret to byte 1 with lost_right, while a strict following-referent
attachment detaches without an offset. A valid undo restoring b makes the same
attachment resolve at byte 1; a redo detaches. Loss of only its other neighbor
need not detach it. Empty/end are scoped edge tags; local_clear invalidates
position history even when a token has only edge tags. These are specified and
model-checked choices; S04b still implements production behavior and token bytes.
