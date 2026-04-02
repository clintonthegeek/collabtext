# CollabText CRDT Engine Specification

The internal data structures and algorithms for a text CRDT that supports
concurrent editing, undo/redo, and convergence across an arbitrary number
of replicas. This document specifies the engine itself — the thing that
`CRDT_SYNC_SPEC.md` transports operations for.

The design is derived from Zed's production CRDT implementation.

---

## 1. Conceptual Overview

The document is represented as an ordered sequence of **fragments**. Each
fragment is a contiguous chunk of text from a single insertion operation.
Fragments are never physically removed — deleted fragments are marked
invisible (tombstones). The visible document text is the concatenation of
all visible fragments in order.

Fragments are ordered by **Locators** — fractional position identifiers
that allow insertion between any two fragments without renumbering. When
two replicas concurrently insert at the same position, the Lamport
timestamp determines which insertion appears first (lower timestamp wins).

All state is maintained in a **SumTree** — a B+ tree where each node
caches aggregate summaries of its subtree. This enables O(log n) seeks
by byte offset, line number, or fragment ID.

```
Document = ordered list of Fragments
Fragment = { id: Locator, timestamp: Lamport, text_range, visible, deletions, ... }
Visible text = concatenation of fragment.text for all fragments where visible == true
```

---

## 2. Clock Types

### 2.1 Replica ID

```
ReplicaId = u16
```

Each editing session gets a unique replica ID. IDs are assigned by the
transport layer (see `CRDT_SYNC_SPEC.md` §3). The CRDT engine treats
them as opaque u16 values.

### 2.2 Sequence Number

```
Seq = u32
```

A monotonically increasing counter per replica.

### 2.3 Lamport Timestamp

```
Lamport = { value: Seq, replica_id: ReplicaId }
```

Uniquely identifies every operation across all replicas. Two operations
from different replicas may have the same `value` but never the same
`(value, replica_id)` pair.

**Operations:**

- **tick()**: Returns the current value, then increments. Used when
  creating a new operation:
  ```
  fn tick(&mut self) -> Lamport:
      result = Lamport { value: self.value, replica_id: self.replica_id }
      self.value += 1
      return result
  ```

- **observe(other)**: Updates the local clock to be strictly ahead of an
  observed timestamp. Called when receiving a remote operation:
  ```
  fn observe(&mut self, other: Lamport):
      self.value = max(self.value, other.value) + 1
  ```

**Ordering**: Lamport timestamps are totally ordered. Compare by `value`
first; if equal, break ties by `replica_id`. This ensures a deterministic
total order even for concurrent operations:

```
fn cmp(a: Lamport, b: Lamport) -> Ordering:
    if a.value != b.value:
        return a.value.cmp(b.value)
    return a.replica_id.cmp(b.replica_id)
```

**Sentinels:**
- `Lamport::MIN = { value: 0, replica_id: 0 }`
- `Lamport::MAX = { value: u32::MAX, replica_id: u16::MAX }`

### 2.4 Vector Clock (Global)

```
Global = { values: Vec<Seq> }
    // values[i] = highest sequence number observed from replica i
    // absent entries are implicitly 0
```

Tracks the causal state of a replica — which operations from which
replicas have been seen.

**Operations:**

- **observe(timestamp: Lamport)**: Record that we've seen this operation.
  ```
  fn observe(&mut self, ts: Lamport):
      if ts.value > 0:
          grow values to length ts.replica_id + 1 if needed
          values[ts.replica_id] = max(values[ts.replica_id], ts.value)
  ```

- **observed(timestamp: Lamport) -> bool**: Have we seen this operation?
  ```
  fn observed(&self, ts: Lamport) -> bool:
      return self.get(ts.replica_id) >= ts.value
  ```

- **observed_all(other: Global) -> bool**: Have we seen everything the
  other clock has seen? Used for causal readiness checks.
  ```
  fn observed_all(&self, other: &Global) -> bool:
      if self.values.len() < other.values.len():
          return false
      for i in 0..other.values.len():
          if self.values[i] < other.values[i]:
              return false
      return true
  ```

- **join(other: Global)**: Component-wise maximum. Merges another clock
  into this one.
  ```
  fn join(&mut self, other: &Global):
      grow values to max(self.len(), other.len())
      for i in 0..other.values.len():
          values[i] = max(values[i], other.values[i])
  ```

- **meet(other: Global)**: Component-wise minimum. Used for GC watermark
  computation.
  ```
  fn meet(&mut self, other: &Global):
      for i in 0..min(self.len(), other.len()):
          if self.values[i] > 0 and other.values[i] > 0:
              values[i] = min(values[i], other.values[i])
          // if either is 0, keep the non-zero value
  ```

---

## 3. The Locator: Fractional Position Identifiers

### 3.1 Definition

```
Locator = Vec<u64>    // typically length 1 or 2
```

