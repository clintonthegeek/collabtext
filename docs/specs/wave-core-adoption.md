# Wave Core adoption contract (2026-10-11)

This contract extends the native FugueMax text sequence. It supersedes the
plaintext-only scope for typed reply markers and the candidate-bound CTBO/0002
reader policy. Wave owns canonical events and transactions; Core owns sequence
semantics. No external consumer files are changed by this workstream.

## Inline marker

A marker is one identity-bearing ordered item, with empty text and zero UTF-8
and UTF-16 width. It uses the existing FugueMax link/order and causal undo rules;
there is no separate marker ordering structure. Text range removal excludes
markers, including a whole-body replacement. Removal explicitly targets a typed
marker identity. Hidden markers resolve as `detached`; missing retained state
resolves as `history_unavailable`. Tokens check document, stream, epoch and mint
frontier. A duplicate insertion never revives a removed marker. Compensating undo
of its removal restores the original identity. Fresh replay reproduces prefix
visibility. Markers carry no thread payload; Wave atomically stores their operation
and manifest update in one accepted event.

Numeric insertion uses UTF-8 scalar-boundary offsets and precedes the retained
items following the preceding visible character (including markers at that gap).
Explicit before/after-marker insertion selects the exact sequence gap and avoids
ambiguity when several markers have the same byte offset. These APIs do not
change ordinary UTF-16 text-edit coordinates. Strict text attachments continue
selecting characters and never select markers.

Marker edits reserve one item plus one operation identity for insertion, or one
operation identity for removal. They enter bounded local undo history and use
ordinary callbacks and causal apply outcomes. Malformed text deletions targeting
markers and marker deletions targeting text are rejected before mutation.

## Readers

`decode_operation` retains CTBO/0002 indefinitely for durable consumer archives.
Identity, dependencies, allocation intervals, FugueMax placement, deletion and
undo meaning are preserved, not merely visible strings. Existing text/undo writers
retain canonical CTBO/0002 bytes. Marker edits use CTBO/0003: the same edit object
plus required numeric `marker=1`, and required numeric `m=1` on its single marker
insertion fragment. Removal has no insertion fragments and exactly one singleton
deletion run. CTPT/1 remains unchanged; TextMarker uses CTPT/2 kind 5, left affinity,
text scope and a required referent in place of boundary fences. Both versions
have permanent reader obligations. Unknown versions return `unsupported_version`.
Old bytes need no rewriting. Alpha ABI/source stability is separate from these
reader obligations. New APIs are additive; consumers rebuild against the package.

Checkpoint engine sections with markers use version 3, adding one boolean kind
byte before each text fragment. Section versions 2/3 are current; obsolete engine section 1 is refused. Other
checkpoint sections remain unchanged. Collection remains disabled for initial
Wave adoption; durable marker pins/collection need their own certified gate.

## Local history

CrdtEngine exposes the same bounded history snapshot, restore, coalescing, next
group and retirement-acknowledgment operations as IdListEngine. Restore occurs
after complete shared history replay and before local editing. Consumer-owned
metadata is not a second shared log. Persist snapshot and allocation authority
consistently; restore cannot grant authority to reuse identities. Replay alone
never invents a local undo stack. Publish each returned compensation once, using
either the return value or the local callback. No arbitrary historical-operation
undo capability is promised.

## Deterministic legacy projection

Use a stable, versioned, domain-separated actor per conversation/stream/adapter,
not one actor per canonical event. An event-per-actor scheme eventually exceeds
the 512-actor format bound. Use `ProjectionAllocator::create(scope, adapter_name, adapter_version)` for
the supported v1 SHA-256 derivation and checked in-memory counter; check namespace collisions and keep it disjoint from user writers.
Replay the same canonical order from the beginning with a fresh projection
allocator starting at 1. Every accepted transition must have a specified edit
shape; counters advance deterministically and observe incoming identities.
Do not publish synthetic operations as device edits or reuse a projection actor
for independent writable branches. Scope includes conversation, block stream
and epoch. Retain/version the translator: cross-version encoder and edit-shape
byte determinism is not an archive-reader promise. Immutable Wave event bytes,
IDs and hashes remain Wave's responsibility.


Derivation v1 hashes the ASCII bytes `collabtext/projection-actor/v1`, then
UTF-8/opaque scope document and stream strings each prefixed with their unsigned
32-bit big-endian byte length, primitive byte 1=text/2=list, unsigned 64-bit
big-endian epoch, adapter string prefixed the same way, and unsigned 32-bit
big-endian adapter version. The actor is the first 16 SHA-256 digest bytes.
All-zero output is refused. A consumer checks the derived actor against its
namespace registry; probabilistic collision resistance is not a registry.
Canonical event IDs identify/order transitions in Wave; they are not actor IDs.


2026-10-11 clean-cut amendment: the user requires no compatibility for earlier
alpha stores. Superseded readers and source-compatibility parameters are removed
once current replacements pass. CME1 and unbound checkpoint engine section 1 are
unsupported; presence-only SyncManager no longer takes an ignored engine pointer.
CTBO/0002–0003 and CTPT/1–2 remain current writer formats; the reader obligation
above starts with this adoption candidate for newly durable Wave data. This is
not an obligation to upgrade earlier experimental stores.
