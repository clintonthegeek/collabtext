# SumTree Optimization Spec

Five optimizations that exploit the SumTree infrastructure added in commit
`08394cc`. Each is self-contained: any one can be implemented independently.
They are listed in recommended implementation order.

**Prerequisites**: Familiarity with `CRDT_ENGINE_SPEC.md`, the SumTree
cursor API, and the current Buffer implementation.

**Build & test**: All code is in `libs/collabtext/`. Build with
`cmake --build build-dev`. Run tests with `ctest --output-on-failure` from
`build-dev/`, or run individual executables like
`./libs/collabtext/tst_buffer`. Tests use Qt Test (`QTest`). Add new test
executables in `libs/collabtext/CMakeLists.txt` using the
`add_crdt_test(name)` macro, which creates an executable from
`tests/{name}.cpp`, links it to `collabtext` and `Qt6::Test`, and registers
it with CTest.

---

## Optimization 1: Operation Queue with Deferred Replica Tracking

**Goal**: Replace `std::vector<Operation> m_deferred` with a
`SumTree<OperationEntry>` ordered by Lamport timestamp, and add deferred
replica tracking per spec §13.3 to skip replicas whose causal dependencies
are not yet met.

**Current state**: `Buffer.h:167` declares `std::vector<Operation> m_deferred`.
`retry_deferred()` (`Buffer.cpp:735-757`) loops through the vector,
attempting each operation. If applied, it erases from the vector (O(n) shift).
The outer loop repeats until no progress is made. Worst case: O(n^2) total
work when operations arrive reverse-causally-ordered.

### 1.1 Define OperationEntry and Summary

Create `src/crdt/OperationQueue.h`:

```cpp
struct OperationSummary {
    Lamport max_timestamp = Lamport::min();

    static OperationSummary zero() { return {}; }
    void add_summary(const OperationSummary& other) {
        if (other.max_timestamp > max_timestamp)
            max_timestamp = other.max_timestamp;
    }
};

struct TimestampDim {
    Lamport value = Lamport::min();

    static TimestampDim zero() { return {Lamport::min()}; }
    void add_summary(const OperationSummary& s) { value = s.max_timestamp; }

    auto operator<=>(const TimestampDim&) const = default;
    bool operator==(const TimestampDim&) const = default;
};

struct OperationEntry {
    using Summary = OperationSummary;

    Lamport timestamp;
    Operation op;

    OperationSummary summary() const { return {timestamp}; }
};

static constexpr std::size_t OP_QUEUE_B = 2;
using OperationQueue = SumTree<OperationEntry, OP_QUEUE_B>;
```

A helper to extract the timestamp from an `Operation` variant is needed:

```cpp
inline Lamport get_op_timestamp(const Operation& op) {
    return std::visit([](const auto& o) { return o.timestamp; }, op);
}
```

### 1.2 Replace m_deferred

In `Buffer.h`, replace:
```cpp
std::vector<Operation> m_deferred;
```
with:
```cpp
OperationQueue m_deferred_queue;
std::set<uint16_t> m_deferred_replicas;
```

### 1.3 Refactor apply_ops

In `Buffer.cpp`, change `apply_ops()`:

```cpp
void Buffer::apply_ops(const std::vector<Operation>& ops) {
    for (auto& op : ops) {
        Lamport ts = get_op_timestamp(op);
        uint16_t replica = ts.replica_id;

        if (m_deferred_replicas.count(replica)) {
            // This replica already has a deferred op — defer this one too
            m_deferred_queue.push_item({ts, op});
            continue;
        }

        bool applied = try_apply(op);
        if (!applied) {
            m_deferred_replicas.insert(replica);
            m_deferred_queue.push_item({ts, op});
        }
    }
    retry_deferred();
}
```

Where `try_apply` wraps the existing `std::visit` apply logic.

### 1.4 Refactor retry_deferred

