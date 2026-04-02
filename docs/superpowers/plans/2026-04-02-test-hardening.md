# Test Hardening Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close 50+ coverage gaps across UTF-8, Rope, undo/remote interplay, split relocations, anchors, and boundary cases — ensuring the CRDT engine is rock-solid before building Rope integration (Optimization 4).

**Architecture:** Four new test files targeting the critical gaps. Tests are written against the existing API and should all PASS against the current implementation. Any test that FAILS exposes a real bug that must be fixed before proceeding.

**Tech Stack:** C++20, Qt6 Test (`QTest`), CMake (`add_crdt_test` macro).

**Build & test commands:**
- Build: `cmake --build build-dev`
- Run all: `cd build-dev && ctest --output-on-failure`
- Run single: `./build-dev/libs/collabtext/<test_name>`

---

## File Map

| Action | File | Tests | Focus |
|--------|------|-------|-------|
| Create | `libs/collabtext/tests/tst_utf8.cpp` | ~12 | Multi-byte chars through every code path |
| Create | `libs/collabtext/tests/tst_rope.cpp` | ~10 | Standalone Rope validation (Opt 4 prereq) |
| Modify | `libs/collabtext/tests/tst_buffer.cpp` | ~12 new | Undo+remote, split relocations, boundary cases |
| Modify | `libs/collabtext/tests/tst_anchor.cpp` | ~8 new | Complex edit sequences, undo, remote |
| Modify | `libs/collabtext/CMakeLists.txt` | — | Register `tst_utf8` and `tst_rope` |

---

## Task 1: UTF-8 Multi-Byte Character Tests

Every code path that counts characters, splits strings, or resolves byte offsets must be exercised with 2-byte (e.g. `"\xc3\xa9"` = é), 3-byte (e.g. `"\xe4\xb8\xad"` = 中), and 4-byte (e.g. `"\xf0\x9f\x9a\x80"` = rocket emoji) UTF-8 sequences.

**Files:**
- Create: `libs/collabtext/tests/tst_utf8.cpp`
- Modify: `libs/collabtext/CMakeLists.txt` — add `add_crdt_test(tst_utf8)`

- [ ] **Step 1: Register test in CMakeLists.txt**

Add after the last `add_crdt_test` line:

```cmake
add_crdt_test(tst_utf8)
```

- [ ] **Step 2: Create tst_utf8.cpp with all tests**

