# vNext Engine Checkpoint Format

**Status:** format contract for G04; bounded framing, all shareable section
codecs, private companion section codecs, the combined core validator, and
in-memory engine snapshot adapters are implemented. Content-addressed artifact
storage and alternating atomic manifests are implemented. DocumentSession now
exposes a text shareable-checkpoint capture API; actor-private capture/restore,
provider validation during session startup, and replay from checkpoint coverage
remain in progress.

This format defines a validated shareable snapshot and a separately bound
actor-local companion. It is not a replacement for the retained operation log
or writer state. G05 owns atomic publication, recovery selection, and replay
after the checkpoint frontier.

## 1. Artifact framing

A v1 artifact has this exact framing:

```text
9 bytes   "CTCK0001:"
8 bytes   unsigned big-endian payload length
N bytes   section sequence
32 bytes  SHA-256("CTCK0001:" || length || payload)
```

The declared payload length must equal the exact remaining section bytes before
the digest. No trailing bytes or incomplete records are accepted. The digest
detects corruption; it does not authenticate the producer. Membership
acknowledgements provide authenticity and bind the digest to an epoch proposal.

The payload begins with an unsigned 16-bit big-endian section count (1–32). Each
section is:

```text
u16 section_id
u16 section_version
u64 section_length
section_length bytes of section payload
```

Sections appear once each in ascending `section_id` order. Unknown IDs, unknown
versions, duplicates, omissions of required sections, overflows, truncation, and
trailing content fail validation. A reader retains the source bytes for retry or
upgrade; it never skips an unsupported section and treats the artifact as
complete.

Maximum total artifact size is 256 MiB, with at most 32 sections. Nested arrays
and strings have explicit per-field limits no larger than the containing
section. All counters and lengths use checked integer arithmetic. Text is valid
UTF-8. Actor IDs and operation identities use their canonical public codecs;
native C++ object layouts, pointers, allocators, and tree node layout are never
serialized.

## 2. Shareable state

A shareable checkpoint has the following required sections:

| ID | Name | Contents |
|---:|---|---|
| 1 | Context | document, stream, nonzero epoch, parent descriptor hash (empty only at genesis), engine kind (`text` or `id-list`) |
| 2 | Engine | text fragments or list entries in logical order, stable origins, placement links, values/text, and deletion operation identities |
| 3 | Causal state | exact sparse operation version vector and complete UndoMap entries/counts |
| 4 | Deferred input | original delivery token and original operation bytes for each unapplied operation |
| 5 | Coverage | exact per-producer operation and delivery frontiers represented by the checkpoint |
| 6 | Pins | canonical external pin projection identity and frontier, including completeness metadata |
| 7 | Anchor translation | supported token versions, translation manifest hash, and translations required to preserve live pins |

The typed `encode_checkpoint_core` path builds these sections in this canonical
order and validates cross-section causal references, coverage, and deferred
delivery state before framing. `decode_checkpoint_core` applies the matching
cross-section checks after decoding each bounded section.

`DocumentSession::capture_shareable_checkpoint` assembles the current Buffer
state and durable delivery coverage with a caller-supplied
`SessionCheckpointProviderState`. The result contains the encoded artifact,
its full-artifact SHA-256, operation and delivery coverage, the pin projection
hash, and the canonical translation manifest hash. The caller owns recovery and
validation of the canonical pin-event log and membership descriptor before
publication; the session checks projection completeness, pin/token scopes,
translation consistency, and exact represented log coverage. This API produces
the shareable text artifact only; local writer/history companions are not
included.

The context deliberately does not contain the child descriptor hash: that
descriptor contains the checkpoint digest, so embedding it would create a hash
cycle. A membership transition verifier compares the checkpoint's document,
stream, epoch, parent descriptor, represented coverage, pin projection, and
translation metadata to the child descriptor and the durable proposal.

The Context section payload is exactly:

```text
u16 engine_kind                 # 1=text, 2=id-list
u16 document_byte_length
document_byte_length UTF-8 bytes
u16 stream_byte_length
stream_byte_length UTF-8 bytes
u64 epoch                       # nonzero
u8  has_parent_descriptor_hash  # 0 or 1
[32 bytes raw SHA-256]           # present iff flag is 1
```

Document and stream are nonempty, valid UTF-8, contain no NUL byte, and are at
most 256 bytes each. A non-genesis parent hash is exactly 32 raw digest bytes;
the public typed value renders it as 64 lowercase hexadecimal digits. Complete
section consumption is required.

The Causal state section payload is encoded as:

```text
u32 version_actor_count
version_actor_count × (16-byte actor ID, u64 nonzero counter)
u32 undo_entry_count
undo_entry_count × (edit identity, undo operation identity, u32 nonzero count)
```

Each identity is a 16-byte actor ID followed by a u64 nonzero counter. Actor
IDs in the version vector are strictly increasing by canonical actor order;
UndoMap entries use the engine's strict `(edit identity, undo identity)` order.
Each count is limited to 1,000,000, and complete section consumption is
required. Restore rebuilds the UndoMap and rejects invalid identities, duplicate
or unordered keys, and zero counts.

The Deferred input section begins with a u32 record count (at most 4096). The
stream and epoch are inherited from Context; each record stores a 16-byte
producer, u64 delivery sequence, u8 disposition (`0=deferred`, `1=quarantined`),
u32 original operation byte length and bytes, then a u16 reason byte length and
UTF-8 reason bytes. Records are strictly ordered by producer ID and delivery
sequence, which also forbids duplicate tokens. Delivery sequences are nonzero
and cannot be exhausted. Deferred operation bytes must decode as the Context's
engine kind, name the token producer, remain outside applied state, and still
be causally deferred. Quarantined bytes and their reason are retained even when
the payload cannot decode; quarantined input never advances applied coverage.
Operation bytes are limited to 16 MiB each, reasons to 2048 bytes, and total
section size to the artifact bound. Complete consumption is required.

The Coverage section contains two sparse producer frontiers, each encoded as
`u32 count` followed by that many `(16-byte actor ID, u64 frontier)` pairs:
first operation frontiers, then delivery frontiers. The stream and epoch come
from Context. Actor IDs are valid and strictly ordered, zero frontiers are
preserved, and the combined maps contain at most 262,144 cells. Complete section
consumption is required. Membership validation compares both maps exactly to
the context stream's base frontier in the named epoch and parent descriptor.
Operation coverage cannot exceed the causal vector; a deferred or quarantined
delivery sequence must be beyond its producer's committed delivery frontier.

The Pins section stores u64 projected and canonical event frontiers, a u8
completeness flag, a 32-byte canonical projection hash, and a u32 live-pin
count (at most 65,536). Each pin stores a u16 reference-ID byte length and
printable ASCII ID, a u64 source epoch, then a u32 token length and the original
CTPT bytes. IDs are strictly ordered and unique; each token must decode under
the Context's document, stream, and primitive, with a nonzero source epoch no
later than the checkpoint epoch. This preserves the original scope of an old
live pin until section 7 provides any required epoch translation. Token bytes
are limited to 64 KiB per pin. Completeness is true exactly when the projected frontier equals the
canonical frontier; an incomplete projection blocks checkpoint acceptance.
The caller supplies the canonical event-log projection hash and compares it to
recovered provider state before relying on the pin set.

The Anchor translation section begins with a u32 supported-token-version count
(1–64), the strictly increasing u32 versions, and a u32 translation count (at
most 65,536). Each entry stores a u16 printable-ASCII pin reference ID, u64
source epoch, u32 source-token length and exact source CTPT bytes, then u32
current-token length and exact translated CTPT bytes. Entries are ordered by
reference ID. Every live pin older than the checkpoint epoch has exactly one
entry; no current-epoch pin has one. The source bytes and epoch must equal the
corresponding Pins record, and both tokens must validate for the same document,
stream and primitive at their respective epochs. A final 32-byte SHA-256 digest
binds all preceding section bytes and is the translation manifest hash. The
membership verifier compares this hash and the exact supported-version list
with the immutable epoch descriptor. The codec checks identity, scope, ordering
and digest; the provider that creates a translation remains responsible for
proving that it preserves boundary meaning before publication. Unsupported
token versions fail closed in the CTPT reader and require a checked translation.
The combined membership validator additionally requires the exact full-artifact
SHA-256 to equal the descriptor's base-checkpoint hash and validates stream
coverage, token versions, and the translation manifest as one binding before
the checkpoint can be used as an epoch base.

The Engine section starts with `u16 engine_kind` and `u32 entry_count`, with a
maximum of 1,000,000 entries. Every identity uses the 24-byte encoding above.
Each entry then stores its origin identity and a placement link: a parent
presence byte and optional identity, a side byte (`0=left`, `1=right`), and a
right-origin presence byte and optional identity. Presence bytes are only 0 or
1; a left link's parent and right origin must agree.

For text engines, each record continues with a u32 deletion count and that many
deletion identities, followed by u32 UTF-8 byte length, u32 Unicode scalar
length, then exactly that many text bytes. For IdList, each record instead has
a u64 value after its link, then the deletion count and identities. The decoder
checks UTF-8 and scalar lengths, nonzero identities, non-overlapping text
identity ranges, unique list origins, placement consistency, configured entry
and byte limits, and complete consumption. Visibility and summaries are
recomputed by the restore adapter.

