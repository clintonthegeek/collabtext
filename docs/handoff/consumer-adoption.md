# Consumer adoption handoff: Wave and Markoff

Date: 2026-10-11. Source branch: **`feat/fuguemax-placement`**, isolated in
`.worktrees/reliability-v2`. Current candidate: **`collabtext-wave-core-2026-10-11`**,
package `0.2.0-alpha.3`, manifest v15. Resolve the annotated local tag to its
commit SHA for consumer locks and test evidence. The prior adoption tag
`collabtext-adoption-2026-10-11` at `0d21670896d02dda3dc44d18d37f9657935333ed`
is immutable. A local tag is not publication or distribution.

Wave-specific requirements, APIs and adoption checks are settled in
[wave-core-response.md](wave-core-response.md); this document covers the shared
consumer responsibilities and master merge gate.

## Recommendation and stability boundary

Begin adoption against this snapshot. The principal performance improvements
can preserve the public API: incremental tree mutation, persistent indexes,
compatible run coalescing, compressed undo metadata, and faster coordinate and
position queries. Use installed public headers only. Do not include `src/`,
inspect operation payloads, or depend on fragment/tree layout.

The C++ API/ABI remains alpha. Pin and rebuild both consumers on an upgrade;
this handoff does not override that policy. Prefer additive APIs for future
batching, range reads, and edit notifications. Existing synchronous behavior,
coordinate units, position semantics, and operation identity must remain
compatibility gates for optimization. A proposed breaking change requires an
explicit consumer migration and an updated snapshot.

`CTBO/0002` text/undo and `CTBO/0003` marker operations now have indefinite
supported-reader obligations. `CTPT/1` and marker `CTPT/2` have supported
persisted-token readers. Keep canonical event bytes unchanged. Other operation,
coverage/gap, private-history/store/checkpoint formats retain their stated alpha
limits; validate upgrades explicitly. Recreating a populated CRDT from visible
text loses identities, tokens and undo history.

## Choose one owner for persistence and publication

| Consumer path | Library entry point | Consumer responsibility |
|---|---|---|
| Wave's block/document model and durable cross-block references | `CrdtEngine::create`, `IdListEngine::create`, opaque positions, `AnchorPinRegistry` | Stable block-to-stream mapping, canonical log, durable allocation, routing, recovery, undo metadata, pin events and transactions |
| Markoff's exact Markdown text with its own history/transport | Scoped `CrdtEngine`, optionally `IdListEngine` if its model needs structure | Preserve exact bytes; own the same durability/routing obligations as above |
| A single file-backed text stream needing built-in replay and persistent local undo | `DocumentSession` in `CollabText::FileTransport` | Private/shared paths, stable scope, initial seed, membership/provider integration when enabled |

These are recommended integration shapes, not assumptions that either real
consumer is identical to a test fixture. Confirm the chosen owner in each
consumer's integration record before persisting its first operation.

`DocumentSession` currently exposes byte-based edits and deltas, but only legacy
`Crdt::Anchor` queries; it does not expose scoped `TextBoundary`/`TextAttachment`
mint/resolve methods. Do not persist those legacy anchors. If a consumer needs
the session and durable scoped positions together, either use a consumer-owned
Core composition or request additive scoped position methods on the session
before implementing that path. Do not reach into the session's buffer.

A local operation is published once. On Core, select either the returned
operation or the local-op callback as the publication source; do not broadcast
both. On `DocumentSession`, the session owns its durable publication. Do not
add a second publisher around `apply_local`, undo, or redo.

## Coordinates: settle these in the adapter

| API/data | Unit |
|---|---|
| `CrdtEngine::try_insert`, `try_remove`, `try_replace`, `length` | UTF-16 code units |
| `CrdtEngine::boundary_at`, `attachment_at`, resolved `byte_offset` | UTF-8 bytes at Unicode scalar boundaries |
| `DocumentSession::apply_local`, `visible_length`, `SessionTextEdit` offsets | UTF-8 bytes at scalar boundaries |
| Scoped list boundary/read resolution | Element index |
| Existing list mutation | Item anchor from `try_anchor_of`, or `Crdt::Anchor::min()` for head insertion |

Use explicit adapter types/names such as `Utf16Offset` and `Utf8ByteOffset`.
Centralize conversions against the current editor revision. Never reuse a
conversion after another edit without updating its revision. Grapheme positions
are a UI concern; neither unit is a count of user-perceived characters.

Test `A😀é`: its end is UTF-16 offset 4 and UTF-8 byte offset 7. Inserting after
the emoji is UTF-16 offset 3 or UTF-8 byte offset 5, depending on the API.
Reject surrogate-pair and UTF-8 scalar splits. Preserve valid UTF-8 exactly;
normalizing Unicode, line endings, whitespace, or Markdown must be an explicit
consumer edit rather than an implicit transport/conversion side effect.