```cpp
#include <QTest>
#include "crdt/Buffer.h"

using namespace CollabText::Crdt;

class TestUtf8 : public QObject {
    Q_OBJECT

private:
    // Test strings
    // 2-byte: é = C3 A9
    static constexpr const char* ACUTE_E = "\xc3\xa9";          // 2 bytes, 1 char
    // 3-byte: 中 = E4 B8 AD
    static constexpr const char* CJK_MID = "\xe4\xb8\xad";     // 3 bytes, 1 char
    // 4-byte: 🚀 = F0 9F 9A 80
    static constexpr const char* ROCKET  = "\xf0\x9f\x9a\x80";  // 4 bytes, 1 char

    static std::string repeat(const char* s, int n) {
        std::string r;
        for (int i = 0; i < n; ++i) r += s;
        return r;
    }

private slots:

    // --- Basic insert/delete with multi-byte ---

    void insert_2byte_chars() {
        Buffer buf(1);
        std::string text = repeat(ACUTE_E, 5); // "ééééé" = 10 bytes, 5 chars
        buf.apply_local_edit({{0, 0}}, {text});
        QCOMPARE(buf.text(), text);
        QCOMPARE(buf.visible_length(), 10u); // visible_length is byte count
    }

    void insert_3byte_chars() {
        Buffer buf(1);
        std::string text = repeat(CJK_MID, 4); // "中中中中" = 12 bytes, 4 chars
        buf.apply_local_edit({{0, 0}}, {text});
        QCOMPARE(buf.text(), text);
        QCOMPARE(buf.visible_length(), 12u);
    }

    void insert_4byte_chars() {
        Buffer buf(1);
        std::string text = repeat(ROCKET, 3); // 12 bytes, 3 chars
        buf.apply_local_edit({{0, 0}}, {text});
        QCOMPARE(buf.text(), text);
        QCOMPARE(buf.visible_length(), 12u);
    }

    void delete_mid_multibyte() {
        Buffer buf(1);
        // "a中b" = 1 + 3 + 1 = 5 bytes
        std::string text = std::string("a") + CJK_MID + "b";
        buf.apply_local_edit({{0, 0}}, {text});
        QCOMPARE(buf.visible_length(), 5u);

        // Delete the 中 character (bytes 1-4)
        buf.apply_local_edit({{1, 4}}, {""});
        QCOMPARE(buf.text(), std::string("ab"));
        QCOMPARE(buf.visible_length(), 2u);
    }

    void replace_with_multibyte() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        // Replace "ell" (bytes 1-4) with "🚀"
        buf.apply_local_edit({{1, 4}}, {std::string(ROCKET)});
        std::string expected = std::string("h") + ROCKET + "o";
        QCOMPARE(buf.text(), expected);
        QCOMPARE(buf.visible_length(), 6u); // 1 + 4 + 1
    }

    // --- Split at multi-byte boundaries ---

    void split_between_multibyte_chars() {
        Buffer buf(1);
        // "中中中" = 9 bytes. Insert at byte 3 (between first and second 中)
        std::string text = repeat(CJK_MID, 3);
        buf.apply_local_edit({{0, 0}}, {text});
        buf.apply_local_edit({{3, 3}}, {"X"});
        std::string expected = std::string(CJK_MID) + "X" + CJK_MID + CJK_MID;
        QCOMPARE(buf.text(), expected);
    }

    void delete_partial_multibyte_sequence() {
        Buffer buf(1);
        // "🚀🚀🚀" = 12 bytes. Delete first rocket (bytes 0-4)
        std::string text = repeat(ROCKET, 3);
        buf.apply_local_edit({{0, 0}}, {text});
        buf.apply_local_edit({{0, 4}}, {""});
        std::string expected = repeat(ROCKET, 2);
        QCOMPARE(buf.text(), expected);
        QCOMPARE(buf.visible_length(), 8u);
    }

    // --- Mixed ASCII and multi-byte ---

    void mixed_ascii_and_multibyte() {
        Buffer buf(1);
        // "hello中world🚀!" = 5 + 3 + 5 + 4 + 1 = 18 bytes
        std::string text = std::string("hello") + CJK_MID + "world" + ROCKET + "!";
        buf.apply_local_edit({{0, 0}}, {text});
        QCOMPARE(buf.visible_length(), 18u);

        // Delete "中world" = bytes 5-13 (3 + 5 = 8 bytes)
        buf.apply_local_edit({{5, 13}}, {""});
        std::string expected = std::string("hello") + ROCKET + "!";
        QCOMPARE(buf.text(), expected);
        QCOMPARE(buf.visible_length(), 10u);
    }

    // --- Remote edit convergence with multi-byte ---

    void concurrent_multibyte_inserts_converge() {
        Buffer bufA(1), bufB(2);
        // A inserts emoji, B inserts CJK
        auto opA = bufA.apply_local_edit({{0, 0}}, {repeat(ROCKET, 2)});
        auto opB = bufB.apply_local_edit({{0, 0}}, {repeat(CJK_MID, 3)});

        bufA.apply_ops({opB});
        bufB.apply_ops({opA});

        QCOMPARE(bufA.text(), bufB.text());
        // All characters present: 2 rockets (8 bytes) + 3 CJK (9 bytes)
        QCOMPARE(bufA.visible_length(), 17u);
    }

    // --- Undo with multi-byte ---

    void undo_multibyte_insert() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"abc"});
        buf.apply_local_edit({{3, 3}}, {repeat(ROCKET, 2)});
        QCOMPARE(buf.visible_length(), 11u); // 3 + 8

        buf.undo();
        QCOMPARE(buf.text(), std::string("abc"));
        QCOMPARE(buf.visible_length(), 3u);

        buf.redo();
        std::string expected = std::string("abc") + repeat(ROCKET, 2);
        QCOMPARE(buf.text(), expected);
    }

    // --- Anchor with multi-byte ---

    void anchor_on_multibyte_char() {
        Buffer buf(1);
        // "a中b" = bytes [0]=a, [1-3]=中, [4]=b
        std::string text = std::string("a") + CJK_MID + "b";
        buf.apply_local_edit({{0, 0}}, {text});

        // Anchor at byte 1 (start of 中)
        auto anchor = buf.anchor_at(1, Bias::Left);
        QCOMPARE(buf.resolve_anchor(anchor), 1u);

        // Insert "X" at beginning — anchor should shift by 1
        buf.apply_local_edit({{0, 0}}, {"X"});
        QCOMPARE(buf.resolve_anchor(anchor), 2u);
    }

    void anchor_survives_multibyte_delete() {
        Buffer buf(1);
        // "中中中" = 9 bytes. Anchor at byte 6 (start of third 中)
        std::string text = repeat(CJK_MID, 3);
        buf.apply_local_edit({{0, 0}}, {text});
        auto anchor = buf.anchor_at(6, Bias::Left);
        QCOMPARE(buf.resolve_anchor(anchor), 6u);

        // Delete first 中 (bytes 0-3)
        buf.apply_local_edit({{0, 3}}, {""});
        // Anchor should shift to byte 3 (was 6, minus 3 deleted)
        QCOMPARE(buf.resolve_anchor(anchor), 3u);
    }
};

QTEST_MAIN(TestUtf8)
#include "tst_utf8.moc"
```

