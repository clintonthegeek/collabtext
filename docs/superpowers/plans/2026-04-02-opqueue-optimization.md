# Optimization 1: Operation Queue with Deferred Replica Tracking

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the `std::vector<Operation> m_deferred` with a `SumTree<OperationEntry>` ordered by Lamport timestamp, and add per-replica deferred tracking to skip replicas whose causal dependencies aren't met, bringing worst-case retry from O(n^2) to O(n).

**Architecture:** We extract the existing `std::visit`-based apply logic into a `try_apply()` helper, introduce an `OperationQueue` (SumTree-backed) and `std::set<uint16_t> m_deferred_replicas`, and rewrite `apply_ops()` and `retry_deferred()` to use them. The key insight: once any op from a replica is deferred, all subsequent ops from that replica must also be deferred (causal chain), so we track deferred replicas and skip them without attempting application.

**Tech Stack:** C++20, Qt6 Test, SumTree (existing `libs/collabtext/src/crdt/SumTree.h`), CMake.

**Spec reference:** `docs/specs/sumtree-optimizations.md` sections 1.1-1.5

**Build & test commands:**
- Build: `cmake --build build-dev`
- Run all tests: `cd build-dev && ctest --output-on-failure`
- Run single test: `./build-dev/libs/collabtext/tst_opqueue`
- Run buffer tests: `./build-dev/libs/collabtext/tst_buffer`
- Run convergence tests: `./build-dev/libs/collabtext/tst_convergence`

---

## File Map

| Action | File | Responsibility |
|--------|------|----------------|
| Create | `libs/collabtext/src/crdt/Operations.h` | Extract `EditOperation`, `UndoOperation`, `Operation` variant out of `Buffer.h` to break circular include |
| Create | `libs/collabtext/src/crdt/OperationQueue.h` | `OperationEntry`, `OperationSummary`, `TimestampDim`, `get_op_timestamp()`, type alias `OperationQueue` |
| Modify | `libs/collabtext/src/crdt/Buffer.h:23-63,156-168` | Include `Operations.h` (remove inline type defs); replace `m_deferred` vector with `OperationQueue m_deferred_queue` + `std::set<uint16_t> m_deferred_replicas`; declare `try_apply()` |
| Modify | `libs/collabtext/src/crdt/Buffer.cpp:716-757` | Rewrite `apply_ops()` and `retry_deferred()` to use new queue + replica tracking; extract `try_apply()` |
| Create | `libs/collabtext/tests/tst_opqueue.cpp` | 6 test cases per spec |
| Modify | `libs/collabtext/CMakeLists.txt:43` | Register `tst_opqueue` test |

**Note on circular includes:** `OperationQueue.h` needs the `Operation` variant (for `OperationEntry::op`), and `Buffer.h` needs both `Operation` and `OperationQueue`. If `Operation` stays in `Buffer.h`, `OperationQueue.h` would need to include `Buffer.h`, creating a cycle. We break this by extracting `EditOperation`, `UndoOperation`, and `Operation` to a standalone `Operations.h` that both `Buffer.h` and `OperationQueue.h` include.

---

## Task 1: Extract Operations.h and create OperationQueue.h

**Files:**
- Create: `libs/collabtext/src/crdt/Operations.h`
- Create: `libs/collabtext/src/crdt/OperationQueue.h`
- Modify: `libs/collabtext/src/crdt/Buffer.h`

- [ ] **Step 1: Create Operations.h**

Extract `EditOperation`, `UndoOperation`, and the `Operation` type alias from `Buffer.h` into a standalone header:

```cpp
#pragma once

#include "crdt/Clock.h"
#include "crdt/Locator.h"
#include "crdt/UndoMap.h"

#include <string>
#include <variant>
#include <vector>

namespace CollabText::Crdt {

// Operation types for sync
struct EditOperation {
    Lamport timestamp;
    Global version;
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    std::vector<std::string> new_text;

    struct InsertedFragment {
        Lamport origin;
        Locator locator;
        std::string content;
        uint32_t length;
    };
    std::vector<InsertedFragment> inserted_fragments;

    std::vector<Lamport> deleted_timestamps;

    struct SplitRelocation {
        Lamport fragment_origin;
        uint32_t split_offset;
        uint32_t fragment_length;
        Locator new_locator;
    };
    std::vector<SplitRelocation> split_relocations;
};

struct UndoOperation {
    Lamport timestamp;
    Global version;
    std::vector<UndoMapKey> undo_keys;
    std::vector<UndoMapKey> undelete_keys;
    bool is_redo = false;
};

using Operation = std::variant<EditOperation, UndoOperation>;

} // namespace CollabText::Crdt
```

- [ ] **Step 2: Create OperationQueue.h**

```cpp
#pragma once

#include "crdt/Clock.h"
#include "crdt/Operations.h"
#include "crdt/SumTree.h"

namespace CollabText::Crdt {

/// Extract the Lamport timestamp from any Operation variant.
inline Lamport get_op_timestamp(const Operation& op) {
    return std::visit([](const auto& o) -> Lamport { return o.timestamp; }, op);
}

// ============================================================================
// SumTree-backed operation queue, ordered by Lamport timestamp
// ============================================================================

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

} // namespace CollabText::Crdt
```

- [ ] **Step 3: Update Buffer.h to include Operations.h instead of inline definitions**

Replace the `EditOperation`, `UndoOperation`, and `Operation` definitions (lines 23-63 of Buffer.h) with:

```cpp
#include "crdt/Operations.h"
```