A Locator is a variable-length sequence of unsigned 64-bit integers. It
defines a position in the fragment ordering. Locators are compared
lexicographically: compare the first element; if equal, compare the
second; and so on. Shorter sequences are implicitly padded with 0 on the
left side and u64::MAX on the right (depending on context in `between()`).

**Sentinels:**
- `Locator::min() = [0]`
- `Locator::max() = [u64::MAX]`

### 3.2 The between() Algorithm

Given two Locators `lhs` and `rhs` where `lhs < rhs`, produce a new
Locator `mid` such that `lhs < mid < rhs`.

```
fn between(lhs: Locator, rhs: Locator) -> Locator:
    // Pad both sequences to infinite length
    lhs_padded = lhs ++ repeat(0)          // pad with minimum
    rhs_padded = rhs ++ repeat(u64::MAX)   // pad with maximum

    result = []
    for (l, r) in zip(lhs_padded, rhs_padded):
        mid = l + ((r - l) >> 48)          // biased midpoint
        result.push(mid)
        if mid > l:
            break                          // found separation
    return result
```

**The >> 48 shift is the critical optimization.** Instead of computing
the true midpoint `(l + r) / 2`, this computes approximately
`l + (r - l) / 281474976710656`. This biases the result extremely close
to `l`, which has a counterintuitive benefit:

When typing sequentially (appending characters), each new Locator is
generated as `between(previous, Locator::max())`. The gap between
`previous` and `max` is enormous, so even dividing by 2^48 yields a
value comfortably greater than `previous`. This keeps the Locator at
depth 1 (a single u64) for over 100,000 sequential insertions.

When the gap at a given depth is too small (the shift produces
`mid == l`), the algorithm descends to the next depth level. This happens
only for adversarial insertion patterns (e.g., always inserting at the
exact midpoint of a shrinking range).

