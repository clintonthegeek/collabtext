# Unified Deletion Tracking — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `delete_count` with `deletions: Vec<Lamport>` on Fragment, unifying insertion-undo and deletion-undo into a single UndoMap parity mechanism.

**Architecture:** Phase 1 changes Fragment + visibility model (breaking compilation). Phase 2 updates all Buffer mutation sites (restoring compilation). Phase 3 simplifies UndoOperation and undo/redo. Phase 4 adds targeted tests. All changes are atomic — the codebase compiles and passes only after Phase 2.

**Tech Stack:** C++20, Qt6 Test, SumTree, UndoMap (SumTree-backed)

---

## File Map

| File | Action | Changes |
|------|--------|---------|
| `libs/collabtext/src/crdt/Fragment.h` | Modify | `delete_count` → `deletions`, update `compute_visible`, `deleted()`, `was_visible()` |
| `libs/collabtext/src/crdt/Buffer.h` | Modify | Update `UndoEntry` struct |
| `libs/collabtext/src/crdt/Buffer.cpp` | Modify | Update all 13 `delete_count` sites: mark_deleted, split_frag, split_fragment_at, normalize, apply_remote_edit, undo, redo, apply_remote_undo |
| `libs/collabtext/src/crdt/Operations.h` | Modify | Remove `undelete_keys`, `is_redo` from UndoOperation |
| `libs/collabtext/tests/tst_buffer.cpp` | Modify | Add 3 new tests for unified deletion model |
| `libs/collabtext/tests/tst_fuzz.cpp` | Modify | Update debug print |

---

### Task 1: Update Fragment model + visibility

**Files:**
- Modify: `libs/collabtext/src/crdt/Fragment.h`

This task changes the data model. The codebase will NOT compile until Task 2 updates all call sites.

- [ ] **Step 1: Replace delete_count with deletions vector**

In `libs/collabtext/src/crdt/Fragment.h`, replace line 120:

```cpp
    uint32_t delete_count = 0;  ///< Number of active delete votes (deleted when > 0)
```

with:

```cpp
    std::vector<Lamport> deletions;  ///< Timestamps of operations that deleted this fragment
```

- [ ] **Step 2: Update deleted() method**

Replace line 133:

```cpp
    bool deleted() const { return delete_count > 0; }
```

with:

```cpp
    bool deleted() const { return !deletions.empty(); }
```

- [ ] **Step 3: Update compute_visible()**

Replace lines 135-140:

```cpp
    /// Compute visibility from delete_count and undo_map.
    /// Use this when building the tree to set the `visible` flag.
    bool compute_visible(const UndoMap &undo_map) const {
        if (delete_count > 0) return false;
        return !undo_map.is_undone(origin);
    }
```

with:

```cpp
    /// Compute visibility from deletions and undo_map.
    /// A fragment is visible if its insertion is not undone AND
    /// every deletion targeting it has been undone.
    bool compute_visible(const UndoMap &undo_map) const {
        if (undo_map.is_undone(origin)) return false;
        for (auto &del : deletions) {
            if (!undo_map.is_undone(del)) return false;
        }
        return true;
    }
```

- [ ] **Step 4: Update was_visible()**

Replace lines 148-152:

```cpp
    /// True if this fragment was ever visible (inserted but not deleted).
    /// Ignores undo state.
    bool was_visible() const {
        return delete_count == 0;
    }
```

with:

```cpp
    /// True if this fragment has never been deleted.
    /// Ignores undo state.
    bool was_visible() const {
        return deletions.empty();
    }
```

- [ ] **Step 5: Verify Fragment.h compiles in isolation (no linkage)**

This won't compile yet since Buffer.cpp still references `delete_count`. That's expected — Task 2 fixes it.

---

### Task 2: Update all Buffer.cpp call sites

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.cpp`
- Modify: `libs/collabtext/src/crdt/Buffer.h`
- Modify: `libs/collabtext/src/crdt/Operations.h`

This is the big task. All 13 `delete_count` sites in Buffer.cpp must be updated, plus UndoEntry and UndoOperation. After this task, the codebase compiles and all existing tests pass.

- [ ] **Step 1: Update UndoEntry in Buffer.h**

Replace lines 156-159 in `libs/collabtext/src/crdt/Buffer.h`:

```cpp
    struct UndoEntry {
        std::vector<UndoMapKey> inserted_keys;  // Characters we inserted
        std::vector<UndoMapKey> deleted_keys;    // Characters we deleted
    };
