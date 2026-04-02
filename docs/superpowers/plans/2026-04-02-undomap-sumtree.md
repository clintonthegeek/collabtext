# UndoMap as SumTree — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `std::map<UndoMapKey, uint32_t>` with `SumTree<UndoMapEntry>` keyed by `(edit_id, undo_id)` per CRDT spec §7, enabling O(log n) seeks and version-aware undo queries (`was_undone`).

**Architecture:** Two-phase migration. Phase 1 rewrites UndoMap internals with SumTree storage while providing legacy `undo()`/`redo()` shims so all existing code compiles and passes. Phase 2 migrates Buffer and UndoOperation to the native `insert()`/`counts` API and removes shims. This avoids a single broken-compilation commit.

**Tech Stack:** C++20, Qt6 Test, SumTree (existing), Cursor seek/slice API

---

## File Map

| File | Action | Responsibility |
|------|--------|---------------|
| `libs/collabtext/src/crdt/UndoMap.h` | Rewrite | New types (UndoTreeKey, UndoMapSummary, UndoTreeKeyDim, UndoMapEntry), new API + legacy shims |
| `libs/collabtext/src/crdt/UndoMap.cpp` | Rewrite | SumTree-backed insert, undo_count, is_undone, was_undone |
| `libs/collabtext/src/crdt/Operations.h` | Modify | Add `counts` field to UndoOperation |
| `libs/collabtext/src/crdt/Fragment.h` | Modify | Simplify compute_visible to use `Lamport` directly |
| `libs/collabtext/src/crdt/Buffer.cpp` | Modify | undo(), redo(), apply_remote_undo() use new API |
| `libs/collabtext/tests/tst_undomap.cpp` | Create | 7 tests: parity, was_undone, concurrent undo, cursor seek |
| `libs/collabtext/CMakeLists.txt` | Modify | Add `add_crdt_test(tst_undomap)` |

---

### Task 1: Rewrite UndoMap with SumTree backend + legacy shims

**Files:**
- Rewrite: `libs/collabtext/src/crdt/UndoMap.h`
- Rewrite: `libs/collabtext/src/crdt/UndoMap.cpp`
- Create: `libs/collabtext/tests/tst_undomap.cpp`
- Modify: `libs/collabtext/CMakeLists.txt`

- [ ] **Step 1: Rewrite UndoMap.h**

Replace the entire content of `libs/collabtext/src/crdt/UndoMap.h` with:

