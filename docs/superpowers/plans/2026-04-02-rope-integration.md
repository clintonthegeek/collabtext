# Rope Integration into Buffer — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Separate text storage from fragment metadata by maintaining two Ropes (`m_visible_text`, `m_deleted_text`) in Buffer, per spec §6.2 / Optimization 4.

**Architecture:** Every code path that mutates fragments calls `set_fragments()`. We rebuild both Ropes there from fragment content (O(n), same cost as the existing tree rebuild). `text()` switches to `m_visible_text.to_string()`. Debug assertions enforce rope–tree byte parity. `Fragment::content` is retained for now (spec §4.5).

**Tech Stack:** C++20, Qt6 Test, SumTree, Rope (already implemented in `src/crdt/Rope.h`)

---

## File Map

| File | Action | Responsibility |
|------|--------|---------------|
| `libs/collabtext/src/crdt/Buffer.h` | Modify | Add `#include "crdt/Rope.h"`, `Rope m_visible_text`, `Rope m_deleted_text`, test accessors |
| `libs/collabtext/src/crdt/Buffer.cpp` | Modify | Rebuild ropes in `set_fragments()`, use rope in `text()`, add assertions, add accessors |
| `libs/collabtext/tests/tst_rope_integration.cpp` | Create | 7 tests verifying rope tracks all mutation paths |
| `libs/collabtext/tests/tst_fuzz.cpp` | Modify | Add INV-8 (rope consistency) to `check_invariants()` |
| `libs/collabtext/CMakeLists.txt` | Modify | Add `add_crdt_test(tst_rope_integration)` |

---

### Task 1: Rope scaffolding in Buffer + first failing test

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.h`
- Modify: `libs/collabtext/src/crdt/Buffer.cpp`
- Create: `libs/collabtext/tests/tst_rope_integration.cpp`
- Modify: `libs/collabtext/CMakeLists.txt`

- [ ] **Step 1: Create tst_rope_integration.cpp with rope_tracks_inserts test**

Create `libs/collabtext/tests/tst_rope_integration.cpp`:

```cpp
#include <QTest>
#include "crdt/Buffer.h"

using namespace CollabText::Crdt;

class TestRopeIntegration : public QObject {
    Q_OBJECT
private slots:

    void rope_tracks_inserts() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        QCOMPARE(buf.visible_rope_len(), 5u);
        QCOMPARE(buf.deleted_rope_len(), 0u);
        QCOMPARE(buf.text(), std::string("hello"));

        buf.apply_local_edit({{5, 5}}, {" world"});
        QCOMPARE(buf.visible_rope_len(), 11u);
        QCOMPARE(buf.deleted_rope_len(), 0u);
        QCOMPARE(buf.text(), std::string("hello world"));
    }
};

QTEST_MAIN(TestRopeIntegration)
#include "tst_rope_integration.moc"
```

- [ ] **Step 2: Register the test in CMakeLists.txt**

Add to `libs/collabtext/CMakeLists.txt` after the `add_crdt_test(tst_fuzz)` line:

```cmake
add_crdt_test(tst_rope_integration)
```

- [ ] **Step 3: Add Rope members and test accessors to Buffer.h**

In `libs/collabtext/src/crdt/Buffer.h`, add the include after the existing includes (after line 6, the `#include "crdt/Fragment.h"` line):

```cpp
#include "crdt/Rope.h"
```

Add public test accessors after the `fragments()` method (after line 71):

```cpp
    /// For testing: visible rope byte length.
    uint32_t visible_rope_len() const;

    /// For testing: deleted rope byte length.
    uint32_t deleted_rope_len() const;
```

Add private Rope members after `m_insertion_index` (after line 139):

```cpp
    Rope m_visible_text;
    Rope m_deleted_text;
```

- [ ] **Step 4: Add stub accessors to Buffer.cpp**

In `libs/collabtext/src/crdt/Buffer.cpp`, add after the `visible_length()` method (after line 70):

```cpp
uint32_t Buffer::visible_rope_len() const {
    return m_visible_text.len();
}

uint32_t Buffer::deleted_rope_len() const {
    return m_deleted_text.len();
}
```

- [ ] **Step 5: Build and verify the test fails**

Run:
```bash
cmake --build build-dev --target tst_rope_integration 2>&1
./build-dev/libs/collabtext/tst_rope_integration
```

Expected: Compiles successfully. Test FAILS because `visible_rope_len()` returns 0 (ropes are never populated).

- [ ] **Step 6: Commit scaffolding**

```bash
git add libs/collabtext/src/crdt/Buffer.h libs/collabtext/src/crdt/Buffer.cpp \
       libs/collabtext/tests/tst_rope_integration.cpp libs/collabtext/CMakeLists.txt
git commit -m "feat: add Rope members to Buffer with test scaffolding (red)"
```

---