```

with:

```cpp
    struct UndoEntry {
        std::vector<UndoMapKey> inserted_keys;  // Characters we inserted
        Lamport deletion_id;                     // Edit timestamp (for undoing our deletions)
        bool had_deletions = false;              // Whether this edit deleted characters
    };
```

- [ ] **Step 2: Update UndoOperation in Operations.h**

Replace lines 45-51 in `libs/collabtext/src/crdt/Operations.h`:

```cpp
struct UndoOperation {
    Lamport timestamp;
    Global version;
    std::vector<std::pair<Lamport, uint32_t>> counts;  // (edit_id, new_count)
    std::vector<UndoMapKey> undelete_keys;  // Characters to un-delete/re-delete
    bool is_redo = false;                   // Direction for undelete_keys
};
```

with:

```cpp
struct UndoOperation {
    Lamport timestamp;
    Global version;
    std::vector<std::pair<Lamport, uint32_t>> counts;  // (edit_id, new_count)
};
```

- [ ] **Step 3: Update mark_deleted lambda in apply_local_edit**

In `libs/collabtext/src/crdt/Buffer.cpp`, the `mark_deleted` lambda and the code before it needs changes. First, add a pre-ticked deletion timestamp after `UndoEntry undo_entry;` (around line 410):

Add after `UndoEntry undo_entry;`:
```cpp
    // Pre-tick a deletion timestamp for this edit
    Lamport deletion_ts = m_clock.tick();
    undo_entry.deletion_id = deletion_ts;
```

Then replace the `mark_deleted` lambda (lines 440-448):

```cpp
    auto mark_deleted = [&](Fragment& f) {
        f.delete_count++;
        f.visible = false;
        for (uint32_t c = 0; c < f.length; ++c) {
            Lamport ts = f.timestamp_at(c);
            op.deleted_timestamps.push_back(ts);
            undo_entry.deleted_keys.push_back(UndoMapKey(ts));
        }
    };
```

with:

```cpp
    auto mark_deleted = [&](Fragment& f) {
        f.deletions.push_back(deletion_ts);
        f.visible = false;
        undo_entry.had_deletions = true;
        for (uint32_t c = 0; c < f.length; ++c) {
            op.deleted_timestamps.push_back(f.timestamp_at(c));
        }
    };
```

- [ ] **Step 4: Update split_frag lambda**

Replace lines 460 and 468 in the `split_frag` lambda:

```cpp
        first.delete_count = f.delete_count;
```
with:
```cpp
        first.deletions = f.deletions;
```

And:
```cpp
        second.delete_count = f.delete_count;
```
with:
```cpp
        second.deletions = f.deletions;
```

- [ ] **Step 5: Update split_fragment_at()**

Replace line 284:
```cpp
    second.delete_count = orig.delete_count;
```
with:
```cpp
    second.deletions = orig.deletions;
```

- [ ] **Step 6: Update normalize_fragments()**

Replace line 365:
```cpp
                        single.delete_count = f.delete_count;
```
with:
```cpp
                        single.deletions = f.deletions;
```

- [ ] **Step 7: Update apply_remote_edit() deletions**

Replace lines 801 and 814:
```cpp
            frags[fi].delete_count++;
```
with:
```cpp
            frags[fi].deletions.push_back(op.timestamp);
