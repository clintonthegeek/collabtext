# P06b operation codec implementation contract

Status: native version 1 implementation. P06b acceptance is recorded in the
roadmap registry after public and installed-consumer verification. No alpha
has been published; this format is not a published compatibility commitment.
No migration/legacy reader is required. Reuse the existing native payload builders
and bounded JsonReader, not another CRDT engine.

## Frame and scalar representation

Frames are ASCII header plus one bounded JSON object, with no trailing newline
from the encoder. Fixed nine-byte header: four-byte magic, four lowercase hex
version digits, literal colon, then JSON body. Version is 0001.

| Logical family | Magic | Body |
|---|---|---|
| collabtext.buffer-op | CTBO | edit or undo payload |
| collabtext.idlist-op | CTLO | insert/remove/undo payload |
| collabtext.global | CTGV | sparse actor/head object |
| collabtext.counter-gap | CTGP | actor, first, count, prior causal vector |

The fixed header permits format/version classification before payload allocation.
No operation document/stream/epoch binding is implied: P07 owns that envelope.
The ASCII header avoids introducing newline or NUL into existing line-record
payloads. Unknown bytes still belong to the caller; no reader may skip them and
advance coverage merely because a codec returned an error.

All unsigned integer fields use JSON strings containing **exactly sixteen
lowercase hexadecimal digits**, including uint32 fields and locator digits.
This makes counter/application-value width fixed, preserves every uint64 exactly,
and permits equal-shape small/large-identity size checks. Narrow fields must also
fit their actual uint32 domain. Identity is [32 lowercase actor-hex characters,
16-digit counter string]. Reject invalid/all-zero actors, zero or reserved-max
identity counters, noncanonical integer strings, and out-of-range narrow fields.
Offsets and undo counts may be zero; application uint64 values may be UINT64_MAX.
Never pass any integer through double or a Qt JSON numeric value.

Encode a Global as an object of nonzero actor-hex keys to nonzero head strings,
sorted by full raw actor bytes. Zero components are omitted. Decode rejects
invalid/duplicate actors, zero or reserved-max heads and excessive actors; the
canonical encoder owns sorted output. JSON object field order may vary on input,
but unknown fields in a supported payload schema reject rather than disappearing.
A codec round-trip preserves logical operation data, not formatting of input JSON.
Valid literal UTF-8 and equivalent JSON surrogate-pair escapes must yield the
same UTF-8 content. JsonReader::parse_syntax performs grammar/Unicode checks
without mistaking the payload's causal-vector v field for an envelope version.

## Payload fields and allocated intervals

Retain the current native fields/meanings; change their wire scalar representation,
not engine ordering. Canonical field order follows the table. All listed fields
are mandatory, including empty vectors; no absent field silently defaults.

| Variant | Fields |
|---|---|
| text edit | t=e, ts, v, a, did, r, nt, if, dr, sr |
| text undo/redo | t=u, ts, v, a, c |
| list insert | t=il-i, ts, v, a, id, loc |
| list remove | t=il-r, ts, v, a, to |
| list undo/redo | t=il-u, ts, v, a, c |
| counter gap | actor, first, count, v |

Use existing structures for if (origin, locator, content, scalar length), dr
(first identity/count), sr (origin, split offset/length, new locator), r
(byte-offset pairs), nt (replacement strings) and c (identity/undo-count pairs).
Do not expose these payload structures in installed headers. Build privately from
checked values and freeze into P06a's immutable handles after successful decode.
Check lengths/UTF-8 scalar counts, usable identities/locators, range cardinality
and interval arithmetic before returning a value; state-dependent causal/order
validation still belongs to the engine's apply path.

The allocation field a is [first counter, count], in the operation actor's domain.
For an edit inserting n scalars, its interval has n+1 counters and ends at ts;
all created item identities occupy the preceding n counters with no gaps/overlap.
Derive from actual inserted fragments, not text bytes or locator rank. A
zero-insert edit, undo/redo or list operation occupies one counter. Require did=ts
for text edits under P06a's allocation contract. Check all sum/subtraction bounds;
never wrap a first counter when a malformed builder has more items than ts.
Encoding invalid/default/moved-from values returns a typed refusal, not {}.