### Task 2: Implement rope rebuild and text() via rope

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.cpp:23-32` (set_fragments)
- Modify: `libs/collabtext/src/crdt/Buffer.cpp:59-66` (text)

- [ ] **Step 1: Modify set_fragments() to rebuild ropes from fragment content**

Replace `set_fragments()` in `libs/collabtext/src/crdt/Buffer.cpp` (the entire method, lines 23-32) with:

```cpp
void Buffer::set_fragments(std::vector<Fragment>&& frags) {
    // Set cached visibility flags based on current undo map
    for (auto& f : frags) {
        f.visible = f.compute_visible(m_undo_map);
    }

    // Rebuild ropes from fragment content
    Rope visible_rope, deleted_rope;
    for (const auto& f : frags) {
        if (f.visible) {
            visible_rope.push_str(f.content);
        } else {
            deleted_rope.push_str(f.content);
        }
    }
    m_visible_text = std::move(visible_rope);
    m_deleted_text = std::move(deleted_rope);

    rebuild_insertion_index(frags);
    FragmentTree tree;
    for (auto& f : frags) tree.push_item(std::move(f));
    m_fragment_tree = std::move(tree);

    // Rope consistency invariant (debug builds)
    assert(m_visible_text.len() == m_fragment_tree.summary().visible_bytes);
    assert(m_deleted_text.len() == m_fragment_tree.summary().deleted_bytes);
}
```

- [ ] **Step 2: Modify text() to use rope**

Replace `text()` in `libs/collabtext/src/crdt/Buffer.cpp` (lines 59-66) with:

```cpp
std::string Buffer::text() const {
    return m_visible_text.to_string();
}
```

- [ ] **Step 3: Run the failing test — it should now pass**

Run:
```bash
cmake --build build-dev --target tst_rope_integration 2>&1
./build-dev/libs/collabtext/tst_rope_integration
```

Expected: PASS — `rope_tracks_inserts` passes.

- [ ] **Step 4: Run the full existing test suite to verify no regressions**

Run:
```bash
cmake --build build-dev 2>&1 && cd build-dev && ctest --output-on-failure 2>&1
```

Expected: All 187 existing tests pass (now 188 with the new one).

- [ ] **Step 5: Commit**

```bash
git add libs/collabtext/src/crdt/Buffer.cpp
git commit -m "feat: rebuild Ropes in set_fragments, text() via rope"
```

---

### Task 3: Complete rope integration test suite

**Files:**
- Modify: `libs/collabtext/tests/tst_rope_integration.cpp`

- [ ] **Step 1: Add remaining 6 tests**

Replace the full content of `libs/collabtext/tests/tst_rope_integration.cpp` with:

```cpp
#include <QTest>
#include "crdt/Buffer.h"
#include <random>

using namespace CollabText::Crdt;

class TestRopeIntegration : public QObject {
    Q_OBJECT
private slots:

    void rope_tracks_inserts() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        QCOMPARE(buf.visible_rope_len(), 5u);
        QCOMPARE(buf.deleted_rope_len(), 0u);
        QCOMPARE(buf.text(), std::string("hello"));

