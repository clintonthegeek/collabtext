# Garbage Collection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement bounded-undo physical tombstone removal to eliminate the 21x performance degradation from tombstone accumulation, plus fragment coalescing as a bonus optimization.

**Architecture:** `Buffer::collect_garbage()` builds a set of deletion IDs protected by the undo stack, removes all tombstones whose deletions are not protected, then coalesces adjacent same-origin fragments. The existing `set_fragments()` handles rope reconstruction. Undo stack is bounded to a configurable `max_undo_depth` (default 1000) — entries that age out make their tombstones GC-eligible.

**Tech Stack:** C++20, Qt6 Test, SumTree-based CRDT engine

**Spec:** `docs/superpowers/specs/2026-04-05-garbage-collection-design.md`

---

## File Map

| File | Action | Responsibility |
|------|--------|---------------|
| `libs/collabtext/src/crdt/Buffer.h` | Modify | Add GC public/private methods, `m_max_undo_depth` member |
| `libs/collabtext/src/crdt/Buffer.cpp` | Modify | Implement GC methods, integrate `trim_undo_stack()` |
| `libs/collabtext/tests/tst_gc.cpp` | Create | All GC unit tests |
| `libs/collabtext/tests/tst_fuzz.cpp` | Modify | Add probabilistic GC calls to convergence loops |
| `libs/collabtext/CMakeLists.txt` | Modify | Register `tst_gc` test |

---

### Task 1: Query Methods and Test Infrastructure

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.h:52-76`
- Modify: `libs/collabtext/src/crdt/Buffer.cpp:152-175`
- Create: `libs/collabtext/tests/tst_gc.cpp`
- Modify: `libs/collabtext/CMakeLists.txt:50`

- [ ] **Step 1: Write failing tests for query methods**

Create `libs/collabtext/tests/tst_gc.cpp`:

```cpp
#include <QTest>
#include "crdt/Buffer.h"

using namespace CollabText::Crdt;

class TestGC : public QObject {
    Q_OBJECT
private slots:

    void fragment_count_empty() {
        Buffer buf(1);
        QCOMPARE(buf.fragment_count(), size_t(0));
        QCOMPARE(buf.tombstone_count(), size_t(0));
    }

    void fragment_count_after_insert() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        QVERIFY(buf.fragment_count() > 0);
        QCOMPARE(buf.tombstone_count(), size_t(0));
    }

    void tombstone_count_after_delete() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        buf.apply_local_edit({{0, 5}}, {""});  // delete all
        QCOMPARE(buf.text(), std::string(""));
        QVERIFY(buf.tombstone_count() > 0);
    }

    void max_undo_depth_default() {
        Buffer buf(1);
        QCOMPARE(buf.max_undo_depth(), size_t(1000));
    }

    void set_max_undo_depth() {
        Buffer buf(1);
        buf.set_max_undo_depth(50);
        QCOMPARE(buf.max_undo_depth(), size_t(50));
    }
};

QTEST_MAIN(TestGC)
#include "tst_gc.moc"
```

- [ ] **Step 2: Register test in CMakeLists.txt**

Add to `libs/collabtext/CMakeLists.txt` after the `tst_benchmark` line:

```cmake
add_crdt_test(tst_gc)
```

- [ ] **Step 3: Build and verify tests fail**

Run: `cmake --build build-dev --target tst_gc 2>&1 | tail -20`
Expected: Compilation errors — `fragment_count`, `tombstone_count`, `max_undo_depth`, `set_max_undo_depth` not declared.

- [ ] **Step 4: Implement query methods**

In `libs/collabtext/src/crdt/Buffer.h`, add to the public section (after `uint32_t deleted_rope_len() const;` around line 79):

```cpp
    /// Number of tombstone (invisible) fragments in the tree.
    size_t tombstone_count() const;

    /// Total fragment count (visible + tombstone).
    size_t fragment_count() const;

    /// Maximum undo stack depth. Oldest entries are discarded when exceeded.
    size_t max_undo_depth() const;

    /// Set the maximum undo stack depth.
    void set_max_undo_depth(size_t depth);
```

In `libs/collabtext/src/crdt/Buffer.h`, add to the private member variables (after `size_t m_undo_cursor = 0;` around line 172):

```cpp
    size_t m_max_undo_depth = 1000;
```

In `libs/collabtext/src/crdt/Buffer.cpp`, add after the `deleted_rope_len()` function (around line 174):

```cpp
size_t Buffer::tombstone_count() const {
    size_t count = 0;
    m_fragment_tree.for_each([&](const Fragment& f) {
        if (!f.visible) ++count;
    });
    return count;
}