```cpp
void Buffer::retry_deferred() {
    bool progress = true;
    while (progress) {
        progress = false;
        m_deferred_replicas.clear();

        OperationQueue remaining;
        m_deferred_queue.for_each([&](const OperationEntry& entry) {
            if (m_deferred_replicas.count(entry.timestamp.replica_id)) {
                remaining.push_item(entry);
                return;
            }
            bool applied = try_apply(entry.op);
            if (applied) {
                progress = true;
            } else {
                m_deferred_replicas.insert(entry.timestamp.replica_id);
                remaining.push_item(entry);
            }
        });
        m_deferred_queue = std::move(remaining);
    }
}
```

### 1.5 Tests

Add `tests/tst_opqueue.cpp` with the `add_crdt_test(tst_opqueue)` macro.

| Test | Description |
|------|-------------|
| `empty_queue_noop` | `apply_ops({})` on fresh buffer does nothing. |
| `single_deferred_retries` | Send op2 (depends on op1) first, then op1. After both `apply_ops` calls, text matches. |
| `deferred_replica_tracking` | Send ops from replica A in reverse order. Verify only one scan needed after all arrive. |
| `mixed_replicas_partial_delivery` | 3 replicas, some ops deferred, some immediate. Verify correct result. |
| `stress_reverse_causal_order` | 100 ops from one replica sent in reverse. Verify O(n) total retries (not O(n^2)). Time the test to detect quadratic regression. |
| `convergence_still_passes` | All existing `tst_convergence` tests must still pass (run these, do NOT duplicate them). |

**Acceptance**: All 6 tests above pass. All tests in `tst_buffer` and
`tst_convergence` still pass. No new files beyond `OperationQueue.h` and
`tst_opqueue.cpp`.

---

## Optimization 2: Cursor-Based apply_local_edit

**Goal**: Replace the `get_fragments()` / vector mutation / `set_fragments()`
pattern in `apply_local_edit()` with direct SumTree cursor operations.
Brings local edit complexity from O(n) to O(log^2 n) for small edits.

**Current state**: `apply_local_edit()` (`Buffer.cpp:399-557`) calls
`get_fragments()` (line 405), operates on the vector copy with
`resolve_visible_offset`, `split_fragment_at`, `insert_fragment`,
`locator_between`, and `normalize_fragments`, then calls `set_fragments()`
(line 555).

### 2.1 Processing Strategy

Process ranges **left-to-right** using a single cursor on the old tree.
The cursor reads from the immutable old tree while building a new tree
incrementally.

```
cursor = old_tree.cursor<VisibleOffset>()
cursor.seek({0}, Bias::Left)

new_tree = empty

for each range (sorted ascending by start):
    // 1. Unchanged prefix
    new_tree.push_tree(cursor.slice({range.start}))

    // 2. Handle start-of-range split
    if cursor.position().value < range.start:
        split the current fragment at (range.start - cursor.position().value)
        push the first half to new_tree (unchanged)
        // cursor is conceptually past the first half now

    // 3. Insert new text (if any)
    create new fragment with locator, origin, content
    set frag.visible = true
    new_tree.push_item(new_frag)

    // 4. Delete phase
    delete_remaining = range.end - range.start
    while delete_remaining > 0 and cursor.item():
        frag = copy of *cursor.item()
        if !frag.visible: new_tree.push_item(frag); cursor.next(); continue

        frag_bytes = frag.content.size()
        if frag_bytes <= delete_remaining:
            frag.delete_count++; frag.visible = false
            record deleted timestamps
            new_tree.push_item(frag)
            delete_remaining -= frag_bytes
            cursor.next()
        else:
            split: push deleted prefix, keep rest for next iteration
            delete_remaining = 0

    // 5. Handle end-of-range split (cursor may be mid-fragment)

// 6. Suffix
new_tree.push_tree(cursor.suffix())
m_fragment_tree = new_tree
rebuild_insertion_index(...)
```

### 2.2 Left-to-Right Range Processing

The current code processes ranges right-to-left because vector byte offsets
shift after mutations. With cursor-based processing, offsets are in the
OLD tree's coordinate space and never change. Process left-to-right.

Sort the range indices ascending:
```cpp
std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return ranges[a].first < ranges[b].first;  // ascending, not descending
});
```

### 2.3 Fragment Splitting Without Vectors

The current `split_fragment_at()` works on a vector (inserts the second
half via `insert_fragment`). The cursor-based approach creates two Fragment
objects from one:

```cpp
// Given: fragment f, split at byte offset `split_off`
uint32_t char_count = count_utf8_chars(f.content, split_off);

Fragment first_half;
first_half.origin = f.origin;
first_half.locator = f.locator;
first_half.content = f.content.substr(0, split_off);
first_half.length = char_count;
first_half.delete_count = f.delete_count;
first_half.visible = f.visible;

Fragment second_half;
second_half.origin = Lamport(f.origin.replica_id, f.origin.value + char_count);
second_half.locator = f.locator;  // same locator (suffix keeps original)
second_half.content = f.content.substr(split_off);
second_half.length = f.length - char_count;
second_half.delete_count = f.delete_count;
second_half.visible = f.visible;
```

When inserting new text between halves, the second half needs a new locator
(via `Locator::between()`). Record this as a `SplitRelocation` in the
EditOperation for remote replicas.

### 2.4 Locator Computation

`locator_between()` currently takes a vector and an index. Replace with a
method that takes two Locators:

```cpp
Locator locator_between(const Locator& lo, const Locator& hi) {
    return Locator::between(lo, hi);
}
```

The `lo` is `new_tree.last().locator` (the last fragment pushed to the new
tree). The `hi` is `cursor.item()->locator` (the next fragment in the old
tree) or `Locator::max()` if at end.

### 2.5 Insertion Index Rebuild

After building the new tree, rebuild the insertion index. Use `for_each`
on the new tree:

```cpp
InsertionIndex index;
m_fragment_tree.for_each([&](const Fragment& f) {
    index.push_item(InsertionFragment(f.origin, 0, f.locator,
                                       static_cast<uint32_t>(f.content.size())));
});
m_insertion_index = std::move(index);
```

### 2.6 Normalize After Edit

`normalize_fragments()` atomizes multi-char fragments at shared locators.
With cursor-based building, fragments are pushed in order. Normalization
can still be done as a post-pass:

```cpp
auto frags = m_fragment_tree.items();
normalize_fragments(frags);
set_fragments(std::move(frags));
```

This is O(n) but only needed when concurrent insertions create shared
locators (rare in practice). A future optimization could detect shared
locators during tree construction and atomize inline.

### 2.7 Tests

No new test file needed. All existing tests in `tst_buffer` and
`tst_convergence` must pass. Add supplementary tests to `tst_buffer`:

| Test | Description |
|------|-------------|
| `local_edit_insert_at_start_of_fragment` | Insert at byte offset 0 of a multi-char fragment. Verify no split, correct locator. |
| `local_edit_insert_mid_fragment` | Insert at byte offset 3 of a 5-byte fragment. Verify split relocation emitted, correct locators. |
| `local_edit_delete_spanning_fragments` | Delete a range that covers 3 fragments entirely. Verify all 3 get delete_count++. |
| `local_edit_delete_partial_fragment` | Delete half of a fragment. Verify split, correct fragment lengths. |
| `local_edit_multi_range_left_to_right` | Multi-range edit with ranges [1,2) and [4,5). Verify both applied correctly with left-to-right processing. |
| `local_edit_empty_document` | Edit on empty buffer (insert only). |
| `local_edit_replace_entire_document` | Single range covering all text, with replacement. |

Run `tst_convergence` 10 times with different seeds to verify no ordering
regressions.

**Acceptance**: All `tst_buffer` tests (existing + new) pass. All
`tst_convergence` tests pass on 10 consecutive runs. `visible_length()`
returns correct values after every edit.

---

## Optimization 3: VersionedFullOffset for Remote Edits

**Goal**: Add a `VersionedFullOffset` dimension (spec §10.1) so remote edit
application can seek to the correct position in O(log n) instead of scanning
all fragments.

**Current state**: `apply_remote_edit()` (`Buffer.cpp:563-665`) uses a
flat vector and linear scans to find fragments by timestamp. For each
deleted timestamp, it scans all fragments (O(n) per deletion). For k
deletions, total cost is O(nk).

### 3.1 Augment FragmentSummary

Add insertion version tracking to `FragmentSummary` in `Fragment.h`:

```cpp
struct FragmentSummary {
    // existing fields...
    uint32_t visible_bytes = 0;
    uint32_t deleted_bytes = 0;
    Locator max_locator;
    Lamport max_origin = Lamport::min();
    Global max_version;

    // NEW: insertion version range for VersionedFullOffset pruning
    Global min_insertion_version;
    Global max_insertion_version;

    // ... add_summary merges these:
    void add_summary(const FragmentSummary& other) {
        // ... existing merges ...
        min_insertion_version.meet(other.min_insertion_version);
        max_insertion_version.join(other.max_insertion_version);
    }
};
```

Update `Fragment::summary()` to populate these:

```cpp
FragmentSummary summary() const {
    FragmentSummary s;
    // ... existing ...
    Lamport ins_ts(origin.replica_id, origin.value);
    s.min_insertion_version.observe(ins_ts);
    s.max_insertion_version.observe(ins_ts);
    return s;
}
```

### 3.2 Define VersionedFullOffset Dimension

Add to `Fragment.h` or a new file:

```cpp
struct VersionedFullOffset {
    uint32_t value = 0;
    bool valid = true;  // false = must descend into subtree

    static VersionedFullOffset zero() { return {0, true}; }

    void add_summary(const FragmentSummary& s, const Global& remote_version) {
        if (!valid) return;
        if (remote_version.observed_all(s.max_insertion_version)) {
            value += s.visible_bytes + s.deleted_bytes;
        } else if (!remote_version.observed_all(s.min_insertion_version)) {
            // No fragments from remote version in this subtree — skip
        } else {
            valid = false;  // mixed subtree — must descend
        }
    }

    auto operator<=>(const VersionedFullOffset& other) const {
        return value <=> other.value;
    }
    bool operator==(const VersionedFullOffset&) const = default;
};
```

**Important**: This dimension takes a context parameter (`remote_version`).
The SumTree cursor does NOT currently support contextual dimensions. Two
options:

**Option A**: Add an optional context parameter to `Cursor::seek()` and
`slice()`, threaded through `add_summary()`. This requires changing the
SumTree template or cursor template to accept a context type.

**Option B**: Pre-filter the tree. Walk the tree once, marking which subtrees
are "version-visible" to the remote, then seek on the filtered view.

**Option C** (recommended): Don't change SumTree. Instead, implement a
standalone function that walks the tree manually (not via Cursor) using the
version check to prune subtrees:

```cpp
// Walk the fragment tree seeking to byte offset `target` in the
// remote editor's full-offset space (visible + deleted, filtered
// by remote_version).
struct VersionedSeekResult {
    const Fragment* fragment;
    uint32_t offset_in_fragment;
    uint32_t visible_bytes_before;
};

VersionedSeekResult versioned_seek(
    const FragmentTree& tree,
    uint32_t target,
    const Global& remote_version);
```

This function descends the tree manually, using
`remote_version.observed_all(summary.max_insertion_version)` to skip
entire subtrees that were not visible to the remote editor.

### 3.3 Refactor apply_remote_edit

Replace the flat-vector deletion loop with `versioned_seek()`:

```cpp
for (auto& [start, end] : op.ranges) {
    auto result = versioned_seek(m_fragment_tree, start, op.version);
    // result.fragment is the fragment at position `start` in the remote view
    // Build new tree: prefix + modified fragments + suffix
}
```

The split relocation and insertion logic also benefits from tree-based
seeking, but can remain vector-based initially.

### 3.4 Tests

Add tests to `tst_buffer`:

| Test | Description |
|------|-------------|
| `remote_edit_skips_unseen_fragments` | Replica A inserts text, replica B inserts text. A's edit (which hasn't seen B's text) should not affect B's fragments. |
| `remote_edit_version_filtered_offset` | Create fragments from 3 replicas. Send an edit from replica 1 that only saw replica 2's text. Verify correct offset resolution. |
| `remote_edit_mixed_subtree_descends` | Create a tree where one subtree has mixed version visibility. Verify the versioned seek descends correctly. |

All `tst_convergence` tests must still pass.