```

(Both instances — the length==1 fast path on line 801 and the general path on line 814.)

- [ ] **Step 8: Rewrite apply_remote_undo()**

Replace the entire body of `apply_remote_undo()` (lines 888-924) with:

```cpp
bool Buffer::apply_remote_undo(const UndoOperation &op) {
    if (m_version.observed(op.timestamp))
        return true;

    if (!m_version.observed_all(op.version))
        return false;

    // Insert all undo entries into the map
    for (auto &[edit_id, count] : op.counts) {
        m_undo_map.insert(UndoMapEntry{{edit_id, op.timestamp}, count});
    }

    m_clock.observe(op.timestamp);
    m_version.observe(op.timestamp);
    m_version.join(op.version);

    // Recompute visibility from updated undo map
    auto frags = get_fragments();
    set_fragments(std::move(frags));
    return true;
}
```

- [ ] **Step 9: Rewrite undo()**

Replace the entire `undo()` method (lines 1007-1047) with:

```cpp
std::optional<Operation> Buffer::undo() {
    if (m_undo_cursor == 0)
        return std::nullopt;

    m_undo_cursor--;
    auto &entry = m_undo_stack[m_undo_cursor];

    UndoOperation op;
    op.version = m_version;
    op.timestamp = m_clock.tick();

    // Undo inserted characters (hide them via undo map)
    for (auto &key : entry.inserted_keys) {
        Lamport edit_id(key.replica_id, key.lamport_value);
        uint32_t current = m_undo_map.undo_count(edit_id);
        m_undo_map.insert(UndoMapEntry{{edit_id, op.timestamp}, current + 1});
        op.counts.push_back({edit_id, current + 1});
    }

    // Undo deletions (mark deletion timestamp as undone in undo map)
    if (entry.had_deletions) {
        uint32_t current = m_undo_map.undo_count(entry.deletion_id);
        m_undo_map.insert(
            UndoMapEntry{{entry.deletion_id, op.timestamp}, current + 1});
        op.counts.push_back({entry.deletion_id, current + 1});
    }

    // Recompute visibility from updated undo map
    auto frags = get_fragments();
    set_fragments(std::move(frags));

    m_version.observe(op.timestamp);

    return op;
}
```

- [ ] **Step 10: Rewrite redo()**

Replace the entire `redo()` method (lines 1049-1087) with:

```cpp
std::optional<Operation> Buffer::redo() {
    if (m_undo_cursor >= m_undo_stack.size())
        return std::nullopt;

    auto &entry = m_undo_stack[m_undo_cursor];
    m_undo_cursor++;

    UndoOperation op;
    op.version = m_version;
    op.timestamp = m_clock.tick();

    // Redo: increment undo count (even count = visible again)
    for (auto &key : entry.inserted_keys) {
        Lamport edit_id(key.replica_id, key.lamport_value);
        uint32_t current = m_undo_map.undo_count(edit_id);
        m_undo_map.insert(UndoMapEntry{{edit_id, op.timestamp}, current + 1});
        op.counts.push_back({edit_id, current + 1});
    }

    // Redo deletions: increment count (makes deletion active again)
    if (entry.had_deletions) {
        uint32_t current = m_undo_map.undo_count(entry.deletion_id);
        m_undo_map.insert(
            UndoMapEntry{{entry.deletion_id, op.timestamp}, current + 1});
        op.counts.push_back({entry.deletion_id, current + 1});
    }

    // Recompute visibility from updated undo map
    auto frags = get_fragments();
    set_fragments(std::move(frags));

    m_version.observe(op.timestamp);

    return op;
}
```

- [ ] **Step 11: Update fuzz debug print**

In `libs/collabtext/tests/tst_fuzz.cpp`, replace line 594:

```cpp
                                  << " del=" << frags[fi].delete_count
```
with:
```cpp
                                  << " dels=" << frags[fi].deletions.size()
```

- [ ] **Step 12: Build and run full test suite**

Run:
```bash
cmake --build build-dev 2>&1 && cd build-dev && ctest --output-on-failure 2>&1
```

Expected: ALL existing tests pass. The new visibility model produces identical results for all existing test scenarios.

- [ ] **Step 13: Commit**

```bash
git add libs/collabtext/src/crdt/Fragment.h libs/collabtext/src/crdt/Buffer.h \
       libs/collabtext/src/crdt/Buffer.cpp libs/collabtext/src/crdt/Operations.h \
       libs/collabtext/tests/tst_fuzz.cpp
git commit -m "feat: unified deletion tracking via UndoMap parity model

