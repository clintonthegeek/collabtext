# Scroll Stability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve the local viewport's scroll position when remote edits land above the visible region, so Alice stays on the paragraph she was reading while Bob edits elsewhere. Also populate `EphemeralState.viewport_top/bottom` so a future follow-mode feature has the data it needs.

**Architecture:** Three byte-offset helpers on `CollabPlainTextEdit` (pure Qt, no CRDT dependency) + a new `viewportScrolled()` signal. An orchestration wrapper `applyEditsPreservingScroll` in `EditorPane` (`app/main.cpp`) captures a CRDT `Anchor` before remote edits and scrolls back to it after. The app also caches the current viewport anchors for `PresenceManager` to serialize into ephemeral state — the JSON serializer already handles the field.

**Tech Stack:** C++20, Qt6 Widgets/Test, the existing `CollabText::Crdt::Buffer` + `Anchor` API, the existing `QTEST_MAIN` pattern used in `tst_identity_widgets.cpp`.

---

## File inventory

| File | What changes |
|---|---|
| `libs/collabtext/src/ui/CollabPlainTextEdit.h` | Declare 3 public methods + 1 signal; declare 2 private helpers. |
| `libs/collabtext/src/ui/CollabPlainTextEdit.cpp` | Implement the 3 methods + helpers; emit `viewportScrolled()` from `scrollContentsBy`. |
| `libs/collabtext/tests/tst_collab_plain_text_edit.cpp` | **New file.** Unit tests for the widget API. |
| `libs/collabtext/tests/tst_scroll_stability.cpp` | **New file.** App-level integration test (widget + buffer). |
| `libs/collabtext/CMakeLists.txt` | Register `tst_collab_plain_text_edit` and `tst_scroll_stability`. |
| `app/main.cpp` | Add `applyEditsPreservingScroll`; route all 4 `applyEditsToQt` call sites through it; add cached viewport anchors + slot for `viewportScrolled`; populate `EphemeralState.viewport_top/bottom` in `writeEphemeral`. |

No changes to `Buffer.h`, `Anchor.h`, or `identity/Identity.cpp` — the anchor JSON serializer already handles the fields.

## Pre-flight: verify baseline

- [ ] **Step 1: Confirm the working tree is clean and the suite is green**

```bash
cd /home/clinton/dev/collabtext
git status
cmake --build build-dev -j$(nproc) 2>&1 | tail -3
ctest --test-dir build-dev --output-on-failure 2>&1 | tail -5
```

Expected: `nothing to commit, working tree clean`; build succeeds; `100% tests passed, 0 tests failed out of 24`.

---

## Task 1: Set up the widget test file and CMake registration

**Goal:** create `tst_collab_plain_text_edit.cpp` with a single trivial test that constructs the widget. Register it in CMake. Verify it builds, runs, and passes before writing any real widget tests. This isolates "infrastructure works" from "widget API works" so that a failure in later tasks is unambiguous.

**Files:**
- Create: `libs/collabtext/tests/tst_collab_plain_text_edit.cpp`
- Modify: `libs/collabtext/CMakeLists.txt` (append to the test registrations)

- [ ] **Step 1: Create the test file**

```cpp
// libs/collabtext/tests/tst_collab_plain_text_edit.cpp
#include <QApplication>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTest>
#include "ui/CollabPlainTextEdit.h"

using namespace CollabText::Ui;

class TestCollabPlainTextEdit : public QObject {
    Q_OBJECT

private slots:
    void widget_can_be_constructed() {
        CollabPlainTextEdit edit;
        QVERIFY(edit.document() != nullptr);
    }
};

QTEST_MAIN(TestCollabPlainTextEdit)
#include "tst_collab_plain_text_edit.moc"
```

- [ ] **Step 2: Register the test in CMake**

Open `libs/collabtext/CMakeLists.txt` and add one line to the test-registration list. The list currently ends at `add_crdt_test(tst_identity_widgets)` (see `libs/collabtext/CMakeLists.txt:80`). Append:

```cmake
add_crdt_test(tst_collab_plain_text_edit)
```

So lines 80-81 become:
```cmake
add_crdt_test(tst_identity_widgets)
add_crdt_test(tst_collab_plain_text_edit)
```

- [ ] **Step 3: Re-configure and build**

```bash
cmake -S . -B build-dev 2>&1 | tail -5
cmake --build build-dev --target tst_collab_plain_text_edit -j$(nproc) 2>&1 | tail -5
```

Expected: clean build, `[100%] Built target tst_collab_plain_text_edit`.

- [ ] **Step 4: Run the test**

```bash
./build-dev/libs/collabtext/tst_collab_plain_text_edit 2>&1 | tail -5
```

Expected: `Totals: 3 passed, 0 failed, 0 skipped, 0 blacklisted, ...ms` (the 3 counts init/the-test/cleanup).

- [ ] **Step 5: Commit**

