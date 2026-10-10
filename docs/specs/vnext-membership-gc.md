# Membership epochs and reclamation certificates

**Status:** normative G01 design gate. **Revision:** 1. **Date:** 2026-10-10.
**Depends on:** D01, S06, L08, P07. **Implementation:** G02/G03/G04/G05/G06.

This specification defines the only protocol that may authorize physical
reclamation. `collect_garbage()` and compaction remain inert until G06 implements
the certificate described here and validates it at the deletion boundary. A
transport receipt, a `Global` maximum, a reminder, or a successful checkpoint
write alone is never authority to delete state.

The protocol follows the resolved user decisions in
[`DECISIONS.md`](../roadmap/2026-10-reliability/DECISIONS.md) Q2/Q3 and the
binding constraints in [`vnext-design-constraints.md`](vnext-design-constraints.md)
C2/C3/C5/C6/C10/C14 and R5/R7/R9/R10. Devices may remain offline forever;
local undo and redo survive reopen within configured limits; shared checkpoints
do not replace an actor's private history.

## 1. Identities and evidence domains

These values are independent and may not be substituted for each other:

| Value | Meaning | Scope |
|---|---|---|
| `ActorId` | One durable device/producer identity | Global identity namespace |
| Operation coverage | Integrated operation identities, including causal dependencies | One document stream and producer actor |
| Delivery coverage | Contiguous committed `DeliveryToken` sequences | One stream, epoch, and producer actor |
| Undo-retirement sequence | Durable removal of local restoration groups | One owning actor and its private history log |
| Pin-projection frontier | External canonical pin events replayed completely | One consumer projection/log |
| Epoch descriptor hash | Exact enrolled membership and transition base | Whole membership epoch |
| Checkpoint hash | Exact validated serialized state | One checkpoint artifact/version |

Operation identities and transport delivery tokens are not interchangeable.
Coverage is kept per stream and per producer. The same counter in two streams is
two unrelated facts. Missing coverage is zero. Common coverage is the
component-wise minimum over **every member of the exact epoch**; it is never the
maximum, union, last-seen value, or a scalar watermark.

## 2. Immutable membership epochs

An epoch descriptor is immutable and content addressed. It contains:

- a nonzero epoch number and the hash of its parent descriptor (except genesis);
- the complete sorted set of enrolled `ActorId`s;
- the transition event hash and the validated base checkpoint hash;
- the per-stream operation and delivery frontiers at that base; and
- the supported token versions and the translation manifest hash carried from
  the parent epoch.

The actor set is nonempty. Every stream in the descriptor has an explicit base
frontier, including zero. A member omitted from a descriptor is not considered
retired because it went quiet; omission is valid only as the result of the
explicit retirement transition below. An unknown actor or a descriptor fork
blocks certificate construction until reconciled.

### 2.1 Enrollment

1. An enrolled member proposes an immutable enrollment event naming the exact
   parent descriptor, new actor, and bootstrap checkpoint/frontiers.
2. Every currently enrolled actor acknowledges that event and base checkpoint
   durably. The joining actor separately acknowledges that it durably installed
   the bootstrap checkpoint and will begin producing only in the new epoch.
3. The event becomes the next descriptor only after all those acknowledgements
   are recoverable. The new actor joins at the exact base; it cannot claim
   history or coverage it did not install.
4. Until the transition is complete, no certificate may be issued from either
   the transition proposal or a mixture of old/new membership evidence.

This unanimity rule can stall when an existing member is offline. That is
intentional: explicit retirement is the only way to remove that obligation.
Enrollment does not silently skip an offline device.

### 2.2 Explicit retirement and returning devices

Retirement is an explicit, durable, user-confirmed event naming the device and
the affected documents/streams. The event records the consequence preview and
the last locally known recoverable branch. The retiring device's branch remains
preserved on that device for export/reconciliation; it is never silently
rewritten to the new epoch.

Every non-retiring member acknowledges the exact retirement transition and its
validated base checkpoint. The retiring device need not be reachable; its
durable confirmation is the explicit membership event. If required remaining
members are offline, the transition stays pending and blocks reclamation. The
last enrolled actor cannot be retired into an empty epoch; document/archive
deletion is a separate explicit lifecycle operation. No
timeout, “stale for review” flag, presence expiry, reminder display, snooze,
dismissal, or missing acknowledgement is a retirement event.

A retired device that returns must retain its old local branch and export or
reconcile it explicitly. It cannot mutate the current epoch using its old
membership. Rejoining requires a new enrollment and bootstrap in the current
epoch. The protocol does not promise an automatic lossless merge for arbitrary
old-epoch edits after a certificate has removed their context.

### 2.3 Concurrent proposals, interruption, and recovery