- [ ] **Step 3: Build and run**

Run: `cmake --build build-dev && ./build-dev/libs/collabtext/tst_utf8`
Expected: ALL 12 tests pass. If any fail, there is a real bug to fix.

- [ ] **Step 4: Commit**

```
git add libs/collabtext/tests/tst_utf8.cpp libs/collabtext/CMakeLists.txt
git commit -m "test: add comprehensive UTF-8 multi-byte character tests

12 tests covering 2-byte (é), 3-byte (中), and 4-byte (🚀) chars
through insert, delete, replace, split, concurrent edit, undo/redo,
and anchor code paths."
```

---

## Task 2: Rope Standalone Tests

The Rope class (`libs/collabtext/src/crdt/Rope.h`) is a prerequisite for Optimization 4 but has zero tests. Validate every public method.

**Files:**
- Create: `libs/collabtext/tests/tst_rope.cpp`
- Modify: `libs/collabtext/CMakeLists.txt` — add `add_crdt_test(tst_rope)`

- [ ] **Step 1: Register test in CMakeLists.txt**

Add `add_crdt_test(tst_rope)`.

- [ ] **Step 2: Create tst_rope.cpp with all tests**

```cpp
#include <QTest>
#include "crdt/Rope.h"

using namespace CollabText::Crdt;

class TestRope : public QObject {
    Q_OBJECT
private slots:

    void empty_rope() {
        Rope r;
        QVERIFY(r.empty());
        QCOMPARE(r.len(), 0u);
        QCOMPARE(r.to_string(), std::string(""));
    }

    void from_string() {
        auto r = Rope::from("hello world");
        QVERIFY(!r.empty());
        QCOMPARE(r.len(), 11u);
        QCOMPARE(r.to_string(), std::string("hello world"));
    }

    void push_str_small() {
        Rope r;
        r.push_str("abc");
        r.push_str("def");
        QCOMPARE(r.len(), 6u);
        QCOMPARE(r.to_string(), std::string("abcdef"));
    }

    void push_str_triggers_chunking() {
        Rope r;
        // Push more than CHUNK_MAX_BYTES (128 in release, 16 in test mode)
        // to trigger multiple chunks
        std::string big(500, 'x');
        r.push_str(big);
        QCOMPARE(r.len(), 500u);
        QCOMPARE(r.to_string(), big);
    }

    void append_ropes() {
        auto a = Rope::from("hello ");
        auto b = Rope::from("world");
        a.append(std::move(b));
        QCOMPARE(a.len(), 11u);
        QCOMPARE(a.to_string(), std::string("hello world"));
    }

    void substr_basic() {
        auto r = Rope::from("hello world");
        QCOMPARE(r.substr(0, 5), std::string("hello"));
        QCOMPARE(r.substr(6, 5), std::string("world"));
        QCOMPARE(r.substr(3, 4), std::string("lo w"));
    }

    void substr_empty() {
        auto r = Rope::from("hello");
        QCOMPARE(r.substr(2, 0), std::string(""));
    }

    void substr_entire() {
        auto r = Rope::from("hello");
        QCOMPARE(r.substr(0, 5), std::string("hello"));
    }

    void slice_to_basic() {
        auto r = Rope::from("hello world");
        auto prefix = r.slice_to(5);
        QCOMPARE(prefix.to_string(), std::string("hello"));
        QCOMPARE(r.to_string(), std::string(" world"));
    }

    void slice_to_zero() {
        auto r = Rope::from("hello");
        auto prefix = r.slice_to(0);
        QVERIFY(prefix.empty());
        QCOMPARE(r.to_string(), std::string("hello"));
    }

    void slice_to_full() {
        auto r = Rope::from("hello");
        auto prefix = r.slice_to(5);
        QCOMPARE(prefix.to_string(), std::string("hello"));
        QVERIFY(r.empty() || r.len() == 0);
    }

    void push_str_utf8_boundary() {
        Rope r;
        // Push text that would split a UTF-8 char at chunk boundary
        // Build a string with 中 (3 bytes) that crosses a chunk boundary
        std::string text;
        // Fill near chunk size with ASCII, then add multi-byte
        for (int i = 0; i < 42; ++i) text += "aa"; // 84 bytes
        text += "\xe4\xb8\xad"; // 中 = 3 bytes, total 87
        text += "\xe4\xb8\xad"; // 中 = 3 bytes, total 90

        r.push_str(text);
        QCOMPARE(r.len(), static_cast<uint32_t>(text.size()));
        QCOMPARE(r.to_string(), text);
    }

    void substr_across_chunks() {
        // Build a rope with many chunks
        Rope r;
        std::string big;
        for (int i = 0; i < 100; ++i) {
            std::string chunk(10, 'a' + (i % 26));
            r.push_str(chunk);
            big += chunk;
        }
        QCOMPARE(r.len(), 1000u);

        // substr spanning chunk boundaries
        QCOMPARE(r.substr(125, 10), big.substr(125, 10));
        QCOMPARE(r.substr(0, 1000), big);
    }

    void slice_to_mid_chunk() {
        Rope r;
        std::string text(200, 'x');
        r.push_str(text);
        auto prefix = r.slice_to(75);
        QCOMPARE(prefix.len(), 75u);
        QCOMPARE(r.len(), 125u);
        QCOMPARE(prefix.to_string() + r.to_string(), text);
    }
};

QTEST_MAIN(TestRope)
#include "tst_rope.moc"
```