For session deltas, `old_start`/`old_end` are half-open ranges in the OLD visible
text. Apply nonoverlapping edits from right to left or use an editor transaction
that interprets all offsets against that old revision. If `edits_since` returns
`history_unavailable`, resynchronize the visible editor from current text and
resolve its saved positions; do not fabricate a delta.

## Positions, affinity, and durable references

Use `TextBoundary` for a caret or flexible range endpoint. Left affinity stays
before text inserted into its original gap; right affinity stays after it.
Recommended initial defaults: right affinity for an ordinary caret that should
follow inserted text; left at a selection start and right at its end when the
selection should include new boundary text. Store affinity when minting the
token. If the product wants exclusion or different caret behavior, record and
test that alternative explicitly; the library cannot infer user intent.

Use `TextAttachment` for a strict reference to a selected preceding/following
character, and `ListElement` for an exact structural element. A successful
boundary resolution with `collapsed()` still has a usable offset. `detached`
on a strict reference has no usable fallback offset: show the reference as gone
and retain its original token so selective undo can restore it. Do not turn it
into the nearest live character or silently remint it.

Persist `encode_position` bytes with document, stream, kind, epoch, and the
consumer reference ID. The bytes contain scope; the extra routing fields must
agree with that scope. CTPT is binary, including NUL: use a binary column or a
lossless ASCII encoding such as base64url in JSON. Never pass it through a
UTF-8 text conversion. Decode the expected kind with `decode_position_as` and
validate scope. Keep original bytes when reporting a decoding error.

Token storage is not pin registration. Persist durable reference registration
and release events in the consumer's canonical log, and replay them into
`AnchorPinRegistry` before claiming its projection complete. Tie reference
creation/removal and pin events together through a durable consumer transaction
or recoverable outbox. An uncertain append requires canonical-log recovery,
not an invented sequence or a fresh token.

Wave owns block existence and routing separately from character position
resolution. Markoff owns exact Markdown bytes and semantic interpretation;
tokens identify CRDT content, not a parser node that remains stable after edits.
Do not truncate/hash application string IDs into the list's `uint64_t` values;
maintain a collision-free mapping and avoid duplicate visible values when
using `element_of`/`try_anchor_of` to address by value.

## Callbacks, UI updates, and performance

Keep each engine/session on one owner thread. Text callbacks run synchronously;
do not mutate the engine from them. Copy an opaque operation if it must outlive
the callback, and schedule subsequent mutations after dispatch. Hold text RAII
subscription handles for the intended subscription lifetime. List callbacks use
setter APIs; disconnect them before captured consumer objects are destroyed.

The text facade's change callback is an invalidation signal, not an edit delta.
Reconcile after an apply batch; deferred older operations can drain during that
batch, so the offered operations' outcomes are not a complete UI change log.
Suppress editor-to-engine feedback while rendering remote changes. Avoid a
`text()` copy or position resolve for every callback, annotation, or paint.
Initially coalesce invalidations and resolve visible references once per editor
revision. Request additive delta/batch APIs when real traces show the need.

Current core benchmarks exclude durability and UI work. Record end-to-end
latency, fragment/history growth, many-reference resolution, reopen time, and
the editor's undo grouping during adoption. Do not implement internal fragment
caches in the consumer to work around library performance.

## Identity, errors, receipts, and undo

Core requires a durable `IdentityAllocator`: persist the actor and reserve
checked 64-bit counters before returning them. Use stable document/stream/epoch
scope, and a single writer for private state. Volatile allocators in fixtures
are examples for tests only. Keep device-private state separate from shared
document storage; copying shared content must not clone its actor identity.

Operations do not carry complete routing/epoch context. Retain an envelope with
document context and `(stream, epoch, producer, sequence)`; validate membership
and scope before offering an operation to an engine. Delivery sequence and
operation counter are different domains. A transport receipt is not GC authority.

| Result | Consumer action |
|---|---|
| `applied` / `duplicate` | Eligible for a delivery commit only after its recovery point is durable |
| `deferred` / `awaiting_state` | Retain original envelope; await dependencies; do not mark durable application complete just because memory accepted it |
| Rejected, malformed, unsupported version, wrong scope/epoch | Retain exact bytes and reason; diagnose/quarantine; do not advance a receipt as if applied |
| `deferred_queue_full` / resource limit | Apply backpressure; retain and retry caller-owned input when capacity is available |
| `detached` | Strict referenced content is hidden; preserve token and show loss |
| `history_unavailable` | Required history is absent; recover/translate explicitly, never silently retarget |
| I/O failure or `commit_uncertain` | Inspect readiness/recovery state; reconcile the original durable intent/token before resuming; do not mint a replacement operation blindly |

