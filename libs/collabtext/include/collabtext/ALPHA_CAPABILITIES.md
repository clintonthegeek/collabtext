# collabtext alpha capability manifest

**Version:** 14
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

**What changed at version 14:** The consumer adoption handoff pins the current
branch with `collabtext-adoption-2026-10-11` and defines an explicit master merge
gate. Stale lifecycle statements below are corrected: session recovery, persistent
undo, pin projection, checkpointing, and certified collection exist in source;
complete certificate/provider construction is not an installed turnkey service.
Initial consumer adoption retains history and leaves reclamation disabled. See
`docs/handoff/consumer-adoption.md` in the tagged source. This update declares no
new format support and no C++ stability guarantee.

**What changed at version 13:** `MembershipCoordinator::commit_transitions()`
atomically advances multiple document/stream scopes under one durable callback;
an uncertain batch blocks every included scope and requires an exact full-batch
retry. Version 12 added RAII authorization permits for local writes and inbound
deliveries and serialized a single durable epoch change against those permits.
Ambiguous transition publication blocks the scope until exact retry or
reconstruction from canonical durable state. Consumers still own
acknowledgement authentication, durable membership records,
descriptor/checkpoint validation, and wiring the coordinator into each session.
Version 11 added `DocumentSession` local branch export: it streams its
producer's durable operation deliveries in their original document, stream, and
epoch, including a durable pending operation when transport publication failed.
The export preserves delivery tokens and opaque payload bytes; it does not rebase
or replay operations into a newer epoch. Version 10 added typed remote-apply
outcomes, checked undo/redo results, configurable undo history limits, and an
installed consumer implementation of `OpStream`. `CTPT/1` position tokens
remain supported for consumer persistence; see the serialized-format section
for the exact obligation.

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
| Per-operation outcome vocabulary (applied / duplicate / deferred / rejected) on text and list facades, including batch order | **available** | `tst_crdtengine_op_api`, `collabtext_alpha_consumer`, `tst_operation_validation` |
| Checked text undo/redo outcomes and configurable depth/byte limits | **available** | `tst_crdtengine_op_api`, `tst_persistent_undo` |
| Consumer implementation of the stable transport interface | **available** | installed `consumer_main` implements `OpStream` and exercises registration, push, subscription, polling, and receipts |
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
| **Terminal gap integration and the complete transport/recovery matrix.** Gap codecs validate metadata; decoding alone does not advance coverage. DocumentSession supplies replay and persistent undo, but that does not certify arbitrary consumer transport transactions or terminal gap publication. | **partial; terminal gap publication is not an adoption guarantee** | `tst_session_recovery`, `tst_transport_commit_contract`; P07/L08 retain the broader protocol/recovery obligations. Legacy FileSync/StreamSync containment is not certified recovery. |
| **Persisting legacy Anchor fields.** Scoped `TextBoundary`/`TextAttachment`/`ListBoundary`/`ListElement` and `encode_position` are the supported API shape. Legacy internal resolution still ignores bias or falls back on a miss. | **unsupported legacy helper path** | S04b adds the checked scoped path; A02/S04 and presence integration retire remaining legacy usage. Do not serialize raw layout. |
| **Ordering repair — concurrent insertion (F8).** Two or more peers pasting or typing at the same position keep each author's text contiguous, forward and in reverse, and an author's run is not split into the middle by a peer editing inside it. Placement is FugueMax (a tree link per inserted run), for text and for lists. | **available** | `tst_sequence_differential` (240 seeded traces, production against a literal reference, every minted link compared), `tst_sequence_semantics` (all 18 S01 fixtures through the engines, every arrival order), `tst_evaluation_regressions::f8_*`, `tst_fuguemax_model`. Spec: `docs/specs/vnext-sequence-algorithm.md` §0. |
| **Typed outcome on the text facade.** `CrdtEngine::apply_remote` returns `ApplyOutcome`; `apply_ops` returns one outcome per offered operation. | **available** | `tst_crdtengine_op_api`, `tst_operation_validation`, installed `consumer_main`; duplicate, deferred/drained, rejected, and batch-order cases are covered. |
| **Persistent bounded undo/redo.** Buffer persists actor-local undo/redo through `DocumentSession`; `IdListEngine` exposes replayable history snapshots for consumer-owned persistence. | **available** | `tst_persistent_undo`, `tst_idlist_undo`, `tst_session_recovery`, `tst_editor_recovery` reopen/undo/redo cases. |
| **Crash recovery.** `DocumentSession` full-replays one text stream and recovers its local undo state; the broader sender/receiver arrival and fault matrix is pending. | **partial** | `tst_session_recovery`, `tst_editor_recovery`; L08 owns the cross-transport recovery milestone. |
| **Returning-device local branch export.** `DocumentSession::export_local_branch()` streams only the local producer's retained deliveries for the original document/stream/epoch, preserving tokens and payload bytes and including a recoverable pending commit. | **available** | `tst_session_recovery::localBranchExportKeepsOriginalScopeAndExcludesPeerOperations`, pending-commit export and duplicate reconciliation cases; relocated FileTransport package consumer calls the installed API. Consumers must stage output and publish it only after success; they remain responsible for explicit validation and reconciliation before a newer epoch can use the branch. |
| **Membership authorization coordination.** `MembershipCoordinator` supplies local and inbound RAII permits, atomically advances one or more scopes under a durable callback, and blocks every scope in an uncertain batch until exact full-batch retry or recovery. | **partial** | `tst_session_recovery::membershipCoordinatorFailsClosedAndPublishesOnlyDurableTransitions`, `membershipCoordinatorTransitionsMultipleScopesAsOneGate`, `uncertainMembershipBatchRequiresExactWholeBatchRetry`, and installed Core consumer smoke. Consumers must configure both `DocumentSession` authorization callbacks and provide a durable callback that validates and commits the matching membership event/checkpoint. |
| **Durable identity for consumers.** A `ReplicaAllocator` exists in the source tree but is not exported or installed. | **partial** | Not part of the installed public surface. A consumer supplies its own `IdentityAllocator` and is responsible for durability. Making `ReplicaAllocator` public is a separate decision, because it means committing to its persistence format. |
| **Epoch / document / stream routing.** Core operation values do not carry complete routing context. Public delivery tokens carry stream/epoch/producer/sequence; DocumentSession binds its document scope. | **partial** | `OpStream.h`, `tst_transport_commit_contract`, `tst_session_recovery`; Core consumers must validate their envelope and membership before application. |
| **Automatic returning-branch reconciliation or branch rebase.** Export preserves the original epoch context and the coordinator does not replay or transform operations into a newer descriptor. | **not implemented** | G01/G02 own transition validation and explicit reconciliation; arbitrary old-epoch edits are not promised to merge losslessly. |
| **GC, pins, checkpoints, reclamation.** | **implemented composition with consumer-owned provider obligations; not turnkey installed GC** | `tst_gc_certificate`, `tst_gc_model`, `tst_anchor_retention`, and `tst_session_recovery::sessionCheckpointBindsReceiptAndPreservesTranslatedPinsThroughCollection`. Public pin projection and session checkpoint methods exist. Certified collection requires validated membership/coverage/pins/history and a provider stability lease; certificate construction remains internal. Initial adoption must retain history and disable physical GC. Legacy no-argument collection remains inert. |
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
  consumers must project durable registration/release events before enabling
  certified reclamation.