```cpp
#pragma once

#include "crdt/Clock.h"
#include "crdt/SumTree.h"
#include <cstdint>

namespace CollabText::Crdt {

/// Key identifying a character by its Lamport timestamp.
/// Retained for backward compatibility with Buffer::UndoEntry and UndoOperation.
struct UndoMapKey {
    uint16_t replica_id = 0;
    uint32_t lamport_value = 0;

    UndoMapKey() = default;
    UndoMapKey(uint16_t r, uint32_t v) : replica_id(r), lamport_value(v) {}
    explicit UndoMapKey(Lamport ts) : replica_id(ts.replica_id), lamport_value(ts.value) {}

    auto operator<=>(const UndoMapKey &) const = default;
    bool operator==(const UndoMapKey &) const = default;
};

// ============================================================================
// SumTree types for the UndoMap
// ============================================================================

/// Composite key: (edit_id, undo_id). edit_id is the character's Lamport
/// timestamp; undo_id is the undo operation's timestamp.
struct UndoTreeKey {
    Lamport edit_id;
    Lamport undo_id;

    auto operator<=>(const UndoTreeKey &) const = default;
    bool operator==(const UndoTreeKey &) const = default;
};

struct UndoMapSummary {
    UndoTreeKey max_key;

    static UndoMapSummary zero() { return {}; }
    void add_summary(const UndoMapSummary &other) {
        if (other.max_key > max_key)
            max_key = other.max_key;
    }
};

struct UndoTreeKeyDim {
    UndoTreeKey value;

    static UndoTreeKeyDim zero() { return {}; }
    void add_summary(const UndoMapSummary &s) { value = s.max_key; }

    auto operator<=>(const UndoTreeKeyDim &) const = default;
    bool operator==(const UndoTreeKeyDim &) const = default;
};

struct UndoMapEntry {
    using Summary = UndoMapSummary;

    UndoTreeKey key;
    uint32_t undo_count = 0;

    UndoMapSummary summary() const { return {key}; }
};

static constexpr std::size_t UNDO_MAP_B = 2;

/// Tracks undo/redo state per character using a SumTree keyed by
/// (edit_id, undo_id). Visibility uses count parity: even = visible,
/// odd = undone.
class UndoMap {
public:
    UndoMap() = default;

    /// Insert an undo entry (the native API).
    void insert(UndoMapEntry entry);

    /// Maximum undo count for a character (across all undo operations).
    uint32_t undo_count(Lamport edit_id) const;

    /// Is this character currently undone? (max count is odd)
    bool is_undone(Lamport edit_id) const;

    /// Was this character undone at a specific version? Only considers
    /// undo operations observed by `version`.
    bool was_undone(Lamport edit_id, const Global &version) const;

    /// Legacy overload: check by UndoMapKey.
    bool is_undone(UndoMapKey key) const {
        return is_undone(Lamport(key.replica_id, key.lamport_value));
    }

    // ---- Legacy shims (used until Buffer migrates to native API) ----

    /// Legacy: increment undo counter (uses synthetic undo_id).
    void undo(UndoMapKey key);

    /// Legacy: "decrement" undo counter via parity increment.
    void redo(UndoMapKey key);

    /// Legacy: raw count access.
    uint32_t count(UndoMapKey key) const {
        return undo_count(Lamport(key.replica_id, key.lamport_value));
    }

    /// Number of entries in the tree.
    size_t size() const;

    /// Clear all undo state.
    void clear();

private:
    SumTree<UndoMapEntry, UNDO_MAP_B> m_tree;
    uint32_t m_shim_counter = 0;  // Synthetic undo_id for legacy shims
};

} // namespace CollabText::Crdt
```

- [ ] **Step 2: Rewrite UndoMap.cpp**

Replace the entire content of `libs/collabtext/src/crdt/UndoMap.cpp` with:

```cpp
#include "crdt/UndoMap.h"

namespace CollabText::Crdt {

void UndoMap::insert(UndoMapEntry entry) {
    if (m_tree.empty()) {
        m_tree.push_item(std::move(entry));
        return;
    }
    // Insert in key-sorted order using cursor seek/slice
    UndoTreeKeyDim target{entry.key};
    auto cursor = m_tree.cursor<UndoTreeKeyDim>();
    cursor.seek(UndoTreeKeyDim::zero(), Bias::Left);
    SumTree<UndoMapEntry, UNDO_MAP_B> new_tree;
    new_tree.push_tree(cursor.slice(target));
    new_tree.push_item(std::move(entry));
    new_tree.push_tree(cursor.suffix());
    m_tree = std::move(new_tree);
}

uint32_t UndoMap::undo_count(Lamport edit_id) const {
    uint32_t max_count = 0;
    UndoTreeKey start_key{edit_id, Lamport::min()};
    auto cursor = m_tree.cursor<UndoTreeKeyDim>();
    cursor.seek(UndoTreeKeyDim{start_key}, Bias::Left);
    while (auto *entry = cursor.item()) {
        if (!(entry->key.edit_id == edit_id))
            break;
        if (entry->undo_count > max_count)
            max_count = entry->undo_count;
        cursor.next();
    }
    return max_count;
}

bool UndoMap::is_undone(Lamport edit_id) const {
    return undo_count(edit_id) % 2 == 1;
}

bool UndoMap::was_undone(Lamport edit_id, const Global &version) const {
    uint32_t max_count = 0;
    UndoTreeKey start_key{edit_id, Lamport::min()};
    auto cursor = m_tree.cursor<UndoTreeKeyDim>();
    cursor.seek(UndoTreeKeyDim{start_key}, Bias::Left);
    while (auto *entry = cursor.item()) {
        if (!(entry->key.edit_id == edit_id))
            break;
        if (version.observed(entry->key.undo_id)) {
            if (entry->undo_count > max_count)
                max_count = entry->undo_count;
        }
        cursor.next();
    }
    return max_count % 2 == 1;
}

// ---- Legacy shims ----

void UndoMap::undo(UndoMapKey key) {
    Lamport edit_id(key.replica_id, key.lamport_value);
    uint32_t current = undo_count(edit_id);
    Lamport undo_id(UINT16_MAX, ++m_shim_counter);
    insert(UndoMapEntry{{edit_id, undo_id}, current + 1});
}

void UndoMap::redo(UndoMapKey key) {
    Lamport edit_id(key.replica_id, key.lamport_value);
    uint32_t current = undo_count(edit_id);
    if (current == 0) return;  // Nothing to redo
    Lamport undo_id(UINT16_MAX, ++m_shim_counter);
    insert(UndoMapEntry{{edit_id, undo_id}, current + 1});
}

size_t UndoMap::size() const {
    size_t count = 0;
    m_tree.for_each([&](const UndoMapEntry &) { ++count; });
    return count;
}

void UndoMap::clear() {
    m_tree = {};
    m_shim_counter = 0;
}

} // namespace CollabText::Crdt
```