```bash
git add libs/collabtext/tests/tst_collab_plain_text_edit.cpp libs/collabtext/CMakeLists.txt
git commit -m "$(cat <<'EOF'
test: add tst_collab_plain_text_edit skeleton

Infrastructure-only commit: empty test class with a single constructor
smoke test, registered in CMake.  Subsequent tasks add real tests for
the scroll-stability API.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Implement `topVisibleByteOffset()`

**Goal:** add a public method to `CollabPlainTextEdit` that returns the UTF-8 byte offset of the character at the top-left of the viewport.

**Files:**
- Modify: `libs/collabtext/src/ui/CollabPlainTextEdit.h` (add declaration)
- Modify: `libs/collabtext/src/ui/CollabPlainTextEdit.cpp` (add implementation + two private helpers)
- Modify: `libs/collabtext/tests/tst_collab_plain_text_edit.cpp` (add two tests)

- [ ] **Step 1: Write the failing tests**

Add two test methods to `TestCollabPlainTextEdit` (inside the `private slots:` block, after `widget_can_be_constructed`):

```cpp
    void top_visible_byte_offset_empty_doc() {
        CollabPlainTextEdit edit;
        edit.resize(200, 100);
        edit.show();
        QVERIFY(QTest::qWaitForWindowExposed(&edit));
        QCOMPARE(edit.topVisibleByteOffset(), 0u);
    }

    void top_visible_byte_offset_matches_cursor_for_position() {
        CollabPlainTextEdit edit;
        // 30 short ASCII lines, long enough to force scrolling in the
        // small viewport. Each line "L<i>\n" is 3 or 4 bytes.
        QString text;
        for (int i = 0; i < 30; ++i) text += QString("L%1\n").arg(i);
        edit.setPlainText(text);
        edit.resize(200, 80);
        edit.show();
        QVERIFY(QTest::qWaitForWindowExposed(&edit));

        // Scroll to roughly the middle of the document.
        auto *bar = edit.verticalScrollBar();
        bar->setValue(bar->maximum() / 2);
        QApplication::processEvents();

        // Ground truth: use Qt's own cursorForPosition at (0, 0) and
        // convert the resulting Qt char position to UTF-8 bytes ourselves.
        QTextCursor topCursor = edit.cursorForPosition(QPoint(0, 0));
        QString prefix = edit.toPlainText().left(topCursor.position());
        uint32_t expected = static_cast<uint32_t>(prefix.toUtf8().size());

        QCOMPARE(edit.topVisibleByteOffset(), expected);
    }
```

- [ ] **Step 2: Run the tests and verify they fail to compile**

```bash
cmake --build build-dev --target tst_collab_plain_text_edit -j$(nproc) 2>&1 | tail -10
```

Expected: compile error `'topVisibleByteOffset' is not a member of 'CollabText::Ui::CollabPlainTextEdit'`.

- [ ] **Step 3: Declare the public method and private helpers in the header**

Open `libs/collabtext/src/ui/CollabPlainTextEdit.h`. After the `signals:` section's `void redoRequested();` line (around line 26), before `protected:`, add a new `public:` section for scroll-stability API:

```cpp
public:
    /// Byte offset (UTF-8) of the character at the top-left of the viewport.
    /// Returns 0 for an empty document. Wrap-aware: returns the byte offset
    /// of the visual line at the top, not the containing paragraph.
    uint32_t topVisibleByteOffset() const;
```

And in the `private:` section (around line 35, after `updateCursorLabels();`), add:

```cpp
    /// Convert a UTF-8 byte offset into a UTF-16 QTextDocument position.
    int byteOffsetToQtPos(uint32_t byteOff) const;

    /// Convert a UTF-16 QTextDocument position into a UTF-8 byte offset.
    uint32_t qtPosToByteOffset(int qtPos) const;
```

- [ ] **Step 4: Implement the helpers and the method in the cpp**

Open `libs/collabtext/src/ui/CollabPlainTextEdit.cpp`. At the end of the file (just before the closing `} // namespace CollabText::Ui`), add:

```cpp
int CollabPlainTextEdit::byteOffsetToQtPos(uint32_t byteOff) const {
    QByteArray utf8 = document()->toPlainText().toUtf8();
    uint32_t clamped = qMin(byteOff, static_cast<uint32_t>(utf8.size()));
    return QString::fromUtf8(utf8.data(), static_cast<int>(clamped)).length();
}

uint32_t CollabPlainTextEdit::qtPosToByteOffset(int qtPos) const {
    QString docText = document()->toPlainText();
    if (qtPos <= 0) return 0;
    if (qtPos >= docText.length()) return static_cast<uint32_t>(docText.toUtf8().size());
    return static_cast<uint32_t>(docText.left(qtPos).toUtf8().size());
}

uint32_t CollabPlainTextEdit::topVisibleByteOffset() const {
    if (document()->isEmpty()) return 0;
    QTextCursor c = cursorForPosition(QPoint(0, 0));
    return qtPosToByteOffset(c.position());
}
```

- [ ] **Step 5: Build and run the tests**

```bash
cmake --build build-dev --target tst_collab_plain_text_edit -j$(nproc) 2>&1 | tail -5
./build-dev/libs/collabtext/tst_collab_plain_text_edit -platform offscreen 2>&1 | tail -10
```

Expected: `Totals: 5 passed, 0 failed, 0 skipped, 0 blacklisted, ...ms`. The `-platform offscreen` flag lets the show()/qWaitForWindowExposed() calls work headless.

- [ ] **Step 6: Commit**

