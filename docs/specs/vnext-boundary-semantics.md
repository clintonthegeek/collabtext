# Scoped positions: S04a contract and implementation map

Status: reviewed independent design/model gate and implemented S04b live API/codec;
S02 supplies the complete ordering interpreter and S03 changes integration.
This specification resolves WP2's pending placement/restoration decisions. It
adds no replacement engine. The native fragment tree, item origins and selective
undo remain the production foundations.

## 1. Identity, context and retained order

A scope is `(document, stream, primitive, epoch)`. Document and stream are stable,
nonempty byte strings of at most 256 bytes each; compare all bytes exactly. Epoch
is a nonzero uint64. Primitive is text or list. Caller paths, allocator addresses,
actor table slots and tree locators are not scope identities. Bind a validated
scope immutably on engine construction. Existing allocator-only facades may
continue editing temporarily, but cannot mint or accept scoped positions until
constructed with an explicit scope. The Result-returning `create(allocator, scope)`
factory validates scope before constructing the bound engine.
There is no default document shared by all engines. A local_clear marks position
history unavailable, including for old START/END-only tokens: preserving a version
counter cannot manufacture missing history. Queries/mints refuse until complete
same-history recovery restores that state or a new engine is created with a fresh
document/stream identity. A fresh reseed uses a new document/stream, not an epoch
bump invented independently of the membership protocol.

A text item identity is `(128-bit actor, 64-bit scalar counter)`, as allocated by
P06a. List item identity is its insertion operation identity. Neither is the
caller's uint64 list value. Run splitting preserves these identities: scalar `k`
of a fragment is `origin.counter + k`, with checked arithmetic. A token never
records a fragment address, a locator, a byte index as its sole identity, or a
local actor slot.

The supplied logical order includes **all retained items**, including invisible
deletions and undone insertions. Item widths are Unicode scalar UTF-8 widths
(1–4 bytes) for text and one for list. Visibility is the existing selective-undo
result, not merely "a delete has happened". Undoing one of several active deletes
need not restore visibility. Only visible items contribute to returned offsets.

Ordering obligation: after an item is integrated, subsequent insertion,
delete, undo and redo preserve the relative order of every pair of already
integrated retained items. Newly integrated items may appear between them.
This is an explicit input invariant for S02 and a condition of the boundary
proof. S04a does not assert that every current locator/splitting path meets it.
S04b must reject an inverted fence pair as unavailable order context; S02 must
retain a reduced counterexample and refine integration before claiming supported
ordering. Physical removal/translation needs S05/G-series proofs.

Each token also retains its **mint causal frontier**, not the frontier when it
was encoded later. A frontier is canonical sparse per-actor integrated operation
coverage in this stream, at most 512 actors. Initial seed items in model fixtures represent already integrated
seed operations; the adapter includes a symbolic seed observation in mint coverage. Only accepted causally integrated
state counts; receipts, queued operations and reservations do not. Valid actor
chains/gap records make a per-actor head imply its predecessors. The independent
model uses a set of symbolic observed operation IDs instead of production Global;
its subset test expresses the same coverage obligation without implementing
compression, reservation gaps or sequence integration. P07 binds operation scope;
position scope alone does not reject foreign operation payloads today.

## 2. Tokens, minting and resolution

TextBoundary and ListBoundary contain scope, affinity, mint frontier and two
immutable fences. The left fence is the visible preceding item at mint, or a
**scoped START tag**. The right fence is the visible following item, or a **scoped
END tag**. START/END are optional endpoint tags, never reserved actor identities.
An empty stream has START/END with the actual scope/frontier/affinity. Left means
stay before subsequent insertions inside the fenced gap; Right means stay after
them. Both are real tokens even at zero and length.

Mint pseudocode:

1. Validate bound scope, affinity and representable visible length (uint32).
   Reject out-of-range offset. Text offsets must be scalar boundaries in valid
   UTF-8; list offsets are indices. Reject interior UTF-8 bytes without rounding.
2. Traverse visible items, tracking their byte widths/element counts. At the
   requested boundary capture the preceding and following visible identities.
   Invisible retained items are not mint candidates; do not forget them in the
   retained order used later for resolution.
3. Copy the current integrated causal frontier and scope into an immutable value.
   Check the frontier/encoded-size limits. Minting consumes no actor counter,
   creates no undo entry, emits no notification and registers no pin.