- [ ] **Step 3: Create tst_undomap.cpp**

Create `libs/collabtext/tests/tst_undomap.cpp`:

```cpp
#include <QTest>
#include "crdt/UndoMap.h"
#include <chrono>

using namespace CollabText::Crdt;

class TestUndoMap : public QObject {
    Q_OBJECT
private slots:

    void undo_count_parity() {
        UndoMap map;
        Lamport edit_id(1, 10);
        Lamport undo1(1, 100);
        Lamport undo2(1, 200);
        Lamport undo3(1, 300);

        // Initially not undone
        QCOMPARE(map.undo_count(edit_id), 0u);
        QVERIFY(!map.is_undone(edit_id));

        // Undo: count=1, odd → undone
        map.insert(UndoMapEntry{{edit_id, undo1}, 1});
        QCOMPARE(map.undo_count(edit_id), 1u);
        QVERIFY(map.is_undone(edit_id));

        // Redo: count=2, even → visible
        map.insert(UndoMapEntry{{edit_id, undo2}, 2});
        QCOMPARE(map.undo_count(edit_id), 2u);
        QVERIFY(!map.is_undone(edit_id));

        // Undo again: count=3, odd → undone
        map.insert(UndoMapEntry{{edit_id, undo3}, 3});
        QCOMPARE(map.undo_count(edit_id), 3u);
        QVERIFY(map.is_undone(edit_id));
    }

    void was_undone_version_filter() {
        UndoMap map;
        Lamport edit_id(1, 10);
        Lamport undoA(2, 50);   // Replica 2 undoes
        Lamport undoB(3, 60);   // Replica 3 undoes

        map.insert(UndoMapEntry{{edit_id, undoA}, 1});
        map.insert(UndoMapEntry{{edit_id, undoB}, 1});

        // Version that has seen replica 2's undo but not replica 3's
        Global versionA;
        versionA.observe(undoA);

        QVERIFY(map.was_undone(edit_id, versionA));

        // Version that has seen neither undo
        Global versionEmpty;
        QVERIFY(!map.was_undone(edit_id, versionEmpty));

        // Version that has seen both
        Global versionBoth;
        versionBoth.observe(undoA);
        versionBoth.observe(undoB);
        QVERIFY(map.was_undone(edit_id, versionBoth));
    }

    void concurrent_undo_both_survive() {
        // Alice and Bob both undo edit E — E should stay undone
        UndoMap map;
        Lamport editE(1, 10);
        Lamport undoAlice(2, 100);
        Lamport undoBob(3, 100);

        map.insert(UndoMapEntry{{editE, undoAlice}, 1});
        map.insert(UndoMapEntry{{editE, undoBob}, 1});

        // max(1, 1) = 1, odd → undone
        QCOMPARE(map.undo_count(editE), 1u);
        QVERIFY(map.is_undone(editE));
    }

    void concurrent_undo_then_redo() {
        // A undoes E, B undoes E, then A redoes E
        // Final: max(1, 2, 1) = 2, even → visible
        UndoMap map;
        Lamport editE(1, 10);
        Lamport undoA1(2, 100);
        Lamport undoB(3, 100);
        Lamport redoA(2, 200);

        map.insert(UndoMapEntry{{editE, undoA1}, 1});
        map.insert(UndoMapEntry{{editE, undoB}, 1});
        map.insert(UndoMapEntry{{editE, redoA}, 2});

        QCOMPARE(map.undo_count(editE), 2u);
        QVERIFY(!map.is_undone(editE));  // Even → visible, redo wins
    }

    void legacy_undo_redo_shims() {
        // Verify the legacy API works via SumTree backend
        UndoMap map;
        UndoMapKey key(1, 10);

        QVERIFY(!map.is_undone(key));
        QCOMPARE(map.count(key), 0u);

        map.undo(key);
        QVERIFY(map.is_undone(key));

        map.redo(key);
        QVERIFY(!map.is_undone(key));

        // Redo when not undone is a no-op
        map.redo(key);
        QVERIFY(!map.is_undone(key));
    }

    void multiple_edits_independent() {
        UndoMap map;
        Lamport editA(1, 10);
        Lamport editB(1, 20);
        Lamport undo1(1, 100);

        // Undo editA only
        map.insert(UndoMapEntry{{editA, undo1}, 1});

        QVERIFY(map.is_undone(editA));
        QVERIFY(!map.is_undone(editB));
    }

    void undo_map_cursor_seek() {
        // Insert 1000 entries, verify undo_count uses cursor seek (time it)
        UndoMap map;
        Lamport undo_ts(99, 1);
        for (uint32_t i = 0; i < 1000; ++i) {
            Lamport edit_id(1, i);
            map.insert(UndoMapEntry{{edit_id, undo_ts}, 1});
        }

        // All should be undone
        QVERIFY(map.is_undone(Lamport(1, 0)));
        QVERIFY(map.is_undone(Lamport(1, 500)));
        QVERIFY(map.is_undone(Lamport(1, 999)));
        QVERIFY(!map.is_undone(Lamport(1, 1000)));

        // Time 10000 lookups — should be fast (O(log n) per lookup)
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < 10000; ++i) {
            map.undo_count(Lamport(1, i % 1000));
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - start);
        // 10000 O(log n) lookups on 1000 entries should be well under 1 second
        QVERIFY2(elapsed.count() < 1000,
                 qPrintable(QString("10000 lookups took %1ms").arg(elapsed.count())));
    }
};

QTEST_MAIN(TestUndoMap)
#include "tst_undomap.moc"
```