```bash
git add libs/collabtext/src/ui/CollabPlainTextEdit.h libs/collabtext/src/ui/CollabPlainTextEdit.cpp libs/collabtext/tests/tst_collab_plain_text_edit.cpp
git commit -m "$(cat <<'EOF'
feat: CollabPlainTextEdit::topVisibleByteOffset

Returns the UTF-8 byte offset of the character at the top-left of the
viewport, wrap-aware (returns the visual line, not the containing
paragraph).  Uses cursorForPosition(QPoint(0,0)) as the ground truth.

Adds two private UTF-8 ↔ UTF-16 conversion helpers used by this method
and the rest of the scroll-stability API.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Implement `bottomVisibleByteOffset()`

**Goal:** same shape as Task 2 but for the bottom-right pixel.

**Files:**
- Modify: `libs/collabtext/src/ui/CollabPlainTextEdit.h` (add declaration)
- Modify: `libs/collabtext/src/ui/CollabPlainTextEdit.cpp` (add implementation)
- Modify: `libs/collabtext/tests/tst_collab_plain_text_edit.cpp` (add tests)

- [ ] **Step 1: Write the failing tests**

Append to `TestCollabPlainTextEdit::private slots:`:

```cpp
    void bottom_visible_byte_offset_empty_doc() {
        CollabPlainTextEdit edit;
        edit.resize(200, 100);
        edit.show();
        QVERIFY(QTest::qWaitForWindowExposed(&edit));
        QCOMPARE(edit.bottomVisibleByteOffset(), 0u);
    }

    void bottom_visible_byte_offset_greater_than_top_in_nonempty_doc() {
        CollabPlainTextEdit edit;
        QString text;
        for (int i = 0; i < 30; ++i) text += QString("L%1\n").arg(i);
        edit.setPlainText(text);
        edit.resize(200, 80);
        edit.show();
        QVERIFY(QTest::qWaitForWindowExposed(&edit));

        uint32_t top = edit.topVisibleByteOffset();
        uint32_t bottom = edit.bottomVisibleByteOffset();
        QVERIFY(bottom > top);
        QVERIFY(bottom <= static_cast<uint32_t>(edit.toPlainText().toUtf8().size()));
    }

    void bottom_visible_byte_offset_end_of_doc_when_scrolled_to_bottom() {
        CollabPlainTextEdit edit;
        QString text;
        for (int i = 0; i < 30; ++i) text += QString("L%1\n").arg(i);
        edit.setPlainText(text);
        edit.resize(200, 80);
        edit.show();
        QVERIFY(QTest::qWaitForWindowExposed(&edit));

        auto *bar = edit.verticalScrollBar();
        bar->setValue(bar->maximum());
        QApplication::processEvents();

        uint32_t expected = static_cast<uint32_t>(edit.toPlainText().toUtf8().size());
        QCOMPARE(edit.bottomVisibleByteOffset(), expected);
    }
```

- [ ] **Step 2: Verify the tests fail to compile**

```bash
cmake --build build-dev --target tst_collab_plain_text_edit -j$(nproc) 2>&1 | tail -10
```

Expected: compile error `'bottomVisibleByteOffset' is not a member of ...`.

- [ ] **Step 3: Declare the method in the header**

Add to the public scroll-stability section of `libs/collabtext/src/ui/CollabPlainTextEdit.h` (directly after `topVisibleByteOffset`):

```cpp
    /// Byte offset (UTF-8) of the character just past the bottom-right
    /// of the viewport. Returns 0 for an empty document and returns the
    /// document's full UTF-8 byte length when the viewport shows the
    /// end of the document.
    uint32_t bottomVisibleByteOffset() const;
```

- [ ] **Step 4: Implement it in the cpp**

Add to `libs/collabtext/src/ui/CollabPlainTextEdit.cpp` directly after `topVisibleByteOffset`:

```cpp
uint32_t CollabPlainTextEdit::bottomVisibleByteOffset() const {
    if (document()->isEmpty()) return 0;
    QPoint bottomRight(viewport()->width() - 1, viewport()->height() - 1);
    QTextCursor c = cursorForPosition(bottomRight);
    // If the bottom-right falls past the end of the document,
    // cursorForPosition clamps to the last valid position, but we want
    // the total document byte length in that case.
    int lastPos = document()->characterCount() - 1;
    if (c.position() >= lastPos) {
        return static_cast<uint32_t>(document()->toPlainText().toUtf8().size());
    }
    return qtPosToByteOffset(c.position());
}
```

- [ ] **Step 5: Build and run**

```bash
cmake --build build-dev --target tst_collab_plain_text_edit -j$(nproc) 2>&1 | tail -5
./build-dev/libs/collabtext/tst_collab_plain_text_edit -platform offscreen 2>&1 | tail -10
```

Expected: `Totals: 8 passed, 0 failed, ...`.

- [ ] **Step 6: Commit**

```bash
git add libs/collabtext/src/ui/CollabPlainTextEdit.h libs/collabtext/src/ui/CollabPlainTextEdit.cpp libs/collabtext/tests/tst_collab_plain_text_edit.cpp
git commit -m "$(cat <<'EOF'
feat: CollabPlainTextEdit::bottomVisibleByteOffset

Mirror of topVisibleByteOffset for the bottom-right pixel of the
viewport.  Clamps to the document's total byte length when the
viewport extends past the end.  Used by the scroll-stability
orchestration to populate EphemeralState.viewport_bottom.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Implement `scrollByteOffsetToTop()` (basic restoration)