- [ ] **Step 3: Build and run**

Run: `cmake --build build-dev && ./build-dev/libs/collabtext/tst_rope`
Expected: ALL 14 tests pass.

- [ ] **Step 4: Commit**

```
git add libs/collabtext/tests/tst_rope.cpp libs/collabtext/CMakeLists.txt
git commit -m "test: add standalone Rope tests (14 tests)

Covers empty, from(), push_str, append, substr, slice_to,
chunking boundaries, UTF-8 boundary handling, and cross-chunk
operations. Prerequisite for Optimization 4 (Rope integration)."
```

---

## Task 3: Undo/Remote Interplay, Split Relocations, and Boundary Cases

Extend `tst_buffer.cpp` with tests that exercise the dangerous intersections: undo with remote edits, explicit split relocation verification, and apply_local_edit boundary cases.

**Files:**
- Modify: `libs/collabtext/tests/tst_buffer.cpp`

- [ ] **Step 1: Add all 12 new tests**

Add before the closing `};` of TestBuffer (before line 418):

```cpp
    // -----------------------------------------------------------------------
    // Undo/redo with remote operations
    // -----------------------------------------------------------------------

    void undo_after_remote_edit() {
        // A inserts "hello", B inserts "world". A receives B's edit.
        // A then undoes its own "hello". Only "world" should remain.
        Buffer bufA(1), bufB(2);
        auto opA = bufA.apply_local_edit({{0, 0}}, {"hello"});
        auto opB = bufB.apply_local_edit({{0, 0}}, {"world"});

        bufA.apply_ops({opB});
        // A has "hello" + "world" (in some order)
        QCOMPARE(bufA.visible_length(), 10u);

        auto undoOp = bufA.undo();
        QVERIFY(undoOp.has_value());
        // Only B's text should remain
        QCOMPARE(bufA.visible_length(), 5u);
        QCOMPARE(bufA.text(), std::string("world"));
    }

    void redo_after_remote_edit() {
        Buffer bufA(1), bufB(2);
        auto opA = bufA.apply_local_edit({{0, 0}}, {"hello"});
        auto opB = bufB.apply_local_edit({{0, 0}}, {"world"});

        bufA.apply_ops({opB});
        bufA.undo(); // undo "hello"
        QCOMPARE(bufA.text(), std::string("world"));

        bufA.redo(); // redo "hello"
        QCOMPARE(bufA.visible_length(), 10u);
        QVERIFY(bufA.text().find("hello") != std::string::npos);
        QVERIFY(bufA.text().find("world") != std::string::npos);
    }

    void remote_undo_broadcast() {
        // A inserts "hello", B receives it.
        // A undoes → broadcasts UndoOperation.
        // B applies the undo → "hello" should be gone on B too.
        Buffer bufA(1), bufB(2);
        auto opIns = bufA.apply_local_edit({{0, 0}}, {"hello"});
        bufB.apply_ops({opIns});
        QCOMPARE(bufB.text(), std::string("hello"));

        auto undoOp = bufA.undo();
        QVERIFY(undoOp.has_value());
        bufB.apply_ops({*undoOp});
        QCOMPARE(bufB.text(), std::string(""));
    }

    void undo_delete_with_remote_interleaving() {
        // A inserts "abcdef", A deletes "cd" (bytes 2-4).
        // B receives insert, inserts "X" at position 3 (between c and d).
        // A receives B's insert. A undoes the delete.
        // "cd" should reappear; "X" should survive.
        Buffer bufA(1), bufB(2);
        auto op1 = bufA.apply_local_edit({{0, 0}}, {"abcdef"});
        bufB.apply_ops({op1});

        auto opDel = bufA.apply_local_edit({{2, 4}}, {""});
        auto opBins = bufB.apply_local_edit({{3, 3}}, {"X"});

        bufA.apply_ops({opBins});
        bufB.apply_ops({opDel});

        // Both converge
        QCOMPARE(bufA.text(), bufB.text());

        // A undoes the delete — "cd" reappears
        auto undoOp = bufA.undo();
        QVERIFY(undoOp.has_value());

        // Text should have a, b, c, d, e, f AND X (all present)
        std::string text = bufA.text();
        QVERIFY(text.find('a') != std::string::npos);
        QVERIFY(text.find('b') != std::string::npos);
        QVERIFY(text.find('c') != std::string::npos);
        QVERIFY(text.find('d') != std::string::npos);
        QVERIFY(text.find('e') != std::string::npos);
        QVERIFY(text.find('f') != std::string::npos);
        QVERIFY(text.find('X') != std::string::npos);
    }

    void new_edit_clears_redo_stack() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"abc"});
        buf.apply_local_edit({{3, 3}}, {"def"});
        buf.undo(); // undo "def"
        QCOMPARE(buf.text(), std::string("abc"));

        // New edit should invalidate redo
        buf.apply_local_edit({{3, 3}}, {"xyz"});
        auto op = buf.redo();
        QVERIFY(!op.has_value()); // redo should be empty
        QCOMPARE(buf.text(), std::string("abcxyz"));
    }

    // -----------------------------------------------------------------------
    // Split relocation verification
    // -----------------------------------------------------------------------

    void split_relocation_remote_convergence() {
        // A inserts "abcdef", then inserts "X" at position 3 (mid-fragment).
        // This creates a split relocation. B should apply it correctly.
        Buffer bufA(1), bufB(2);
        auto op1 = bufA.apply_local_edit({{0, 0}}, {"abcdef"});
        bufB.apply_ops({op1});

        auto op2 = bufA.apply_local_edit({{3, 3}}, {"X"});
        bufB.apply_ops({op2});

        QCOMPARE(bufA.text(), std::string("abcXdef"));
        QCOMPARE(bufB.text(), std::string("abcXdef"));
    }

    void split_relocation_with_concurrent_insert() {
        // A and B both have "abcdef". A inserts "X" at 3, B inserts "Y" at 3.
        // Both create split relocations. After cross-delivery, both converge.
        Buffer bufA(1), bufB(2);
        auto op1 = bufA.apply_local_edit({{0, 0}}, {"abcdef"});
        bufB.apply_ops({op1});

        auto opA = bufA.apply_local_edit({{3, 3}}, {"X"});
        auto opB = bufB.apply_local_edit({{3, 3}}, {"Y"});

        bufA.apply_ops({opB});
        bufB.apply_ops({opA});

        QCOMPARE(bufA.text(), bufB.text());
        std::string text = bufA.text();
        QCOMPARE(text.size(), size_t(8)); // 6 + X + Y
        QVERIFY(text.find('X') != std::string::npos);
        QVERIFY(text.find('Y') != std::string::npos);
    }

    void replace_mid_fragment_remote_convergence() {
        // This tests the locator bug fix: replace at fragment boundary.
        // A has "hello". A replaces "ll" with "XX". B should converge.
        Buffer bufA(1), bufB(2);
        auto op1 = bufA.apply_local_edit({{0, 0}}, {"hello"});
        bufB.apply_ops({op1});

        auto op2 = bufA.apply_local_edit({{2, 4}}, {"XX"});
        bufB.apply_ops({op2});

        QCOMPARE(bufA.text(), std::string("heXXo"));
        QCOMPARE(bufB.text(), std::string("heXXo"));
    }

    // -----------------------------------------------------------------------
    // apply_local_edit boundary cases
    // -----------------------------------------------------------------------

    void empty_range_insert_only() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        // Empty range: start == end, pure insert
        buf.apply_local_edit({{2, 2}}, {"X"});
        QCOMPARE(buf.text(), std::string("heXllo"));
    }

    void adjacent_ranges() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"abcdef"});
        // Two adjacent ranges: [1,3) and [3,5) — touching but not overlapping
        buf.apply_local_edit({{1, 3}, {3, 5}}, {"X", "Y"});
        QCOMPARE(buf.text(), std::string("aXYf"));
    }

    void delete_everything_then_insert() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        buf.apply_local_edit({{0, 5}}, {"world"});
        QCOMPARE(buf.text(), std::string("world"));

        // Verify visible_length matches text
        QCOMPARE(buf.visible_length(), static_cast<uint32_t>(buf.text().size()));
    }

    void visible_length_always_matches_text() {
        // Stress test: after many edits, visible_length == text().size()
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello world"});
        QCOMPARE(buf.visible_length(), static_cast<uint32_t>(buf.text().size()));

        buf.apply_local_edit({{5, 6}}, {"_"});
        QCOMPARE(buf.visible_length(), static_cast<uint32_t>(buf.text().size()));

        buf.apply_local_edit({{0, 3}}, {""});
        QCOMPARE(buf.visible_length(), static_cast<uint32_t>(buf.text().size()));

        buf.undo();
        QCOMPARE(buf.visible_length(), static_cast<uint32_t>(buf.text().size()));

        buf.redo();
        QCOMPARE(buf.visible_length(), static_cast<uint32_t>(buf.text().size()));
    }
```

