# CollabText IdList CRDT Specification

An ordered-list CRDT over opaque `uint64` elements. This document
specifies the `IdList` engine — the second primitive alongside `Buffer`
in the CollabText CRDT engine. It is introduced at schema version 3.

For clock types (Lamport, Global), see `CRDT_ENGINE_SPEC.md §2`. This
document does not repeat that material.

---

## 1. Conceptual Overview

`IdList` is an ordered list of opaque `uint64` elements — **ids** —
maintained with the same tombstone-based CRDT approach as `Buffer`.
Elements are never physically removed; deleted elements become invisible
tombstones. The visible list is the subsequence of all elements where
`visible == true`, in Locator order.

```
IdList = ordered list of IdListEntry
IdListEntry = { origin: Lamport, locator: Locator, id: uint64,
                deletions: [Lamport], visible: bool }
Visible list = [entry.id for entry in entries if entry.visible]
```

**Structural similarity to Buffer.** `IdList` reuses `SumTree`,
`Locator`, `Anchor`, `Clock`, and `UndoMap` without modification. The
key difference: where `Buffer` inserts UTF-16 text and fragments can
span multiple code units, `IdList` inserts single atomic elements.
There is no splitting, no `byte_length`, no text content. Each entry is
exactly one element.

**Why opaque ids?** The `uint64` value is entirely application-owned.
`IdList` does not interpret it. A block editor might store document-node
IDs; a spreadsheet might store row handles; a slide editor might store
slide identifiers. The CRDT only cares about order and identity across
replicas, not about what the ids mean.

---

## 2. Clock Types

`IdList` uses exactly the same clock types as `Buffer`:

- **`ReplicaId` (`uint16_t`)** — Unique per editing session.
- **`Lamport`** — Globally unique per operation across all replicas.
- **`Global`** — Vector clock; tracks observed operations per replica.

See `CRDT_ENGINE_SPEC.md §2` for full definitions, tick/observe
semantics, ordering rules, and sentinel values.

---

## 3. Entry Model

### 3.1 IdListEntry

```cpp
struct IdListEntry {
    Lamport origin;           // Lamport timestamp of the insertion op
    Locator locator;          // Fractional position in the list
    uint64_t id;              // Opaque application value
    std::vector<Lamport> deletions;  // Lamport timestamps of deletion ops
    bool visible;             // Cached visibility (kept in sync with UndoMap)
};
```

- **`origin`** — The Lamport timestamp of the `IdListInsertOp` that
  created this entry. Unique across all entries in all replicas.
  Serves as the stable identity of the element — not the `id` value,
  which may duplicate.
- **`locator`** — Fractional position identifier. Entries are ordered
  first by `locator`, then by `origin` for tiebreaking concurrent
  inserts at the same position.
- **`id`** — The application's opaque `uint64` element value. Not
  unique from the CRDT's perspective.
- **`deletions`** — The Lamport timestamps of all `IdListRemoveOp`s
  that targeted this entry. Multiple concurrent removes accumulate
  here without conflict.
- **`visible`** — Cached result of visibility computation. An entry
  is visible if its `origin` is not undone and none of its `deletions`
  are active (i.e., each deletion is undone).

### 3.2 Contrast with Buffer's Fragment

| Aspect | `IdListEntry` | Buffer's `Fragment` |
|--------|---------------|---------------------|
| Content | `uint64_t id` | `std::string_view` into rope |
| Byte length | None | `byte_length: uint32_t` |
| Split | Never | On partial delete/insert |
| Identity | `origin` Lamport | `id: Locator` + `timestamp` |
| Multiple code units | No (always exactly 1 element) | Yes |
| Size on heap | ~24 bytes + deletions vec | ~64–128 bytes + text |

The absence of text content and the no-split invariant make `IdList`
considerably simpler. Every operation touches exactly one entry.

---

## 4. SumTree Integration

### 4.1 IdListSummary

```cpp
struct IdListSummary {
    uint32_t visible_count = 0;
    uint32_t deleted_count = 0;
    Locator max_locator;
    Lamport max_origin = Lamport::min();
    Global max_version;
    Global min_insertion_version;
    Global max_insertion_version;
};
```

The `SumTree` aggregates `IdListSummary` upward through the tree using
the standard monoid `add_summary`. This enables:

- **O(log n) index-to-entry seeks** via `VisibleIndex` — counts visible
  entries to the left.
- **O(log n) origin lookups** — the `max_origin` field supports binary
  search for an entry by its Lamport origin.
- **Garbage collection queries** — `min_insertion_version` /
  `max_insertion_version` track the version span covered by a subtree,
  enabling efficient tombstone removal without scanning all entries.

### 4.2 VisibleIndex

