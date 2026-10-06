# collabtext alpha capability manifest

**Version:** 9
**Introduced by:** roadmap task A02a
**Package version at this manifest revision:** `0.2.0-alpha.2`
**Branch:** `feat/fuguemax-placement` (the workstream label "v2" is not a
package major version)
**Status:** alpha. Not a supported release, and not published. E07a and E07b
prepared pinnable candidates; publication is a separate user instruction that has
not been given. `0.2.0-alpha.2` is a SemVer **prerelease**. CMake's
`project(VERSION)` accepts only a numeric version, so the installed
`collabtext-config-version.cmake` reports `0.2.0` and a `find_package` request
cannot name the prerelease label — ask for no version or for `0.2.0`, and read
`COLLABTEXT_VERSION_STRING` from the config file for the full label.

**What changed at version 9:** `CTPT/1` position tokens became a **supported
serialized format for consumer persistence**, with retained golden bytes and a
reader fixture. It was experimental at version 8. Nothing else about the engine
changed; see the serialized-format section for exactly what the support does and
does not include.

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
| Checked versioned text/list/global/counter-gap codecs: `CTBO/0002`, `CTLO/0002`, `CTGV/0001`, `CTGP/0001` frames, fixed-width hex integers, allocation interval validation; version 1 text/list frames are refused | **available** | `tst_serialization_public` golden/width/shape/interval/version tests, `tst_serialization`, installed `consumer_main` |
| Public JSON decoders reject trailing garbage, duplicate keys, invalid Unicode/escapes and bounded input overflow; valid escaped non-BMP text remains exact | **available** | `tst_serialization_public::public_decoders_reject_untrusted_json_syntax`, `escaped_non_bmp_text_preserves_the_actual_operation`, `tst_decode_validation` |
| Scoped immutable text boundaries/strict attachments and list boundaries/elements; explicit document/stream/primitive/epoch factory | **available** | `tst_positions`, installed `consumer_main` |
| Exact affinity, scoped empty/start/end, UTF-8 scalar-boundary positions, explicit collapse and selected-referent detachment/undo restoration | **available** | `tst_anchor`, `tst_positions` exact/concurrent/differential cases |
| Typed wrong-context, delayed-state, missing-history/off-end/list-value failures; position queries allocate no identity and emit no callback | **available** | `tst_positions`, `tst_anchor_codec` |
| Independently versioned binary CTPT v1 position bytes, strict EOF and owned decode; original token replay in a fresh process | **available; a supported serialized format since `0.2.0-alpha.2`** | `tst_anchor_codec` goldens/all prefixes/all 256 trailing values; `tst_positions::fresh_process_original_token_replay`; `tst_position_token_reader` retained goldens for all four kinds, independent literal reader, per-case replay |
| Per-operation outcome vocabulary (applied / duplicate / deferred / rejected) on the **list** facade | **available** | `collabtext_alpha_consumer`, `tst_operation_validation` |
| Two peers converge over the wire codec | **available** | `tst_convergence`, `tst_opstream_convergence_*`, `tst_sequence_differential` |
| Core builds and links with no Qt, no zstd, no hash provider | **available** | `collabtext_core_smoke` under `CMAKE_DISABLE_FIND_PACKAGE_Qt6/zstd/OpenSSL=ON` |
| Every advertised public header compiles as the first and only library include | **available** | `collabtext_public_headers_core`, `collabtext_public_headers_qt` |
| The file-backed transport (`StreamSync`) | implemented, **not advertised** | `tst_stream_sync`, `tst_opstream_streamsync` — its header names zstd-reaching segment types, so it is not self-sufficient yet. A06a installed the implemented public surface and left this out rather than export a header that cannot compile alone; splitting those types out of the public header is what would let it ship. |
| **The installed package.** Component targets, headers, capability manifest and config files, installable and consumable after relocation | **available** | `tools/verify-consumer-package.sh` — installs to prefix A, moves to prefix B, builds a separate consumer against B with Qt/zstd/OpenSSL discovery disabled |
| **Installed strict position probes.** WP1-WP5/WP7 plus the no-GC containment smoke, through a consumer-owned block map, with a second independent Markdown consumer: exact Markdown bytes, optional transport routing, and caret collapse shown apart from strict attachment loss | **available** | `tools/verify-consumer-package.sh --gate live` — installed `consumer_wave_contract` (16/16 strict cases, 18 rows incl. the pending ones) and `consumer_markdown_contract` (4/4), in Debug, Release and ASan. See `docs/reports/vnext-consumer-live-position-gate.md`. |