- [ ] **Step 2: Build and run**

Run: `cmake --build build-dev && ./build-dev/libs/collabtext/tst_buffer`
Expected: All 47 tests pass (35 existing + 12 new). Any failure exposes a real bug.

- [ ] **Step 3: Commit**

```
git add libs/collabtext/tests/tst_buffer.cpp
git commit -m "test: add undo/remote, split relocation, and boundary tests (12 tests)

Covers: undo after remote edit, redo after remote, remote undo
broadcast, undo delete with remote interleaving, redo stack
invalidation, split relocation convergence, concurrent mid-fragment
inserts, replace-mid-fragment remote convergence, empty ranges,
adjacent ranges, delete-all-then-insert, visible_length invariant."
```

---

## Task 4: Anchor Complex Scenario Tests

Extend `tst_anchor.cpp` with tests exercising anchors through undo/redo, remote edits, fragment normalization, and multi-edit sequences.

**Files:**
- Modify: `libs/collabtext/tests/tst_anchor.cpp`

- [ ] **Step 1: Add all 8 new tests**

Add before the closing `};` of TestAnchor (before line 144):

```cpp
    // -----------------------------------------------------------------------
    // Complex anchor scenarios
    // -----------------------------------------------------------------------

    void anchor_through_undo_redo() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"abcdef"});
        auto anchor = buf.anchor_at(3, Bias::Left); // at 'd'
        QCOMPARE(buf.resolve_anchor(anchor), 3u);

        buf.apply_local_edit({{1, 3}}, {""}); // delete "bc" → "adef"
        QCOMPARE(buf.resolve_anchor(anchor), 1u); // 'd' shifted to 1

        buf.undo(); // restore "bc" → "abcdef"
        QCOMPARE(buf.resolve_anchor(anchor), 3u); // 'd' back at 3

        buf.redo(); // re-delete → "adef"
        QCOMPARE(buf.resolve_anchor(anchor), 1u); // 'd' at 1 again
    }

    void anchor_with_remote_insert() {
        Buffer bufA(1), bufB(2);
        auto op = bufA.apply_local_edit({{0, 0}}, {"abcdef"});
        bufB.apply_ops({op});

        // A creates anchor at position 3 ('d')
        auto anchor = bufA.anchor_at(3, Bias::Left);
        QCOMPARE(bufA.resolve_anchor(anchor), 3u);

        // B inserts "XX" at position 1 → "aXXbcdef"
        auto opB = bufB.apply_local_edit({{1, 1}}, {"XX"});
        bufA.apply_ops({opB});

        // Anchor should shift by 2 → position 5
        QCOMPARE(bufA.resolve_anchor(anchor), 5u);
    }

    void anchor_with_concurrent_inserts_at_same_position() {
        Buffer bufA(1), bufB(2);
        auto op = bufA.apply_local_edit({{0, 0}}, {"abc"});
        bufB.apply_ops({op});

        // Both insert at position 2
        auto opA = bufA.apply_local_edit({{2, 2}}, {"X"});
        auto opB = bufB.apply_local_edit({{2, 2}}, {"Y"});

        // A creates anchor at position 2 BEFORE receiving B's edit
        auto anchor = bufA.anchor_at(2, Bias::Left);

        bufA.apply_ops({opB});
        bufB.apply_ops({opA});

        // Both converge
        QCOMPARE(bufA.text(), bufB.text());
        // Anchor should resolve to a valid position
        uint32_t pos = bufA.resolve_anchor(anchor);
        QVERIFY(pos <= bufA.visible_length());
    }

    void anchor_min_max_always_stable() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});

        auto amin = Anchor::min();
        auto amax = Anchor::max();

        QCOMPARE(buf.resolve_anchor(amin), 0u);
        QCOMPARE(buf.resolve_anchor(amax), 5u);

        buf.apply_local_edit({{0, 0}}, {"XX"});
        QCOMPARE(buf.resolve_anchor(amin), 0u);
        QCOMPARE(buf.resolve_anchor(amax), 7u);

        buf.apply_local_edit({{0, 7}}, {""});
        QCOMPARE(buf.resolve_anchor(amin), 0u);
        QCOMPARE(buf.resolve_anchor(amax), 0u);

        buf.undo();
        QCOMPARE(buf.resolve_anchor(amin), 0u);
        QCOMPARE(buf.resolve_anchor(amax), 7u);
    }

    void anchor_through_many_edits() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"0123456789"});
        // Anchor at '5' (position 5)
        auto anchor = buf.anchor_at(5, Bias::Left);

        // 10 edits: insert "X" at position 0 each time
        for (int i = 0; i < 10; ++i) {
            buf.apply_local_edit({{0, 0}}, {"X"});
        }

        // Anchor should have shifted by 10
        QCOMPARE(buf.resolve_anchor(anchor), 15u);

        // Delete first 10 characters (the X's)
        buf.apply_local_edit({{0, 10}}, {""});
        QCOMPARE(buf.resolve_anchor(anchor), 5u);
    }

    void compare_anchors_with_edits() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"abcdef"});

        auto a = buf.anchor_at(2, Bias::Left);
        auto b = buf.anchor_at(4, Bias::Left);
        QVERIFY(buf.compare_anchors(a, b) < 0);

        // Insert between them
        buf.apply_local_edit({{3, 3}}, {"XYZ"});
        // a is still before b
        QVERIFY(buf.compare_anchors(a, b) < 0);

        // Delete everything between a and b
        buf.apply_local_edit({{2, 7}}, {""}); // "ab___f" → "abf"
        // a and b might collapse to same position or b might be gone
        // but compare should not crash
        int cmp = buf.compare_anchors(a, b);
        Q_UNUSED(cmp); // just verifying no crash
    }

    void anchor_on_empty_document() {
        Buffer buf(1);
        auto anchor = buf.anchor_at(0, Bias::Left);
        QCOMPARE(buf.resolve_anchor(anchor), 0u);

        // Insert text — anchor should still resolve
        buf.apply_local_edit({{0, 0}}, {"hello"});
        uint32_t pos = buf.resolve_anchor(anchor);
        QVERIFY(pos <= buf.visible_length());
    }

    void anchor_on_fully_deleted_text() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        auto anchor = buf.anchor_at(2, Bias::Left);
        QCOMPARE(buf.resolve_anchor(anchor), 2u);

        // Delete everything
        buf.apply_local_edit({{0, 5}}, {""});
        // Anchor was on deleted text — should resolve to something valid
        uint32_t pos = buf.resolve_anchor(anchor);
        QVERIFY(pos <= buf.visible_length());
    }
```