- [ ] **Step 4: Register test in CMakeLists.txt**

Add after `add_crdt_test(tst_rope_integration)` in `libs/collabtext/CMakeLists.txt`:

```cmake
add_crdt_test(tst_undomap)
```

- [ ] **Step 5: Build and run new tests**

Run:
```bash
cmake --build build-dev 2>&1
./build-dev/libs/collabtext/tst_undomap -v2
```

Expected: All 7 tests PASS.

- [ ] **Step 6: Run full existing test suite**

Run:
```bash
cd build-dev && ctest --output-on-failure 2>&1
```

Expected: All tests pass (legacy shims keep existing code working).

- [ ] **Step 7: Commit**

```bash
git add libs/collabtext/src/crdt/UndoMap.h libs/collabtext/src/crdt/UndoMap.cpp \
       libs/collabtext/tests/tst_undomap.cpp libs/collabtext/CMakeLists.txt
git commit -m "feat: rewrite UndoMap with SumTree backend, add was_undone"
```

---

### Task 2: Migrate Buffer + Operations to native UndoMap API

**Files:**
- Modify: `libs/collabtext/src/crdt/Operations.h`
- Modify: `libs/collabtext/src/crdt/Fragment.h`
- Modify: `libs/collabtext/src/crdt/Buffer.cpp`
- Modify: `libs/collabtext/src/crdt/UndoMap.h` (remove legacy shims)
- Modify: `libs/collabtext/src/crdt/UndoMap.cpp` (remove legacy shims)