Visibility flags, summary-tree nodes, lookup tables, and other indexes are
derived. Import rebuilds them from logical entries and UndoMap state, then
checks visible text/count, fragment ranges, identities, links, and summaries
before exposing the engine. Serialized visibility is never authoritative.

The core artifact decoder parses Context, Engine, Causal, Deferred input, and
Coverage together. It rejects engine-kind disagreement and any engine link,
deletion, item range, or UndoMap identity not covered by the causal version
vector. It also requires every deferred delivery to match the context and
remain causally unapplied, and checks operation/delivery frontiers against the
state they describe. The caller binds Coverage exactly to the membership
descriptor before accepting the checkpoint. Primitive restore then performs
its additional state and owner-allocator checks before swapping live state.

Deferred records retain the original operation bytes and delivery token. They
are not included in applied operation coverage. Quarantined records remain
separately retained with their reason and original bytes; they do not become
applied state or advance coverage. A checkpoint cannot claim complete coverage
if required deferred or quarantined input is absent.

Pins and anchor translations are provider-owned logical records. Their section
versions are independently validated. A count without a complete projection
is invalid. Missing translation for a supported live token blocks checkpoint
validation; the decoder never fabricates an end position.

## 3. Actor-private companion

The private companion is a distinct artifact, so sharing or copying the
shareable checkpoint cannot copy local undo state. Its v1 frame is:

```text
9 bytes   "CTCL0001:"
8 bytes   unsigned big-endian payload length
N bytes   section sequence
32 bytes  SHA-256("CTCL0001:" || length || payload)
```

It uses the same section encoding, canonical ordering, checked-length rules,
and 256 MiB total limit as the shareable artifact. It has exactly three
required sections: owner context (actor ID and shareable checkpoint digest),
local history (coalesced undo/redo groups, cursor, and pending trim/retirement
obligations), and writer state (persistent writer identity and reserved counter
state). Unknown sections or versions are retained as unsupported companion
bytes and prevent local restore; they do not invalidate the shareable engine
checkpoint.
The typed `encode_checkpoint_local_core` path composes these sections and
computes the owner binding from the supplied shareable artifact bytes; the
writer actor and context scope must match the local owner and shareable context.

The owner section is exactly a 16-byte actor ID followed by the 32-byte raw
digest of its shareable checkpoint. The history section stores the owner actor,
u64 undo cursor, u32 active-group count, u32 retired-group count, then each
active and retired group in preserved order. A group stores its 24-byte group
identity, three u32-counted identity vectors (operation IDs, inserted items,
deletion operations), and u64 retained-byte charge. IDs must belong to the
owner, be nonzero and below the exhausted-counter sentinel, and be unique
within each vector. Group IDs cannot repeat across active and retired groups.
The cursor cannot exceed the active-group count; total groups are limited to
100,000, total history identities to 2,000,000, and the section to 64 MiB.

The writer section stores owner actor, u16-length document and stream strings,
then u64 epoch, next identity counter, and durable delivery frontier. Its scope
must equal the shareable Context and its actor must equal the owner section.
The codec preserves this state for validation; import compares it to the
owner's durable identity state and operation log. It never adopts counters from
the artifact as authority. CheckpointStore also validates cross-artifact
consistency before publication and recovery: the reserved identity counter must
be greater than the owner's shareable causal frontier, the local durable
delivery frontier must equal the owner's shareable delivery coverage, and every
active or retired local history identity must be represented in the shareable
causal vector. These checks establish internal consistency; they do not replace
the caller's comparison against durable owner state during import.

Undo/redo groups, cursor position, pending trim obligations, pending local
operation intent, writer identity, and counter-reservation state stay in the
owner's private durable store. The private companion names the exact shareable
checkpoint digest and actor ID it accompanies. It is validated against replayed
operations by the owning engine before restore. Importing a shareable
checkpoint never adopts another actor's private history, identity, counters, or
writer lease.

Pending local operation intent stays in the private durable operation log; the
companion binds to that log's durable frontier in its writer-state section and
does not duplicate operation payloads. This avoids two competing authorities
for a write whose log append may have an uncertain outcome.

The private companion can be missing or stale without making the shareable
engine state corrupt, but the owner must report incomplete local recovery and
must not silently discard undo/redo or pending local work. G05 defines how the
companion and shareable checkpoint are published and selected together.

## 4. Validation and replacement