**Goal:** scroll so that the visual line containing a given byte offset is at the top of the viewport. No cursor safety net yet (that's Task 5).

**Files:**
- Modify: `libs/collabtext/src/ui/CollabPlainTextEdit.h`
- Modify: `libs/collabtext/src/ui/CollabPlainTextEdit.cpp`
- Modify: `libs/collabtext/tests/tst_collab_plain_text_edit.cpp`

- [ ] **Step 1: Write the failing test**

Append to `TestCollabPlainTextEdit::private slots:`:

```cpp
    void scroll_byte_offset_to_top_roundtrip() {
        CollabPlainTextEdit edit;
        QString text;
        for (int i = 0; i < 50; ++i) text += QString("Line%1\n").arg(i);
        edit.setPlainText(text);
        edit.resize(200, 80);
        edit.show();
        QVERIFY(QTest::qWaitForWindowExposed(&edit));

        // Scroll to somewhere in the middle, capture the top byte offset.
        auto *bar = edit.verticalScrollBar();
        bar->setValue(bar->maximum() / 2);
        QApplication::processEvents();
        uint32_t captured = edit.topVisibleByteOffset();

        // Scroll away from that position, then restore.
        bar->setValue(0);
        QApplication::processEvents();
        edit.scrollByteOffsetToTop(captured, /*keepCursorVisible=*/false);
        QApplication::processEvents();

        // After restoration, the new top byte offset must equal the captured
        // one. Allow one line of drift because integer line-height math may
        // round slightly; verify the restored position is on the same visual
        // line by comparing to the captured value exactly.
        QCOMPARE(edit.topVisibleByteOffset(), captured);
    }
```

- [ ] **Step 2: Verify it fails to compile**

```bash
cmake --build build-dev --target tst_collab_plain_text_edit -j$(nproc) 2>&1 | tail -10
```

Expected: `'scrollByteOffsetToTop' is not a member of ...`.

- [ ] **Step 3: Declare the method**

Add to the public scroll-stability section of `libs/collabtext/src/ui/CollabPlainTextEdit.h` (after `bottomVisibleByteOffset`):

```cpp
    /// Scroll so that the visual line containing `byteOff` is at the top
    /// of the viewport. If `keepCursorVisible` is true and the
    /// restoration would put the local cursor off-screen, the viewport
    /// is adjusted to keep the cursor visible (Qt's ensureCursorVisible
    /// semantics).
    void scrollByteOffsetToTop(uint32_t byteOff, bool keepCursorVisible);
```

- [ ] **Step 4: Implement it**

Add to `libs/collabtext/src/ui/CollabPlainTextEdit.cpp` after `bottomVisibleByteOffset`:

```cpp
void CollabPlainTextEdit::scrollByteOffsetToTop(uint32_t byteOff,
                                                 bool keepCursorVisible) {
    int qtPos = byteOffsetToQtPos(byteOff);
    QTextCursor target(document());
    target.setPosition(qtPos);

    auto *bar = verticalScrollBar();
    if (!bar) return;
    int lineHeight = qMax(1, fontMetrics().height());

    // Iterate a small number of times to converge: the cursorRect after
    // each scroll reflects the new viewport, and we want rect.top() ≈ 0.
    for (int iter = 0; iter < 4; ++iter) {
        QRect rect = cursorRect(target);
        int delta = rect.top() / lineHeight;
        if (delta == 0) break;
        bar->setValue(bar->value() + delta);
    }

    if (keepCursorVisible) {
        ensureCursorVisible();
    }
}
```

- [ ] **Step 5: Build and run**

```bash
cmake --build build-dev --target tst_collab_plain_text_edit -j$(nproc) 2>&1 | tail -5
./build-dev/libs/collabtext/tst_collab_plain_text_edit -platform offscreen 2>&1 | tail -10
```

Expected: `Totals: 9 passed, 0 failed, ...`.

- [ ] **Step 6: Commit**

```bash
git add libs/collabtext/src/ui/CollabPlainTextEdit.h libs/collabtext/src/ui/CollabPlainTextEdit.cpp libs/collabtext/tests/tst_collab_plain_text_edit.cpp
git commit -m "$(cat <<'EOF'
feat: CollabPlainTextEdit::scrollByteOffsetToTop

Scrolls so that the visual line containing the given UTF-8 byte offset
is at the top of the viewport.  Iterative cursor-rect math converges
after a few passes.  The keepCursorVisible flag delegates the cursor
safety net to Qt's own ensureCursorVisible (tested in the next task).

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Verify `keepCursorVisible` safety net

**Goal:** add a test that exercises the `keepCursorVisible=true` branch — scroll to a position that would put the primary cursor off-screen, and verify the cursor is still visible afterwards.

**Files:**
- Modify: `libs/collabtext/tests/tst_collab_plain_text_edit.cpp`

No production code changes: the behaviour is already implemented via `ensureCursorVisible()`. This task only adds test coverage and verifies the contract.

- [ ] **Step 1: Write the test**

Append to `TestCollabPlainTextEdit::private slots:`:

```cpp
    void scroll_byte_offset_to_top_keeps_cursor_visible() {
        CollabPlainTextEdit edit;
        QString text;
        for (int i = 0; i < 50; ++i) text += QString("Line%1\n").arg(i);
        edit.setPlainText(text);
        edit.resize(200, 80);
        edit.show();
        QVERIFY(QTest::qWaitForWindowExposed(&edit));

        // Put the local cursor at the end of the document (far below).
        QTextCursor tc = edit.textCursor();
        tc.movePosition(QTextCursor::End);
        edit.setTextCursor(tc);
        QApplication::processEvents();

        // Try to anchor the viewport at byte offset 0 (document start).
        // Without the safety net, the cursor would be scrolled off-screen.
        edit.scrollByteOffsetToTop(0, /*keepCursorVisible=*/true);
        QApplication::processEvents();

        // The cursor must be inside the viewport rectangle.
        QRect cursorR = edit.cursorRect(edit.textCursor());
        QRect viewR = edit.viewport()->rect();
        QVERIFY2(viewR.intersects(cursorR),
                 "cursor must remain visible when keepCursorVisible=true");
    }

    void scroll_byte_offset_to_top_ignores_cursor_when_flag_false() {
        CollabPlainTextEdit edit;
        QString text;
        for (int i = 0; i < 50; ++i) text += QString("Line%1\n").arg(i);
        edit.setPlainText(text);
        edit.resize(200, 80);
        edit.show();
        QVERIFY(QTest::qWaitForWindowExposed(&edit));

        // Cursor at end (far below).
        QTextCursor tc = edit.textCursor();
        tc.movePosition(QTextCursor::End);
        edit.setTextCursor(tc);
        QApplication::processEvents();

        // Anchor viewport at byte offset 0 with the safety net disabled.
        edit.scrollByteOffsetToTop(0, /*keepCursorVisible=*/false);
        QApplication::processEvents();

        // The top of the document must be what's shown, regardless of
        // where the cursor is.
        QCOMPARE(edit.topVisibleByteOffset(), 0u);
    }
```

- [ ] **Step 2: Build and run**

```bash
cmake --build build-dev --target tst_collab_plain_text_edit -j$(nproc) 2>&1 | tail -5
./build-dev/libs/collabtext/tst_collab_plain_text_edit -platform offscreen 2>&1 | tail -10
```

Expected: `Totals: 11 passed, 0 failed, ...`. Both new tests should pass without any production change — the safety net is already wired.

- [ ] **Step 3: Commit**

```bash
git add libs/collabtext/tests/tst_collab_plain_text_edit.cpp
git commit -m "$(cat <<'EOF'
test: scrollByteOffsetToTop cursor safety net

Two tests covering the keepCursorVisible flag:
- true: scrolling to a position that would push the cursor off-screen
  must end with the cursor still visible (ensureCursorVisible wins).
- false: the top of the viewport follows the byte offset exactly,
  regardless of where the cursor is.

No production change — both branches are already implemented.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Emit `viewportScrolled()` signal from `scrollContentsBy`

**Goal:** add a signal that fires on every local scroll event. The app uses this to keep its cached viewport anchors fresh.

**Files:**
- Modify: `libs/collabtext/src/ui/CollabPlainTextEdit.h`
- Modify: `libs/collabtext/src/ui/CollabPlainTextEdit.cpp`
- Modify: `libs/collabtext/tests/tst_collab_plain_text_edit.cpp`

- [ ] **Step 1: Write the failing test**

Append to `TestCollabPlainTextEdit::private slots:`:

```cpp
    void viewport_scrolled_signal_fires_on_scroll() {
        CollabPlainTextEdit edit;
        QString text;
        for (int i = 0; i < 30; ++i) text += QString("L%1\n").arg(i);
        edit.setPlainText(text);
        edit.resize(200, 80);
        edit.show();
        QVERIFY(QTest::qWaitForWindowExposed(&edit));

        QSignalSpy spy(&edit, &CollabPlainTextEdit::viewportScrolled);
        QVERIFY(spy.isValid());

        auto *bar = edit.verticalScrollBar();
        bar->setValue(bar->maximum() / 2);
        QApplication::processEvents();

        QVERIFY(spy.count() >= 1);
    }
```

- [ ] **Step 2: Verify it fails to compile**

```bash
cmake --build build-dev --target tst_collab_plain_text_edit -j$(nproc) 2>&1 | tail -10
```

Expected: `'viewportScrolled' is not a member of CollabText::Ui::CollabPlainTextEdit`.

- [ ] **Step 3: Declare the signal**

In `libs/collabtext/src/ui/CollabPlainTextEdit.h`, inside the existing `signals:` block (which currently contains `undoRequested` and `redoRequested`), add:

```cpp
    /// Emitted from scrollContentsBy after the base class has processed
    /// a local scroll event. The embedding app listens for this so it
    /// can refresh cached viewport anchors for ephemeral presence.
    void viewportScrolled();
```

- [ ] **Step 4: Emit from `scrollContentsBy`**

In `libs/collabtext/src/ui/CollabPlainTextEdit.cpp`, find the existing `scrollContentsBy` override (around line 192):

```cpp
void CollabPlainTextEdit::scrollContentsBy(int dx, int dy) {
    QPlainTextEdit::scrollContentsBy(dx, dy);
    updateCursorLabels();
}
```

Change it to:

```cpp
void CollabPlainTextEdit::scrollContentsBy(int dx, int dy) {
    QPlainTextEdit::scrollContentsBy(dx, dy);
    updateCursorLabels();
    emit viewportScrolled();
}
```

- [ ] **Step 5: Build and run**

```bash
cmake --build build-dev --target tst_collab_plain_text_edit -j$(nproc) 2>&1 | tail -5
./build-dev/libs/collabtext/tst_collab_plain_text_edit -platform offscreen 2>&1 | tail -10
```

Expected: `Totals: 12 passed, 0 failed, ...`.

- [ ] **Step 6: Commit**

```bash
git add libs/collabtext/src/ui/CollabPlainTextEdit.h libs/collabtext/src/ui/CollabPlainTextEdit.cpp libs/collabtext/tests/tst_collab_plain_text_edit.cpp
git commit -m "$(cat <<'EOF'
feat: CollabPlainTextEdit::viewportScrolled signal

Emits after QPlainTextEdit::scrollContentsBy processes a local scroll
event.  The embedding app uses it to refresh cached viewport anchors
for EphemeralState.viewport_top/bottom without polling.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: App-side `applyEditsPreservingScroll` + route all call sites

**Goal:** add the orchestration helper in `EditorPane` and replace every `applyEditsToQt` call site with it.

**Files:**
- Modify: `app/main.cpp`

This task has no automated test (the app isn't in a test harness). Verification is: the code compiles, the full `ctest` suite still passes, and a manual smoke run looks right.

- [ ] **Step 1: Add the helper method**

Open `app/main.cpp`. Find the definition of `applyEditsToQt` (around line 427). Directly above it, add the new helper:

```cpp
    /// Apply remote edits while preserving the local viewport scroll
    /// and the local cursor visibility. Capture a CRDT anchor at the
    /// top-visible byte offset before mutating, restore it after.
    /// This is the public entry point for any remote-edit path; the
    /// private applyEditsToQt is the raw UTF-16 mutation helper.
    void applyEditsPreservingScroll(const std::vector<TextEdit> &edits) {
        if (edits.empty()) return;

        // Capture viewport anchors before the tree mutates.
        uint32_t topByteOff = m_edit->topVisibleByteOffset();
        Anchor topAnchor = m_buffer.anchor_at(topByteOff, Bias::Left);

        uint32_t bottomByteOff = m_edit->bottomVisibleByteOffset();
        Anchor bottomAnchor = m_buffer.anchor_at(bottomByteOff, Bias::Left);

        applyEditsToQt(edits);

        // Restore scroll.
        uint32_t newTopByteOff = m_buffer.resolve_anchor(topAnchor);
        m_edit->scrollByteOffsetToTop(newTopByteOff,
                                      /*keepCursorVisible=*/true);

        // Cache the anchors for the next EphemeralState flush.
        m_viewportTopAnchor = topAnchor;
        m_viewportBottomAnchor = bottomAnchor;
    }
```

Note: `m_viewportTopAnchor` and `m_viewportBottomAnchor` don't exist yet — they're added in Step 2.

- [ ] **Step 2: Add the cached anchor members**

Still in `app/main.cpp`, find the `EditorPane` private members (around line 447-471, where `m_syncing`, `m_buffer`, `m_edit`, and the undo-coalescing state live). After `m_lastUndoDepth` and before the closing brace of the class, add:

```cpp

    // -------- Cached viewport anchors for ephemeral broadcast --------
    // Refreshed whenever a remote edit cycle runs (via
    // applyEditsPreservingScroll) or the local user scrolls the widget
    // (via onViewportScrolled). writeEphemeral reads these into
    // EphemeralState.viewport_top / viewport_bottom on each flush.
    std::optional<Anchor> m_viewportTopAnchor;
    std::optional<Anchor> m_viewportBottomAnchor;
```

At the top of `app/main.cpp`, make sure `<optional>` and the `Anchor` type are available. The existing includes should cover it — `Buffer.h` already includes `Anchor.h`, and `<optional>` is pulled in transitively. Verify the code builds before moving on.

- [ ] **Step 3: Replace all four `applyEditsToQt` call sites**

Search for `applyEditsToQt(` in `app/main.cpp`. There are **four** non-definition call sites (plus the method definition itself, which we keep). Replace each:

Call site 1, `poll()` (line ~148):
```cpp
// BEFORE:
            auto edits = m_buffer.edits_since(before);
            applyEditsToQt(edits);
// AFTER:
            auto edits = m_buffer.edits_since(before);
            applyEditsPreservingScroll(edits);
```

Call site 2, gremlin tick (line ~358):
```cpp
// BEFORE:
        applyEditsToQt(m_buffer.edits_since(before));
// AFTER:
        applyEditsPreservingScroll(m_buffer.edits_since(before));
```

Call site 3, `undoLocal()` (line ~377):
```cpp
// BEFORE:
        applyEditsToQt(m_buffer.edits_since(before));
// AFTER:
        applyEditsPreservingScroll(m_buffer.edits_since(before));
```

Call site 4, `redoLocal()` (line ~393):
```cpp
// BEFORE:
        applyEditsToQt(m_buffer.edits_since(before));
// AFTER:
        applyEditsPreservingScroll(m_buffer.edits_since(before));
```

After the edits, `applyEditsToQt` should only be called from one place: inside `applyEditsPreservingScroll`.

- [ ] **Step 4: Build the app and run the full test suite**

```bash
cmake --build build-dev -j$(nproc) 2>&1 | tail -5
ctest --test-dir build-dev --output-on-failure 2>&1 | tail -10
```

Expected: clean build; `100% tests passed, 0 tests failed out of 25` (one new test target from Task 1).

- [ ] **Step 5: Commit**

```bash
git add app/main.cpp
git commit -m "$(cat <<'EOF'
feat: EditorPane::applyEditsPreservingScroll + route all callers

Wraps applyEditsToQt with CRDT-anchor-based scroll preservation:
capture a top-line anchor before mutating the QTextDocument, then
scroll back to it after (cursor visibility wins on conflict).
Also caches top/bottom anchors for EphemeralState broadcasting.

All four remote-edit call sites (poll, gremlin tick, undo, redo)
now go through the wrapper.  The raw applyEditsToQt stays as the
private UTF-16 mutation helper; only one place in the app calls it.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: Wire `viewportScrolled` signal + populate `EphemeralState`

**Goal:** keep the cached viewport anchors fresh on local scrolls, and read them into `EphemeralState` in `writeEphemeral`.

**Files:**
- Modify: `app/main.cpp`

- [ ] **Step 1: Add the `viewportScrolled` slot**

In `app/main.cpp`, inside `EditorPane`'s existing `private slots:` block (where `undoLocal`, `redoLocal`, `onContentsChange`, etc. live), add a new slot:

```cpp
    /// Refresh cached viewport anchors after a local scroll event.
    /// Called from the CollabPlainTextEdit::viewportScrolled signal.
    void onViewportScrolled() {
        uint32_t topByteOff = m_edit->topVisibleByteOffset();
        uint32_t bottomByteOff = m_edit->bottomVisibleByteOffset();
        m_viewportTopAnchor = m_buffer.anchor_at(topByteOff, Bias::Left);
        m_viewportBottomAnchor = m_buffer.anchor_at(bottomByteOff, Bias::Left);
    }
```

- [ ] **Step 2: Wire the signal in the `EditorPane` constructor**

Find the block of `connect()` calls in the `EditorPane` constructor (around line 115-128 where `contentsChange`, `undoRequested`, `redoRequested` are connected). Add one more `connect` after the undo/redo wiring:

```cpp
        connect(m_edit, &CollabPlainTextEdit::viewportScrolled,
                this, &EditorPane::onViewportScrolled);
```

- [ ] **Step 3: Populate the anchors in `writeEphemeral`**

Find `writeEphemeral` in `app/main.cpp` (around line 173). It currently builds `EphemeralState` with `seq`, `timestamp`, `activity`, and a single cursor. Add the viewport anchor fields. The existing code:

```cpp
    /// Write ephemeral.json with cursor anchors.
    void writeEphemeral(uint64_t seq) {
        auto cursor = m_edit->textCursor();
        uint32_t bytePos = qtPosToByteOffset(cursor.position());
        uint32_t byteAnchor = qtPosToByteOffset(cursor.anchor());

        EphemeralState es;
        es.seq = seq;
        es.timestamp = now_iso8601();
        es.activity = "editing";

        auto posAnchor = m_buffer.anchor_at(bytePos, Bias::Right);
        auto selAnchor = m_buffer.anchor_at(byteAnchor, Bias::Left);
        es.cursors.push_back({selAnchor, posAnchor});

        m_presence.write_ephemeral(es);
    }
```

Change it to:

```cpp
    /// Write ephemeral.json with cursor and viewport anchors.
    void writeEphemeral(uint64_t seq) {
        auto cursor = m_edit->textCursor();
        uint32_t bytePos = qtPosToByteOffset(cursor.position());
        uint32_t byteAnchor = qtPosToByteOffset(cursor.anchor());

        EphemeralState es;
        es.seq = seq;
        es.timestamp = now_iso8601();
        es.activity = "editing";

        auto posAnchor = m_buffer.anchor_at(bytePos, Bias::Right);
        auto selAnchor = m_buffer.anchor_at(byteAnchor, Bias::Left);
        es.cursors.push_back({selAnchor, posAnchor});

        // Viewport anchors (populated by applyEditsPreservingScroll and
        // onViewportScrolled). nullopt before the first scroll event.
        es.viewport_top = m_viewportTopAnchor;
        es.viewport_bottom = m_viewportBottomAnchor;

        m_presence.write_ephemeral(es);
    }
```

- [ ] **Step 4: Build the app and run the full test suite**

```bash
cmake --build build-dev -j$(nproc) 2>&1 | tail -5
ctest --test-dir build-dev --output-on-failure 2>&1 | tail -10
```

Expected: clean build; `100% tests passed, 0 tests failed out of 25`.

- [ ] **Step 5: Commit**

```bash
git add app/main.cpp
git commit -m "$(cat <<'EOF'
feat: populate EphemeralState.viewport_top/bottom in writeEphemeral

Wire CollabPlainTextEdit::viewportScrolled to a new slot that refreshes
cached viewport anchors on every local scroll.  writeEphemeral reads
those anchors into EphemeralState so presence.json carries the current
viewport for future follow-mode.

Identity.cpp already serializes these optional fields, so no
serializer changes are required.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: App-level integration test for scroll stability

**Goal:** a black-box test with two `Buffer`s, a widget, and a `QTextDocument`. Scroll the widget, inject a remote edit that inserts text above the viewport, verify the top-visible byte offset points to the same original content after restoration.

**Files:**
- Create: `libs/collabtext/tests/tst_scroll_stability.cpp`
- Modify: `libs/collabtext/CMakeLists.txt`

- [ ] **Step 1: Create the integration test file**

```cpp
// libs/collabtext/tests/tst_scroll_stability.cpp
//
// Integration test: exercise the full scroll-preservation pipeline
// without running the app. Wire a Buffer + QTextDocument + widget the
// way EditorPane does, then verify that a remote edit above the
// viewport does not shift the visible text.

#include <QApplication>
#include <QScrollBar>
#include <QTest>
#include <QTextDocument>
#include <QTextCursor>

#include "crdt/Buffer.h"
#include "ui/CollabPlainTextEdit.h"

using namespace CollabText::Crdt;
using namespace CollabText::Ui;

class TestScrollStability : public QObject {
    Q_OBJECT

private slots:
    void remote_insert_above_viewport_preserves_top_anchor() {
        // Set up Alice's buffer and widget, populated with a 100-line doc.
        Buffer aliceBuf(1);
        {
            std::string text;
            for (int i = 0; i < 100; ++i) {
                text += "Line ";
                text += std::to_string(i);
                text += "\n";
            }
            aliceBuf.apply_local_edit({{0, 0}}, {text});
        }

        CollabPlainTextEdit edit;
        edit.setPlainText(QString::fromStdString(aliceBuf.text()));
        edit.resize(300, 120);
        edit.show();
        QVERIFY(QTest::qWaitForWindowExposed(&edit));

        // Scroll Alice to the middle of the document.
        auto *bar = edit.verticalScrollBar();
        bar->setValue(bar->maximum() / 2);
        QApplication::processEvents();

        uint32_t topByteBefore = edit.topVisibleByteOffset();
        QVERIFY(topByteBefore > 0);  // sanity: we actually scrolled

        // Capture the original content at the top.
        QString topLineBefore =
            edit.cursorForPosition(QPoint(0, 0)).block().text();

        // Simulate a remote insertion above the viewport. For the
        // scroll-preservation pipeline it doesn't matter whether the
        // mutation comes from the local replica or a remote one — the
        // anchor_at → mutate → resolve_anchor round-trip is the same.
        // Using apply_local_edit keeps the test self-contained without
        // needing a second Buffer and a sync path.
        Global beforeVersion = aliceBuf.version();
        std::string injected = "INSERTED BY BOB\n";
        aliceBuf.apply_local_edit({{0, 0}}, {injected});

        // Capture the CRDT anchor at the top BEFORE applying edits to Qt.
        Anchor topAnchor = aliceBuf.anchor_at(topByteBefore, Bias::Left);

        auto edits = aliceBuf.edits_since(beforeVersion);
        QVERIFY(!edits.empty());

        // Apply the edits to the QTextDocument by hand (integration test
        // shouldn't depend on EditorPane).
        for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
            QString docText = edit.document()->toPlainText();
            QByteArray utf8 = docText.toUtf8();
            uint32_t clampedStart = qMin(
                it->old_start, static_cast<uint32_t>(utf8.size()));
            uint32_t clampedEnd = qMin(
                it->old_end, static_cast<uint32_t>(utf8.size()));
            int qtStart = QString::fromUtf8(
                utf8.data(), static_cast<int>(clampedStart)).length();
            int qtEnd = QString::fromUtf8(
                utf8.data(), static_cast<int>(clampedEnd)).length();
            QTextCursor c(edit.document());
            c.setPosition(qtStart);
            if (qtEnd > qtStart)
                c.setPosition(qtEnd, QTextCursor::KeepAnchor);
            c.insertText(QString::fromUtf8(
                it->new_text.data(),
                static_cast<int>(it->new_text.size())));
        }

        // Restore scroll using the captured anchor.
        uint32_t restoredByte = aliceBuf.resolve_anchor(topAnchor);
        edit.scrollByteOffsetToTop(restoredByte, /*keepCursorVisible=*/true);
        QApplication::processEvents();

        // The new top-visible byte offset should equal the restored byte
        // offset (same visual content at the top).
        QCOMPARE(edit.topVisibleByteOffset(), restoredByte);

        // And the top line's text should still match what was there before.
        QString topLineAfter =
            edit.cursorForPosition(QPoint(0, 0)).block().text();
        QCOMPARE(topLineAfter, topLineBefore);
    }

    void remote_insert_below_viewport_leaves_top_unchanged() {
        Buffer buf(1);
        {
            std::string text;
            for (int i = 0; i < 100; ++i) {
                text += "Line ";
                text += std::to_string(i);
                text += "\n";
            }
            buf.apply_local_edit({{0, 0}}, {text});
        }

        CollabPlainTextEdit edit;
        edit.setPlainText(QString::fromStdString(buf.text()));
        edit.resize(300, 120);
        edit.show();
        QVERIFY(QTest::qWaitForWindowExposed(&edit));

        auto *bar = edit.verticalScrollBar();
        bar->setValue(bar->maximum() / 4);
        QApplication::processEvents();

        uint32_t topByteBefore = edit.topVisibleByteOffset();

        // Insert text at the END of the document (past the viewport).
        Global beforeVersion = buf.version();
        uint32_t docLen = static_cast<uint32_t>(buf.text().size());
        buf.apply_local_edit({{docLen, docLen}}, {"APPENDED\n"});

        Anchor topAnchor = buf.anchor_at(topByteBefore, Bias::Left);

        auto edits = buf.edits_since(beforeVersion);
        for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
            QString docText = edit.document()->toPlainText();
            QByteArray utf8 = docText.toUtf8();
            uint32_t clampedStart = qMin(
                it->old_start, static_cast<uint32_t>(utf8.size()));
            uint32_t clampedEnd = qMin(
                it->old_end, static_cast<uint32_t>(utf8.size()));
            int qtStart = QString::fromUtf8(
                utf8.data(), static_cast<int>(clampedStart)).length();
            int qtEnd = QString::fromUtf8(
                utf8.data(), static_cast<int>(clampedEnd)).length();
            QTextCursor c(edit.document());
            c.setPosition(qtStart);
            if (qtEnd > qtStart)
                c.setPosition(qtEnd, QTextCursor::KeepAnchor);
            c.insertText(QString::fromUtf8(
                it->new_text.data(),
                static_cast<int>(it->new_text.size())));
        }

        uint32_t restoredByte = buf.resolve_anchor(topAnchor);
        edit.scrollByteOffsetToTop(restoredByte, /*keepCursorVisible=*/true);
        QApplication::processEvents();

        // A below-viewport edit must not change the top byte offset.
        QCOMPARE(restoredByte, topByteBefore);
        QCOMPARE(edit.topVisibleByteOffset(), topByteBefore);
    }
};