- [ ] **Step 1: Add counts field to UndoOperation**

In `libs/collabtext/src/crdt/Operations.h`, replace the UndoOperation struct (lines 45-51) with:

```cpp
struct UndoOperation {
    Lamport timestamp;
    Global version;
    std::vector<std::pair<Lamport, uint32_t>> counts;  // (edit_id, new_count)
    std::vector<UndoMapKey> undelete_keys;  // Characters to un-delete/re-delete
    bool is_redo = false;                   // Direction for undelete_keys
};
```

Note: `undo_keys` is removed. The `counts` field replaces it.

- [ ] **Step 2: Update Fragment::compute_visible to use Lamport directly**

In `libs/collabtext/src/crdt/Fragment.h`, replace `compute_visible` (line 137-139) with:

```cpp
    bool compute_visible(const UndoMap &undo_map) const {
        if (delete_count > 0) return false;
        return !undo_map.is_undone(origin);
    }
```

This uses the `is_undone(Lamport)` overload directly instead of wrapping in UndoMapKey.

- [ ] **Step 3: Update Buffer::undo() to use native API**

In `libs/collabtext/src/crdt/Buffer.cpp`, replace `Buffer::undo()` (lines 1014-1051) with:

```cpp
std::optional<Operation> Buffer::undo() {
    if (m_undo_cursor == 0)
        return std::nullopt;

    m_undo_cursor--;
    auto &entry = m_undo_stack[m_undo_cursor];

    UndoOperation op;
    op.version = m_version;
    op.is_redo = false;
    op.timestamp = m_clock.tick();

    // Undo inserted characters (hide them via undo map)
    for (auto &key : entry.inserted_keys) {
        Lamport edit_id(key.replica_id, key.lamport_value);
        uint32_t current = m_undo_map.undo_count(edit_id);
        m_undo_map.insert(UndoMapEntry{{edit_id, op.timestamp}, current + 1});
        op.counts.push_back({edit_id, current + 1});
    }

    // Undo deleted characters (decrement delete counter)
    auto frags = get_fragments();
    for (auto &key : entry.deleted_keys) {
        for (auto &f : frags) {
            if (f.origin.replica_id == key.replica_id &&
                key.lamport_value >= f.origin.value &&
                key.lamport_value < f.origin.value + f.length &&
                f.delete_count > 0) {
                f.delete_count--;
                break;
            }
        }
        op.undelete_keys.push_back(key);
    }
    set_fragments(std::move(frags));

    m_version.observe(op.timestamp);

    return op;
}
```

- [ ] **Step 4: Update Buffer::redo() to use native API**

Replace `Buffer::redo()` (lines 1053-1089) with:

```cpp
std::optional<Operation> Buffer::redo() {
    if (m_undo_cursor >= m_undo_stack.size())
        return std::nullopt;

    auto &entry = m_undo_stack[m_undo_cursor];
    m_undo_cursor++;

    UndoOperation op;
    op.version = m_version;
    op.is_redo = true;
    op.timestamp = m_clock.tick();

    // Redo: increment undo count (even count = visible again)
    for (auto &key : entry.inserted_keys) {
        Lamport edit_id(key.replica_id, key.lamport_value);
        uint32_t current = m_undo_map.undo_count(edit_id);
        m_undo_map.insert(UndoMapEntry{{edit_id, op.timestamp}, current + 1});
        op.counts.push_back({edit_id, current + 1});
    }

    // Redo: re-delete deleted characters (increment delete counter)
    auto frags = get_fragments();
    for (auto &key : entry.deleted_keys) {
        for (auto &f : frags) {
            if (f.origin.replica_id == key.replica_id &&
                key.lamport_value >= f.origin.value &&
                key.lamport_value < f.origin.value + f.length) {
                f.delete_count++;
                break;
            }
        }
        op.undelete_keys.push_back(key);
    }
    set_fragments(std::move(frags));

    m_version.observe(op.timestamp);

    return op;
}
```