Resolve pseudocode (uint64 accumulation, checked uint32 result):

1. Validate token structure, enum and scope bounds. Check document, stream,
   primitive and epoch against the bound engine, in that order, **before any item
   lookup or causal-state test**. Return the corresponding typed scope failure.
2. If integrated state does not cover mint frontier, return `awaiting_state`.
   Do not resolve to a guessed location, even if an endpoint happens to exist.
3. If local_clear invalidated retained history, return history_unavailable, even
   for empty-edge tokens. Otherwise traverse retained items in logical order. `lower` is the visible prefix
   **through** the left item, or zero for START. `upper` is the visible prefix
   **before** the right item, or visible length for END. Hidden items contribute
   zero width but keep their original ranks. Track each endpoint's current
   visibility separately.
4. If a non-edge identity is missing despite mint coverage, return
   `history_unavailable`. If left is not before right in retained order, return
   `history_unavailable` (invalid retained order context). Do not swap the fences
   or search for a nearby visible identity. Length overflow is a typed refusal.
5. Left returns lower; Right returns upper. Return `lost_left` and `lost_right`
   for hidden non-edge fences; `collapsed = lost_left || lost_right`. The byte
   offset/index is usable for a caret even when collapsed. Scope edges never
   become "deleted".

Why this context is sufficient under the stated invariant: visible adjacent mint
items fence one gap. Insertion adds ordered retained items without moving either
fence; visibility changes only prefix weights. Scope prevents aliasing with
another stream, and mint coverage distinguishes delayed state from lost history.
Thus the same token on equivalent retained orders/visibility gives the same
result. Two neighbors **without** those scope, causal and order obligations do
not establish this result. The bounded model covers current interleaved and
intended contiguous paste orders as inputs; it is not a general sequence proof.
A restored formerly hidden item inside a gap minted after deletion is treated
like other newly visible content inside that gap, according to affinity.

Comparison resolves both boundaries in the **same specified state**, then
compares usable offsets. Propagate either typed failure; equality is possible
for different tokens, including collapsed tokens. Do not use affinity as a
secondary tie-break when offsets are equal. Comparison is not a history-independent
order, a replacement for event ordering, or a way to compare list and text units.

## 3. Strict attachments and list elements

A TextAttachment contains a TextBoundary plus an explicit selected side:
`preceding` chooses its left item, `following` chooses its right item. Selecting
START/END is invalid input: a strict attachment requires an actual content item.
The side is part of the serialized attachment, so a consumer need not infer it
from affinity or a displayed integer.

Resolve the underlying boundary with all scope/causality/history checks above.
If the selected item is hidden, return **detached, with no usable offset**.
Loss of only the other fence can still yield an attached position with collapse
flags. A valid selective undo that restores the selected item's visibility makes
**the original token** resolve again. Redo hides it again. A newly inserted lookalike
with another identity never restores attachment. If the retained identity is
physically absent, return history_unavailable; never rebind. No pin is implied.

WP2 exact decision: mint the gap before b in abc (a/b, Left). Delete b: the caret
returns byte 1 with lost_right=true; the following strict attachment returns
detached. Undo the sole delete: that same attachment returns byte 1 without
collapse; redo detaches. The previous fixture's a/c pair was a range while b was
visible, not a caret gap. WP8 likewise now selects b, which it actually deletes.
These exact technical choices are roadmap inferences, not attributed consumer
requirements. Consumer-owned range/passage semantics still need two positions
and their own overlap policy; one strict attachment does not represent a paragraph.

ListElement is a separate opaque value carrying scope, insertion identity and
mint frontier. `element_of(uint64)` searches visible caller values, preserving
all 64 bits. A miss returns typed not_found and no token. For duplicate caller
values, select the first visible item in current logical order; a consumer needing
unique values must enforce that policy. Resolve checks scope and mint coverage,
then retained identity: visible -> element index, hidden -> detached, missing ->
history_unavailable. A list boundary at index length is valid; it is never a
missing element token. Element resolution and boundary affinity are distinct.

## 4. Canonical token format candidate, version 1

This is the implemented S04b codec contract, **not currently supported/published
bytes**. Encoding owns its returned binary bytes; decoding owns an immutable
value independent of input lifetime. Binary content is allowed, including NUL.
No C++ layout, Qt type or operation-codec version is the format. No authentication
or malicious-writer protection is claimed. Retain original unknown bytes externally
for a newer reader; failed decoding does not turn them into an empty token.