QTEST_MAIN(TestScrollStability)
#include "tst_scroll_stability.moc"
```

- [ ] **Step 2: Register the test in CMake**

Open `libs/collabtext/CMakeLists.txt` and add after `add_crdt_test(tst_collab_plain_text_edit)`:

```cmake
add_crdt_test(tst_scroll_stability)
```

- [ ] **Step 3: Build and run**

```bash
cmake -S . -B build-dev 2>&1 | tail -5
cmake --build build-dev --target tst_scroll_stability -j$(nproc) 2>&1 | tail -5
./build-dev/libs/collabtext/tst_scroll_stability -platform offscreen 2>&1 | tail -10
```

Expected: clean build; `Totals: 4 passed, 0 failed, 0 skipped, 0 blacklisted, ...ms`.

- [ ] **Step 4: Run the full test suite**

```bash
ctest --test-dir build-dev --output-on-failure 2>&1 | tail -10
```

Expected: `100% tests passed, 0 tests failed out of 26` (two new targets total: `tst_collab_plain_text_edit` and `tst_scroll_stability`).

- [ ] **Step 5: Commit**

```bash
git add libs/collabtext/tests/tst_scroll_stability.cpp libs/collabtext/CMakeLists.txt
git commit -m "$(cat <<'EOF'
test: integration test for scroll-stability pipeline