## What is not available, and why

These are stated so a consumer designs around them rather than discovering them.

| Capability | State | Why / who owns it |
|---|---|---|
| **Terminal gap integration and transactional transport/recovery.** Gap codecs only validate metadata. Legacy FileSync/StreamSync retain and block a failed codec batch before cursor commit; this is containment, not recovery or durable coverage certification. | **not implemented** | P07/L02/L03/L08 own gap publication, terminal coverage, retries and recovered state/cursor transactions. |
| **Persisting legacy Anchor fields.** Scoped `TextBoundary`/`TextAttachment`/`ListBoundary`/`ListElement` and `encode_position` are the supported API shape. Legacy internal resolution still ignores bias or falls back on a miss. | **unsupported legacy helper path** | S04b adds the checked scoped path; A02/S04 and presence integration retire remaining legacy usage. Do not serialize raw layout. |
| **Ordering repair — concurrent insertion (F8).** Two or more peers pasting or typing at the same position keep each author's text contiguous, forward and in reverse, and an author's run is not split into the middle by a peer editing inside it. Placement is FugueMax (a tree link per inserted run), for text and for lists. | **available** | `tst_sequence_differential` (240 seeded traces, production against a literal reference, every minted link compared), `tst_sequence_semantics` (all 18 S01 fixtures through the engines, every arrival order), `tst_evaluation_regressions::f8_*`, `tst_fuguemax_model`. Spec: `docs/specs/vnext-sequence-algorithm.md` §0. |
| **Typed outcome on the text facade.** `CrdtEngine::applyRemoteOp` returns a bool, so deferred and rejected are indistinguishable. | **partial** | A02 renames it to `apply_remote(...) -> ApplyOutcome`. A02a fixed the worse bug — it used to return true unconditionally, which would have told a retrying caller that a rejected operation succeeded. |
| **Persistent undo/redo.** Undo is in-memory and does not survive a reopen. | **not implemented** | L06. Consumer requirement R5. |
| **Crash recovery.** No replay-from-log session exists. A reopened `ReplicaState` knows the counter but not the document. | **not implemented** | L02–L08. The installed fresh-process replay re-reads bytes a consumer persisted itself; it is codec and token evidence, not a recovered store. |
| **Durable identity for consumers.** A `ReplicaAllocator` exists in the source tree but is not exported or installed. | **partial** | Not part of the installed public surface. A consumer supplies its own `IdentityAllocator` and is responsible for durability. Making `ReplicaAllocator` public is a separate decision, because it means committing to its persistence format. |
| **Epoch / document / stream on the operation.** An operation carries no epoch, so a receiver cannot check one. | **not implemented** | P07 owns the envelope; L04 owns its persistence. |
| **GC, pins, checkpoints, reclamation.** | **not implemented, and inert** | G01–G08. `collect_garbage()` and `compact()` return zero and retain everything. **Do not call them expecting reclamation.** |
| **Formatting / annotation layer.** | **not planned** | Deliberately out of scope. A consumer's document layer owns it over character CRDTs. |
| **Qt widget layer** | **available, not part of Core** | `CollabText::Widgets`. The alpha's headless path is Core alone. |

## Serialized formats and what is promised about them

Two different stability questions get answered here, because they are answered
differently and conflating them is how an alpha breaks somebody's storage.