**Depth characteristics (empirically verified by Zed's tests):**
- Sequential typing (appending): depth 1, for 100,000+ insertions
- Typing after a cursor split (common editing): depth 2, for 10,000+
  insertions
- Worst case: depth grows logarithmically with the number of insertions
  in the same shrinking gap

### 3.3 Why Not Just Use Timestamps?

Timestamps provide a total order but not a *positional* order. If Alice
inserts "hello" at position 5 and Bob inserts "world" at position 10,
their timestamps tell us which happened first but not where the text
goes. Locators encode *where* — they sit between the Locators of the
neighboring fragments, regardless of when the insertion happened.

---

## 4. The Fragment

### 4.1 Definition

```
Fragment = {
    id:               Locator          // position in the ordering
    timestamp:        Lamport          // when this fragment was created
    insertion_offset: u32              // byte offset into the insertion's text
    len:              u32              // length of this fragment in bytes
    visible:          bool             // true if not deleted
    deletions:        Vec<Lamport>     // timestamps of operations that deleted this
    max_undos:        Global           // highest undo version seen
}
```

### 4.2 Fragment Lifecycle

1. **Created** by an Edit operation. `visible = true`, `deletions = []`.
   The fragment's `id` is generated by `Locator::between()` using the
   neighboring fragments' Locators.

2. **Split** when a subsequent edit's range cuts through it. The original
   fragment is replaced by two fragments: a prefix and a suffix, each
   with new Locators (generated by `between()` to maintain ordering).
   Both inherit the original's `timestamp` and `visible` flag.

3. **Deleted** when an edit's range covers it. `visible` is set to
   `false`. The deleting operation's timestamp is pushed onto
   `deletions`. The fragment remains in the tree (tombstone).

4. **Undone** when an undo operation targets its creation timestamp.
   Visibility is recomputed (§7).

### 4.3 Fragment Summary (for the SumTree)

```
FragmentSummary = {
    text: {
        visible: usize      // total visible bytes in subtree
        deleted: usize       // total deleted bytes in subtree
    }
    max_id:                Locator   // maximum fragment Locator in subtree
    max_version:           Global    // latest timestamp in subtree
    min_insertion_version: Global    // earliest insertion version in subtree
    max_insertion_version: Global    // latest insertion version in subtree
}
```

The summary enables O(log n) seeks:
- By visible byte offset (using `text.visible`)
- By full offset including deleted text (using `text.visible + text.deleted`)
- By fragment Locator (using `max_id`)

---

## 5. The SumTree

### 5.1 Structure

A B+ tree with branching factor `B` (Zed uses B=6 in production, B=2
in tests). Maximum items per node: `2B`.

```
Node = Internal {
    height:          u8
    summary:         Summary         // aggregate of entire subtree
    child_summaries: Vec<Summary>    // one per child
    child_trees:     Vec<SumTree>    // child subtrees
}
| Leaf {
    summary:         Summary
    items:           Vec<Item>       // actual data (fragments, etc.)
    item_summaries:  Vec<Summary>
}
```

Items are stored only in leaves. Internal nodes store only summaries and
child pointers.

### 5.2 Summary Trait

Every item stored in a SumTree has a Summary type that supports:

```
trait Summary:
    fn zero() -> Self                    // identity element
    fn add_summary(&mut self, other)     // monoid composition (associative)
```

This makes SumTree a **monoid tree** — the summary at any node is the
composition of all item summaries in its subtree. This is what enables
O(log n) seeks: you can binary-search by any dimension that can be
extracted from the summary.

### 5.3 Dimension Trait

A Dimension extracts a specific measurement from a Summary:

```
trait Dimension<Summary>:
    fn zero() -> Self
    fn add_summary(&mut self, summary)
```

Multiple Dimensions can extract different measurements from the same
Summary. For the fragment tree:

| Dimension | What it measures | Used for |
|-----------|-----------------|----------|
| `usize` | Visible byte count | Seeking by document offset |
| `FullOffset` | Visible + deleted byte count | Version-aware offset resolution |
| `Locator` | Maximum fragment ID | Seeking by fragment position |
| `VersionedFullOffset` | Version-filtered full offset | Resolving offsets from a specific version |

### 5.4 Cursor

A Cursor maintains a position in the tree as a stack of
`(node, child_index, cumulative_position)` entries:

```
Cursor = {
    tree:     &SumTree
    stack:    Vec<StackEntry>     // path from root to current leaf
    position: Dimension          // cumulative dimension at current position
}

StackEntry = {
    tree:     &SumTree
    index:    u32                // current child index
    position: Dimension          // cumulative at this level
}
```

**Key operations:**

- **seek(target, bias)**: Navigate to the item at `target` position.
  Descends from root, comparing cumulative summaries at each level.
  O(log n).

- **slice(target, bias) -> SumTree**: Extract all items from the current
  position to `target` as a new SumTree. Moves whole subtrees by
  reference (Arc clone) rather than copying items. O(log n).

- **suffix() -> SumTree**: Extract all items from current position to the
  end. Special case of slice.

- **next() / prev()**: Move to adjacent item. O(log n) amortized O(1).

### 5.5 The slice() Operation

This is the most important SumTree operation for the CRDT. When
rebuilding the fragment tree after an edit, the algorithm uses
cursor.slice() to extract large unchanged regions in O(log n) time:

```
fn slice(target) -> SumTree:
    result = new SumTree
    while position < target:
        if current node is fully before target:
            move entire subtree to result (Arc clone, O(1))
        else:
            descend into partially-covered child
    return result
```

This means processing an edit that touches a small region of a large
document is O(log n) regardless of document size.

---

## 6. Text Storage: The Rope

### 6.1 Structure

```
Rope = SumTree<Chunk>

Chunk = {
    text:       String       // up to 128 bytes (production) or 16 bytes (test)
    chars:      Bitmap       // bit[i] set if byte i starts a UTF-8 character
    newlines:   Bitmap       // bit[i] set if byte i is '\n'
    tabs:       Bitmap       // bit[i] set if byte i is '\t'
}
```

The Rope is a separate SumTree from the fragment tree. It stores the
actual character data. The fragment tree stores *metadata* about which
parts of which insertions are visible.

### 6.2 Two Ropes

The buffer maintains two ropes:

```
BufferSnapshot = {
    visible_text: Rope       // concatenation of visible fragment text
    deleted_text: Rope       // concatenation of deleted fragment text
    fragments:    SumTree<Fragment>
    ...
}
```

Both ropes are kept in sync with the fragment tree. When a fragment
changes visibility (deleted or undone), its text moves between the two
ropes. This allows efficient access to both the current document text
and the deleted text (needed for undo).

### 6.3 RopeBuilder

When the fragment tree is modified, the ropes must be rebuilt. The
RopeBuilder walks the old ropes and the new fragment tree simultaneously,
constructing new ropes:

```
RopeBuilder = {
    old_visible_cursor: Rope::Cursor    // position in old visible rope
    old_deleted_cursor: Rope::Cursor    // position in old deleted rope
    new_visible:        Rope            // being built
    new_deleted:        Rope            // being built
}

fn push_fragment(fragment, was_visible):
    // Extract text from the appropriate old rope
    if was_visible:
        text = old_visible_cursor.slice(len)
    else:
        text = old_deleted_cursor.slice(len)

    // Place in the appropriate new rope
    if fragment.visible:
        new_visible.append(text)
    else:
        new_deleted.append(text)
```

For new text (not in any old rope), the builder appends it directly:

```
fn push_str(text: &str):
    new_visible.push(text)
```

---

## 7. The Undo Map

### 7.1 Structure

```
UndoMap = SumTree<UndoMapEntry>

UndoMapEntry = {
    key: {
        edit_id: Lamport     // the operation being undone
        undo_id: Lamport     // the undo operation itself
    }
    undo_count: u32          // cumulative undo count
}
```

The undo map is a SumTree keyed by `(edit_id, undo_id)`. It tracks how
many times each edit has been undone.

### 7.2 Undo Count Parity

Visibility is determined by parity:
- **Even count (0, 2, 4, ...)**: The edit is **visible** (created or
  re-done).
- **Odd count (1, 3, 5, ...)**: The edit is **undone** (invisible).

This elegantly handles undo, redo, undo-of-redo, and concurrent
undo/redo across replicas.

### 7.3 Key Operations

- **undo_count(edit_id) -> u32**: Returns the highest undo_count across
  all undo operations targeting this edit_id.
  ```
  fn undo_count(edit_id: Lamport) -> u32:
      cursor.seek((edit_id, Lamport::MIN))
      max_count = 0
      while entry.edit_id == edit_id:
          max_count = max(max_count, entry.undo_count)
          cursor.next()
      return max_count
  ```

- **is_undone(edit_id) -> bool**: Is this edit currently undone?
  ```
  fn is_undone(edit_id) -> bool:
      return undo_count(edit_id) % 2 == 1
  ```

- **was_undone(edit_id, version) -> bool**: Was this edit undone at a
  specific version? Only counts undo operations that were observed in
  `version`.
  ```
  fn was_undone(edit_id, version: &Global) -> bool:
      cursor.seek((edit_id, Lamport::MIN))
      max_count = 0
      while entry.edit_id == edit_id:
          if version.observed(entry.undo_id):
              max_count = max(max_count, entry.undo_count)
          cursor.next()
      return max_count % 2 == 1
  ```

### 7.4 Fragment Visibility

Two methods determine whether a fragment is visible:

**Current visibility** (for rendering):
```
fn is_visible(fragment, undos: &UndoMap) -> bool:
    // The insertion must not be undone
    if undos.is_undone(fragment.timestamp):
        return false
    // All deletions must be undone (or no deletions exist)
    for deletion in fragment.deletions:
        if not undos.is_undone(deletion):
            return false
    return true
```

**Historical visibility** (for applying remote edits):
```
fn was_visible(fragment, version: &Global, undos: &UndoMap) -> bool:
    // The insertion must have existed and not been undone at that version
    if not version.observed(fragment.timestamp):
        return false
    if undos.was_undone(fragment.timestamp, version):
        return false
    // All deletions must either not exist yet or be undone at that version
    for deletion in fragment.deletions:
        if version.observed(deletion) and not undos.was_undone(deletion, version):
            return false
    return true
```

The `was_visible` method is critical for `apply_remote_edit` — it
determines which fragments were visible to the remote editor at the time
they made their edit, so that only the correct fragments are deleted.

---

## 8. Operations

### 8.1 Edit Operation

```
EditOperation = {
    timestamp: Lamport           // unique ID for this edit
    version:   Global            // causal dependencies
    ranges:    Vec<Range<FullOffset>>  // byte ranges deleted (in full-text space)
    new_text:  Vec<String>       // text inserted at each range (one per range)
}
```

An edit can delete text, insert text, or both (replace). Each range in
`ranges` is a byte range in the **full offset space** (visible + deleted
text). The corresponding entry in `new_text` is inserted at the start of
the range after deletion.

Empty ranges (start == end) represent pure insertions. Empty strings in
`new_text` represent pure deletions.

### 8.2 Undo Operation

```
UndoOperation = {
    timestamp: Lamport
    version:   Global
    counts:    Map<Lamport, u32>   // edit_id -> new undo count
}
```

An undo operation specifies, for each targeted edit, what its new undo
count should be. Odd count = undo; even count = redo.

### 8.3 Transactions

Multiple edits can be grouped into a transaction for undo purposes:

```
Transaction = {
    id:       Lamport           // transaction identifier
    edit_ids: Vec<Lamport>      // all edits in this transaction
    start:    Global            // version at transaction start
}
```

When the user presses Ctrl+Z, the entire transaction is undone as a
unit: one UndoOperation is created with counts for all edit_ids in the
transaction.

Transactions within a configurable `group_interval` (default 300ms) are
merged: if the user types "hello" as five keystrokes within 300ms, a
single undo reverts all five.

---

## 9. Applying a Local Edit

When the local user types, deletes, or pastes:

```
fn apply_local_edit(ranges: Vec<Range<usize>>, new_text: Vec<String>):
    // 1. Create a new Lamport timestamp for this edit
    timestamp = lamport_clock.tick()

    // 2. Walk the fragment tree by VISIBLE byte offset
    cursor = fragments.cursor::<usize>()

    // 3. For each edit range (in visible-offset space):
    for (range, text) in zip(ranges, new_text):
        // Slice unchanged prefix
        new_fragments.append(cursor.slice(range.start))

        // Convert visible offset to full offset for the operation
        full_start = range.start + cursor.start().deleted
        full_end   = range.end   + cursor.end().deleted

        // Delete fragments in range (mark invisible, record deletion)
        while cursor within range:
            fragment = cursor.item()
            if fragment extends past range:
                split fragment (§9.1)
            fragment.visible = false
            fragment.deletions.push(timestamp)
            new_fragments.push(fragment)
            cursor.next()

        // Insert new text as new fragments
        push_fragments_for_insertion(text, timestamp, &next_locator)

    // 4. Append remaining unchanged suffix
    new_fragments.append(cursor.suffix())

    // 5. Rebuild ropes using RopeBuilder
    rebuild_ropes(old_fragments, new_fragments)

    // 6. Create and return the EditOperation
    return EditOperation {
        timestamp, version: current_version.clone(),
        ranges: [FullOffset(full_start)..FullOffset(full_end), ...],
        new_text: [text, ...]
    }
```

### 9.1 Fragment Splitting

When an edit range starts or ends in the middle of a fragment, the
fragment must be split:

```
fn split_fragment(fragment, split_offset) -> (prefix, suffix):
    prefix = Fragment {
        id:               Locator::between(max_id_so_far, fragment.id)
        timestamp:        fragment.timestamp
        insertion_offset: fragment.insertion_offset
        len:              split_offset
        visible:          fragment.visible
        deletions:        fragment.deletions.clone()
        max_undos:        fragment.max_undos.clone()
    }

    suffix = Fragment {
        id:               fragment.id      // keeps original ID
        timestamp:        fragment.timestamp
        insertion_offset: fragment.insertion_offset + split_offset
        len:              fragment.len - split_offset
        visible:          fragment.visible
        deletions:        fragment.deletions.clone()
        max_undos:        fragment.max_undos.clone()
    }

    return (prefix, suffix)
```

The prefix gets a new Locator (between the running maximum and the
original's Locator). The suffix keeps the original Locator. This
maintains the invariant that Locators are strictly increasing in the
fragment tree.

### 9.2 Inserting New Text

New text is split into fragments of bounded size and assigned Locators:

```
fn push_fragments_for_insertion(text, timestamp, next_fragment_id):
    offset = 0
    while offset < text.len():
        chunk_len = min(MAX_CHUNK_SIZE, text.len() - offset)
        // respect UTF-8 boundaries
        chunk_len = adjust_to_char_boundary(text, offset + chunk_len)

        fragment = Fragment {
            id:               Locator::between(max_id_so_far, next_fragment_id)
            timestamp:        timestamp
            insertion_offset: offset
            len:              chunk_len
            visible:          true
            deletions:        []
            max_undos:        Global::new()
        }
        new_fragments.push(fragment)
        rope_builder.push_str(text[offset..offset+chunk_len])
        offset += chunk_len
```

The `next_fragment_id` is the Locator of the first existing fragment
after the insertion point. This ensures new text is positioned correctly
in the ordering.

---

## 10. Applying a Remote Edit

This is the core CRDT algorithm. It is more complex than local editing
because byte offsets must be resolved against the remote editor's version
of the document, which may differ from the current local version.

### 10.1 VersionedFullOffset

The key insight is that byte offsets in the remote edit's `ranges` are
in the full-text space (visible + deleted) **as seen by the remote
editor's version**. Fragments inserted by replicas that the remote
editor hadn't seen at edit time do not count toward those offsets.

To resolve these offsets in the current fragment tree, we use a special
dimension:

```
VersionedFullOffset:
    fn add_summary(summary, version):
        if version.observed_all(summary.max_insertion_version):
            // All fragments in this subtree existed in the remote version
            self += summary.text.visible + summary.text.deleted
        else if version.observed_any(summary.min_insertion_version):
            // Mixed: some fragments are from after the remote version
            self = INVALID  // cannot resolve, must descend
        // else: no fragments from the remote version, skip entirely
```

When `INVALID`, the cursor descends into the subtree to resolve
individual fragments rather than skipping the whole subtree.

### 10.2 The Algorithm

```
fn apply_remote_edit(version, ranges, new_text, timestamp):
    cx = Some(version)  // context for versioned offset resolution

    // Walk the fragment tree with version-aware offsets
    cursor = fragments.cursor::<(VersionedFullOffset, usize)>(cx)

    for (range, text) in zip(ranges, new_text):
        // 1. SLICE UNCHANGED PREFIX
        //    Everything before range.start in the remote version
        new_fragments.append(cursor.slice(range.start))

        // 2. SKIP CONCURRENT HIGHER-TIMESTAMP INSERTIONS
        //    If a fragment starts at exactly range.start but has a
        //    HIGHER Lamport timestamp than our edit, it was inserted
        //    concurrently. It should appear AFTER our insertion
        //    (lower timestamp wins). So we move it before our
        //    insertion point unchanged.
        while cursor.item().start == range.start
              AND cursor.item().timestamp > timestamp:
            new_fragments.push(cursor.item())
            cursor.next()

        // 3. SPLIT IF RANGE STARTS MID-FRAGMENT
        if cursor.position() < range.start:
            (prefix, _) = split_fragment(cursor.item(), split_point)
            new_fragments.push(prefix)

        // 4. INSERT NEW TEXT
        next_id = cursor.item().id  // or Locator::max() if at end
        push_fragments_for_insertion(text, timestamp, next_id)

        // 5. PROCESS DELETIONS IN RANGE
        while cursor.position() < range.end:
            fragment = cursor.item()
            intersection = fragment.clone()

            // Only delete if the fragment was visible to the remote editor
            if version.observed(fragment.timestamp):
                // Trim to the intersection with range
                intersection.len = min(range.end, fragment_end) - fragment_start
                intersection.insertion_offset += adjustment

                if fragment.was_visible(version, undo_map):
                    intersection.deletions.push(timestamp)
                    intersection.visible = false

            // Assign new Locator and push
            intersection.id = Locator::between(max_id_so_far, intersection.id)
            new_fragments.push(intersection)

            if fragment_end <= range.end:
                cursor.next()
            else:
                break  // partial overlap, suffix handled below

        // 6. SPLIT IF RANGE ENDS MID-FRAGMENT
        // (suffix keeps original Locator)

    // 7. APPEND REMAINING SUFFIX
    new_fragments.append(cursor.suffix())

    // 8. REBUILD ROPES
    rebuild_ropes(old_fragments, new_fragments)

    // 9. UPDATE VERSION
    version.observe(timestamp)
    lamport_clock.observe(timestamp)
```

### 10.3 The Concurrent Insertion Ordering Rule

Step 2 is where the CRDT magic happens. When two replicas insert text at
the same position concurrently:

```
Replica A inserts "hello" at offset 5 with timestamp (42, A)
Replica B inserts "world" at offset 5 with timestamp (42, B)
```

When A processes B's edit, it encounters B's insertion at the same offset
as its own. The rule is:

**Lower Lamport timestamp comes first.**

If A's timestamp < B's timestamp: A's text appears first ("helloworld").
If B's timestamp < A's timestamp: B's text appears first ("worldhello").

Since Lamport timestamps are totally ordered (ties broken by replica_id),
this produces the same result on both replicas.

In the code, this manifests as step 2: when processing a remote edit at
position X, skip over any existing fragments at position X that have a
*higher* timestamp. Those fragments will appear *after* our insertion,
which is correct because our insertion has a lower timestamp.

### 10.4 The version.observed() Check

Step 5 only deletes fragments that `version.observed(fragment.timestamp)`
— fragments that existed in the remote editor's view of the document. If
a fragment was inserted by a third replica that the remote editor hadn't
seen yet, it must not be deleted, even if it falls within the byte range.

This is what makes the CRDT correct under concurrent edits: an edit can
only affect the text that the editor could see when they made the edit.

---

## 11. Applying an Undo

```
fn apply_undo(undo: UndoOperation):
    // 1. Record in undo map
    for (edit_id, count) in undo.counts:
        undo_map.insert(UndoMapEntry {
            key: { edit_id, undo_id: undo.timestamp },
            undo_count: count
        })

    // 2. Find all fragments affected by the undone edits
    affected_fragment_ids = find_fragments_for_edits(undo.counts.keys())

    // 3. Walk fragment tree, recompute visibility
    cursor = fragments.cursor()
    for fragment_id in affected_fragment_ids:
        cursor.seek(fragment_id)
        fragment = cursor.item().clone()
        old_visible = fragment.visible

        // Recompute visibility using updated undo map
        fragment.visible = is_visible(fragment, undo_map)
        fragment.max_undos.observe(undo.timestamp)

        // Track visibility changes for rope rebuilding
        if old_visible and not fragment.visible:
            // Text disappeared — move from visible rope to deleted rope
        else if not old_visible and fragment.visible:
            // Text reappeared — move from deleted rope to visible rope

        new_fragments.push(fragment)

    // 4. Rebuild ropes
    rebuild_ropes(old_fragments, new_fragments)

    // 5. Update version
    version.observe(undo.timestamp)
    lamport_clock.observe(undo.timestamp)
```

### 11.1 Concurrent Undo Example

Alice and Bob both undo the same edit E:

1. Alice creates UndoOp_A: `counts = { E: 1 }`
2. Bob creates UndoOp_B: `counts = { E: 1 }`
3. Both propagate.

After both are applied:
- UndoMap has entries: `(E, A, 1)` and `(E, B, 1)`
- `undo_count(E) = max(1, 1) = 1` (odd = undone)
- Fragment is invisible. Correct.

Alice then redoes:
4. Alice creates UndoOp_A2: `counts = { E: 2 }`
5. UndoMap now has: `(E, A, 1)`, `(E, A2, 2)`, `(E, B, 1)`
6. `undo_count(E) = max(1, 2, 1) = 2` (even = visible)
7. Fragment is visible again. Alice's redo "wins" over Bob's undo.

This is correct: the redo is a newer operation that supersedes both undos.

---

## 12. Anchors

### 12.1 Definition

```
Anchor = {
    timestamp:  Lamport      // the insertion operation that created nearby text
    offset:     u32          // byte offset within that insertion
    bias:       Bias         // Left or Right
    buffer_id:  BufferId     // which buffer (for multi-buffer scenarios)
}

Bias = Left | Right
```

An Anchor is a stable reference to a position in the document. Unlike
byte offsets, Anchors do not change when text is inserted or deleted
elsewhere in the document.

### 12.2 Creating an Anchor

To create an Anchor at a visible byte offset:

1. Walk the fragment tree to find the fragment containing the offset.
2. Record the fragment's `timestamp` and the byte offset within the
   fragment (`offset = byte_offset - fragment_start + fragment.insertion_offset`).
3. Set `bias` based on the desired behavior:
   - `Left`: The anchor sticks to the character on its left. If text is
     inserted exactly at the anchor's position, the anchor stays before
     the new text.
   - `Right`: The anchor sticks to the character on its right. If text is
     inserted exactly at the anchor's position, the anchor moves after
     the new text.

### 12.3 Resolving an Anchor to a Byte Offset

To convert an Anchor back to a visible byte offset:

1. Look up the Anchor's `timestamp` in the **insertion index**
   (a SumTree of InsertionFragments, keyed by `(timestamp, offset)`).
   This yields the `fragment_id` (Locator) of the fragment containing
   the anchored position.

2. Seek to `fragment_id` in the fragment tree. The cursor's accumulated
   visible byte count gives the fragment's start offset.

3. If the fragment is visible, add the within-fragment offset:
   `result = fragment_start + (anchor.offset - insertion.split_offset)`

4. If the fragment is invisible (deleted), the anchor resolves to the
   position where the fragment *was*. The bias determines whether
   it resolves to the left edge or right edge.

**Sentinels:**
- `Anchor::min()`: Resolves to offset 0 (document start).
- `Anchor::max()`: Resolves to the document length (document end).

### 12.4 Anchor Comparison

Two anchors are compared by resolving their fragment IDs and comparing
positions:

```
fn cmp(a: Anchor, b: Anchor, buffer: &BufferSnapshot) -> Ordering:
    if a.timestamp == b.timestamp:
        return a.offset.cmp(b.offset).then(a.bias.cmp(b.bias))
    frag_a = buffer.fragment_id_for_anchor(a)
    frag_b = buffer.fragment_id_for_anchor(b)
    return frag_a.cmp(frag_b)
        .then(a.offset.cmp(b.offset))
        .then(a.bias.cmp(b.bias))
```

### 12.5 The Insertion Index

```
InsertionFragment = {
    timestamp:    Lamport     // insertion operation timestamp
    split_offset: u32         // byte offset where this fragment starts within the insertion
    fragment_id:  Locator     // which fragment in the fragment tree
}
```

This is a SumTree keyed by `(timestamp, split_offset)`. It maps from
an Anchor's `(timestamp, offset)` to the fragment's Locator, enabling
O(log n) anchor resolution.

When fragments are split, new InsertionFragment entries are created to
track the split points.

---

## 13. Operation Queue and Causal Ordering

### 13.1 The Queue

```
OperationQueue = SumTree<Operation>   // ordered by Lamport timestamp
```

Operations whose causal dependencies are not yet satisfied are held in
the queue.

### 13.2 Causal Readiness

An operation is ready to apply when:

```
fn can_apply(op: Operation, local_version: Global) -> bool:
    // The operation's version must be fully observed
    return local_version.observed_all(op.version)
```

If the local version vector does not include all timestamps in the
operation's version vector, the operation depends on edits we haven't
seen yet. It must wait.

### 13.3 Deferred Replicas

Once a replica has a deferred operation, **all subsequent operations from
that replica are also deferred**, even if their individual dependencies
are satisfied. This preserves per-replica causal ordering.

```
fn apply_ops(ops):
    deferred = []
    for op in ops:
        if op.replica_id in deferred_replicas:
            deferred.push(op)
        else if can_apply(op, version):
            apply_op(op)
        else:
            deferred_replicas.add(op.replica_id)
            deferred.push(op)

    queue.insert(deferred)
    flush_deferred()

fn flush_deferred():
    deferred_replicas.clear()
    remaining = []
    for op in queue.drain():
        if can_apply(op, version):
            apply_op(op)
        else:
            deferred_replicas.add(op.replica_id)
            remaining.push(op)
    queue.insert(remaining)
```

After every successful `apply_op`, `flush_deferred` is called to retry
queued operations. Applying one operation may satisfy another's
dependencies.

---

## 14. Buffer Snapshot and State

### 14.1 The Snapshot

```
BufferSnapshot = {
    visible_text: Rope                    // current document text
    deleted_text: Rope                    // deleted text (for undo)
    fragments:    SumTree<Fragment>       // the fragment tree
    insertions:   SumTree<InsertionFragment>  // insertion index (for anchors)
    undo_map:     UndoMap                 // undo/redo state
    version:      Global                  // current vector clock
    replica_id:   ReplicaId              // this buffer's replica
}
```

### 14.2 Initialization

For a new empty document:
```
fragments = [Fragment { id: Locator::min(), len: 0, visible: false, ... }]
visible_text = Rope::new()
deleted_text = Rope::new()
version = Global::new()
```

For a document with initial text:
```
// Split text into chunks, create fragments
for each chunk:
    fragment = Fragment {
        id: Locator::between(previous_locator, Locator::max())
        timestamp: initial_timestamp
        insertion_offset: chunk_start
        len: chunk_len
        visible: true
        deletions: []
    }
    fragments.push(fragment)
visible_text = Rope::from(initial_text)
version.observe(initial_timestamp)
```

### 14.3 Serialization (for Snapshots)

The buffer state can be serialized for the snapshot mechanism described
in `CRDT_SYNC_SPEC.md` §9. The snapshot must include:

- All fragments (with their Locators, timestamps, visibility, deletions)
- The undo map
- The current version vector
- The visible and deleted text content (or enough information to
  reconstruct it from fragments)

A new replica loading a snapshot reconstructs the BufferSnapshot directly,
then applies any operations newer than the snapshot's version.

---

## 15. Correctness Invariants

These invariants must hold at all times. They are the definition of
"correct" for the CRDT engine.

### 15.1 Convergence

If two replicas have observed the same set of operations (their version
vectors are equal), their `visible_text` is identical, byte for byte.

This follows from:
- Fragments are ordered by Locator (deterministic).
- Concurrent insertions at the same position are ordered by Lamport
  timestamp (deterministic, total order).
- Fragment visibility is a deterministic function of the operation set
  (via the undo map and deletion lists).

### 15.2 Fragment Tree Ordering

Fragment Locators are strictly increasing:
```
for all adjacent fragments (f1, f2):
    f1.id < f2.id
```

### 15.3 Rope Consistency

```
sum(fragment.len for fragment where fragment.visible) == visible_text.len()
sum(fragment.len for fragment where not fragment.visible) == deleted_text.len()
```

### 15.4 Insertion Index Consistency

Every fragment has a corresponding InsertionFragment entry that maps its
`(timestamp, insertion_offset)` to its `id` (Locator).

### 15.5 Causal Safety

An operation is never applied before all of its dependencies:
```
for all applied operations op:
    version.observed_all(op.version) was true at apply time
```

### 15.6 Idempotence

Applying the same operation twice has no effect. The check
`if version.observed(op.timestamp): skip` prevents double application.

### 15.7 Commutativity

The result is independent of the order in which operations are applied
(given that causal dependencies are satisfied). This is the fundamental
CRDT property.

---

## 16. Testing Strategy

### 16.1 Randomized Convergence Tests

The primary correctness test:

1. Create N replicas (default 5) with identical initial text.
2. For M iterations (default 10):
   - With 50% probability: generate a random edit on a random replica.
   - With 20% probability: generate 1-5 random undo/redo operations.
   - With 30% probability: deliver pending messages (with random
     reordering and duplication).
3. Drain all pending messages (deliver everything).
4. Assert: all replicas have identical `visible_text`.
5. Assert: all replicas pass `check_invariants()`.

### 16.2 Network Simulation

The test network simulates:
- **Out-of-order delivery**: Messages are inserted at random positions
  in each recipient's inbox.
- **Duplicate delivery**: Each message is inserted 1-3 times randomly.
- **Partial delivery**: Recipients receive a random subset of pending
  messages on each receive call.
- **Disconnection**: Peers can be disconnected, causing messages to be
  dropped.

### 16.3 Specific Edge Cases

- Concurrent inserts at the same position (deterministic ordering)
- Partially overlapping deletes with concurrent inserts
- Undo of a concurrently modified edit
- Multi-byte UTF-8 characters at fragment boundaries
- Large insertions that trigger fragment splitting
- Empty documents receiving concurrent edits

---

## 17. Implementation Guidance

### 17.1 Suggested Data Structures in C++

| Zed (Rust) | C++ Equivalent |
|------------|---------------|
| `SumTree<T>` | Custom B+ tree with summary aggregation |
| `SmallVec<[T; N]>` | `QVarLengthArray<T, N>` or `boost::container::small_vector` |
| `Arc<str>` | `std::shared_ptr<std::string>` or `QString` |
| `Rope` | Could reuse the SumTree-based approach, or use an existing rope library |
| `HashMap` | `std::unordered_map` or `QHash` |

### 17.2 The SumTree is the Foundation

Nearly every data structure in the CRDT is a SumTree: the fragment tree,
the insertion index, the undo map, the rope, the operation queue. A
correct and efficient SumTree implementation is the single most important
piece. Get it right first, with thorough tests, before building the CRDT
on top of it.

### 17.3 Start with Tests

Implement the randomized convergence test (§16.1) first, using an
in-memory network simulation (§16.2). Run it with 2 replicas and simple
edits. Once that passes, scale to 5 replicas with undo/redo.

The test is the specification. If the test passes, the CRDT is correct.