```cpp
struct VisibleIndex {
    uint32_t value = 0;
    void add_summary(const IdListSummary& s) { value += s.visible_count; }
};
```

`VisibleIndex` is the seek dimension used for index-based access:
`anchor_at_index(n)` seeks to where `VisibleIndex == n`.

### 4.3 Tree type

```cpp
using IdListTree = SumTree<IdListEntry, IDLIST_TREE_B>;
// IDLIST_TREE_B = 6 (matching Buffer's branching factor)
```

All tree mechanics (node splitting, cursor traversal, O(log n)
seeks and edits) are identical to `Buffer`'s fragment tree.

---

## 5. Operation Types

`IdList` uses three operation variants, distinct from `Buffer`'s
`Operation` type. **They do not share a C++ variant.** `Buffer`'s ABI
is unchanged.

### 5.1 IdListInsertOp

```cpp
struct IdListInsertOp {
    Lamport timestamp;  // Origin of the new element; becomes entry.origin
    Global  version;    // Causal dependencies at time of insertion
    uint64_t id;        // Opaque application value
    Locator  locator;   // Pre-computed fractional position
};
```

Wire tag: `"il-i"`.

The `locator` is computed by the inserting replica from the current
list state and encoded into the op. Receivers place the entry using
`(locator, timestamp)` order — no re-computation of position from
context is required.

JSON wire format:
```json
{"t":"il-i", "ts":[replica_id, seq], "v":{...}, "id":42, "loc":[...]}
```

### 5.2 IdListRemoveOp

```cpp
struct IdListRemoveOp {
    Lamport timestamp;       // Lamport of this deletion op (recorded as deletion_id)
    Global  version;         // Causal dependencies
    Lamport target_origin;   // Origin of the entry being removed
};
```

Wire tag: `"il-r"`.

The entry to be removed is identified by its `origin` Lamport, not its
`id` value. If two entries carry the same `id`, only the specific one
identified by `target_origin` is removed.

JSON wire format:
```json
{"t":"il-r", "ts":[replica_id, seq], "v":{...}, "to":[replica_id, seq]}
```

### 5.3 IdListUndoOpVariant

```cpp
struct IdListUndoOpVariant {
    Lamport timestamp;
    Global  version;
    std::vector<std::pair<Lamport, uint32_t>> counts;  // (edit_id, new_parity)
};
```

Wire tag: `"il-u"`.

Mirrors `Buffer`'s `UndoOperation` exactly. The `counts` field carries
the full updated parity state for every edit being undone or redone.
See §8 (Undo model) for semantics.

JSON wire format:
```json
{"t":"il-u", "ts":[replica_id, seq], "v":{...}, "c":[[[r,s],parity], ...]}
```

### 5.4 IdListOperation variant

```cpp
using IdListOperation =
    std::variant<IdListInsertOp, IdListRemoveOp, IdListUndoOpVariant>;
```

Helper accessors:
- `get_idlist_op_timestamp(op)` — extracts Lamport from any variant
- `get_idlist_op_version(op)` — extracts Global from any variant

---

## 6. Concurrent Semantics

### 6.1 Concurrent inserts at the same position

When two replicas concurrently insert after the same anchor, they
compute the same `Locator::between(lo, hi)` result independently. The
tiebreak is `(locator, origin)` order — lower `locator` first, then
lower `origin` Lamport. This is deterministic and total across all
replicas given the same set of operations, regardless of delivery order.

Equivalent to `Buffer`'s fragment ordering. The tree invariant is:

```
for all adjacent entries a, b:
    (a.locator, a.origin) < (b.locator, b.origin)
```

### 6.2 Concurrent removes of the same element

If two replicas independently remove the same element, both
`IdListRemoveOp`s are applied. The entry's `deletions` vector
accumulates both timestamps. The entry becomes and remains invisible.
No conflict; no last-write-wins behavior. Both removes survive in the
tombstone record and are individually respectable by undo.

### 6.3 Insert-after-deleted anchor

An insert operation carries a pre-computed `Locator`, not a pointer to
a live entry. If the anchor entry is concurrently deleted, the insert
still lands at the correct position because the locator is position
data, not a reference. The inserted element appears at the position
the anchor occupied, adjacent to surviving neighbors.

This is the key advantage of fractional-index locators over pointer-
based approaches: deletion of the anchor does not invalidate the
position.

### 6.4 Insert-vs-remove conflict

An insert and a concurrent remove of different elements are trivially
independent. An insert and a concurrent remove of the element being
inserted after the insert has already been applied is handled the same
as a local remove: the `deletions` vector is appended, visibility is
recomputed. No special-casing required.

---

## 7. Anchor Model

`IdList` uses the same `Crdt::Anchor` type as `Buffer`:

```cpp
struct Anchor {
    uint16_t replica_id;
    uint64_t char_value;   // In IdList context: the origin.value of the target entry
    Bias     bias;         // Left or Right
};
```