- [ ] **Step 5: Update Buffer::apply_remote_undo() to use counts**

Replace the undo_keys handling in `apply_remote_undo()` (lines 918-924 in Buffer.cpp) with:

```cpp
    // Handle counts (insert undo entries into map)
    for (auto &[edit_id, count] : op.counts) {
        m_undo_map.insert(UndoMapEntry{{edit_id, op.timestamp}, count});
    }
```

The undelete_keys handling (lines 927-921) stays exactly the same.

- [ ] **Step 6: Remove legacy shims from UndoMap.h**

In `libs/collabtext/src/crdt/UndoMap.h`, remove:
- The `void undo(UndoMapKey key);` declaration
- The `void redo(UndoMapKey key);` declaration
- The `uint32_t count(UndoMapKey key) const { ... }` inline method
- The `uint32_t m_shim_counter = 0;` member

- [ ] **Step 7: Remove legacy shims from UndoMap.cpp**

In `libs/collabtext/src/crdt/UndoMap.cpp`, remove the entire `// ---- Legacy shims ----` section (the `undo()` and `redo()` methods). Also remove the `m_shim_counter = 0` line from `clear()`.

- [ ] **Step 8: Build and run full test suite**

Run:
```bash
cmake --build build-dev 2>&1 && cd build-dev && ctest --output-on-failure 2>&1
```

Expected: All tests pass — the `legacy_undo_redo_shims` test in tst_undomap will fail (shims removed). That test needs updating too.

- [ ] **Step 9: Update tst_undomap legacy test**

Replace the `legacy_undo_redo_shims` test in `libs/collabtext/tests/tst_undomap.cpp` with:

```cpp
    void insert_and_query() {
        // Verify insert + is_undone via UndoMapKey overload (used by Fragment)
        UndoMap map;
        UndoMapKey key(1, 10);
        Lamport edit_id(1, 10);
        Lamport undo_ts(2, 50);

        QVERIFY(!map.is_undone(key));  // UndoMapKey overload

        map.insert(UndoMapEntry{{edit_id, undo_ts}, 1});
        QVERIFY(map.is_undone(key));   // Delegates to Lamport overload

        map.insert(UndoMapEntry{{edit_id, Lamport(2, 60)}, 2});
        QVERIFY(!map.is_undone(key));  // Even → visible
    }
```

- [ ] **Step 10: Rebuild and verify**

Run:
```bash
cmake --build build-dev 2>&1 && cd build-dev && ctest --output-on-failure 2>&1
```

Expected: All tests pass.

- [ ] **Step 11: Commit**

```bash
git add libs/collabtext/src/crdt/UndoMap.h libs/collabtext/src/crdt/UndoMap.cpp \
       libs/collabtext/src/crdt/Operations.h libs/collabtext/src/crdt/Fragment.h \
       libs/collabtext/src/crdt/Buffer.cpp libs/collabtext/tests/tst_undomap.cpp
git commit -m "feat: migrate Buffer to native UndoMap API, remove legacy shims"
```

---

### Task 3: Concurrent undo integration tests

**Files:**
- Modify: `libs/collabtext/tests/tst_buffer.cpp`

- [ ] **Step 1: Add concurrent undo tests to tst_buffer.cpp**

Add to the end of the test class in `libs/collabtext/tests/tst_buffer.cpp` (before the closing `};`):