All integers are unsigned big endian. Exact field order:

| Field | Encoding / validation |
|---|---|
| magic | four literal bytes `CTPT` |
| version | uint16 = 1; another version -> unsupported_version |
| kind | uint8: 1 TextBoundary, 2 ListBoundary, 3 ListElement, 4 TextAttachment |
| affinity | uint8: 0 Left, 1 Right; ListElement must use 0 |
| document | uint16 length, 1–256 bytes, then exact bytes |
| stream | uint16 length, 1–256 bytes, then exact bytes |
| epoch | nonzero uint64 |
| selection, kind 4 only | uint8: 1 preceding, 2 following; selected edge forbidden |
| fences, kinds 1/2/4 | left uint8 tag 0 START or 1 item; item adds 16 actor bytes + uint64 counter; right tag 0 END or 1 item with the same item encoding |
| referent, kind 3 only | 16 actor bytes + uint64 counter, no edge tag |
| mint frontier | uint16 count 0–512; each entry 16 actor bytes + uint64 integrated head, sorted strictly by raw actor bytes |
| end | input must end exactly here; no trailing byte, extension or padding |

Actors must be nonzero; counters/heads are in [1, UINT64_MAX-1]. Duplicate,
zero-head or unsorted frontier entries are malformed, not normalized by the
reader. A fence/referent's actor/head must be covered by mint frontier under the
P06a item/operation allocation rules (seed insertions count as integrated history).
Left and right item identities must differ. The declared kind supplies the
primitive; generic decode returns a tagged union of the four opaque types.
Decoding a typed token of another kind reports wrong_primitive/type.

Total input cap is 64 KiB before any untrusted allocation. Version 1's maximal
legal content is smaller (12,873 bytes with the largest strict-attachment payload);
64 KiB is a reader safety bound, not permission for padding. Encoding rejects
out-of-bounds context/frontier instead of truncating identities. Decode reads with
checked remaining-byte tests, validates fields, checks canonical invariants, then
requires EOF. Unknown magic -> unknown_format; incomplete known magic/fields ->
malformed/truncated; known magic with complete unsupported version ->
unsupported_version; invalid enums/actor/counter/bounds/trailing data -> malformed.
Scope mismatch is an explicit resolution failure after decoding, or a checked
bound-engine decode wrapper; standalone decode has no engine to compare against.

S04b must test every truncated proper prefix, all 256 one-byte trailing suffixes,
invalid enums and selected edges, context/actor/frontier limits, type/version
errors, canonical golden bytes, input lifetime, and byte stability across independent
replay processes. Decode, resolve and compare are read-only and never create pins.
Only a later approved alpha manifest makes a version a supported persisted-data
obligation; no existing pre-vNext data needs conversion.

Wave currently puts tokens through QString::fromUtf8 in canonical JSON. Its adapter
must wrap these bytes in deterministic ASCII (e.g. base64url under its own version
prefix); raw binary there would lose information. Core stays binary and transport
agnostic. Wave owns block existence: Gone for an absent block, Stale for unavailable
strict attachment/unknown token in a live block. Its current resolved-offset type
needs expansion to expose caret collapse flags. Affinity must map by tested behavior,
because Wave's current header comments reverse the requested Left/Right rule.

## 5. Native implementation map for S04b and later gates

| Current component | Required change / invariant |
|---|---|
| Buffer::anchor_at (Buffer.cpp) | New checked mint traversal of visible UTF-8 scalars; capture two fences, scope and mint frontier. Empty/length get tagged scoped values; reject interior/off-end offsets. Keep legacy editing helper only privately until its callers are migrated. |
| Buffer::resolve_anchor | Traverse retained fragments including invisible ones, expand scalar identity via Fragment::item_at, accumulate visible scalar widths; typed missing/awaiting/collapse results, both affinities. Never use visible_length as a miss fallback. |
| Fragment::item_at, split helpers | Checked origin+scalar offset, stable identity on split/relocation; widths read from validated UTF-8, not origin arithmetic in bytes. No locator serialized into tokens. |
| UndoMap and cached fragment visibility | Use actual current visibility after selective undo/redo; do not remint or modify tokens on changes. Test active competing deletions and undone insertions. |
| IdList::try_anchor_of / anchor_of | Preserve the existing typed miss; new element token uses origin identity/context/frontier. Replace public fallback-to-end wrappers for supported scoped APIs. |
| IdList::resolve_anchor | New element and list-boundary reads traverse retained entries, separately return index and detached/missing outcomes. Application uint64 values are never token identities. |
| compare_anchors in both engines | Checked resolution at one state; compare offsets, propagate errors, no affinity tie at equal offsets. |
| CrdtEngine / IdListEngine facades | Checked contextual factory and separate opaque typed positions; clear/reset invalidates the position-history flag. Existing text edit offsets are UTF-16: new position queries use UTF-8 bytes explicitly; do not silently reinterpret editing arguments. |
| AnchorCodec.cpp (new) | Implement §4 using bounded checked reader; no implicit pin/engine mutation; installed public declarations contain no payload/tree/locator layout. |
| legacy public Engine.h Anchor and presence users | Keep transitional use truthfully marked until A02/S04b migrates callers; do not accidentally certify legacy methods by implementing only a new wrapper. |