**Acceptance**: New tests pass. Convergence tests pass on 10 runs. No
regressions in `tst_buffer`.

---

## Optimization 4: Rope Integration into Buffer

**Goal**: Separate text storage from fragment metadata. The Buffer maintains
two Ropes (`visible_text`, `deleted_text`) per spec §6.2. Text moves
between ropes when fragment visibility changes.

**Current state**: Text is stored inline in `Fragment::content`. The
`text()` method (`Buffer.cpp:58-65`) concatenates visible fragments'
content strings. No Rope is used in Buffer despite `Rope.h` being
implemented.

### 4.1 Add Rope Members to Buffer

In `Buffer.h`:
```cpp
#include "crdt/Rope.h"

class Buffer {
    // ... existing ...
private:
    Rope m_visible_text;
    Rope m_deleted_text;
};
```

### 4.2 RopeBuilder

Create `src/crdt/RopeBuilder.h`. The RopeBuilder walks old fragments and
ropes in parallel, constructing new ropes:

```cpp
class RopeBuilder {
public:
    RopeBuilder(const Rope& old_visible, const Rope& old_deleted);

    // Called for each fragment in the new tree, in order.
    // was_visible: was this fragment visible in the OLD tree?
    // now_visible: is this fragment visible in the NEW tree?
    // If the fragment is NEW (not from old tree), call push_new_text() instead.
    void push_fragment(uint32_t byte_length, bool was_visible, bool now_visible);

    // Called when new text is inserted (not from any old rope).
    void push_new_text(std::string_view text);

    Rope finish_visible();
    Rope finish_deleted();

private:
    uint32_t m_old_visible_pos = 0;
    uint32_t m_old_deleted_pos = 0;
    const Rope& m_old_visible;
    const Rope& m_old_deleted;
    Rope m_new_visible;
    Rope m_new_deleted;
};
```

Implementation:
```cpp
void RopeBuilder::push_fragment(uint32_t len, bool was_visible, bool now_visible) {
    // Extract text from old rope
    std::string text;
    if (was_visible) {
        text = m_old_visible.substr(m_old_visible_pos, len);
        m_old_visible_pos += len;
    } else {
        text = m_old_deleted.substr(m_old_deleted_pos, len);
        m_old_deleted_pos += len;
    }

    // Place in new rope
    if (now_visible) {
        m_new_visible.push_str(text);
    } else {
        m_new_deleted.push_str(text);
    }
}

void RopeBuilder::push_new_text(std::string_view text) {
    m_new_visible.push_str(text);
}
```

### 4.3 Modify set_fragments

```cpp
void Buffer::set_fragments(std::vector<Fragment>&& frags) {
    // Compute visibility
    for (auto& f : frags) f.visible = f.compute_visible(m_undo_map);

    // Build ropes
    RopeBuilder builder(m_visible_text, m_deleted_text);
    for (auto& f : frags) {
        bool was_visible = /* need to track this */;
        if (f.is_new_insertion) {
            builder.push_new_text(f.content);
        } else {
            builder.push_fragment(f.content.size(), was_visible, f.visible);
        }
    }
    m_visible_text = builder.finish_visible();
    m_deleted_text = builder.finish_deleted();

    // Build tree and index
    rebuild_insertion_index(frags);
    FragmentTree tree;
    for (auto& f : frags) tree.push_item(std::move(f));
    m_fragment_tree = std::move(tree);
}
```

**Challenge**: Determining `was_visible` requires comparing the old and new
fragment lists. One approach: before modifying fragments, snapshot their
visibility. Another: pass the old fragment tree alongside the new fragments
and match by origin timestamp.

### 4.4 Modify text()

```cpp
std::string Buffer::text() const {
    return m_visible_text.to_string();
}
```

This is the same O(n) complexity but avoids the visibility check per
fragment — the rope already contains only visible text.

### 4.5 Fragment::content Retention

For now, keep `Fragment::content` populated. It's used by remote edit
application (matching text, split operations) and by anchor resolution
(byte offset computation within fragments). Removing it is a separate,
more invasive optimization.

### 4.6 Rope Consistency Invariant

After every operation, verify (in debug builds):