```cpp
    // -----------------------------------------------------------------------
    // Concurrent undo (Optimization 5)
    // -----------------------------------------------------------------------

    void concurrent_undo_both_hide() {
        // Alice and Bob both undo the same edit — text should be hidden
        Buffer bufA(1), bufB(2), bufC(3);
        auto ins = bufA.apply_local_edit({{0, 0}}, {"hello"});
        bufB.apply_ops({ins});
        bufC.apply_ops({ins});

        auto undoA = bufA.undo();
        auto undoB = bufB.undo();
        QVERIFY(undoA.has_value());
        QVERIFY(undoB.has_value());

        // Cross-apply undos
        bufA.apply_ops({*undoB});
        bufB.apply_ops({*undoA});
        bufC.apply_ops({*undoA, *undoB});

        // All should converge on empty (both undos agree)
        QCOMPARE(bufA.text(), std::string(""));
        QCOMPARE(bufB.text(), std::string(""));
        QCOMPARE(bufC.text(), std::string(""));
    }

    void concurrent_undo_then_redo_wins() {
        // A undoes, B undoes, then A redoes — redo should win (higher count)
        Buffer bufA(1), bufB(2), bufC(3);
        auto ins = bufA.apply_local_edit({{0, 0}}, {"hello"});
        bufB.apply_ops({ins});
        bufC.apply_ops({ins});

        auto undoA = bufA.undo();
        auto undoB = bufB.undo();
        QVERIFY(undoA.has_value());
        QVERIFY(undoB.has_value());

        bufA.apply_ops({*undoB});
        auto redoA = bufA.redo();
        QVERIFY(redoA.has_value());

        // Apply all ops to C
        bufC.apply_ops({*undoA, *undoB, *redoA});
        bufB.apply_ops({*undoA, *redoA});

        // Redo wins — text should be visible
        QCOMPARE(bufA.text(), std::string("hello"));
        QCOMPARE(bufB.text(), std::string("hello"));
        QCOMPARE(bufC.text(), std::string("hello"));
    }

    void remote_undo_with_counts() {
        // Verify the new counts-based UndoOperation wire format works
        Buffer bufA(1), bufB(2);
        auto ins = bufA.apply_local_edit({{0, 0}}, {"hello"});
        bufB.apply_ops({ins});

        auto undoOp = bufA.undo();
        QVERIFY(undoOp.has_value());

        // Apply undo to B via remote path
        bufB.apply_ops({*undoOp});
        QCOMPARE(bufB.text(), std::string(""));

        // Redo on A, apply to B
        auto redoOp = bufA.redo();
        QVERIFY(redoOp.has_value());
        bufB.apply_ops({*redoOp});
        QCOMPARE(bufB.text(), std::string("hello"));
        QCOMPARE(bufA.text(), bufB.text());
    }
```

- [ ] **Step 2: Build and run**

Run:
```bash
cmake --build build-dev --target tst_buffer 2>&1
./build-dev/libs/collabtext/tst_buffer -v2
```

Expected: All tests pass (including the 3 new ones).

- [ ] **Step 3: Commit**

```bash
git add libs/collabtext/tests/tst_buffer.cpp
git commit -m "test: add concurrent undo integration tests"
```

---

### Task 4: Full verification

**Files:** None (verification only)

- [ ] **Step 1: Run full test suite**

Run:
```bash
cmake --build build-dev 2>&1 && cd build-dev && ctest --output-on-failure 2>&1
```

Expected: All tests pass.

- [ ] **Step 2: Run convergence 10 times**

Run:
```bash
for i in $(seq 1 10); do ./build-dev/libs/collabtext/tst_convergence -silent && echo "Run $i: PASS"; done
```

Expected: 10/10 PASS.

- [ ] **Step 3: Run fuzz suite 10 times**

Run:
```bash
for i in $(seq 1 10); do ./build-dev/libs/collabtext/tst_fuzz -silent && echo "Fuzz $i: PASS"; done
```

Expected: 10/10 PASS.