### 7.1 API

```cpp
// Create an anchor for the entry at visible index n
Anchor anchor_at_index(uint32_t index, Bias bias = Bias::Left) const;

// Create an anchor for a specific element by its opaque id
// Precondition: exactly one visible entry with this id exists (or the
// first visible match if duplicates exist)
Anchor anchor_of(uint64_t id, Bias bias = Bias::Left) const;

// Resolve an anchor to a visible index (0-based)
uint32_t resolve_anchor(const Anchor& a) const;

// Compare two anchors by their resolved positions
int compare_anchors(const Anchor& a, const Anchor& b) const;
```

### 7.2 Anchor stability across edits

Anchors survive concurrent edits on remote replicas. If the element
an anchor points to is deleted, `resolve_anchor` returns the position
the element occupied — the index of the next visible entry to its right
(for Left-biased anchors) or left (for Right-biased anchors).

This mirrors `Buffer`'s anchor contract exactly. Cursors tracking a
specific list element survive that element's deletion without becoming
invalid.

---

## 8. Undo Model

### 8.1 Parity-based UndoMap

`IdList` reuses `UndoMap` without modification. The undo state for each
edit (identified by its Lamport timestamp) is a parity count:

- Even count (0, 2, 4, …) → edit is **active** (visible)
- Odd count (1, 3, 5, …) → edit is **undone** (invisible)

An element is visible when:
1. Its insertion parity is even (not undone), AND
2. Every deletion in its `deletions` vector has odd parity (all deletions undone)

### 8.2 Local undo/redo

```cpp
std::optional<IdListOperation> undo();   // returns op to broadcast; nullopt if stack empty
std::optional<IdListOperation> redo();   // returns op to broadcast; nullopt if at top
bool coalesce_last_undo();               // merge the last two undo stack entries into one
size_t undo_depth() const;
size_t max_undo_depth() const;
void set_max_undo_depth(size_t depth);
```

`undo()` increments parity for all inserts and deletions in the top
undo stack entry, emits an `IdListUndoOpVariant` carrying the new
counts, and broadcasts it to peers. `redo()` decrements parity back.

`coalesce_last_undo()` merges the last two entries in the undo stack
so that a subsequent `undo()` reverses both atomically — useful for
grouping a remove+insert sequence into one logical action.

### 8.3 Collaborative undo

Undo ops carry full parity counts for all affected edits. A peer
receives the `IdListUndoOpVariant` via `apply_ops` and applies the
count updates directly to its `UndoMap`. All replicas that have seen
the same undo op converge to the same visibility state for the affected
entries.

Only local edits (by this replica) appear on the local undo stack.
Remote replicas' undo ops are applied via `apply_ops` but do not
manipulate the local undo stack.

---

## 9. Garbage Collection

### 9.1 collect_garbage()

```cpp
size_t collect_garbage();  // returns number of entries removed
```

Removes tombstones that are safe to discard without coordination:

- The entry is invisible (`visible == false`)
- Every timestamp in `deletions` originated from the local replica
- The insertion is not protected by the local undo stack

This is conservative: tombstones deleted by a remote replica are left
in place until `compact()` is called with a sufficient watermark. The
invariant is that `collect_garbage()` never removes an entry that any
replica might need to resolve an anchor or apply a deferred op.

### 9.2 compact(watermark)

```cpp
size_t compact(const Global& watermark);  // returns number of entries removed
```

The `watermark` is the meet (component-wise minimum) of all known
replicas' version vectors — the set of operations observed by every
replica. An entry can be compacted if:

- It is invisible, AND
- All its `deletions` timestamps are covered by the watermark (i.e.,
  every replica has observed the deletion), AND
- The entry is not undo-stack protected

This mirrors `Buffer::compact()` exactly. The SyncManager is
responsible for computing and broadcasting the watermark; the engine
only consumes it.

### 9.3 Undo stack protection

An entry whose insertion or deletion participates in the local undo
stack is exempt from both `collect_garbage()` and `compact()`. This
prevents GC from discarding state that would be needed to execute an
undo or redo.

---

## 10. Wire Format

Operations are serialized as JSON objects. The `schema_version` field
in the sidecar manifest must be `3` (set by default in
`SidecarManifest::schema_version`) for IdList operations to appear in
op streams. Files written at schema version 1 or 2 (Buffer-only) can
still be decoded; schema_version 3 is additive.

### 10.1 Common fields

All three op types share:

| Field | Key | Encoding |
|-------|-----|----------|
| Type tag | `"t"` | `"il-i"`, `"il-r"`, or `"il-u"` |
| Timestamp | `"ts"` | `[replica_id, seq]` |
| Version vector | `"v"` | `{replica_id: seq, ...}` (sparse) |

