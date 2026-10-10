# vNext Engine Checkpoint Format

**Status:** format contract for G04; bounded frame codec is implemented, while
section payload codecs and engine snapshot adapters are still in progress.

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

The context deliberately does not contain the child descriptor hash: that
descriptor contains the checkpoint digest, so embedding it would create a hash
cycle. A membership transition verifier compares the checkpoint's document,
stream, epoch, parent descriptor, represented coverage, pin projection, and
translation metadata to the child descriptor and the durable proposal.

Visibility flags, summary-tree nodes, lookup tables, and other indexes are
derived. Import rebuilds them from logical entries and UndoMap state, then
checks visible text/count, fragment ranges, identities, links, and summaries
before exposing the engine. Serialized visibility is never authoritative.

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

The bounded framing codec and in-memory text/IdList logical snapshot and
failure-atomic restore adapters now exist, with focused Debug, Release, and
ASan/UBSan regressions. Section payload serialization is still incomplete, as
are provider-owned coverage/pin/translation records, original deferred envelope
bytes and tokens, and validation of those fields against membership evidence.
The format is not yet a complete durable checkpoint. Atomic files, checkpoint
selection, cursor publication, and replay fallback remain G05.