Implement straightforward checked scans first; no index/performance project in
S04b. Differential tests read actual retained item order/visibility as inputs to
the independent model and retain hand-written exact fixtures too. Existing core
operation callbacks/queueing stay intact. No document rich text, Markdown parsing,
transport requirement or second sequence engine is introduced.

S02 must generate supplied orders and prove the relative-order invariant with
full nested/reverse/concurrent history coverage. S04b implements live queries and
fresh-process operation/token replay; W03 checks installed strict probes. S04
certifies actual durable store recovery plus repaired ordering. S05 implements
pins; G-series certifies reclamation/translation. GC remains disabled. Persistent
undo is still L06/L08 work; a visibility model's restoration case is not recovery
evidence.

## 6. Model evidence and bounds

BoundaryModel.h is stdlib-only and never includes production identity, anchor,
codec, tree or ordering helpers. Its QtTest harness links QtTest only. Input
retained-orders.json specifies the logical order including hidden items for all
21 token-bearing S01/W01 fixtures. The harness reconstructs each mint snapshot
from initial items and observed causal closures, checks actual adjacent visible
fences, then verifies exact positions/comparisons and same handles on every
supplied final arrival permutation. WP3 block map and WP5 codec/physical pins
remain consumer/later-layer obligations; their names are not claimed as native
runtime passes. WP7 includes all scope axes and missing visible list-value lookup.

Enumeration bounds: two/three actors, each one/two inserted scalars inside an
original ab gap; all 236 supplied per-actor-order-preserving permutations per primitive (including
interleaved and contiguous orders), text and list for 472 orders / 75,328 visibility
states over inserted and endpoint items, mixed text widths 1–4 / list width one,
both affinities, selected deletion/undo/redo, missing causal state in every order,
and equivalent reconstructed states with shuffled duplicate observations. The
fixed seed controls widths and duplicate observation shuffle, not the order oracle.
Eight result assertions per state give 602,624 checks, plus 944 missing-state checks. This is exhaustive **within
those supplied-order/visibility bounds**, not exhaustive CRDT delivery integration.
Separate cases cover empty/start/end, Unicode interior-byte rejection, post-merge
nested insertion, hidden items restored inside a newly minted gap, reversed fences,
missing mint state, missing retained history, and uint64 list values above 2^63.

Three reduced mutation cases demonstrate detected violations: ab/X Left returning
2; foreign-document token resolving locally; covered mint state with missing a
mapping to document end. These are model mutation checks, not a claim of production
mutation testing. The fixture validator independently checks provenance and exact
collapse/restoration oracles, with its fail-closed self-test. See task log for actual
commands, counts, seed, trace digest and configuration evidence.

## Ordering repair (S02)

The relative-order-preservation requirement this contract states for already
integrated retained items is now specified and modelled:
[vnext-sequence-algorithm.md](vnext-sequence-algorithm.md). The independent
interpreter `tests/reference/SequenceModel.h` produces the total order that
`tests/reference/BoundaryModel.h` then resolves tokens against, and
`tst_sequence_model` checks all 22 named positions and 14 token comparisons
across the 8 token-bearing S01 fixtures.

The two models agree that a concurrent insertion at a minted boundary leaves the
boundary's Left and Right at the edges of the inserted groups rather than between
their scalars. No token encoding, mint or resolve rule in this document changes,
and S04a's resolved/detached/collapsed vocabulary is unaffected. The ordering
repair is still unapplied: production interleaves concurrent passages, and S03
owns applying it.