### 10.2 IdListInsertOp (`"il-i"`)

```json
{
  "t":   "il-i",
  "ts":  [2, 7],
  "v":   {"1": 4, "2": 6},
  "id":  1099511627776,
  "loc": [3, 14159, 0]
}
```

Additional fields:

| Field | Key | Type | Description |
|-------|-----|------|-------------|
| Opaque id | `"id"` | `uint64` | Application element value |
| Locator | `"loc"` | `[uint64, ...]` | Variable-length fractional position |

### 10.3 IdListRemoveOp (`"il-r"`)

```json
{
  "t":  "il-r",
  "ts": [2, 8],
  "v":  {"1": 4, "2": 7},
  "to": [1, 3]
}
```

Additional field:

| Field | Key | Type | Description |
|-------|-----|------|-------------|
| Target origin | `"to"` | `[replica_id, seq]` | Lamport of entry to remove |

### 10.4 IdListUndoOpVariant (`"il-u"`)

```json
{
  "t":  "il-u",
  "ts": [2, 9],
  "v":  {"1": 4, "2": 8},
  "c":  [[[1, 3], 1], [[2, 7], 1]]
}
```

Additional field:

| Field | Key | Type | Description |
|-------|-----|------|-------------|
| Parity counts | `"c"` | `[[[r,s], parity], ...]` | Updated UndoMap entries |

---

## 11. Memory Cost

Per entry:

- `origin` (Lamport): 6 bytes
- `locator` (Locator, small-vector of uint64): 8–40 bytes typical
- `id` (uint64): 8 bytes
- `deletions` (std::vector of Lamport, usually empty): 24 bytes
  overhead + 6 bytes per deletion
- `visible` (bool, likely padded): 1 byte
- **Approximate total (0 deletions):** ~48–80 bytes per live entry

For comparison, a `Buffer` fragment with a single character is ~64–128
bytes when accounting for text storage and SumTree overhead. For
applications that need an ordered list of N document blocks (where N
is in the hundreds, not millions), `IdList`'s per-entry cost is
practical.

Tombstones (entries with `visible == false`) remain in the tree until
GC runs. Long-lived documents that accumulate many removes benefit from
`compact()` to reclaim memory.

---

## 12. Public API Summary

```cpp
class IdList {
public:
    explicit IdList(uint16_t replica_id);

    // Editing — returns op to broadcast
    IdListOperation insert_after(const Anchor& after, uint64_t id);
    IdListOperation remove_at(const Anchor& target);

    // Remote op application (any order, any delay, duplicates safe)
    void apply_ops(const std::vector<IdListOperation>& ops);

    // Undo/redo — returns op to broadcast; nullopt if stack empty/top
    std::optional<IdListOperation> undo();
    std::optional<IdListOperation> redo();
    bool coalesce_last_undo();
    size_t undo_depth() const;
    size_t max_undo_depth() const;
    void set_max_undo_depth(size_t depth);

    // List state
    std::vector<uint64_t> ids() const;   // visible elements in order
    uint32_t size() const;               // visible element count

    // Position tracking
    Anchor anchor_of(uint64_t id, Bias bias = Bias::Left) const;
    Anchor anchor_at_index(uint32_t index, Bias bias = Bias::Left) const;
    uint32_t resolve_anchor(const Anchor& a) const;
    int compare_anchors(const Anchor& a, const Anchor& b) const;

    // Version / identity
    const Global& version() const;
    uint16_t replica_id() const;

    // Garbage collection
    size_t collect_garbage();
    size_t compact(const Global& watermark);

    // Diagnostics
    size_t entry_count() const;
    size_t tombstone_count() const;
    std::vector<IdListEntry> entries() const;  // all entries including tombstones

    // Change notification
    void set_on_change(std::function<void()> cb);
};
```

---

## 13. What IdList Is Not

`IdList` is a deliberately narrow primitive. It does not support:

- **`moveAfter` / reordering** — Moves are expressed as `remove_at` +
  `insert_after`. Native move support is explicitly deferred; see
  `docs/specs/2026-05-04-d-evolution-response.md` §"What we won't do".
- **Per-element values** — Elements carry a single `uint64_t` id.
  There is no `setValue`, no attribute map, no embedded text.
- **Nested CRDTs** — IdList is not a container of Buffers. Composition
  (`IdList` of block IDs + one `Buffer` per block) is the application's
  responsibility.
- **Map, Counter, Register, JSON CRDT** — `IdList` is the only
  non-text primitive. There is no plan for general-purpose CRDT types.
  See the D-evolution response for the full rationale.

This narrowness is intentional. The goal is to cover the one case
`Buffer` cannot handle (ordering of atomic structural elements) without
turning CollabText into a general-purpose CRDT framework.