Counter-gap data is transport/protocol metadata, not another document primitive.
Its actor/first/count interval is nonempty, ends below UINT64_MAX, and cannot
alias observed prior actor state. Its v states prior causal context. Gap decoding
only validates/owns the record; it does not apply, issue identities, advance
coverage or mutate an engine. P07/L02 own terminal gap integration, conflict checks
and publication from durable reservation evidence. A largest observed counter
alone cannot certify contiguity. L02 must account for both abandoned reservations
and permanently unused jumps caused by raising a local counter after remote
observation; do not declare those skipped counters implicitly covered.

## Checked APIs and bounds

Public encode/decode families return the existing Result<T> with owned immutable
values/bytes or typed errors. Use distinct operation/list/global/gap functions;
remove the optional-only public contract when the new format lands. Update every
internal test/consumer caller to check ok() before value(); add no legacy-format
reader or optional wrapper that erases version errors.

Use explicit bounded defaults aligned with JsonLimits: frame at most 8 MiB,
decoded individual string at most 4 MiB, nesting at most 32, total/container
values at most 100,000, keys at most 1,024 bytes, frontier at most 512 actors.
Encoder checks the same limits and returns an error for a frame it cannot encode;
it must never output something its own decoder rejects. Reject oversize input
before allocation. A legitimate large local operation may exceed wire limits;
report the refusal and retain it at its owner. This codec task must not invent
silent truncation, partial application or counter reuse to solve that refusal.

Check header/length/version before parsing body. Unknown magic or obsolete bare
JSON -> unknown_format; recognized truncated header -> malformed; unsupported
complete version -> unsupported_version. Unknown supported-schema operation tag
-> unsupported_operation. Grammar/shape/canonical integer/bound failures ->
malformed (invalid Unicode -> invalid_unicode; invalid identity -> invalid_identity).
Retain newer-format/unknown-variant bytes for an upgrade; quarantine malformed
bytes with diagnostic context. Neither outcome advances coverage. A standalone
codec cannot commit a cursor or prove a recovery point.

Reuse JsonReader's complete-input, duplicate-key and Unicode checks and consume
its checked JsonValue tree directly in the final decoder. The interim preflight
plus legacy field parser is a correctness repair, not the final versioned codec.
Remove duplicate parsing when the new builder path replaces it; this is ordinary
implementation, not a benchmarking/optimization project. The installed capability
manifest must continue to call typed/versioned codecs unavailable until executable
public and installed-consumer cases pass.

## Acceptance sequence for the executor

1. Keep the existing public garbage/duplicate/invalid-Unicode/oversize and escaped
   non-BMP regressions green. Write the typed version/format regression before
   replacing public signatures. Confirm the old API cannot provide that verdict.
2. Implement public Result declarations, header classification, canonical scalar
   helpers and bounded family readers/writers. Replace the old unversioned decoder;
   no second decoder may silently remain on the public path.
3. Implement allocated interval and terminal gap codecs. Exercise all arithmetic
   edges against the allocation model's rules. Keep gap integration/store work
   explicitly pending in P07/L02.
4. Update current facade/transport/test/example callers for Result ownership and
   errors. A failed decode is not permission to commit its record; retain/report
   bytes. Record legacy SegmentReader/FileSync/StreamSync transaction shortcomings
   as L03/P07 work instead of claiming codec tests certify their durability.
5. Freeze exact new-format golden bytes for every variant, empty/sparse vectors,
   multiple full-width actors, counters above 2^53 and UINT64_MAX-1, and list values
   UINT64_MAX. Compare small/large equal-shape frames: lengths must be equal.
6. Negative matrix: obsolete and unknown formats/versions, missing/unknown fields,
   invalid actor/counter/domain/interval, duplicate keys, every proper truncated
   prefix, trailing non-whitespace input, malformed Unicode and encoded/decoded
   bounds. JSON trailing whitespace is permitted; the separate position binary
   codec rejects every trailing byte. No helper creates a pin or mutates an engine.
7. Run affected Debug/Release/leak-enabled sanitizers and relocated installed-only
   consumers using actual operations, both primitive families and gap metadata.
   Retain exact selection/logs. Benchmarks stay excluded. Update the authoritative
   registry only after this entire contract passes; the syntax repair is partial.
