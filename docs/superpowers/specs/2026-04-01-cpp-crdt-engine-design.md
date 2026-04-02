# C++ CRDT Engine Design

## Goal

Replace the yrs/yffi Rust dependency with a native C++ CRDT engine that
is fully encapsulated within the collabtext library. The engine handles
concurrent text editing, undo/redo, and sync. The test suite — not the
GUI — is the primary deliverable.

## What Changes

- **Delete:** `YrsWrapper.h`, `YrsWrapper.cpp`, `vendor/y-crdt/`
- **New:** `CrdtEngine.h` (public API), `src/crdt/*.h/*.cpp` (internals)
- **New:** `tests/tst_clock.cpp`, `tst_locator.cpp`, `tst_buffer.cpp`,
  `tst_convergence.cpp`
- **Modify:** `CollabDocument.h/.cpp` (swap YrsWrapper for CrdtEngine)
- **Modify:** `app/main.cpp` (remove yrs-specific workarounds)
- **Modify:** `CMakeLists.txt` (remove yrs, add tests)

## Library Structure

```
libs/collabtext/
  include/collabtext/
    CrdtEngine.h              # Public API (only engine header consumers see)
    CollabDocument.h          # Qt bridge (modified)
    SyncManager.h             # File sync (unchanged)
  src/
    CollabDocument.cpp
    SyncManager.cpp
    crdt/
      Clock.h / Clock.cpp     # Lamport timestamp + Global vector clock
      Locator.h / Locator.cpp # Fractional position IDs
      Fragment.h              # Fragment struct + visibility logic
      UndoMap.h / UndoMap.cpp # Parity-based undo tracking
      Buffer.h / Buffer.cpp   # CRDT buffer (ties everything together)
  tests/
    tst_clock.cpp
    tst_locator.cpp
    tst_buffer.cpp
    tst_convergence.cpp
```

## Public API

`CrdtEngine.h` is the only header consumers include. Everything in
`src/crdt/` is private.

```cpp
namespace CollabText {

class CrdtEngine {
public:
    CrdtEngine();
    ~CrdtEngine();

    // Text operations. Offsets are UTF-16 code unit positions
    // (matching QTextCursor::position()).
    void insert(int position, const std::string &text);
    void remove(int position, int length);
    std::string text() const;
    int length() const;

    // Undo / redo
    bool undo();
    bool redo();
    bool canUndo() const;
    bool canRedo() const;

    // Sync
    std::vector<uint8_t> stateVector() const;
    std::vector<uint8_t> encodeUpdate(
        const std::vector<uint8_t> &remoteStateVector) const;
    bool applyUpdate(const std::vector<uint8_t> &update);
    std::vector<uint8_t> encodeState() const;

    // Change notification (fires after any mutation)
    using ChangeCallback = std::function<void()>;
    void setOnChange(ChangeCallback cb);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
```

**Encoding:** UTF-16 code unit offsets at the API boundary (matching Qt).
Internal text stored as UTF-8 (`std::string`). The engine counts UTF-16
code units by scanning UTF-8 bytes: 4-byte sequences count as 2 UTF-16
units, everything else counts as 1. This scan happens inside `insert()`
and `remove()` to convert the UTF-16 offset to a UTF-8 byte offset for
internal use.

**Pimpl:** All internals behind `std::unique_ptr<Impl>`. No CRDT types
in the public header.

## Internal Architecture

### Clock (Clock.h)

```cpp
struct Lamport {
    uint32_t value;
    uint16_t replica_id;

    Lamport tick();               // return current, then increment
    void observe(Lamport other);  // value = max(value, other.value) + 1
    auto operator<=>(const Lamport &) const; // value first, replica_id tiebreak
};

class Global {
    std::vector<uint32_t> values; // values[i] = highest seq from replica i

    void observe(Lamport ts);         // track a timestamp
    bool observed(Lamport ts) const;  // have we seen this?
    bool observed_all(const Global &other) const; // causal readiness
    void join(const Global &other);   // component-wise max
    void meet(const Global &other);   // component-wise min
};
```

### Locator (Locator.h)

```cpp
class Locator {
    std::vector<uint64_t> parts; // typically length 1-2

    static Locator min();        // [0]
    static Locator max();        // [UINT64_MAX]
    static Locator between(const Locator &lhs, const Locator &rhs);
    auto operator<=>(const Locator &) const; // lexicographic
};
```

`between()` uses the `>> 48` biased midpoint from CRDT_ENGINE_SPEC.md
section 3.2. Sequential appends stay at depth 1 for 100,000+
insertions.

### Fragment (Fragment.h)

```cpp
struct Fragment {
    Locator id;
    Lamport timestamp;
    uint32_t insertion_offset;  // byte offset into insertion text
    uint32_t len;               // byte length
    bool visible;
    std::vector<Lamport> deletions; // which ops deleted this
    Global max_undos;

    bool is_visible(const UndoMap &undos) const;
    bool was_visible(const Global &version, const UndoMap &undos) const;
};
```

### UndoMap (UndoMap.h)

```cpp
struct UndoMapKey {
    Lamport edit_id;
    Lamport undo_id;
    auto operator<=>(const UndoMapKey &) const;
};

class UndoMap {
    std::map<UndoMapKey, uint32_t> entries;

    void insert(Lamport edit_id, Lamport undo_id, uint32_t count);
    uint32_t undo_count(Lamport edit_id) const;
    bool is_undone(Lamport edit_id) const;       // count % 2 == 1
    bool was_undone(Lamport edit_id, const Global &version) const;
};
```

### Buffer (Buffer.h)

The CRDT buffer. Owns all state.