```cpp
assert(m_visible_text.len() == m_fragment_tree.summary().visible_bytes);
assert(m_deleted_text.len() == m_fragment_tree.summary().deleted_bytes);
```

### 4.7 Tests

Add `tests/tst_rope_integration.cpp`:

| Test | Description |
|------|-------------|
| `rope_tracks_inserts` | Insert text, verify `m_visible_text.to_string()` matches `text()`. |
| `rope_tracks_deletes` | Delete text, verify visible rope shrinks and deleted rope grows. |
| `rope_tracks_undo` | Undo a delete, verify text moves from deleted rope back to visible. |
| `rope_tracks_redo` | Redo after undo, verify text moves back to deleted rope. |
| `rope_tracks_remote_edit` | Apply remote insert and delete. Verify rope consistency. |
| `rope_consistency_invariant` | After 100 random edits, assert visible_bytes + deleted_bytes == total bytes. |
| `rope_survives_convergence` | Run convergence test, verify rope invariant on all replicas at end. |

**Acceptance**: All 7 new tests pass. All `tst_buffer`, `tst_convergence`,
`tst_anchor` tests still pass. Debug build asserts do not fire during any
test.

---

## Optimization 5: UndoMap as SumTree

**Goal**: Replace `std::map<UndoMapKey, uint32_t>` with
`SumTree<UndoMapEntry>` keyed by `(edit_id, undo_id)`, per spec §7. This
enables version-aware undo queries (`was_undone(edit_id, version)`) needed
for correct `apply_remote_edit` when concurrent undo operations are in
flight.

**Current state**: `UndoMap.h` uses `std::map<UndoMapKey, uint32_t>` where
`UndoMapKey = {replica_id, lamport_value}`. The counter tracks total
undo/redo toggles. The `is_undone()` check is `counter > 0`. This does NOT
match the spec's `(edit_id, undo_id)` keying and does NOT support
`was_undone(edit_id, version)`.

**WARNING**: This is the highest-risk optimization. It changes the UndoMap
interface, which is used by `Buffer.cpp` in apply_local_edit, undo, redo,
apply_remote_undo, and Fragment::is_visible. All these call sites must be
updated.

### 5.1 New UndoMap Types

Replace `UndoMap.h` entirely:

```cpp
struct UndoMapKey {
    Lamport edit_id;     // the operation being undone
    Lamport undo_id;     // the undo operation itself

    auto operator<=>(const UndoMapKey&) const {
        if (auto cmp = edit_id <=> other.edit_id; cmp != 0) return cmp;
        return undo_id <=> other.undo_id;
    }
    bool operator==(const UndoMapKey&) const = default;
};

struct UndoMapSummary {
    UndoMapKey max_key;

    static UndoMapSummary zero() { return {}; }
    void add_summary(const UndoMapSummary& other) {
        if (other.max_key > max_key) max_key = other.max_key;
    }
};

struct UndoMapKeyDim {
    UndoMapKey value;
    static UndoMapKeyDim zero() { return {}; }
    void add_summary(const UndoMapSummary& s) { value = s.max_key; }
    auto operator<=>(const UndoMapKeyDim&) const = default;
};

struct UndoMapEntry {
    using Summary = UndoMapSummary;
    UndoMapKey key;
    uint32_t undo_count = 0;
    UndoMapSummary summary() const { return {key}; }
};

class UndoMap {
public:
    void insert(UndoMapEntry entry);
    uint32_t undo_count(Lamport edit_id) const;
    bool is_undone(Lamport edit_id) const;
    bool was_undone(Lamport edit_id, const Global& version) const;
private:
    SumTree<UndoMapEntry, 2> m_tree;
};
```

### 5.2 Key Method: was_undone

```cpp
bool UndoMap::was_undone(Lamport edit_id, const Global& version) const {
    uint32_t max_count = 0;
    m_tree.for_each([&](const UndoMapEntry& entry) {
        if (entry.key.edit_id == edit_id && version.observed(entry.key.undo_id)) {
            max_count = std::max(max_count, entry.undo_count);
        }
    });
    return max_count % 2 == 1;
}
```

