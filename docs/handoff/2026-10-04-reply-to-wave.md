# Draft reply to Wave: external positions and integration gates

**Date:** 2026-10-04. **Delivery:** draft for the project owner to relay; no message
has been sent and no Wave files have been changed.
**Reviewed:** your handoff, accepted ADR 0004, protocol 0001, and stage-4 planning.
**Implementation branch:** `roadmap/reliability-v2`; master remains the original
evaluated source. [Independent progress review](../reports/2026-10-04-reliability-progress-review.md).

Your requirements fit the engine we are improving. We accept the external
position-token, affinity, retention/error, durability, and installed-header asks
as explicit acceptance criteria. They are technically viable within the existing
C++ Buffer/IdList architecture, subject to the sequence and reclamation design
gates. They are **not yet available as a supported integration contract**, so
your stage-4 gate and whole-block replacement fallback are appropriate.

We have amended [the implementation roadmap](../roadmap/2026-10-reliability/README.md)
and added a binding [external-position contract](../specs/vnext-external-position-contract.md).
The following describes committed scope, rather than an availability date:

| Your ask | Our answer and implementation ownership |
| --- | --- |
| R1: externally persisted token | S04 provides an opaque value and library-owned, independently versioned encode/decode. You own the outer canonical-log envelope. Supported published vNext token bytes remain readable independently of C++ layout; copying Anchor/Locator/Global fields is not a stable encoding. |
| R2: affinity and real end token | S01/S02/S04 specify scoped empty/start/end boundaries, deletion collapse, and exact left/right behavior. `ab` → `aXb` must resolve the old boundary to 1/2. S06 compares the **same minted token** across concurrent delivery orders, duplication, and restart. |
| R3: GC survival or explicit failure | S05 exposes durable external pins; G01/G06/G08 account for them in certificates and epoch translations. Registered live tokens retain necessary context/translation. Missing context/history is a typed outcome, never end fallback. Device retirement does not release a live reply's pin. |
| R4: formatting | Considered and **outside the current plain-text/IdList release roadmap**. Plan to own formatting/annotations in Wave's Document layer. We have no scheduled built-in attribute CRDT. Anchored spans help locate endpoints but do not solve concurrent attributes, inheritance, overlaps, or formatting undo. |
| R5: offline save/reopen/crash | L01 supplies checked filesystem primitives. L02–L08 still need to wire logs, recovery, persistent undo/redo, and real editor save. Your durability gate remains necessary. |
| R6: installed public boundary | A01/A02/A06 provide installed component targets and self-contained headers, with a headless consumer fixture requiring no internal source paths or Qt Widgets/Test. E05 repeats the fixture after certified GC. |

Please keep your own `Document` interface. The intended library adapter beneath
it is the native **DocumentSession**/supported primitive facade, not today's
unfinished `CollabDocument`. One Buffer per block remains a good composition;
using IdList for structure is optional. Your block IDs and parent block identity
remain your responsibility. IdList currently carries uint64 values: do not
truncate or blindly hash string block IDs into them; keep your own structure or
define a collision-checked mapping.

Two integration distinctions matter for your canonical log:

1. **Stored bytes do not create a pin.** We need explicit durable registration
   keyed by your stable reference ID. The adapter must establish protection
   before publishing the reply, or project canonical pin events to a certified
   frontier before GC. Partial commits and orphan recovery need a specified
   protocol; two successful writes in independent stores are not atomic. Pins
   attach to live document references, independently of which device wrote them.
2. **Current reconstruction and historical playback differ.** Replaying accepted
   events into current state must restore pins before reclamation. Playback at
   an earlier sequence uses an isolated projection with matching history/epoch.
   Live pins do not retain all old text or unlimited `edits_since` history.
   Your append-only archive can supply old operations/checkpoints. We will report
   unavailable history explicitly if the engine's supported floor is exceeded.

Your authorization and canonical event sequence remain authoritative. The
adapter must specify optimistic operations, server rejection, pending outbox
restart, and compensation without deleting accepted events or reusing operation
IDs. Library session durability is not a multi-block conversation transaction.

We also need to qualify the progress claim in your note. V03 contains unsafe GC
by making it inert; P03 adds the correct coverage intersection; P04/P05 provide
strict-reader and semantic-validation foundations. The old operation decoder
still awaits P06b, and the L-series is not connected end to end. These tasks do
not yet amount to a complete codec, save, or reclamation guarantee. During our
review we fixed additional ownership, validation, and durable-write defects and
added `IdList::try_anchor_of` with a typed miss. The assertion/Release mutation
path has already been made checked by P05; the legacy anchor sentinel remains
provisional until S04/A02 replace that surface.

Keep the rule against direct `collect_garbage()`/`compact()` calls. The eventual
destructive path is DocumentSession-managed, certificate-authorized reclamation,
not a scalar watermark passed to the old API. Original probes remain useful
historical evidence, but stage 4 should also run the new installed token/pin/
recovery cases: some old probes intentionally use APIs whose semantics changed.

The [named acceptance matrix](../specs/vnext-external-position-contract.md#required-named-cases-and-implementation-ownership)
now makes your cases executable obligations. Your small integration reports will
be useful evidence against those gates. We cannot promise a delivery date or
paragraph-level semantic attachment after arbitrary complete rewrites: the
guarantee is a convergent content boundary with defined collapse behavior and
explicit failure. Indefinitely live pins also have a metadata cost; we will
measure it rather than promise bounded storage with unlimited retained references.