```cpp
struct EditOperation {
    Lamport timestamp;
    Global version;
    std::vector<std::pair<uint32_t, uint32_t>> ranges; // (start, end) byte offsets
    std::vector<std::string> new_text;
};

struct UndoOperation {
    Lamport timestamp;
    Global version;
    std::vector<std::pair<Lamport, uint32_t>> counts; // (edit_id, undo_count)
};

using Operation = std::variant<EditOperation, UndoOperation>;

struct Transaction {
    Lamport id;
    std::vector<Lamport> edit_ids;
    Global start_version;
};

class Buffer {
    std::vector<Fragment> fragments;
    std::string visible_text;
    std::string deleted_text;
    UndoMap undo_map;
    Global version;
    Lamport lamport_clock;
    uint16_t replica_id;

    // Deferred operations (waiting for causal dependencies)
    std::vector<Operation> deferred_ops;
    std::set<uint16_t> deferred_replicas;

    // Undo/redo history
    std::vector<Transaction> undo_stack;
    std::vector<Transaction> redo_stack;
    int32_t group_interval_ms = 300;

    // --- Core operations ---
    Operation apply_local_edit(
        const std::vector<std::pair<int, int>> &ranges,
        const std::vector<std::string> &new_text);
    void apply_remote_edit(const EditOperation &op);
    void apply_undo(const UndoOperation &op);
    void apply_ops(const std::vector<Operation> &ops);

    // --- Queries ---
    std::string text() const;         // visible text
    int length_utf16() const;         // visible length in UTF-16 units

    // --- Undo/redo ---
    std::optional<Operation> undo();
    std::optional<Operation> redo();

    // --- Sync ---
    bool can_apply_op(const Operation &op) const;
    void flush_deferred_ops();
};
```

**No Rope, no SumTree.** `std::vector<Fragment>` and `std::string` for
the first version. Linear scans for offset lookups. Correct first,
optimize with a B+ tree later when profiling shows it's needed.

**Fragment splitting:** When an edit range cuts through a fragment, the
fragment is split in place. The prefix gets a new Locator via
`Locator::between()`. The suffix keeps the original Locator.

**Concurrent insertion ordering:** When two replicas insert at the same
position, lower Lamport timestamp appears first. Enforced during
`apply_remote_edit` by skipping over existing fragments with higher
timestamps at the insertion point.

## Serialization

JSON for now. Debuggable, good enough for file-based sync at 500ms.

**Operations:** One JSON object per line in the sync files.

```json
{"type":"edit","ts":[1,42],"ver":[[0,10],[1,41]],"ranges":[[5,8]],"text":["hello"]}
{"type":"undo","ts":[1,43],"ver":[[0,10],[1,42]],"counts":[[1,42,1]]}
```

**State vector:** Array of `[replica_id, seq]` pairs.

```json
[[0,100],[1,42],[2,15]]
```

**Full state (snapshots):** JSON serialization of the complete fragment
list, undo map, and version. Future optimization: binary format.

## Test Strategy

Tests are the primary deliverable. All headless, run via `ctest`.

### tst_clock.cpp
- Lamport tick increments
- Lamport observe jumps ahead
- Lamport total ordering (value first, replica_id tiebreak)
- Global observe/observed per-replica tracking
- Global observed_all causal readiness
- Global join (component-wise max)
- Global meet (component-wise min)

### tst_locator.cpp
- between(min, max) produces a strictly intermediate value
- 1000 sequential appends stay at depth 1
- 1000 sequential prepends stay at depth 1-2
- Adversarial midpoint bisection grows logarithmically
- Lexicographic comparison consistency

### tst_buffer.cpp
- Insert at beginning, middle, end
- Delete from beginning, middle, end
- Insert + delete (replace)
- Undo reverses last edit
- Redo restores undone edit
- Multiple rapid edits undo as one group
- Apply a remote edit, verify text integration
- Concurrent inserts at same position: deterministic ordering
- Fragment splitting on partial-range delete
- Remote edit that deletes text concurrently edited locally

### tst_convergence.cpp — The Correctness Proof
- Create N replicas (default 5, configurable)
- M random iterations (default 100, configurable):
  - 50%: random edit on random replica, broadcast operation
  - 20%: random undo/redo, broadcast operation
  - 30%: deliver a pending operation to a random replica
- Operations delivered out of order, with random duplication
- After draining all pending: assert all replicas have identical text()
- Print RNG seed on failure for reproduction
- Parametric: run with 2, 3, 5 replicas
- Parametric: ASCII, multi-byte UTF-8, emoji text

### Test framework
Qt Test (`QTest`). The engine is Qt-free, but the test runner uses
QTest for consistency with the rest of the project.

## Qt Integration

`CollabDocument` changes from wrapping `YrsDocument` to wrapping
`CrdtEngine`. The API is nearly identical:

- `insertText(pos, text)` → `m_engine->insert(pos, text.toStdString())`
- `removeText(pos, len)` → `m_engine->remove(pos, len)`
- `undo()` → `m_engine->undo()`
- Sync: `m_engine->encodeUpdate()` / `m_engine->applyUpdate()`

`setOnChange` replaces the yrs observer signals. CollabDocument
connects it to push CRDT state into QTextDocument when remote edits
arrive.

The `m_applyingRemote` flag and all the re-entrancy guards from the
yrs integration are no longer needed — the C++ engine doesn't have
yrs's single-transaction-at-a-time restriction. Multiple reads and
writes can interleave freely (single-threaded, no locking).

## Not In Scope

- SumTree / B+ tree (future optimization)
- Rope (future optimization)
- Binary serialization format (future optimization)
- Direct channel transport (separate feature)
- Identity / presence / chat (separate feature)