**The C++ API/ABI is unstable on purpose.** Between alpha candidates a header,
a signature or a type may change without notice or deprecation. This alpha
removed a public type (`Locator`), so the break already happened. Do not pin to
a C++ symbol; pin to the candidate SHA and rebuild.

**A serialized format's obligation is separate, and starts only when a version is
declared supported.** Declaring it supported means: this reader exists, its
golden bytes are retained, and later supported releases keep reading those bytes
or translate them. Publishing an alpha does **not** retroactively declare
anything supported.

| Identifier | Version | Status | Reader obligation |
|---|---|---|---|
| `CTBO` — text/buffer operation frame | 0002 | **alpha interchange.** Versioned, checked, strict-decoding, and what an installed consumer actually exchanges operations over. Version 0001 is refused with `unsupported_version`. | **none yet.** Usable between peers pinned to the same candidate. Not a storage format. |
| `CTLO` — list operation frame | 0002 | as above | as above |
| `CTGV` — sparse global/coverage vector | 0001 | alpha interchange | as above |
| `CTGP` — terminal counter-gap record | 0001 | alpha interchange. Validating a gap record does **not** publish the gap or advance coverage. | as above |
| `CTPT` — position token, **binary** | 1 | **SUPPORTED for consumer persistence, since `0.2.0-alpha.2`.** | **Active.** Retain the reader and these bytes, or provide a checked translation. |

**`CTPT/1` is a supported token version.** That is a promise about *bytes*, and
it is deliberately narrow. A later supported release keeps reading stored version
1 tokens — or translates them — and reports a version it cannot read as
`unsupported_version` rather than `malformed`. It never remints a stored token to
make a resolve succeed.

Storing these bytes is safe. What it does **not** buy you:

- **It is not a pin.** Storing a token creates no retention obligation, and
  `collect_garbage()` currently retains everything anyway, so nothing can be
  reclaimed out from under one yet.
- **It does not survive reclamation.** When S05/G06/G08 enable physical GC, a
  live registered pin must keep its referent alive or a boundary stub must keep
  the bytes resolving. That does not exist yet.
- **It does not survive an epoch change.** A token from another epoch reports
  `wrong_epoch`; G01/G02 own translation.
- **It does not survive `local_clear()`.** That invalidates position history,
  including for empty-edge tokens; stored bytes then report `history_unavailable`.
- **It is not a recovered store.** The same bytes resolve in a fresh process given
  operations the consumer itself retained. There is no crash-recovered store.

No pre-vNext or earlier experimental token is supported, and none will be
converted: there is no production data to convert.

Retained golden bytes for all four token kinds, and the reader fixture that keeps
them working, are `tests/fixtures/position-tokens/` in the source repository.

There is no supported **store** or **checkpoint** format: none exists. There is
nothing to migrate — the user confirmed there is no production data, history or
store — and no pre-vNext format is supported or will be converted.

## Supported configuration

- **Language:** C++20. No exceptions required, no RTTI required.
- **Core link dependencies:** none. No Qt, no zstd, no crypto provider.
- **Offset units:** text offsets are UTF-16 code units (`CrdtEngine::insert`,
  `remove`, `length`). Item identities and counters are 64-bit unsigned.
  Scoped list queries use element indices; legacy list mutation uses item anchors.
- **Position units:** `boundary_at` / `attachment_at` use UTF-8 bytes at scalar boundaries; `resolve` returns byte offsets or list indices with loss flags. Text editing remains UTF-16.
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

- **Placement is O(n) on purpose.** Every insertion walks the tree. This is not a
  capability gap and was not an oversight; it is what makes a placement change
  safe to review. A performance task may change it later, and
  `tst_sequence_differential` is what makes such a change safe.
- Passage order is verified against the independent model over the enumerated S01
  histories and 240 seeded differential traces, not proven exhaustively for all
  histories. S06 owns bounded enumeration of combined sequence/undo histories.
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

# The strict installed position probes and the independent Markdown consumer.
./tools/verify-consumer-package.sh --mode release --gate live --components core

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