The existing include of `"crdt/UndoMap.h"` can stay (it's also used directly by Buffer), and `<variant>` can be removed from Buffer.h's includes since Operations.h provides it.

- [ ] **Step 4: Build to verify the extraction is clean**

Run: `cmake --build build-dev 2>&1 | tail -10`
Expected: Clean build. All existing code that uses `EditOperation`, `UndoOperation`, `Operation` through `Buffer.h` still compiles because `Buffer.h` includes `Operations.h`.

- [ ] **Step 5: Run all tests to verify no regressions**

Run: `cd build-dev && ctest --output-on-failure`
Expected: All 6 tests pass.

- [ ] **Step 6: Commit**

```
git add libs/collabtext/src/crdt/Operations.h libs/collabtext/src/crdt/OperationQueue.h libs/collabtext/src/crdt/Buffer.h
git commit -m "refactor: extract Operations.h, add OperationQueue.h

Move EditOperation, UndoOperation, and Operation variant to
standalone Operations.h to break circular include dependency.
Add OperationQueue.h with SumTree-backed deferred op queue types."
```

---

## Task 2: Register test executable

**Files:**
- Modify: `libs/collabtext/CMakeLists.txt:43`
- Create: `libs/collabtext/tests/tst_opqueue.cpp` (skeleton)

- [ ] **Step 1: Add test registration to CMakeLists.txt**

After the last `add_crdt_test` line (line 43, `add_crdt_test(tst_anchor)`), add:

```cmake
add_crdt_test(tst_opqueue)
```

- [ ] **Step 2: Create skeleton test file**

```cpp
#include <QTest>
#include "crdt/Buffer.h"
#include "crdt/OperationQueue.h"

using namespace CollabText::Crdt;

class TestOpQueue : public QObject {
    Q_OBJECT
private slots:

    void empty_queue_noop() {
        Buffer buf(1);
        buf.apply_ops({});
        QCOMPARE(buf.text(), std::string(""));
        QCOMPARE(buf.visible_length(), 0u);
    }
};

QTEST_MAIN(TestOpQueue)
#include "tst_opqueue.moc"
```

- [ ] **Step 3: Build and run the skeleton test**

Run: `cmake --build build-dev 2>&1 | tail -5`
Then: `./build-dev/libs/collabtext/tst_opqueue`
Expected: 1 test passes (PASS).

- [ ] **Step 4: Commit**

```
git add libs/collabtext/CMakeLists.txt libs/collabtext/tests/tst_opqueue.cpp
git commit -m "test: add tst_opqueue skeleton with empty_queue_noop test"
```

---

## Task 3: Modify Buffer.h — replace m_deferred with OperationQueue

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.h`

- [ ] **Step 1: Add include and replace member declarations**

Add `#include "crdt/OperationQueue.h"` with the other includes (after line 8, with the other crdt includes). Also add `#include <set>`.

Replace the block at lines 154-168:

```cpp
    /// Extract try_apply logic from the std::visit in apply_ops.
    bool try_apply(const Operation& op);

    uint16_t m_replica_id;
    Lamport m_clock;
    Global m_version;
    UndoMap m_undo_map;

    FragmentTree m_fragment_tree;
    InsertionIndex m_insertion_index;

    /// Rebuild the insertion index from the current fragment list.
    void rebuild_insertion_index(const std::vector<Fragment>& frags);

    /// Deferred operations awaiting causal dependencies.
    OperationQueue m_deferred_queue;
    std::set<uint16_t> m_deferred_replicas;
```

The `UndoEntry` struct, `m_undo_stack`, and `m_undo_cursor` that follow remain unchanged.

- [ ] **Step 2: Build — expect compile errors in Buffer.cpp**

Run: `cmake --build build-dev 2>&1 | grep "error:" | head -10`
Expected: Errors in `Buffer.cpp` referencing `m_deferred` (which no longer exists). This confirms the header change took effect.

- [ ] **Step 3: Commit (WIP)**

```
git add libs/collabtext/src/crdt/Buffer.h
git commit -m "wip: replace m_deferred vector with OperationQueue in Buffer.h"
```

---

## Task 4: Implement try_apply, rewrite apply_ops, rewrite retry_deferred

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.cpp:716-757`

- [ ] **Step 1: Add try_apply method**

Add this method before `apply_ops` (e.g. at line 714, before `// apply_ops`):

```cpp
bool Buffer::try_apply(const Operation& op) {
    return std::visit([this](const auto &o) -> bool {
        using T = std::decay_t<decltype(o)>;
        if constexpr (std::is_same_v<T, EditOperation>) {
            return apply_remote_edit(o);
        } else {
            return apply_remote_undo(o);
        }
    }, op);
}
```

- [ ] **Step 2: Rewrite apply_ops**

Replace the entire `Buffer::apply_ops` method (lines 716-733) with:

```cpp
void Buffer::apply_ops(const std::vector<Operation> &ops) {
    for (auto &op : ops) {
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

- [ ] **Step 3: Rewrite retry_deferred**

Replace the entire `Buffer::retry_deferred` method (lines 735-757) with:

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

- [ ] **Step 4: Build**

Run: `cmake --build build-dev 2>&1 | tail -5`
Expected: Clean build, no errors.

- [ ] **Step 5: Run ALL existing tests**

Run: `cd build-dev && ctest --output-on-failure`
Expected: All 7 tests pass (tst_clock, tst_locator, tst_buffer, tst_convergence, tst_sumtree, tst_anchor, tst_opqueue).

This is the critical correctness gate. The refactored apply_ops/retry_deferred must produce identical behavior to the old code for all existing tests, especially `tst_convergence` which exercises out-of-order delivery and duplication.

- [ ] **Step 6: Commit**

```
git add libs/collabtext/src/crdt/Buffer.cpp
git commit -m "feat: implement OperationQueue with deferred replica tracking

Replace std::vector<Operation> m_deferred with SumTree-backed
OperationQueue and per-replica tracking set. Extract try_apply()
helper. Deferred replicas are now skipped without attempting
application, reducing worst-case retry from O(n^2) to O(n)."
```

---

## Task 5: Write remaining test cases

**Files:**
- Modify: `libs/collabtext/tests/tst_opqueue.cpp`

- [ ] **Step 1: Write single_deferred_retries test**

Add to the test class:

```cpp
    void single_deferred_retries() {
        // Replica 1 inserts "hello", then deletes "ell".
        // Replica 2 receives delete first (deferred), then insert.
        Buffer bufA(1);
        auto op1 = bufA.apply_local_edit({{0, 0}}, {"hello"});
        auto op2 = bufA.apply_local_edit({{1, 4}}, {""});

        Buffer bufB(2);
        // op2 depends on op1 — should be deferred
        bufB.apply_ops({op2});
        QCOMPARE(bufB.text(), std::string(""));

        // Now deliver op1 — op2 should retry and succeed
        bufB.apply_ops({op1});
        QCOMPARE(bufB.text(), std::string("ho"));
    }
```

- [ ] **Step 2: Write deferred_replica_tracking test**

```cpp
    void deferred_replica_tracking() {
        // Replica 1 creates 3 ops in sequence: insert "abc", insert "d" at end,
        // delete "a". Send in reverse order to replica 2.
        Buffer bufA(1);
        auto op1 = bufA.apply_local_edit({{0, 0}}, {"abc"});
        auto op2 = bufA.apply_local_edit({{3, 3}}, {"d"});
        auto op3 = bufA.apply_local_edit({{0, 1}}, {""});

        Buffer bufB(2);
        // Send in reverse: op3, op2, op1
        // op3 depends on op1+op2 — deferred. Once replica 1 is marked deferred,
        // op2 should also be deferred without attempting application.
        bufB.apply_ops({op3});
        QCOMPARE(bufB.text(), std::string(""));

        bufB.apply_ops({op2});
        QCOMPARE(bufB.text(), std::string(""));

        // Deliver op1 — all should now apply
        bufB.apply_ops({op1});
        QCOMPARE(bufB.text(), std::string("bcd"));
    }
```

- [ ] **Step 3: Write mixed_replicas_partial_delivery test**

```cpp
    void mixed_replicas_partial_delivery() {
        // 3 replicas. A inserts "aa", B inserts "bb", C inserts "cc".
        // Deliver A's op to B and C. Deliver B's op to A (deferred initially? No,
        // B's op has no causal dependency on A). All ops are independent.
        // Then deliver everything and check convergence.
        Buffer bufA(1);
        Buffer bufB(2);
        Buffer bufC(3);

        auto opA = bufA.apply_local_edit({{0, 0}}, {"aa"});
        auto opB = bufB.apply_local_edit({{0, 0}}, {"bb"});
        auto opC = bufC.apply_local_edit({{0, 0}}, {"cc"});

        // Cross-deliver all ops
        bufA.apply_ops({opB, opC});
        bufB.apply_ops({opA, opC});
        bufC.apply_ops({opA, opB});

        // All three replicas must converge
        QCOMPARE(bufA.text(), bufB.text());
        QCOMPARE(bufB.text(), bufC.text());

        // All 6 characters must be present
        std::string result = bufA.text();
        QCOMPARE(result.size(), size_t(6));
        QVERIFY(result.find("aa") != std::string::npos);
        QVERIFY(result.find("bb") != std::string::npos);
        QVERIFY(result.find("cc") != std::string::npos);
    }
```

- [ ] **Step 4: Write stress_reverse_causal_order test**

```cpp
    void stress_reverse_causal_order() {
        // 100 sequential inserts from replica 1, delivered in reverse to replica 2.
        // With deferred replica tracking, this should be O(n) total retries,
        // not O(n^2). We time it to catch quadratic regression.
        Buffer bufA(1);
        std::vector<Operation> ops;
        for (int i = 0; i < 100; ++i) {
            uint32_t pos = bufA.visible_length();
            auto op = bufA.apply_local_edit({{pos, pos}}, {std::string(1, 'a' + (i % 26))});
            ops.push_back(op);
        }

        // Reverse the ops
        std::reverse(ops.begin(), ops.end());

        Buffer bufB(2);
        auto start = std::chrono::steady_clock::now();

        // Deliver one at a time in reverse order
        for (auto& op : ops) {
            bufB.apply_ops({op});
        }

        auto end = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        // Verify correctness
        QCOMPARE(bufB.text(), bufA.text());

        // Sanity timing check: with O(n) behavior this should be well under 1 second.
        // With O(n^2) on 100 ops it would still be fast, but this catches gross regressions.
        // The real protection is the algorithmic structure.
        qDebug() << "100 reverse-order ops applied in" << ms << "ms";
        QVERIFY2(ms < 5000, "Reverse causal order took too long — possible quadratic regression");
    }
```

- [ ] **Step 5: Write convergence_still_passes test**

```cpp
    void convergence_still_passes() {
        // Run a convergence-style test: 3 replicas, random edits with
        // out-of-order delivery, verify all converge.
        std::mt19937 rng(42);  // Deterministic seed

        Buffer bufA(1), bufB(2), bufC(3);
        std::vector<Operation> pendingB, pendingC;

        for (int i = 0; i < 30; ++i) {
            // Random edit on A
            uint32_t len = bufA.visible_length();
            uint32_t start = len > 0 ? (rng() % (len + 1)) : 0;
            uint32_t end = start + (len > start ? (rng() % (len - start + 1)) : 0);
            int ins_len = rng() % 4;
            std::string text;
            for (int j = 0; j < ins_len; ++j)
                text += static_cast<char>('a' + (rng() % 26));

            auto op = bufA.apply_local_edit({{start, end}}, {text});
            pendingB.push_back(op);
            pendingC.push_back(op);
        }

        // Deliver to B in random order
        std::shuffle(pendingB.begin(), pendingB.end(), rng);
        bufB.apply_ops(pendingB);

        // Deliver to C in reverse order
        std::reverse(pendingC.begin(), pendingC.end());
        bufC.apply_ops(pendingC);

        // Flush deferred
        for (int i = 0; i < 10; ++i) {
            bufB.apply_ops({});
            bufC.apply_ops({});
        }

        QCOMPARE(bufA.text(), bufB.text());
        QCOMPARE(bufA.text(), bufC.text());
    }
```

- [ ] **Step 6: Add required includes at top of test file**

The full top of the file should be:

```cpp
#include <QTest>
#include "crdt/Buffer.h"
#include "crdt/OperationQueue.h"
#include <algorithm>
#include <chrono>
#include <random>

using namespace CollabText::Crdt;
```

- [ ] **Step 7: Build and run all tst_opqueue tests**

Run: `cmake --build build-dev && ./build-dev/libs/collabtext/tst_opqueue`
Expected: All 6 tests pass.

- [ ] **Step 8: Run full test suite**

Run: `cd build-dev && ctest --output-on-failure`
Expected: All 7 test executables pass (including tst_convergence).

- [ ] **Step 9: Run convergence tests 10 times**

```bash
for i in $(seq 1 10); do
    ./build-dev/libs/collabtext/tst_convergence || echo "FAIL on run $i"
done
```

Expected: All 10 runs pass. (Convergence tests use random seeds, so repeated runs test different orderings.)

- [ ] **Step 10: Commit**

```
git add libs/collabtext/tests/tst_opqueue.cpp
git commit -m "test: add full tst_opqueue test suite (6 tests)

Tests cover: empty queue noop, single deferred retry, deferred
replica tracking, mixed replicas partial delivery, stress test
with 100 reverse-order ops, and convergence with random delivery."
```

---

## Acceptance Criteria

All of the following must be true before this batch is complete:

1. All 6 `tst_opqueue` tests pass
2. All existing `tst_buffer` tests pass (no regressions)
3. All `tst_convergence` tests pass on 10 consecutive runs
4. All `tst_anchor` tests pass
5. No new files beyond `Operations.h`, `OperationQueue.h`, and `tst_opqueue.cpp`
6. `m_deferred` vector no longer exists in `Buffer.h`
7. `m_deferred_replicas` is cleared at the start of each `retry_deferred()` pass