With cursor optimization (seeking to the edit_id range):
```cpp
auto cursor = m_tree.cursor<UndoMapKeyDim>();
cursor.seek({edit_id, Lamport::min()}, Bias::Left);
while (auto* e = cursor.item()) {
    if (e->key.edit_id != edit_id) break;
    if (version.observed(e->key.undo_id))
        max_count = std::max(max_count, e->undo_count);
    cursor.next();
}
```

### 5.3 Update Fragment Visibility

`Fragment::is_visible()` currently takes a reference to the old UndoMap.
Update to use the new API:

```cpp
bool is_visible_with(const UndoMap& undos) const {
    if (delete_count > 0) return false;
    return !undos.is_undone(origin);
}
```

The function signature is the same, but `UndoMap::is_undone()` now takes
a `Lamport` (the edit's origin timestamp) instead of an `UndoMapKey`.

### 5.4 Update Undo/Redo in Buffer

Current undo stores `inserted_keys` and `deleted_keys` as
`std::vector<UndoMapKey>`. Change `UndoEntry` to store edit-level
information:

```cpp
struct UndoEntry {
    Lamport edit_timestamp;           // the edit being undone
    std::vector<Lamport> inserted;    // characters inserted by this edit
    std::vector<Lamport> deleted;     // characters deleted by this edit
};
```

In `undo()`, create an UndoOperation that records the undo_count for
each targeted edit:

```cpp
UndoOperation op;
op.timestamp = m_clock.tick();
for (auto& key : entry.inserted) {
    uint32_t current = m_undo_map.undo_count(key);
    m_undo_map.insert(UndoMapEntry{{key, op.timestamp}, current + 1});
}
```

### 5.5 Update UndoOperation Wire Format

```cpp
struct UndoOperation {
    Lamport timestamp;
    Global version;
    std::vector<std::pair<Lamport, uint32_t>> counts;  // edit_id -> new count
};
```

### 5.6 Tests

Update `tests/tst_buffer.cpp` and add to `tests/tst_undo.cpp`:

| Test | Description |
|------|-------------|
| `undo_count_parity` | Insert, undo (count=1, undone), redo (count=2, visible), undo again (count=3, undone). |
| `was_undone_version_filter` | Two replicas undo the same edit. `was_undone(edit, versionA)` returns true. `was_undone(edit, versionBefore)` returns false. |
| `concurrent_undo_both_survive` | A and B both undo edit E. After merging, E is still undone (max count = 1, odd). |
| `concurrent_undo_then_redo` | A undoes E, B undoes E, then A redoes E. Final count = 2 (even, visible). |
| `undo_map_cursor_seek` | Insert 1000 undo entries. Verify `undo_count()` uses cursor seek (time it). |
| `all_buffer_tests_pass` | Run all existing `tst_buffer` tests with new UndoMap. |
| `all_convergence_tests_pass` | Run `tst_convergence` with new UndoMap. |

**Acceptance**: All 7 new tests pass. All `tst_buffer` and
`tst_convergence` tests pass. No compile errors. Undo/redo behavior
matches spec §11.1 concurrent undo example exactly.

---

## Invariants

After implementing ANY of these optimizations, the following invariants
(spec §15) must hold. Verify by running the full test suite and checking
debug assertions.

1. **Convergence** (§15.1): All replicas with the same version vector produce
   identical `text()`. Verified by `tst_convergence`.
2. **Fragment ordering** (§15.2): Fragment locators strictly increase (modulo
   same-locator groups sorted by origin). Verified by `normalize_fragments`.
3. **Rope consistency** (§15.3): `visible_text.len() == summary.visible_bytes`
   and `deleted_text.len() == summary.deleted_bytes`. Only applies after Opt 4.
4. **Insertion index** (§15.4): Every fragment has a corresponding
   `InsertionFragment` entry. Verified by `rebuild_insertion_index`.
5. **Causal safety** (§15.5): No operation applied before its dependencies.
   Verified by `tst_convergence` out-of-order delivery.
6. **Idempotence** (§15.6): Duplicate operations have no effect. Verified by
   `tst_buffer::duplicate_ops_idempotent` and `tst_convergence` duplication.