Replace delete_count with deletions: Vec<Lamport> on Fragment.
Deletion undo now goes through UndoMap (same parity mechanism as
insertion undo). Remove undelete_keys and is_redo from UndoOperation."
```

---

### Task 3: Add unified deletion tests

**Files:**
- Modify: `libs/collabtext/tests/tst_buffer.cpp`

- [ ] **Step 1: Add 3 new tests**

Add before the closing `};` in `libs/collabtext/tests/tst_buffer.cpp`:

```cpp
    // -----------------------------------------------------------------------
    // Unified deletion tracking
    // -----------------------------------------------------------------------

    void fragment_with_multiple_deletions() {
        // Two replicas delete the same character. Undo one — still invisible.
        // Undo both — visible.
        Buffer bufA(1), bufB(2), bufC(3);
        auto ins = bufA.apply_local_edit({{0, 0}}, {"hello"});
        bufB.apply_ops({ins});
        bufC.apply_ops({ins});

        // A deletes "h", B deletes "h"
        auto delA = bufA.apply_local_edit({{0, 1}}, {""});
        auto delB = bufB.apply_local_edit({{0, 1}}, {""});

        // C receives both deletes
        bufC.apply_ops({delA, delB});
        QCOMPARE(bufC.text(), std::string("ello"));

        // A undoes its delete — but B's delete still active
        auto undoA = bufA.undo();
        QVERIFY(undoA.has_value());
        bufC.apply_ops({*undoA});
        QCOMPARE(bufC.text(), std::string("ello"));  // Still deleted by B

        // B undoes its delete — now both deletions undone, "h" visible
        auto undoB = bufB.undo();
        QVERIFY(undoB.has_value());
        bufC.apply_ops({*undoB});
        QCOMPARE(bufC.text(), std::string("hello"));
    }

    void deletion_undo_roundtrip() {
        // Delete, undo, redo — verify the parity model works end-to-end
        Buffer bufA(1), bufB(2);
        auto ins = bufA.apply_local_edit({{0, 0}}, {"hello"});
        bufB.apply_ops({ins});

        auto del = bufA.apply_local_edit({{1, 4}}, {""});  // delete "ell"
        bufB.apply_ops({del});
        QCOMPARE(bufA.text(), std::string("ho"));
        QCOMPARE(bufB.text(), std::string("ho"));

        // Undo delete — "ell" reappears
        auto undo = bufA.undo();
        QVERIFY(undo.has_value());
        bufB.apply_ops({*undo});
        QCOMPARE(bufA.text(), std::string("hello"));
        QCOMPARE(bufB.text(), std::string("hello"));

        // Redo delete — "ell" disappears again
        auto redo = bufA.redo();
        QVERIFY(redo.has_value());
        bufB.apply_ops({*redo});
        QCOMPARE(bufA.text(), std::string("ho"));
        QCOMPARE(bufB.text(), std::string("ho"));
    }

    void concurrent_delete_and_insertion_undo() {
        // A inserts "hello", B receives it.
        // A deletes "ell". B undoes A's insertion (hides all of "hello").
        // After merge: everything invisible.
        // B redoes (un-undoes insertion) — only "ho" visible (A's delete still active).
        // A undoes the delete — "hello" visible again.
        Buffer bufA(1), bufB(2);
        auto ins = bufA.apply_local_edit({{0, 0}}, {"hello"});
        bufB.apply_ops({ins});

        auto del = bufA.apply_local_edit({{1, 4}}, {""});   // delete "ell"
        auto undoB = bufB.undo();  // undo insertion of "hello"
        QVERIFY(undoB.has_value());

        // Cross-apply
        bufA.apply_ops({*undoB});
        bufB.apply_ops({del});

        // Both should see "" — insertion undone hides everything
        QCOMPARE(bufA.text(), std::string(""));
        QCOMPARE(bufB.text(), std::string(""));

        // B redoes (makes insertion visible again)
        auto redoB = bufB.redo();
        QVERIFY(redoB.has_value());
        bufA.apply_ops({*redoB});

        // Now insertion is visible, but A's delete still active → "ho"
        QCOMPARE(bufA.text(), std::string("ho"));
        QCOMPARE(bufB.text(), std::string("ho"));

        // A undoes the delete
        auto undoA = bufA.undo();
        QVERIFY(undoA.has_value());
        bufB.apply_ops({*undoA});

        // Full "hello" visible again
        QCOMPARE(bufA.text(), std::string("hello"));
        QCOMPARE(bufB.text(), std::string("hello"));
    }
```

- [ ] **Step 2: Build and run**

Run:
```bash
cmake --build build-dev --target tst_buffer 2>&1
./build-dev/libs/collabtext/tst_buffer -v2
```

Expected: All tests pass including the 3 new ones.

- [ ] **Step 3: Commit**

```bash
git add libs/collabtext/tests/tst_buffer.cpp
git commit -m "test: add unified deletion tracking tests (multi-delete, roundtrip, concurrent)"
```

---

### Task 4: Full verification

- [ ] **Step 1: Full test suite**

```bash
cmake --build build-dev 2>&1 && cd build-dev && ctest --output-on-failure 2>&1
```

Expected: All 12 executables pass.

- [ ] **Step 2: Convergence 10x**

```bash
for i in $(seq 1 10); do /home/clinton/dev/collabtext/build-dev/libs/collabtext/tst_convergence -silent && echo "Conv $i: PASS"; done
```

Expected: 10/10 PASS.

- [ ] **Step 3: Fuzz 10x**

```bash
for i in $(seq 1 10); do /home/clinton/dev/collabtext/build-dev/libs/collabtext/tst_fuzz -silent && echo "Fuzz $i: PASS"; done
```

Expected: 10/10 PASS.