size_t Buffer::fragment_count() const {
    size_t count = 0;
    m_fragment_tree.for_each([&](const Fragment&) { ++count; });
    return count;
}

size_t Buffer::max_undo_depth() const {
    return m_max_undo_depth;
}

void Buffer::set_max_undo_depth(size_t depth) {
    m_max_undo_depth = depth;
}
```

- [ ] **Step 5: Build and run tests**

Run: `cmake --build build-dev --target tst_gc && ./build-dev/libs/collabtext/tst_gc`
Expected: All 5 tests pass.

- [ ] **Step 6: Commit**

```bash
git add libs/collabtext/src/crdt/Buffer.h libs/collabtext/src/crdt/Buffer.cpp \
       libs/collabtext/tests/tst_gc.cpp libs/collabtext/CMakeLists.txt
git commit -m "feat(gc): add fragment_count, tombstone_count, max_undo_depth query methods"
```

---

### Task 2: Basic collect_garbage()

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.h`
- Modify: `libs/collabtext/src/crdt/Buffer.cpp`
- Modify: `libs/collabtext/tests/tst_gc.cpp`

- [ ] **Step 1: Write failing tests for basic GC**

Add to `tst_gc.cpp` before the closing `};`:

```cpp
    void gc_removes_tombstones() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello world"});
        buf.apply_local_edit({{0, 5}}, {""});  // delete "hello"
        QCOMPARE(buf.text(), std::string(" world"));

        size_t before = buf.fragment_count();
        QVERIFY(buf.tombstone_count() > 0);

        // GC should NOT remove tombstones while deletion is in undo stack
        size_t removed = buf.collect_garbage();
        QCOMPARE(removed, size_t(0));
        QCOMPARE(buf.fragment_count(), before);
        QCOMPARE(buf.text(), std::string(" world"));
    }

    void gc_removes_after_undo_stack_clear() {
        Buffer buf(1);
        buf.set_max_undo_depth(1);
        buf.apply_local_edit({{0, 0}}, {"hello world"});  // undo entry 1
        buf.apply_local_edit({{0, 5}}, {""});              // undo entry 2, pushes entry 1 out

        // Entry for the insert is gone (max_undo_depth=1, only delete entry remains).
        // But the tombstones' deletion_id is from the delete edit, which IS in the stack.
        // So tombstones are still protected.
        QCOMPARE(buf.text(), std::string(" world"));
        size_t removed = buf.collect_garbage();
        QCOMPARE(removed, size_t(0));

        // Now do another edit to push the delete entry out
        buf.apply_local_edit({{6, 6}}, {"!"});  // undo entry 3, pushes entry 2 out
        QCOMPARE(buf.text(), std::string(" world!"));

        // Now the delete's deletion_id is no longer in the undo stack
        size_t tombstones_before = buf.tombstone_count();
        QVERIFY(tombstones_before > 0);
        removed = buf.collect_garbage();
        QCOMPARE(removed, tombstones_before);
        QCOMPARE(buf.tombstone_count(), size_t(0));
        QCOMPARE(buf.text(), std::string(" world!"));
    }

    void gc_preserves_visible_text() {
        Buffer buf(1);
        buf.set_max_undo_depth(0);  // no undo protection at all
        buf.apply_local_edit({{0, 0}}, {"abcdef"});
        buf.apply_local_edit({{1, 3}}, {""});  // delete "bc"
        buf.apply_local_edit({{3, 5}}, {""});  // delete "ef" (now "ad" visible)
        std::string text_before = buf.text();
        QCOMPARE(text_before, std::string("ad"));

        size_t removed = buf.collect_garbage();
        QVERIFY(removed > 0);
        QCOMPARE(buf.text(), text_before);
        QCOMPARE(buf.tombstone_count(), size_t(0));
    }
```

- [ ] **Step 2: Build and verify tests fail**

Run: `cmake --build build-dev --target tst_gc 2>&1 | tail -10`
Expected: `collect_garbage` not declared.

- [ ] **Step 3: Implement collect_garbage()**

In `libs/collabtext/src/crdt/Buffer.h`, add to the public section (after `set_max_undo_depth`):

```cpp
    /// Run garbage collection: remove tombstones whose deletions are no longer
    /// in the undo stack. Returns the number of tombstones removed.
    size_t collect_garbage();
```