- [ ] **Step 2: Build and run**

Run: `cmake --build build-dev && ./build-dev/libs/collabtext/tst_anchor`
Expected: All tests pass (9 existing + 8 new = 17 total).

- [ ] **Step 3: Run full test suite**

Run: `cd build-dev && ctest --output-on-failure`
Expected: All test executables pass.

- [ ] **Step 4: Run convergence 10x**

```bash
for i in $(seq 1 10); do
    ./build-dev/libs/collabtext/tst_convergence 2>&1 | tail -1
done
```
Expected: All 10 pass.

- [ ] **Step 5: Commit**

```
git add libs/collabtext/tests/tst_anchor.cpp
git commit -m "test: add complex anchor scenario tests (8 tests)

Covers: anchor through undo/redo, remote inserts, concurrent inserts
at same position, min/max stability, 10+ edit sequences, compare
with edits, empty document, fully deleted text."
```

---

## Acceptance Criteria

All of the following must be true:

1. `tst_utf8`: 12 tests pass (new file)
2. `tst_rope`: 14 tests pass (new file)
3. `tst_buffer`: 47 tests pass (35 existing + 12 new)
4. `tst_anchor`: 17 tests pass (9 existing + 8 new)
5. All other test executables still pass (tst_clock, tst_locator, tst_sumtree, tst_opqueue, tst_convergence)
6. `tst_convergence` passes 10 consecutive runs
7. Total test count across all executables: ~120+
8. Any test failure that exposes a real bug is FIXED, not skipped