Every transition names one parent hash and one event hash. A transition whose
parent is no longer current is stale. Concurrent incompatible proposals form an
unresolved branch; implementations do not choose a winner by arrival order,
wall clock, or actor counter. They block new certificates until all current
members reconcile onto one immutable descriptor.

The proposal, acknowledgement set, descriptor publication, and checkpoint
publication are durable records. On reopen, recovery reconstructs the same
state from those records:

| Crash/recovery state | Membership result | Certificate result |
|---|---|---|
| Proposal missing or incomplete | Existing descriptor remains current; proposal may be retried | Block while proposal state is ambiguous |
| Proposal durable, acknowledgements incomplete | Transition pending; old membership retained | Block |
| All required acknowledgements durable, descriptor not published | Recover and publish the same descriptor, or remain pending | Block until descriptor is recovered |
| Descriptor published and checkpoint verified | New descriptor is current | New-epoch evidence is required |
| Two proposals share a parent but differ | Unresolved branch | Block both until explicit reconciliation |

An implementation may durably abort an incomplete proposal only with a
recoverable abort record accepted by every member whose acknowledgement could
have been published. Silence cannot abort it.

## 3. Durable frontiers and undo obligations

For a stream `s`, actor `a`, and member `m`, evidence is a tuple of independently
validated frontiers:

```
E(m, s, a) = (operation_coverage, delivery_coverage, undo_retirement)
```

Operation coverage means the receiver integrated that causal operation prefix.
Delivery coverage means the application state and matching recovery point are
durable and the transport receipt committed that contiguous token prefix, as
specified in [`vnext-transport.md`](vnext-transport.md). A deferred operation,
quarantined payload, missing callback, undecodable segment, or merely durable
sender copy does not advance the receiver's coverage.

Undo and redo groups are private to their owning actor and are durable across
reopen. Each retained group names every origin/state needed to restore it. If a
candidate deletion removes any such state, eligibility is false. Trimming a
group or truncating a redo branch creates a stable, idempotent retirement event;
the owner publishes it only after the new private history state is durable. A
crash before acknowledgement leaves the event pending and therefore keeps the
obligation alive.

For each producer and stream, common operation and delivery coverage are the
minimum among all current members. Undo-retirement coverage is likewise the
minimum per owning actor. A member's acknowledgement is valid only when it is
durable and bound to the exact current epoch, proposal, checkpoint, and pin
projection. A stale ack from an earlier epoch or competing proposal is not
reused. An unavailable member is still a member and must supply current evidence
or remain a blocker.

F1/F2 are represented as explicit negative cases: an insertion retained in any
actor's redo group blocks deletion of its origin; a remote actor's retained
undo/redo group blocks deletion of the state it can restore. An actor's
acknowledgement must prove its current private history is recoverable and that
all earlier trim retirements through the proposed floor are durable. Shared
checkpoints never import another actor's undo stack or writer identity.

## 4. Checkpoints and external anchors

A checkpoint used by a certificate is immutable, versioned, durable, integrity
checked, and bound to one descriptor hash and exact common frontier. Its
shareable section contains sufficient logical state to resume operation
integration: text/list runs or entries by stable identities, causal/version
state, pending/deferred operations and their original bytes, coverage, external
pin projection, and the boundary/translation metadata needed by live supported
tokens. Derived tree/index pointers are rebuilt and validated; process addresses
and C++ object layouts are never serialized.

Actor-private undo/redo groups, writer identity/counter reservation state, and
their local journals remain private durable artifacts. They are paired with the
checkpoint by their owners and validated in member acknowledgements. A shared
checkpoint cannot replace them. An incomplete or corrupt checkpoint cannot
advance a frontier; replay from retained source data remains necessary.

External pin events are replayed to each consumer's declared canonical frontier
before collection is enabled. The pin projection must prove completeness, not
just a count. A live pin belongs to the referenced document object, not to the
device that authored it. Retirement never removes another live object's pin. If
the certified deletion would remove a pinned boundary's source identities, the
checkpoint must carry a validated translation for the original supported token
bytes, and that translation must remain available while the pin is live. Missing
or incomplete projection, token reader, or translation blocks the certificate.
An orphan pin is conservatively retained until canonical projection resolves
it. Unlimited live pins imply an explicit metadata cost; this protocol does not
claim a fixed bound while references remain live. The cost is reported as exact
stored bytes, not object-count estimates:

```
retained pin bytes = live canonical pin-event bytes
                   + live projection/index bytes
                   + live token-translation artifact bytes
                   + unresolved orphan-event bytes
```

Every category is counted while live and falls only after its durable release
and projection checkpoint permit removal. Saturating arithmetic is used when
summing untrusted or extremely large inventories. G08 records the concrete
encoding sizes, retained-byte curves, and workload results; an unbounded count
of live pins is never advertised as bounded memory.

Historical playback at an earlier event sequence requires a matching archived
projection/checkpoint and epoch. A current-state checkpoint or pin does not
preserve arbitrary historical text. Requests below the preserved floor report
unavailable; they are never relocated to document end.