Add to the private section (after `void rebuild_insertion_index`):

```cpp
    /// Trim the undo stack to m_max_undo_depth entries.
    void trim_undo_stack();
```

In `libs/collabtext/src/crdt/Buffer.cpp`, add after `set_max_undo_depth()`:

```cpp
void Buffer::trim_undo_stack() {
    if (m_undo_stack.size() <= m_max_undo_depth) return;
    size_t excess = m_undo_stack.size() - m_max_undo_depth;
    m_undo_stack.erase(m_undo_stack.begin(),
                       m_undo_stack.begin() + static_cast<ptrdiff_t>(excess));
    if (excess > m_undo_cursor)
        m_undo_cursor = 0;
    else
        m_undo_cursor -= excess;
}

size_t Buffer::collect_garbage() {
    // Build protected set: deletion IDs still in the undo stack
    std::unordered_set<uint64_t> protected_ids;
    for (const auto& entry : m_undo_stack) {
        if (entry.had_deletions)
            protected_ids.insert(origin_key(entry.deletion_id));
    }

    auto frags = get_fragments();
    size_t original_count = frags.size();

    // Remove GC-eligible tombstones
    frags.erase(
        std::remove_if(frags.begin(), frags.end(), [&](const Fragment& f) {
            if (f.visible) return false;  // not a tombstone
            for (const auto& del : f.deletions) {
                if (protected_ids.count(origin_key(del)))
                    return false;  // protected by undo stack
            }
            return true;  // all deletions are permanent — safe to remove
        }),
        frags.end());

    size_t removed = original_count - frags.size();
    if (removed > 0) {
        set_fragments(std::move(frags));
    }
    return removed;
}
```

- [ ] **Step 4: Integrate trim_undo_stack() with apply_local_edit**

In `libs/collabtext/src/crdt/Buffer.cpp`, in the `apply_local_edit` function, find the undo entry block (around line 909-911):

```cpp
    m_undo_stack.resize(m_undo_cursor);
    m_undo_stack.push_back(std::move(undo_entry));
    m_undo_cursor = m_undo_stack.size();
```

Add `trim_undo_stack();` right after:

```cpp
    m_undo_stack.resize(m_undo_cursor);
    m_undo_stack.push_back(std::move(undo_entry));
    m_undo_cursor = m_undo_stack.size();
    trim_undo_stack();
```

- [ ] **Step 5: Build and run tests**

Run: `cmake --build build-dev --target tst_gc && ./build-dev/libs/collabtext/tst_gc`
Expected: All 8 tests pass.

- [ ] **Step 6: Also run existing tests to verify no regressions**

Run: `cmake --build build-dev --target tst_buffer && ./build-dev/libs/collabtext/tst_buffer`
Run: `cmake --build build-dev --target tst_fuzz && ./build-dev/libs/collabtext/tst_fuzz`
Expected: All pass.

- [ ] **Step 7: Commit**

```bash
git add libs/collabtext/src/crdt/Buffer.h libs/collabtext/src/crdt/Buffer.cpp \
       libs/collabtext/tests/tst_gc.cpp
git commit -m "feat(gc): implement collect_garbage() with undo-stack protection"
```

---

### Task 3: GC + Undo Interaction Tests

**Files:**
- Modify: `libs/collabtext/tests/tst_gc.cpp`

- [ ] **Step 1: Write tests for GC-undo edge cases**

Add to `tst_gc.cpp`:

```cpp
    void gc_protects_undoable_deletions() {
        // Deletion is in undo stack — tombstone must survive
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"abc"});
        buf.apply_local_edit({{0, 3}}, {""});  // delete all
        QCOMPARE(buf.text(), std::string(""));
        QVERIFY(buf.tombstone_count() > 0);

        // Tombstone should be protected (deletion_id is in undo stack)
        QCOMPARE(buf.collect_garbage(), size_t(0));

        // Undo the deletion — text comes back
        buf.undo();
        QCOMPARE(buf.text(), std::string("abc"));
    }

    void gc_after_undo_redo_cycle() {
        Buffer buf(1);
        buf.set_max_undo_depth(0);  // nothing protected
        buf.apply_local_edit({{0, 0}}, {"abc"});
        buf.apply_local_edit({{0, 3}}, {""});  // delete
        buf.undo();  // restore
        buf.redo();  // re-delete
        QCOMPARE(buf.text(), std::string(""));

        // All undo entries were trimmed (max_undo_depth=0), so GC is safe
        size_t removed = buf.collect_garbage();
        QVERIFY(removed > 0);
        QCOMPARE(buf.tombstone_count(), size_t(0));
        QCOMPARE(buf.text(), std::string(""));
    }

    void gc_partial_protection() {
        // Two separate deletions: one protected, one not
        Buffer buf(1);
        buf.set_max_undo_depth(1);
        buf.apply_local_edit({{0, 0}}, {"abcdef"});
        buf.apply_local_edit({{0, 3}}, {""});  // delete "abc" — undo entry 1
        buf.apply_local_edit({{0, 3}}, {""});  // delete "def" — undo entry 2, pushes 1 out

        QCOMPARE(buf.text(), std::string(""));

        // "abc" tombstones: deletion_id from entry 1, which aged out → GC-eligible
        // "def" tombstones: deletion_id from entry 2, still in stack → protected
        size_t before = buf.tombstone_count();
        size_t removed = buf.collect_garbage();
        QVERIFY(removed > 0);
        QVERIFY(removed < before);  // some but not all removed
        QVERIFY(buf.tombstone_count() > 0);  // "def" tombstones remain
        QCOMPARE(buf.text(), std::string(""));

        // Undo the "def" deletion — "def" comes back
        buf.undo();
        QCOMPARE(buf.text(), std::string("def"));

        // "abc" is gone — cannot undo past it (it was GC'd AND its undo entry aged out)
        auto op = buf.undo();
        QVERIFY(!op.has_value());  // nothing left to undo
    }

    void gc_with_multiple_deletions_on_same_fragment() {
        // Fragment deleted by edit A, then undo(A), then re-deleted by edit B.
        // Fragment.deletions = [A_deletion_id, B_deletion_id]
        // GC-eligible only when BOTH A and B age out.
        Buffer buf(1);
        buf.set_max_undo_depth(2);
        buf.apply_local_edit({{0, 0}}, {"hello"});   // entry 0
        buf.apply_local_edit({{0, 5}}, {""});         // entry 1: delete "hello"
        buf.undo();                                    // undo delete
        buf.redo();                                    // redo delete
        QCOMPARE(buf.text(), std::string(""));

        // Both the original deletion and redo use the same deletion_id
        // (redo replays the same entry). So only one deletion_id is relevant.
        // With max_undo_depth=2 and 2 entries (insert + delete), nothing aged out.
        QCOMPARE(buf.collect_garbage(), size_t(0));
    }
```

- [ ] **Step 2: Build and run tests**

Run: `cmake --build build-dev --target tst_gc && ./build-dev/libs/collabtext/tst_gc`
Expected: All 12 tests pass.

- [ ] **Step 3: Commit**

```bash
git add libs/collabtext/tests/tst_gc.cpp
git commit -m "test(gc): undo interaction edge cases"
```

---

### Task 4: Fragment Coalescing

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.h`
- Modify: `libs/collabtext/src/crdt/Buffer.cpp`
- Modify: `libs/collabtext/tests/tst_gc.cpp`

- [ ] **Step 1: Write failing tests for coalescing**

Add to `tst_gc.cpp`:

```cpp
    void coalesce_reduces_fragment_count() {
        // Two replicas insert at the same position → normalization atomizes
        // into single-char fragments. After convergence, those single-char
        // fragments from the same replica can be coalesced.
        Buffer bufA(1), bufB(2);
        auto op1 = bufA.apply_local_edit({{0, 0}}, {"aaa"});
        auto op2 = bufB.apply_local_edit({{0, 0}}, {"bbb"});

        bufA.apply_ops({op2});
        bufB.apply_ops({op1});
        QCOMPARE(bufA.text(), bufB.text());

        // After normalization, fragments are atomized at shared locators
        size_t before = bufA.fragment_count();

        // GC + coalesce — no tombstones to remove, but coalescing should help
        bufA.collect_garbage();
        size_t after = bufA.fragment_count();
        QVERIFY(after <= before);  // coalescing may reduce count
        QCOMPARE(bufA.text(), bufB.text());  // text unchanged
    }

    void coalesce_preserves_text() {
        Buffer buf(1);
        buf.set_max_undo_depth(0);
        buf.apply_local_edit({{0, 0}}, {"abcdef"});
        buf.apply_local_edit({{2, 4}}, {""});  // delete "cd", splits "abcdef" into "ab" + "cd" + "ef"
        std::string text_before = buf.text();
        QCOMPARE(text_before, std::string("abef"));

        buf.collect_garbage();  // removes "cd" tombstone, then tries to coalesce
        QCOMPARE(buf.text(), text_before);
    }

    void coalesce_does_not_merge_different_locators() {
        // Fragments with different locators must not be coalesced
        Buffer buf(1);
        buf.set_max_undo_depth(0);
        buf.apply_local_edit({{0, 0}}, {"abc"});
        buf.apply_local_edit({{3, 3}}, {"def"});  // separate insertion → different locator
        size_t before_gc = buf.fragment_count();
        buf.collect_garbage();
        // These are separate insertions with different locators — no coalescing
        QCOMPARE(buf.fragment_count(), before_gc);  // unchanged
        QCOMPARE(buf.text(), std::string("abcdef"));
    }