Black-box test that wires a Buffer + QTextDocument + CollabPlainTextEdit
the way EditorPane does and verifies:
- Remote edits above the viewport do not shift the top-visible text.
- Remote edits below the viewport leave the top byte offset unchanged.

Both cases exercise the full capture-apply-restore cycle including
Buffer::anchor_at and Buffer::resolve_anchor.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Post-flight: final verification

- [ ] **Step 1: Full test suite + benchmark regression check**

```bash
cd /home/clinton/dev/collabtext
cmake --build build-dev -j$(nproc) 2>&1 | tail -3
ctest --test-dir build-dev --output-on-failure 2>&1 | tail -10
```

Expected: `100% tests passed, 0 tests failed out of 26`. Benchmark delta is not a hard gate, but nothing should regress significantly.

- [ ] **Step 2: Verify the working tree is clean**

```bash
git status
git log --oneline -10
```

Expected: nothing uncommitted. The last ~9 commits are this plan's tasks.

---

## Definition of done

- All 9 tasks committed.
- `tst_collab_plain_text_edit` and `tst_scroll_stability` run green.
- Full `ctest` suite (26 tests) green.
- No changes to `Buffer.h`, `Anchor.h`, or `identity/Identity.cpp`.
- Manual smoke in the app: scroll one pane down mid-document; type into the other pane above the visible region; the scrolled pane does not jump.
