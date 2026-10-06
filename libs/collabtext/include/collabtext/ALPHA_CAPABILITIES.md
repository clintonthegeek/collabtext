# collabtext alpha capability manifest

**Version:** 3
**Introduced by:** roadmap task A02a
**Branch:** `roadmap/reliability-v2`
**Status:** alpha. Not a supported release, and not published. E07a prepares a
pinnable candidate; publication is a separate user instruction that has not been
given.

This manifest exists because an alpha surface that does not say what it cannot do
is worse than no alpha surface. Every line below is a fact about *this* branch,
and the "verified by" column names the executable evidence rather than the
documentation. A capability with no evidence is marked `not implemented`, not
"probably works".

## What a consumer can rely on

| Capability | State | Verified by |
|---|---|---|
| Persistent actor identity, 128-bit, minted once and persisted before use | **available** | `tst_actor_identity`, `tst_identity_types` |
| Operation identity is actor + checked 64-bit counter, ordered counter-first | **available** | `tst_identity_types` (order laws), `tst_identity_ordering_model` (independent) |
| One durable reservation per local operation; an operation's identity is strictly above every item it created | **available** | `tst_identity_allocation_engine`, `tst_identity_allocation_model` |
| No identity reissue across a clean reopen | **available** | `tst_identity_allocation_engine::engine_identities_are_not_reissued_after_a_reopen` |
| An all-zero actor is refused as `invalid_identity`, in Release as well as Debug | **available** | `tst_identity_allocation_engine::an_invalid_actor_is_refused_rather_than_treated_as_actor_zero` |
| Text: local insert/remove, read, undo/redo. Offsets are UTF-16 code units | **available** | `collabtext_alpha_consumer`, `tst_crdtengine_op_api` |
| Structural list: insert after anchor, remove at anchor, read ids/size, undo/redo | **available** | `collabtext_alpha_consumer` |
| Opaque immutable text/list operation values; read-only copies survive engine/callback lifetime | **available** | `tst_operation_values`, installed `consumer_main`, `tst_serialization_public` compile-time variant exclusion |
| Interim JSON operation wire round trip (payload version not yet implemented) | **available** | `tst_serialization_public`, `tst_serialization` |
| Public JSON decoders reject trailing garbage, duplicate keys, invalid Unicode/escapes and bounded input overflow; valid escaped non-BMP text remains exact | **available** | `tst_serialization_public::public_decoders_reject_untrusted_json_syntax`, `escaped_non_bmp_text_preserves_the_actual_operation`, `tst_decode_validation` |
| Per-operation outcome vocabulary (applied / duplicate / deferred / rejected) on the **list** facade | **available** | `collabtext_alpha_consumer`, `tst_operation_validation` |
| Two peers converge over the wire codec | **available, with a known limit** | `tst_convergence`, `tst_opstream_convergence_*` — see "Ordering repair" below |
| Core builds and links with no Qt, no zstd, no hash provider | **available** | `collabtext_core_smoke` under `CMAKE_DISABLE_FIND_PACKAGE_Qt6/zstd/OpenSSL=ON` |
| Every advertised public header compiles as the first and only library include | **available** | `collabtext_public_headers_core`, `collabtext_public_headers_qt` |
| The file-backed transport (`StreamSync`) | implemented, **not advertised** | `tst_stream_sync`, `tst_opstream_streamsync` — its header names zstd-reaching segment types, so it is not self-sufficient yet. A06a installed the implemented public surface and left this out rather than export a header that cannot compile alone; splitting those types out of the public header is what would let it ship. |
| **The installed package.** Component targets, headers, capability manifest and config files, installable and consumable after relocation | **available** | `tools/verify-consumer-package.sh` — installs to prefix A, moves to prefix B, builds a separate consumer against B with Qt/zstd/OpenSSL discovery disabled |

## What is not available, and why

These are stated so a consumer designs around them rather than discovering them.