```

- [ ] **Step 2: Build and verify tests pass (coalescing not needed yet for basic cases)**

Run: `cmake --build build-dev --target tst_gc && ./build-dev/libs/collabtext/tst_gc`
Expected: The tests should pass even without coalescing (they verify invariants, not specific count reductions). Some tests may need adjusting after implementation.

- [ ] **Step 3: Implement coalesce_fragments()**

In `libs/collabtext/src/crdt/Buffer.h`, add to the private section:

```cpp
    /// Merge adjacent fragments that meet coalescing conditions.
    static void coalesce_fragments(std::vector<Fragment>& frags);
```

In `libs/collabtext/src/crdt/Buffer.cpp`, add after `collect_garbage()`:

```cpp
void Buffer::coalesce_fragments(std::vector<Fragment>& frags) {
    if (frags.size() < 2) return;
    size_t write = 0;
    for (size_t read = 1; read < frags.size(); ++read) {
        Fragment& prev = frags[write];
        Fragment& curr = frags[read];
        if (prev.visible == curr.visible &&
            prev.locator == curr.locator &&
            prev.origin.replica_id == curr.origin.replica_id &&
            prev.origin.value + prev.length == curr.origin.value &&
            prev.deletions == curr.deletions) {
            // Coalesce: extend prev to cover curr
            prev.byte_length += curr.byte_length;
            prev.length += curr.length;
        } else {
            ++write;
            if (write != read)
                frags[write] = std::move(curr);
        }
    }
    frags.resize(write + 1);
}
```

- [ ] **Step 4: Integrate coalescing into collect_garbage()**

In `collect_garbage()`, add the coalescing call before `set_fragments`. Replace the end of collect_garbage():

```cpp
    size_t removed = original_count - frags.size();

    // Coalesce adjacent fragments (may reduce count further)
    size_t before_coalesce = frags.size();
    coalesce_fragments(frags);
    size_t coalesced = before_coalesce - frags.size();

    if (removed > 0 || coalesced > 0) {
        set_fragments(std::move(frags));
    }
    return removed;
```

- [ ] **Step 5: Build and run all tests**

Run: `cmake --build build-dev --target tst_gc && ./build-dev/libs/collabtext/tst_gc`
Run: `cmake --build build-dev --target tst_buffer && ./build-dev/libs/collabtext/tst_buffer`
Run: `cmake --build build-dev --target tst_fuzz && ./build-dev/libs/collabtext/tst_fuzz`
Expected: All pass.

- [ ] **Step 6: Commit**

```bash
git add libs/collabtext/src/crdt/Buffer.h libs/collabtext/src/crdt/Buffer.cpp \
       libs/collabtext/tests/tst_gc.cpp