Parsing, structural validation, and semantic validation finish before live state
changes. Import builds a temporary engine with the caller's existing private
identity allocator, restores and validates local history separately, and only
then swaps the rebuilt state into the live session. Any failure leaves the
existing engine unchanged and returns a typed error with the failing section.

Validation checks include:

- format identifier/version, exact length, digest, section ordering, section
  count, and every configured byte/count limit;
- document, stream, epoch, engine-kind, parent-descriptor, and expected
  checkpoint context;
- valid UTF-8 and exact fragment byte/character lengths;
- unique operation and item identities, valid actors/counters, and checked
  run-end arithmetic;
- placement links and deletion references against the checkpoint's logical
  state and declared coverage;
- canonical sparse version vectors and nonzero, ordered UndoMap records;
- deferred records whose original bytes decode to their named producer and
  delivery token, while remaining outside applied coverage;
- exact coverage maps, complete pin projection, and supported token translation
  metadata;
- rebuilt indexes and summaries agreeing with the logical entries.

State-dependent operation applicability is checked by the engine restore
adapter, not assumed from syntactic decode success. A checkpoint with missing
history, unknown bytes, or a failed semantic invariant is rejected as a whole;
the caller keeps the retained log and uses replay fallback. This codec does not
delete covered logs or advance a receive cursor; that is G05/G07 work.

## 5. Required automated cases

`tst_checkpoint_codec` must cover text and IdList round trips with empty and
populated state, tombstones, non-ASCII text, causal vectors, UndoMap entries,
deferred operations, coverage, pins, token translation, and private-history
separation. After round trip, both engines must match the source for current
text/list values, anchors, undo/redo behavior, and future operations delivered
in reordered and duplicated traces.

Negative tests truncate at every frame/section boundary, alter every required
context field, tamper payload and digest, duplicate or reorder sections,
inject unknown versions, exceed each size/count limit, corrupt UTF-8 and
identity ranges, omit referenced operations, and mismatch engine kind, epoch,
descriptor, coverage, pins, or token manifest. Every failed import must leave a
pre-populated live engine unchanged. Run bounded decoder fuzzing under
ASan/UBSan with the same allocation and byte budgets as production.

## 6. Implementation boundary

The bounded framing codec, every typed shareable and local section codec, the
cross-section validator, and in-memory text/IdList logical snapshot and
failure-atomic restore adapters are implemented. Session deferred envelopes
retain original bytes and delivery tokens. Focused Debug, Release, and
ASan/UBSan tests cover the codecs and engine continuation behavior. The
DocumentSession does not yet capture or restore these complete structures, and
provider-owned pin/translation state is not yet gathered or checked as part of
session startup.

## 7. Durable store and remaining recovery work

`CheckpointStore` writes artifacts under their SHA-256 content names. It
publishes shareable bytes and an optional local companion with
`replace_atomically` before publishing a manifest, so the manifest never names
an artifact whose write and directory sync have not succeeded. The store holds
an OS process lease while open. It does not delete previous checkpoint files or
covered logs; G06 owns reclamation.

Each manifest is exactly 113 bytes:

```text
8 bytes   "CTCPTR01"
8 bytes   nonzero big-endian generation
1 byte    local companion present (0 or 1)
32 bytes  raw SHA-256 of shareable artifact
32 bytes  raw SHA-256 of local artifact, or zero bytes when absent
32 bytes  SHA-256 of all preceding manifest bytes
```

Odd generations replace `manifest-1`; even generations replace `manifest-0`.
The untouched slot is the previous publication. Recovery checks candidates in
descending generation order, validates the content hash and typed sections,
checks local owner/context/hash binding, and calls a mandatory application
validator before selecting a candidate. That validator must compare membership
and provider-owned pin/translation state. A rejected, missing, or corrupt
candidate falls back to the other manifest. The shareable digest is the
checkpoint ID suitable for a transport recovery point.

`FileOpStream::delivery_frontiers()` scans and validates the retained stream,
then returns each producer’s highest contiguous delivery sequence.
`replay_uncovered()` validates supplied checkpoint delivery frontiers against
those retained sequences before invoking its visitor; a frontier beyond a
producer’s log (including a missing producer) fails with `corrupt_record`.
This checks retained-log availability only. It does not prove that checkpoint
engine state represents the covered deliveries or authenticate the checkpoint.

This storage layer does not yet let `DocumentSession` capture a checkpoint,
restore local history and allocator state, advance transport cursors only after
checkpoint publication, or replay only uncovered records. Full-log replay
remains the recovery path until those behaviors and full-replay equivalence are
implemented and tested. Threshold/manual creation and separate recovery-time
measurement are also outstanding.
