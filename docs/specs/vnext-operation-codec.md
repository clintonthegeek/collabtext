# Native operation codec contract

Current as of 2026-10-11, package alpha.3. The installed public
`<collabtext/Serialization.h>` exposes checked opaque-operation readers/writers.
[wave-core-adoption.md](wave-core-adoption.md) specifies supported archival
semantics and marker/projection composition. Pre-vNext CTBO/CTLO/0001 remains
unsupported; supported current versions must retain readers and literal goldens.

## Frames and scalar representation

Frames have a fixed nine-byte ASCII header: four-byte magic, four lowercase hex
version digits, colon, then one bounded JSON object. Encoders emit no newline.
JSON trailing whitespace is accepted; trailing non-whitespace rejects.

| Family | Versions | Body |
|---|---|---|
| CTBO | 0002 | Text edit or undo/redo |
| CTBO | 0003 | Exactly one typed marker insertion or removal |
| CTLO | 0002 | IdList insert/remove/undo |
| CTGV | 0001 | Sparse actor/head object |
| CTGP | 0001 | Actor, first, count, prior causal vector |

No operation document/stream/epoch binding is implied by these frames; consumers
route and validate their canonical event/envelope context. Decoding never commits
consumer coverage or receipts. Unknown bytes remain caller-owned.

Unsigned integers use strings with exactly sixteen lowercase hex digits, also
for uint32 fields and enum values. Narrow fields must fit their domain. Identity
is `[32-character actor hex, 16-character counter hex]`. Reject invalid/all-zero
actors, zero or reserved-max identity counters, noncanonical integers, overflows
and unusable intervals. Offsets/undo counts may be zero where the operation
schema permits it; an IdList application value may be UINT64_MAX. No integer is
passed through a floating-point representation.

Global is an object of nonzero actor keys and nonzero head strings; the writer
sorts full actor bytes. Decode rejects duplicate/invalid actors, zero/reserved-max
heads and excessive actors. Object field order may vary on input; unknown fields
reject. Logical payload data survives decode, not original JSON formatting.
Literal UTF-8 and equivalent surrogate escapes yield identical content.

## Payload fields (canonical writer order)

Every listed field is required, including empty vectors. No field silently defaults.

| Variant | Fields |
|---|---|
| CTBO/0002 edit | t=e, ts, v, a, did, r, nt, if, dr |
| CTBO/0002 undo/redo | t=u, ts, v, a, c |
| CTBO/0003 marker edit | t=e, ts, v, a, marker=1, did, r, nt, if, dr |
| CTLO/0002 insert | t=il-i, ts, v, a, id, k |
| CTLO/0002 remove | t=il-r, ts, v, a, to |
| CTLO/0002 undo/redo | t=il-u, ts, v, a, c |
| CTGP/0001 gap | actor, first, count, v |

`r` is byte-offset range pairs; `nt` replacement strings; `if` insertion objects
`{o,k,c,n}` (origin, link, content, scalar/item count). Link `k` is `{p,s,r}`:
optional parent as identity/empty array, side 0=Left/1=Right, optional right origin.
Left links require parent=right origin. `dr` is first-identity/count pairs; `c`
is target-identity/undo-count pairs. No locator or split-relocation data exists.

CTBO/0003 requires empty `r`/`nt` and exactly one of: one insertion object
`{o,k,c,n,m}` with c empty, n=1, m=1 and empty `dr`; or empty `if` and one
singleton deletion run. Marker removal targets a marker in retained state;
ordinary text removal may not target one. State-dependent type validation occurs
before commit. Markers contribute one identity but zero text width. Compensating
undo remains the existing CTBO/0002 undo variant; item type is retained in state.

Payload structures are private. Decode freezes checked owned payloads into
immutable public handles. Check identity, content counts, cardinality, links and
interval arithmetic before returning a value; causal applicability and retained
state are checked by `apply_remote`/`apply_ops`.

Allocation `a` is `[first,count]` in the operation actor domain. An edit creating
n ordered items reserves n+1 counters ending at ts; created item identities occupy
the preceding n counters without gaps/overlap. A deletion-only edit, undo/redo or
list operation reserves one. Text/marker edit did=ts. Marker insertion counts as
one item. Default/moved-from operations are invalid and refused.

Counter-gap metadata describes a nonempty interval ending below UINT64_MAX and
not overlapping prior actor state. Decoding does not issue identities, publish a
gap or prove terminal coverage. The durable protocol must account for abandoned
reservations and counter raises; a largest observed head is not contiguity proof.

## Errors, limits and retained readers

Default limits: 8 MiB frame, 4 MiB decoded string, depth 32, 100,000 JSON values,
1,024-byte keys, 512 actors. Writer checks the same bounds; never truncates. A
large local operation may exceed wire bounds: report refusal and retain the
operation at its owner. Do not acknowledge it as a durable consumer event.

Unknown magic/bare JSON -> unknown_format. Recognized truncated header ->
malformed. Unknown complete version -> unsupported_version. Unknown tag in a
supported format -> unsupported_operation. Invalid grammar/shape/integers/bounds
-> malformed; Unicode/identity checks retain their typed errors. No decoder
creates a pin, mutates an engine or advances coverage.

CTBO/0002 and CTBO/0003 have indefinite durable-reader obligations. Retain old
item/operation identities, causal dependencies, allocation, FugueMax placement,
deletions and compensation semantics, not merely visible strings. Existing
CTBO/0002 encoder goldens remain unchanged. Marker insertion/removal and CTPT/2
literal goldens are also retained in the public installed `consumer_wave_core`
trace. Unknown-version inputs use a future version; known-version malformed
inputs must continue rejecting. CTLO/0002 and CTGP/0001 also have indefinite durable readers from the Markoff
adoption cut; see [Markoff contract](markoff-core-adoption.md). CTGV retains
its separately declared alpha interchange status. Archive reading is independent of encoder byte
choices for new operations across future library versions.