## 5. Certificate contents and eligibility predicate

The immutable certificate binds:

```
certificate_id
epoch_number, epoch_descriptor_hash
per-stream, per-producer common operation coverage
per-stream, per-producer common delivery coverage
per-owner common undo-retirement coverage
checkpoint format/version/hash and exact covered frontiers
consumer pin-projection identities and frontiers
supported position-token versions and translation-manifest hash
all-member acknowledgement set and its digest
```

Let `M(E)` be the nonempty actor set in the finalized descriptor for epoch `E`.
Let `O_m`, `D_m`, and `U_m` be member `m`'s validated operation coverage,
delivery coverage, and per-owner durable undo-retirement vector. Let `P_m` be
its acknowledged complete external-pin projection frontier. A candidate `C` is
eligible only if all conditions hold:

1. `E` is the exact current finalized descriptor: no transition is pending, no
   competing descriptor branch exists, and no unknown member evidence exists.
2. A validated durable checkpoint `K` has the same epoch hash, exact candidate
   frontiers and integrity hash. It contains complete logical state, deferred
   input, coverage, pin projection and required token translations. Every
   enrolled member's private undo/redo state is independently recoverable.
3. For every `m in M(E)`, a durable acknowledgement names the exact `(E, C, K)`
   and proves recoverable application state. Missing acknowledgements block.
4. `C.operation[s,a] = min(m in M(E), O_m[s,a])` and
   `C.delivery[s,a] = min(m in M(E), D_m[s,a])`, with absence interpreted as
   zero. `C.undo[owner] = min(m in M(E), U_m[owner])`. The proposal contains
   these exact intersections; an overclaim is rejected.
5. No retained undo or redo group on any enrolled actor can restore data in the
   deletion range. Each retired group through `C.undo` is durable in the owner's
   private history and acknowledged by every member.
6. Every external pin projection is complete through the candidate frontier. A
   live pin whose referent/boundary would be removed has a validated retained
   translation for its original supported token version.
7. The certificate is recomputed from its bound records after restart. A hash
   or ID supplied by a caller is only a lookup key, never proof by itself.

An empty member set, one missing/unknown/stale member, a deferred or quarantined
operation in the required prefix, a retained remote undo/redo, absent anchor
translation, corrupt checkpoint, or incomplete external projection makes the
predicate false. No availability heuristic changes this result. With exactly
one enrolled actor, that actor's durable local evidence may satisfy the
intersection; with no available actor to produce current evidence, no new
certificate is possible.

## 6. State machine for proposals and collection

```
CurrentEpoch
  -> Candidate(checkpoint, exact common frontiers)
  -> AwaitingMemberAcks
  -> Validating (all coverage, undo, pin, translation, checkpoint inputs)
  -> CertificatePublished (immutable, epoch-bound)
  -> CollectionRestrictedToCertificate
```

Any missing evidence returns the candidate to a retained/pending state. The
candidate can be retried after recovery, but its ID cannot be reused with
different bytes. A membership transition, changed frontier, changed checkpoint,
new pin event, undo/redo mutation, or competing candidate invalidates the
unpublished proposal; the system constructs a new proposal ID. A published
certificate remains proof only for its exact epoch, checkpoint, and covered
range; it is not widened by later observations. A newer epoch needs a new
certificate.

G06 must validate the certificate at the physical deletion call and delete only
the covered artifacts the certificate enumerates. Live tails, incomplete
segments, pending local-history retirements, external pin events, and data beyond
the checkpoint/frontiers remain protected. Deletion itself must use the durable
publication/barrier requirements in L01/L02. An uncertain directory barrier is
reconciled from disk; it is not treated as proof that deletion did or did not
occur.

## 7. Independent model and adversarial cases

[`GcModel.h`](../../libs/collabtext/tests/reference/GcModel.h) is an independent
value model. It includes no production engine, coverage, pin, store, or
checkpoint helper. `tst_gc_model` exercises the predicate and state transitions
against explicit cases and enumerates all 729 bounded combinations of three
members' undo/redo slots at, below, or beyond a proposed floor. Named cases cover
F1 insertion redo, F2 remote undo, F3 component-wise intersection, missing and
stale member evidence, offline membership, explicit retirement, returning-device
branch preservation, enrollment bootstrap, competing transitions/proposals,
checkpoint corruption/incompleteness, deferred and cross-stream coverage, live
pin survival after author retirement, and missing translations.

The model is a bounded executable gate, not an implementation or proof for
unbounded histories. G02/G03/G04/G06 must differentially check their serialized
state and production eligibility predicate against it before physical GC can be
enabled. W-series cases still owned by later implementation layers remain
pending; this gate specifies their inputs and invariants but does not claim those
later behaviors are implemented.