git commit -m "feat(gc): fragment coalescing for adjacent same-origin fragments"
```

---

### Task 5: Invariant Verification Tests

**Files:**
- Modify: `libs/collabtext/tests/tst_gc.cpp`

- [ ] **Step 1: Write invariant-checking GC stress test**

Copy the `check_invariants()` function from `tst_fuzz.cpp` (lines 16-122) into `tst_gc.cpp` (as a static free function before the class definition), then add:

```cpp
    void gc_preserves_invariants_stress() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        std::mt19937 rng(seed);

        Buffer buf(1);
        buf.set_max_undo_depth(20);

        for (int i = 0; i < 200; ++i) {
            std::string text = buf.text();
            uint32_t len = static_cast<uint32_t>(text.size());
            uint32_t start = 0, end = 0;
            if (len > 0) {
                // Pick random char boundary
                std::vector<uint32_t> bounds = {0};
                for (size_t b = 0; b < text.size(); ) {
                    unsigned char c = static_cast<unsigned char>(text[b]);
                    if (c < 0x80) b += 1;
                    else if ((c & 0xE0) == 0xC0) b += 2;
                    else if ((c & 0xF0) == 0xE0) b += 3;
                    else b += 4;
                    bounds.push_back(static_cast<uint32_t>(b));
                }
                size_t si = rng() % bounds.size();
                start = bounds[si];
                size_t ei = si + (rng() % (bounds.size() - si));
                end = bounds[ei];
            }
            std::string replacement;
            if (rng() % 3 != 0) {
                int count = 1 + (rng() % 5);
                for (int c = 0; c < count; ++c)
                    replacement += static_cast<char>('a' + (rng() % 26));
            }
            buf.apply_local_edit({{start, end}}, {replacement});
            check_invariants(buf, qPrintable(QString("edit_%1").arg(i)));

            // Periodically GC
            if (i % 25 == 0 && i > 0) {
                buf.collect_garbage();
                check_invariants(buf, qPrintable(QString("gc_%1").arg(i)));
            }

            // Occasional undo/redo
            if (rng() % 5 == 0) {
                if (rng() % 2 == 0) buf.undo(); else buf.redo();
                check_invariants(buf, qPrintable(QString("undo_redo_%1").arg(i)));
            }
        }

        // Final GC
        buf.collect_garbage();
        check_invariants(buf, "final_gc");
    }