Use checked `try_*` APIs. Empty optional undo means no undoable entry; an error
is different. `discard_local_view()` is not collaborative deletion or replay
reset: it preserves observed versions, and old operations will be duplicates.
Recover into a fresh engine/valid checkpoint instead.

Core's operation log alone does not restore local undo cursor/grouping. Both
CrdtEngine and IdListEngine expose `local_history_snapshot()` and
`restore_local_history()` for consumer-owned persistence, plus coalescing, next
groups and retirement acknowledgments. Restore after shared replay with valid
allocation authority. Persist metadata/compensation transitions consistently.
Publish retirement acknowledgments only after their consumer event is durable;
trimming alone is not reclamation permission. Optional DocumentSession owns this
metadata for its own file stream; Wave uses Core with its existing canonical log.

## Initial lifecycle policy

Start in one epoch, retain operation history, and leave physical GC/log pruning
disabled in consumer integrations. The source contains certified reclamation,
checkpoints, membership coordination, and pin protection, but the complete
provider/certificate construction surface is not installed. The public session
methods alone are not a turnkey GC service. Never substitute a timer, scalar
ack, fake validator, or fabricated certificate.

Offline devices stay enrolled until explicitly retired. If adoption enables
retirement, wire both local and inbound authorization and validate canonical
membership durably. A returning old-epoch branch must stay exportable and
read-only until explicit reconciliation. Original tokens require validated epoch
translation; arbitrary old-epoch edits are not automatically rebased.

## Build and installed-consumer checks

Use the exact lowercase package name from the installed configuration:

```cmake
find_package(collabtext CONFIG REQUIRED COMPONENTS core)
target_link_libraries(my_consumer PRIVATE CollabText::Core)
```

Request `file-transport` and link `CollabText::FileTransport` for sessions;
request `qt` and the appropriate Qt target only when needed. Record
`COLLABTEXT_VERSION_STRING` and the dependency SHA. Do not depend on an
uncreated release tag or a moving branch tip.

Producer checks (these test fixtures, not the actual consumer applications):

```sh
./tools/verify-consumer-package.sh --mode release --gate package --components core,file-transport,qt
./tools/verify-consumer-package.sh --mode release --gate live --components core
```

Actual consumer acceptance tests must compile against a relocated installation
without source include paths. Cover each consumer's own editing/routing path:
Unicode offsets; concurrent typing/paste; duplicate/reordered delivery; callback
feedback suppression; caret affinity versus strict loss; original-token reopen;
undo/redo after reopen when promised; missing/corrupt input and failed durable
writes; exact Markdown bytes; block deletion and unknown block/stream routing.
Add retirement/translation/pin recovery cases only if those features are enabled,
and verify that unimplemented lifecycle paths remain disabled.

## The point at which this merges into master

The target branch in this repository is **`master`**. The adoption tag starts
integration; it does not trigger a merge. Close the adoption gate when:

1. Wave and Markoff each record a tested dependency SHA, installed components,
   persistence owner, coordinate adapter, reference-affinity policy, and enabled
   lifecycle features. Their actual installed-package integration suites pass.
2. Consumer blockers are fixed on this branch. Both consumers are retested
   against the SAME final candidate SHA. Do not move the starting tag; create a
   new annotated candidate tag when fixes change the baseline.
3. Package/live-position gates and the applicable library correctness/recovery
   checks pass on that candidate. Record compiler/configuration, nonempty test
   selection, results, and candidate SHA. Check the merge result against current
   master if it differs from the validated candidate.
4. Every durable format the consumers use has an explicit candidate-pinning or
   tested reader/migration decision. No enabled feature relies on unpublished
   internal APIs. Performance work retains these consumer compatibility gates.
5. Prepare a merge review containing the final candidate, both consumer evidence
   records, known limitations, and deferred performance work. Merge into master
   after the user's explicit merge instruction. Publication is a separate step.

Completion of a general performance overhaul is NOT a merge prerequisite.
A concrete adoption workload that misses a consumer's stated latency/memory
requirement IS a blocker; resolve or explicitly accept it before closing the gate.

After merge, consumers pin the resulting validated master commit or a new
explicit integration tag. Future performance PRs run both consumer gates.

## Executable reference material

- [Installed Core and optional-session smoke](../../tests/consumer-smoke/main.cpp)
- [Wave-shaped adapter](../../tests/consumer-smoke/wave-contract/Adapter.h)
- [Markoff-shaped adapter](../../tests/consumer-smoke/markdown-contract/Markoff.h)
- [Installed package verifier](../../tools/verify-consumer-package.sh)
- [Capability manifest](../../libs/collabtext/include/collabtext/ALPHA_CAPABILITIES.md)
- [Session recovery/fault cases](../../libs/collabtext/tests/tst_session_recovery.cpp)

These fixtures demonstrate contracts. Passing them does not certify either
real consumer's persistence or UI integration.