- **Bytes alone do not protect against reclamation.** `AnchorPinRegistry`
  exposes consumer-event projection and live-pin obligations. Source-level
  certified collection protects validated pins, including translated pins;
  consumers must supply canonical durable records and provider validation.
  Initial installed-consumer adoption leaves physical GC disabled.
- **It does not automatically survive an epoch change.** A token from another
  epoch reports `wrong_epoch`; consumer-owned validated translations are required.
- **It does not survive `discard_local_view()`.** That invalidates position history,
  including for empty-edge tokens; stored bytes then report `history_unavailable`.
- **It is not a built-in recovered store.** A consumer supplies its canonical
  persistence callback and replays pin events before enabling future GC. The
  registry does not own that log or automatically recover it.

No pre-vNext or earlier experimental token is supported, and none will be
converted: there is no production data to convert.

Retained golden bytes for all four token kinds, and the reader fixture that keeps
them working, are `tests/fixtures/position-tokens/` in the source repository.

Private state, local-history journals, and checkpoint artifacts exist, but no
cross-version **store/checkpoint** reader obligation is declared here. Bind
adoption stores to the exact candidate. Before an upgrade with populated stores,
validate a reader/translation and preserve recovery input. No pre-vNext format
is supported or will be converted.

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

- **Mutation is not logarithmic end to end.** Buffer edits copy fragments and
  rebuild the tree. General placement rebuilds a sorted identity index and can
  climb ancestry; a checked tail append skips general placement only. Scoped
  position queries materialize individual retained items. Performance changes
  must preserve ordering/position semantics and installed consumer gates.
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