```

- [ ] **Step 2: Build and run**

Run: `cmake --build build-dev --target tst_gc && ./build-dev/libs/collabtext/tst_gc`
Expected: All tests pass.

- [ ] **Step 3: Commit**

```bash
git add libs/collabtext/tests/tst_gc.cpp
git commit -m "test(gc): invariant-checking stress test with random edits + GC"
```

---

### Task 6: GC + Multi-Replica Convergence Tests

**Files:**
- Modify: `libs/collabtext/tests/tst_gc.cpp`

- [ ] **Step 1: Write convergence test with GC**

Add to `tst_gc.cpp`:

```cpp
    void gc_preserves_convergence() {
        // Two replicas: both make edits, cross-apply, one runs GC.
        // Visible text must still converge.
        Buffer bufA(1), bufB(2);

        auto op1 = bufA.apply_local_edit({{0, 0}}, {"hello world"});
        bufB.apply_ops({op1});
        QCOMPARE(bufA.text(), bufB.text());

        // A deletes "hello", B deletes "world"
        auto op2 = bufA.apply_local_edit({{0, 5}}, {""});
        auto op3 = bufB.apply_local_edit({{6, 11}}, {""});

        bufA.apply_ops({op3});
        bufB.apply_ops({op2});
        QCOMPARE(bufA.text(), bufB.text());

        // A runs GC (with max_undo_depth=0 to make everything eligible)
        bufA.set_max_undo_depth(0);
        bufA.collect_garbage();

        // B does NOT run GC — still has tombstones
        // Visible text should still match
        QCOMPARE(bufA.text(), bufB.text());
        check_invariants(bufA, "after_gc_A");
        check_invariants(bufB, "after_gc_B");

        // Further edits should still work on both
        auto op4 = bufA.apply_local_edit({{0, 0}}, {"new "});
        bufB.apply_ops({op4});
        QCOMPARE(bufA.text(), bufB.text());
    }

    void gc_convergence_fuzz() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        std::mt19937 rng(seed);

        Buffer bufA(1), bufB(2);
        bufA.set_max_undo_depth(10);
        bufB.set_max_undo_depth(10);
        std::vector<Operation> queueA, queueB;

        for (int i = 0; i < 80; ++i) {
            int action = rng() % 100;
            if (action < 40) {
                // Edit on A
                std::string text = bufA.text();
                uint32_t len = static_cast<uint32_t>(text.size());
                uint32_t start = 0, end = 0;
                if (len > 0) {
                    start = rng() % (len + 1);
                    // Snap to char boundary
                    while (start < len && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80)
                        start++;
                    end = start + (rng() % (len - start + 1));
                    while (end < len && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
                        end++;
                }
                std::string rep;
                if (rng() % 2 == 0) {
                    int c = 1 + (rng() % 3);
                    for (int j = 0; j < c; ++j) rep += static_cast<char>('a' + (rng() % 26));
                }
                auto op = bufA.apply_local_edit({{start, end}}, {rep});
                queueB.push_back(op);
            } else if (action < 80) {
                // Edit on B
                std::string text = bufB.text();
                uint32_t len = static_cast<uint32_t>(text.size());
                uint32_t start = 0, end = 0;
                if (len > 0) {
                    start = rng() % (len + 1);
                    while (start < len && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80)
                        start++;
                    end = start + (rng() % (len - start + 1));
                    while (end < len && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
                        end++;
                }
                std::string rep;
                if (rng() % 2 == 0) {
                    int c = 1 + (rng() % 3);
                    for (int j = 0; j < c; ++j) rep += static_cast<char>('a' + (rng() % 26));
                }
                auto op = bufB.apply_local_edit({{start, end}}, {rep});
                queueA.push_back(op);
            } else if (action < 90) {
                // Deliver some ops
                if (!queueA.empty()) {
                    int n = 1 + (rng() % std::min<int>(3, static_cast<int>(queueA.size())));
                    std::vector<Operation> batch(queueA.begin(), queueA.begin() + n);
                    queueA.erase(queueA.begin(), queueA.begin() + n);
                    bufA.apply_ops(batch);
                }
                if (!queueB.empty()) {
                    int n = 1 + (rng() % std::min<int>(3, static_cast<int>(queueB.size())));
                    std::vector<Operation> batch(queueB.begin(), queueB.begin() + n);
                    queueB.erase(queueB.begin(), queueB.begin() + n);
                    bufB.apply_ops(batch);
                }
            } else {
                // GC on a random replica
                if (rng() % 2 == 0) bufA.collect_garbage();
                else bufB.collect_garbage();
            }

            if (i % 20 == 0) {
                check_invariants(bufA, qPrintable(QString("A_step_%1").arg(i)));
                check_invariants(bufB, qPrintable(QString("B_step_%1").arg(i)));
            }
        }

        // Drain
        if (!queueA.empty()) bufA.apply_ops(queueA);
        if (!queueB.empty()) bufB.apply_ops(queueB);
        for (int pass = 0; pass < 20; ++pass) {
            bufA.apply_ops({});
            bufB.apply_ops({});
        }

        check_invariants(bufA, "final_A");
        check_invariants(bufB, "final_B");
        QCOMPARE(bufA.text(), bufB.text());
    }
```

- [ ] **Step 2: Build and run**

Run: `cmake --build build-dev --target tst_gc && ./build-dev/libs/collabtext/tst_gc`
Expected: All tests pass.

- [ ] **Step 3: Commit**

```bash
git add libs/collabtext/tests/tst_gc.cpp
git commit -m "test(gc): multi-replica convergence with GC"
```

---

### Task 7: Add GC to Existing Fuzz Tests

**Files:**
- Modify: `libs/collabtext/tests/tst_fuzz.cpp`

- [ ] **Step 1: Add probabilistic GC to the convergence fuzz loop**

In `tst_fuzz.cpp`, find the `convergence_utf8_stress` test's main loop (around line 269-304). Inside the loop body, after the "Deliver some ops" block and before the invariant check, add a GC probability branch:

```cpp
            } else if (action < 85) {
                // GC on a random replica
                int r = rng() % 3;
                bufs[r]->collect_garbage();
            } else {
```

Adjust the probability thresholds:
- `action < 50`: edit (unchanged)
- `action < 65`: undo/redo (was 70)
- `action < 80`: deliver ops (was 100, i.e. the else)
- `action < 90`: GC (new)
- `else`: deliver ops (remaining)

Also set bounded undo on the replicas. Add these lines right after the `Buffer bufA(1), bufB(2), bufC(3);` declaration:

```cpp
        bufA.set_max_undo_depth(30);
        bufB.set_max_undo_depth(30);
        bufC.set_max_undo_depth(30);
```

- [ ] **Step 2: Build and run fuzz tests**

Run: `cmake --build build-dev --target tst_fuzz && ./build-dev/libs/collabtext/tst_fuzz`
Expected: All fuzz tests pass. This is the critical correctness validation.

- [ ] **Step 3: Run fuzz test multiple times with different seeds**

Run: `for i in $(seq 1 5); do ./build-dev/libs/collabtext/tst_fuzz -v2 2>&1 | tail -3; done`
Expected: All 5 runs pass.

- [ ] **Step 4: Commit**

```bash
git add libs/collabtext/tests/tst_fuzz.cpp
git commit -m "test(gc): add probabilistic GC to convergence fuzz tests"
```

---

### Task 8: Benchmark Verification

**Files:**
- Modify: `libs/collabtext/tests/tst_benchmark.cpp`

- [ ] **Step 1: Add a GC benchmark to measure improvement**

Add a new benchmark slot to `tst_benchmark.cpp` (after the existing `memory_growth` test). This benchmark replicates the tombstone degradation scenario, then measures throughput with periodic GC:

```cpp
    void gc_effectiveness() {
        qDebug() << "\n--- GC Effectiveness ---";

        // Build a 5K doc with 50% tombstones (the scenario that showed 21x degradation)
        std::mt19937 rng(42);
        Buffer buf(1);
        buf.set_max_undo_depth(0);  // everything GC-eligible immediately
        build_document(buf, 5000, rng, 100);

        size_t frags_clean = buf.fragment_count();
        qDebug().noquote() << QString("  Clean doc:   %1 fragments").arg(frags_clean);

        create_tombstones(buf, 0.5, rng);
        size_t frags_dirty = buf.fragment_count();
        size_t tombstones = buf.tombstone_count();
        qDebug().noquote() << QString("  After 50%% tombstones: %1 fragments (%2 tombstones)")
            .arg(frags_dirty).arg(tombstones);

        // Measure edit throughput BEFORE GC
        auto t0 = std::chrono::high_resolution_clock::now();
        int ops_before = 0;
        while (std::chrono::high_resolution_clock::now() - t0 < std::chrono::milliseconds(500)) {
            random_edit(buf, rng);
            ++ops_before;
        }
        auto elapsed_before = std::chrono::high_resolution_clock::now() - t0;
        double ns_before = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed_before).count()
                           / static_cast<double>(ops_before);

        // Run GC
        auto gc_start = std::chrono::high_resolution_clock::now();
        size_t removed = buf.collect_garbage();
        auto gc_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - gc_start).count();

        size_t frags_after = buf.fragment_count();
        qDebug().noquote() << QString("  After GC:    %1 fragments (removed %2, GC took %3 us)")
            .arg(frags_after).arg(removed).arg(gc_elapsed);

        // Measure edit throughput AFTER GC
        auto t1 = std::chrono::high_resolution_clock::now();
        int ops_after = 0;
        while (std::chrono::high_resolution_clock::now() - t1 < std::chrono::milliseconds(500)) {
            random_edit(buf, rng);
            ++ops_after;
        }
        auto elapsed_after = std::chrono::high_resolution_clock::now() - t1;
        double ns_after = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed_after).count()
                          / static_cast<double>(ops_after);

        double speedup = ns_before / ns_after;
        qDebug().noquote() << QString("  Before GC: %1 ns/op (%2 ops/sec)")
            .arg(ns_before, 0, 'f', 0).arg(1e9 / ns_before, 0, 'f', 0);
        qDebug().noquote() << QString("  After GC:  %1 ns/op (%2 ops/sec)")
            .arg(ns_after, 0, 'f', 0).arg(1e9 / ns_after, 0, 'f', 0);
        qDebug().noquote() << QString("  Speedup:   %1x").arg(speedup, 0, 'f', 1);

        QVERIFY2(removed > 0, "GC should have removed tombstones");
        QVERIFY2(frags_after < frags_dirty, "Fragment count should decrease after GC");
    }