        buf.apply_local_edit({{5, 5}}, {" world"});
        QCOMPARE(buf.visible_rope_len(), 11u);
        QCOMPARE(buf.deleted_rope_len(), 0u);
        QCOMPARE(buf.text(), std::string("hello world"));
    }

    void rope_tracks_deletes() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello world"});
        QCOMPARE(buf.visible_rope_len(), 11u);
        QCOMPARE(buf.deleted_rope_len(), 0u);

        // Delete " world"
        buf.apply_local_edit({{5, 11}}, {""});
        QCOMPARE(buf.visible_rope_len(), 5u);
        QCOMPARE(buf.deleted_rope_len(), 6u);
        QCOMPARE(buf.text(), std::string("hello"));
    }

    void rope_tracks_undo() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello world"});
        buf.apply_local_edit({{5, 11}}, {""}); // delete " world"
        QCOMPARE(buf.visible_rope_len(), 5u);
        QCOMPARE(buf.deleted_rope_len(), 6u);

        buf.undo(); // undo the delete — " world" becomes visible again
        QCOMPARE(buf.visible_rope_len(), 11u);
        QCOMPARE(buf.text(), std::string("hello world"));
    }

    void rope_tracks_redo() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello world"});
        buf.apply_local_edit({{5, 11}}, {""}); // delete " world"
        buf.undo();
        QCOMPARE(buf.visible_rope_len(), 11u);

        buf.redo(); // re-delete " world"
        QCOMPARE(buf.visible_rope_len(), 5u);
        QCOMPARE(buf.deleted_rope_len(), 6u);
        QCOMPARE(buf.text(), std::string("hello"));
    }

    void rope_tracks_remote_edit() {
        Buffer a(1), b(2);

        auto op1 = a.apply_local_edit({{0, 0}}, {"hello"});
        b.apply_ops({op1});
        QCOMPARE(b.visible_rope_len(), 5u);
        QCOMPARE(b.deleted_rope_len(), 0u);
        QCOMPARE(b.text(), std::string("hello"));

        // Remote replace: delete "hello", insert "world"
        auto op2 = a.apply_local_edit({{0, 5}}, {"world"});
        b.apply_ops({op2});
        QCOMPARE(b.text(), std::string("world"));
        QCOMPARE(b.visible_rope_len(), 5u);
        QCOMPARE(b.deleted_rope_len(), 5u);
    }

    void rope_consistency_invariant() {
        // 100 random edits — after each, assert rope byte totals match fragments.
        std::mt19937 rng(42);
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"the quick brown fox jumps over the lazy dog"});

        for (int i = 0; i < 100; ++i) {
            uint32_t len = buf.visible_length();
            if (len == 0) {
                buf.apply_local_edit({{0, 0}}, {"x"});
            } else {
                std::string text = buf.text();
                uint32_t start = rng() % len;
                uint32_t end = start + (rng() % (len - start + 1));
                // Snap to UTF-8 boundaries
                while (start < text.size() &&
                       (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80)
                    start++;
                while (end < text.size() &&
                       (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
                    end++;
                if (end > static_cast<uint32_t>(text.size()))
                    end = static_cast<uint32_t>(text.size());
                if (start > end) start = end;

                std::string replacement = (rng() % 2) ? "XX" : "";
                buf.apply_local_edit({{start, end}}, {replacement});
            }

            // Rope totals must match fragment byte sums
            auto frags = buf.fragments();
            uint32_t total_bytes = 0;
            for (auto& f : frags)
                total_bytes += static_cast<uint32_t>(f.content.size());
            QCOMPARE(buf.visible_rope_len() + buf.deleted_rope_len(), total_bytes);
            QCOMPARE(buf.visible_rope_len(), buf.visible_length());
        }
    }

    void rope_survives_convergence() {
        Buffer a(1), b(2), c(3);

        auto op1 = a.apply_local_edit({{0, 0}}, {"hello"});
        auto op2 = b.apply_local_edit({{0, 0}}, {"world"});

        // Cross-apply
        a.apply_ops({op2});
        b.apply_ops({op1});
        c.apply_ops({op1, op2});

        // All replicas converge
        QCOMPARE(a.text(), b.text());
        QCOMPARE(b.text(), c.text());

        // Rope consistency on every replica
        for (auto* buf : {&a, &b, &c}) {
            auto frags = buf->fragments();
            uint32_t total = 0;
            for (auto& f : frags)
                total += static_cast<uint32_t>(f.content.size());
            QCOMPARE(buf->visible_rope_len() + buf->deleted_rope_len(), total);
            QCOMPARE(buf->visible_rope_len(), buf->visible_length());
        }
    }
};

QTEST_MAIN(TestRopeIntegration)
#include "tst_rope_integration.moc"
```

- [ ] **Step 2: Build and run**

Run:
```bash
cmake --build build-dev --target tst_rope_integration 2>&1
./build-dev/libs/collabtext/tst_rope_integration -v2
```

Expected: All 7 tests PASS.

- [ ] **Step 3: Commit**

```bash
git add libs/collabtext/tests/tst_rope_integration.cpp
git commit -m "test: complete rope integration suite (7 tests)"
```

---

### Task 4: Add rope invariant to fuzz suite + full verification

**Files:**
- Modify: `libs/collabtext/tests/tst_fuzz.cpp`

- [ ] **Step 1: Add INV-8 to check_invariants()**

In `libs/collabtext/tests/tst_fuzz.cpp`, add the following after the INV-7 block (after line 106, before the closing `}` of `check_invariants`):

```cpp
    // INV-8: rope consistency — rope byte lengths match fragment sums
    if (buf.visible_rope_len() != vis_sum) {
        QFAIL(qPrintable(QString("INV-8 violated at %1: visible_rope_len=%2 but fragment vis_sum=%3")
            .arg(context).arg(buf.visible_rope_len()).arg(vis_sum)));
    }
    if (buf.deleted_rope_len() != del_sum) {
        QFAIL(qPrintable(QString("INV-8 violated at %1: deleted_rope_len=%2 but fragment del_sum=%3")
            .arg(context).arg(buf.deleted_rope_len()).arg(del_sum)));
    }
```

Note: `vis_sum` and `del_sum` are already computed in INV-2 (lines 28-34).

- [ ] **Step 2: Build and run fuzz suite**

Run:
```bash
cmake --build build-dev --target tst_fuzz 2>&1
./build-dev/libs/collabtext/tst_fuzz -v2
```

Expected: All 15 fuzz tests PASS with the new INV-8 check.

- [ ] **Step 3: Run full test suite**

Run:
```bash
cmake --build build-dev 2>&1 && cd build-dev && ctest --output-on-failure 2>&1
```

Expected: All 194 tests pass (187 existing + 7 new rope integration).

- [ ] **Step 4: Run convergence 10 times**

Run:
```bash
for i in $(seq 1 10); do ./build-dev/libs/collabtext/tst_convergence -silent && echo "Run $i: PASS"; done
```

Expected: 10/10 PASS.

- [ ] **Step 5: Run fuzz suite 10 times (different random seeds each run)**

Run:
```bash
for i in $(seq 1 10); do ./build-dev/libs/collabtext/tst_fuzz -silent && echo "Fuzz $i: PASS"; done
```

Expected: 10/10 PASS.

- [ ] **Step 6: Commit**

```bash
git add libs/collabtext/tests/tst_fuzz.cpp
git commit -m "test: add rope consistency invariant (INV-8) to fuzz suite"
```