| Capability | State | Why / who owns it |
|---|---|---|
| **Typed versioned operation codecs.** Interim optional decoding cannot classify unsupported versions; failures must retain original input. Legacy file-reader cursor/skip behavior remains uncertified and is not repaired merely by a stricter decoder. | **not implemented** | P06b format/typed errors and P07/L03 transactional transport/recovery remain work. |
| **Affinity semantics.** An anchor's `Bias` is stored but **not read** by `resolve_anchor`. Left and right of the same character resolve identically. | **not implemented** | Named by the external consumer as its critical path. S04a specifies it, S04b implements it. A consumer needing it must treat every resolved offset as approximate. |
| **Typed unresolvable anchors.** `resolve_anchor` falls back to end-of-document when an anchor names an item the engine does not have, and says nothing. | **not implemented** | The consumer's requirement R3: a silent relocation is data corruption. S05 specifies retention or a typed miss; G06/G08 own the GC interaction. `IdList::try_anchor_of` **is** typed (`unresolvable_anchor`) — the engine's item lookup is fixed, the position resolution path is not. |
| **End-of-document position tokens.** `Anchor::end()` is an explicit past-the-end flag, not a token comparable with later-minted positions. | **not implemented** | S04b mints real scoped positions. |
| **Position token persistence.** No stable, versioned, library-owned encoding of a position exists. Do not store `Anchor`/`Locator` fields in an external log: the public header reserves their layout as evolution-reserved, and it now has 128-bit actors in it. | **not implemented** | S04a/S04b, then W03/E07b certify it against a fresh process. |
| **Ordering repair (F8).** Two peers inserting at the same position interleave character-by-character. Convergent, and wrong for prose. | **not implemented** | S01 specifies, S02 designs, S03 applies. The core smoke asserts the character *multiset* for exactly this reason. |
| **Typed outcome on the text facade.** `CrdtEngine::applyRemoteOp` returns a bool, so deferred and rejected are indistinguishable. | **partial** | A02 renames it to `apply_remote(...) -> ApplyOutcome`. A02a fixed the worse bug — it used to return true unconditionally, which would have told a retrying caller that a rejected operation succeeded. |
| **Persistent undo/redo.** Undo is in-memory and does not survive a reopen. | **not implemented** | L06. Consumer requirement R5. |
| **Crash recovery.** No replay-from-log session exists. A reopened `ReplicaState` knows the counter but not the document. | **not implemented** | L02–L08. |
| **Durable identity for consumers.** A `ReplicaAllocator` exists in the source tree but is not exported or installed. | **partial** | Not part of the installed public surface. A consumer supplies its own `IdentityAllocator` and is responsible for durability. Making `ReplicaAllocator` public is a separate decision, because it means committing to its persistence format. |
| **Epoch / document / stream on the operation.** An operation carries no epoch, so a receiver cannot check one. | **not implemented** | P07 owns the envelope; L04 owns its persistence. |
| **GC, pins, checkpoints, reclamation.** | **not implemented, and inert** | G01–G08. `collect_garbage()` and `compact()` return zero and retain everything. **Do not call them expecting reclamation.** |
| **Formatting / annotation layer.** | **not planned** | Deliberately out of scope. A consumer's document layer owns it over character CRDTs. |
| **Qt widget layer** | **available, not part of Core** | `CollabText::Widgets`. The alpha's headless path is Core alone. |

## Supported configuration

- **Language:** C++20. No exceptions required, no RTTI required.
- **Core link dependencies:** none. No Qt, no zstd, no crypto provider.
- **Offset units:** text offsets are UTF-16 code units (`CrdtEngine::insert`,
  `remove`, `length`). Item identities and counters are 64-bit unsigned.
  `IdListEngine` has no offset space at all.
- **Byte lengths vs scalars:** internal fragment accounting tracks both; a
  fragment's `length` is Unicode scalars and `byte_length` is UTF-8 bytes. Not
  part of the public surface.
- **Stability:** the narrow contract in `<collabtext/Operations.h>` and
  `<collabtext/IdListOperations.h>`. Operation payload structs and variant alternatives are private and absent from installed headers; `Identity::actor`/`counter` and the `op_identity` /
  `idlist_op_identity` accessors are the stable part.
- **Uniqueness context:** an identity is unique within one (document, stream).
  The same counter value in two streams is two independent facts. A consumer
  maintaining its own identifier must not hash or truncate a string id into
  `IdListEngine::try_insert_after`'s opaque `id` expecting global uniqueness.

## Known limits that are not capability gaps

- Two peers' visible text can be character-interleaved after concurrent inserts
  at one position (F8, above). Everything else about convergence holds.
- `Global::coverage_intersection` treats absence as zero coverage, which is
  correct for a watermark and is why `meet` is a different operation. Do not use
  `meet` to ask what every peer has seen.
- A seed operation (`op_for_seed`) is minted under a content-derived actor with
  a volatile counter, so two peers reading the same seed file compute identical
  operations. It is reproducible, not durable.

## How to check this manifest rather than trust it

Every claim above names an executable test. Run the cheap ones while working, and
the full sweep once per arc — see the verification tiers in
`docs/roadmap/2026-10-reliability/EXECUTION.md`.

```sh
# Just what this manifest claims, from the warm tree. Routed, not the full sweep.
./tools/verify-changed.sh --target 'collabtext_alpha_consumer|collabtext_core_smoke'

# The installed package: producer, install to A, move to B, separate consumer
# against B with Qt/zstd/OpenSSL discovery disabled.
./tools/verify-consumer-package.sh --mode release --gate package --components core

# The alpha surface on public headers only, no Qt/zstd/hash discovery.
cmake -S . -B build-alpha \
  -DCMAKE_BUILD_TYPE=Release \
  -DCOLLABTEXT_BUILD_QT=OFF -DCOLLABTEXT_BUILD_APPS=OFF \
  -DCOLLABTEXT_BUILD_TESTS=OFF -DCOLLABTEXT_BUILD_FILE_TRANSPORT=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_Qt6=ON -DCMAKE_DISABLE_FIND_PACKAGE_ZSTD=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
cmake --build build-alpha -j "$(nproc)"
./build-alpha/libs/collabtext/collabtext_core_smoke

# That the advertised header set has not drifted from the tree.
./tools/check-public-headers.sh

# Acceptance tier: the whole thing, from scratch.
./tools/verify-roadmap.sh --fresh debug '.'
```