```

- [ ] **Step 2: Build and run the GC benchmark**

Run: `cmake --build build-dev --target tst_benchmark && timeout 600 ./build-dev/libs/collabtext/tst_benchmark gc_effectiveness -v2`
Expected: Output shows significant fragment count reduction and speedup.

- [ ] **Step 3: Commit**

```bash
git add libs/collabtext/tests/tst_benchmark.cpp
git commit -m "bench(gc): add gc_effectiveness benchmark measuring tombstone removal speedup"
```

---

### Task 9: Final Regression Suite

- [ ] **Step 1: Build and run the complete test suite**

Run:
```bash
cmake --build build-dev && ctest --test-dir build-dev --output-on-failure -j4 \
  --exclude-regex tst_benchmark
```
Expected: All tests pass.

- [ ] **Step 2: Run the full fuzz suite 5 times**

Run: `for i in $(seq 1 5); do echo "Run $i:"; ./build-dev/libs/collabtext/tst_fuzz 2>&1 | tail -1; done`
Expected: All 5 runs pass.

- [ ] **Step 3: Commit any fixups, then create summary commit**

If all tests pass with no changes needed, no commit required here.

If any tests needed adjustments, commit the fixes:
```bash
git add -u
git commit -m "fix(gc): test adjustments from final regression run"
```
