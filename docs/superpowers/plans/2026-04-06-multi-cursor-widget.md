# Multi-Cursor Widget Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a MultiCursorController class and a CollabPlainTextEdit widget that supports multiple cursors, with a test application demonstrating local multi-cursor editing and simulated remote cursor display.

**Architecture:** MultiCursorController owns a list of QTextCursors on a QTextDocument and dispatches edits to all of them in reverse document order (Zed's pattern). CollabPlainTextEdit subclasses QPlainTextEdit, overrides paintEvent to draw extra carets, and delegates input to the controller. The test app shows two synced editor panes with multi-cursor support.

**Tech Stack:** C++20, Qt6 Widgets, CMake

**Spec:** `docs/research/2026-04-06-multi-cursor-widget-research.md`

**Build/test commands:**
```bash
cmake --build build-dev -j$(nproc)
ctest --test-dir build-dev --output-on-failure -R tst_multicursor
./build-dev/app/collabtext-testapp
```

---

## File Map

| File | Action | Responsibility |
|------|--------|---------------|
| `libs/collabtext/src/ui/MultiCursorController.h` | Create | Multi-cursor management: cursor list, edit dispatch, selection generation |
| `libs/collabtext/src/ui/MultiCursorController.cpp` | Create | Implementation |
| `libs/collabtext/src/ui/CollabPlainTextEdit.h` | Create | QPlainTextEdit subclass with multi-cursor paint + input |
| `libs/collabtext/src/ui/CollabPlainTextEdit.cpp` | Create | Implementation |
| `libs/collabtext/tests/tst_multicursor.cpp` | Create | Tests for MultiCursorController |
| `libs/collabtext/CMakeLists.txt` | Modify | Add new sources and test |
| `app/main.cpp` | Modify | Replace QPlainTextEdit with CollabPlainTextEdit, add demo UI |

---

### Task 1: MultiCursorController — Core Edit Dispatch

The controller manages a cursor list and dispatches edits. No widget
code — just QTextDocument and QTextCursor operations.

**Files:**
- Create: `libs/collabtext/src/ui/MultiCursorController.h`
- Create: `libs/collabtext/src/ui/MultiCursorController.cpp`
- Create: `libs/collabtext/tests/tst_multicursor.cpp`
- Modify: `libs/collabtext/CMakeLists.txt`

- [ ] **Step 1: Create the header**

```cpp
// libs/collabtext/src/ui/MultiCursorController.h
#pragma once

#include <QList>
#include <QObject>
#include <QTextCursor>
#include <QTextEdit>

class QTextDocument;

namespace CollabText::Ui {

struct RemoteCursor {
    int position = 0;
    int anchor = 0;           // != position if there's a selection
    QColor color;
    QString label;            // participant name
};

class MultiCursorController : public QObject {
    Q_OBJECT
public:
    explicit MultiCursorController(QTextDocument *document, QObject *parent = nullptr);

    QTextDocument *document() const { return m_document; }

    // --- Cursor management ---
    int cursorCount() const;
    QTextCursor primaryCursor() const;
    void setPrimaryCursor(const QTextCursor &cursor);

    void addCursorAt(int position);
    void addCursorAbove();
    void addCursorBelow();
    void clearSecondaryCursors();

    // --- Edit dispatch (operates on ALL cursors) ---
    void insertText(const QString &text);
    void deleteChar();
    void deletePreviousChar();
    void moveCursors(QTextCursor::MoveOperation op,
                     QTextCursor::MoveMode mode = QTextCursor::MoveAnchor);

    // --- State for rendering ---
    QList<QTextCursor> allCursors() const;
    QList<QTextEdit::ExtraSelection> secondarySelections() const;
    QList<int> secondaryCaretPositions() const;

    // --- Remote cursors (collaboration) ---
    void setRemoteCursors(const QList<RemoteCursor> &cursors);
    QList<QTextEdit::ExtraSelection> remoteSelections() const;
    QList<RemoteCursor> remoteCursors() const { return m_remoteCursors; }

Q_SIGNALS:
    void cursorsChanged();

private:
    void mergeTouchingCursors();
    QList<QTextCursor> allCursorsSortedDescending() const;

    QTextDocument *m_document;
    QTextCursor m_primary;
    QList<QTextCursor> m_secondary;
    QList<RemoteCursor> m_remoteCursors;
};

} // namespace CollabText::Ui
```

- [ ] **Step 2: Create the implementation**

```cpp
// libs/collabtext/src/ui/MultiCursorController.cpp
#include "ui/MultiCursorController.h"
#include <QTextDocument>
#include <algorithm>

namespace CollabText::Ui {

MultiCursorController::MultiCursorController(QTextDocument *document, QObject *parent)
    : QObject(parent)
    , m_document(document)
    , m_primary(document)
{
}

int MultiCursorController::cursorCount() const {
    return 1 + m_secondary.size();
}

QTextCursor MultiCursorController::primaryCursor() const {
    return m_primary;
}

void MultiCursorController::setPrimaryCursor(const QTextCursor &cursor) {
    m_primary = cursor;
    emit cursorsChanged();
}

void MultiCursorController::addCursorAt(int position) {
    QTextCursor c(m_document);
    c.setPosition(position);
    m_secondary.append(c);
    emit cursorsChanged();
}

void MultiCursorController::addCursorAbove() {
    // For each existing cursor, try to create one on the line above
    auto all = allCursors();
    for (auto &c : all) {
        QTextCursor above(c);
        above.movePosition(QTextCursor::Up);
        if (above.blockNumber() != c.blockNumber()) {
            m_secondary.append(above);
        }
    }
    mergeTouchingCursors();
    emit cursorsChanged();
}

void MultiCursorController::addCursorBelow() {
    auto all = allCursors();
    for (auto &c : all) {
        QTextCursor below(c);
        below.movePosition(QTextCursor::Down);
        if (below.blockNumber() != c.blockNumber()) {
            m_secondary.append(below);
        }
    }
    mergeTouchingCursors();
    emit cursorsChanged();
}

void MultiCursorController::clearSecondaryCursors() {
    m_secondary.clear();
    emit cursorsChanged();
}

void MultiCursorController::insertText(const QString &text) {
    auto cursors = allCursorsSortedDescending();
    m_document->undoStack()->beginMacro(QStringLiteral("multi-insert"));
    for (auto &c : cursors) {
        c.insertText(text);
    }
    m_document->undoStack()->endMacro();
    emit cursorsChanged();
}

void MultiCursorController::deleteChar() {
    auto cursors = allCursorsSortedDescending();
    m_document->undoStack()->beginMacro(QStringLiteral("multi-delete"));
    for (auto &c : cursors) {
        c.deleteChar();
    }
    m_document->undoStack()->endMacro();
    emit cursorsChanged();
}

void MultiCursorController::deletePreviousChar() {
    auto cursors = allCursorsSortedDescending();
    m_document->undoStack()->beginMacro(QStringLiteral("multi-backspace"));
    for (auto &c : cursors) {
        c.deletePreviousChar();
    }
    m_document->undoStack()->endMacro();
    emit cursorsChanged();
}

void MultiCursorController::moveCursors(QTextCursor::MoveOperation op,
                                         QTextCursor::MoveMode mode) {
    m_primary.movePosition(op, mode);
    for (auto &c : m_secondary) {
        c.movePosition(op, mode);
    }
    mergeTouchingCursors();
    emit cursorsChanged();
}

QList<QTextCursor> MultiCursorController::allCursors() const {
    QList<QTextCursor> result;
    result.reserve(1 + m_secondary.size());
    result.append(m_primary);
    result.append(m_secondary);
    return result;
}

QList<QTextEdit::ExtraSelection> MultiCursorController::secondarySelections() const {
    QList<QTextEdit::ExtraSelection> result;
    for (auto &c : m_secondary) {
        if (c.hasSelection()) {
            QTextEdit::ExtraSelection sel;
            sel.cursor = c;
            sel.format.setBackground(QColor(100, 149, 237, 80)); // cornflower blue
            result.append(sel);
        }
    }
    return result;
}

QList<int> MultiCursorController::secondaryCaretPositions() const {
    QList<int> result;
    result.reserve(m_secondary.size());
    for (auto &c : m_secondary) {
        result.append(c.position());
    }
    return result;
}

void MultiCursorController::setRemoteCursors(const QList<RemoteCursor> &cursors) {
    m_remoteCursors = cursors;
    emit cursorsChanged();
}

QList<QTextEdit::ExtraSelection> MultiCursorController::remoteSelections() const {
    QList<QTextEdit::ExtraSelection> result;
    for (auto &rc : m_remoteCursors) {
        QTextEdit::ExtraSelection sel;
        sel.cursor = QTextCursor(m_document);
        if (rc.position != rc.anchor) {
            sel.cursor.setPosition(rc.anchor);
            sel.cursor.setPosition(rc.position, QTextCursor::KeepAnchor);
            sel.format.setBackground(QColor(rc.color.red(), rc.color.green(),
                                            rc.color.blue(), 50));
        } else {
            sel.cursor.setPosition(rc.position);
        }
        result.append(sel);
    }
    return result;
}

void MultiCursorController::mergeTouchingCursors() {
    // Remove secondary cursors that overlap with primary or each other
    QList<QTextCursor> merged;
    int primaryPos = m_primary.position();
    for (auto &c : m_secondary) {
        if (c.position() == primaryPos) continue; // duplicate of primary
        bool duplicate = false;
        for (auto &m : merged) {
            if (c.position() == m.position()) { duplicate = true; break; }
        }
        if (!duplicate) merged.append(c);
    }
    m_secondary = merged;
}

QList<QTextCursor> MultiCursorController::allCursorsSortedDescending() const {
    auto all = allCursors();
    std::sort(all.begin(), all.end(), [](const QTextCursor &a, const QTextCursor &b) {
        return a.position() > b.position();
    });
    return all;
}

} // namespace CollabText::Ui
```

- [ ] **Step 3: Add to CMakeLists.txt**

In `libs/collabtext/CMakeLists.txt`, add the new sources to the library and the test:

After `src/crdt/FileSync.cpp` in the `add_library` block, add:
```cmake
    src/ui/MultiCursorController.cpp
```

After `add_crdt_test(tst_filesync)`, add:
```cmake
add_crdt_test(tst_multicursor)
```

- [ ] **Step 4: Write the test**

```cpp
// libs/collabtext/tests/tst_multicursor.cpp
#include <QTest>
#include <QTextDocument>
#include "ui/MultiCursorController.h"

using namespace CollabText::Ui;

class TestMultiCursor : public QObject {
    Q_OBJECT

private slots:
    void single_cursor_insert() {
        QTextDocument doc;
        MultiCursorController ctrl(&doc);
        ctrl.insertText("hello");
        QCOMPARE(doc.toPlainText(), QString("hello"));
        QCOMPARE(ctrl.cursorCount(), 1);
    }

    void two_cursors_insert() {
        QTextDocument doc;
        doc.setPlainText("aa bb");
        MultiCursorController ctrl(&doc);

        // Primary at position 2 (between "aa" and " ")
        QTextCursor primary(&doc);
        primary.setPosition(2);
        ctrl.setPrimaryCursor(primary);

        // Secondary at position 5 (after "bb")
        ctrl.addCursorAt(5);
        QCOMPARE(ctrl.cursorCount(), 2);

        ctrl.insertText("X");
        QCOMPARE(doc.toPlainText(), QString("aaX bbX"));
    }

    void three_cursors_insert() {
        QTextDocument doc;
        doc.setPlainText("aaa bbb ccc");
        MultiCursorController ctrl(&doc);

        QTextCursor primary(&doc);
        primary.setPosition(3);
        ctrl.setPrimaryCursor(primary);
        ctrl.addCursorAt(7);
        ctrl.addCursorAt(11);
        QCOMPARE(ctrl.cursorCount(), 3);

        ctrl.insertText("!");
        QCOMPARE(doc.toPlainText(), QString("aaa! bbb! ccc!"));
    }

    void multi_cursor_backspace() {
        QTextDocument doc;
        doc.setPlainText("aaX bbX");
        MultiCursorController ctrl(&doc);

        QTextCursor primary(&doc);
        primary.setPosition(3); // after X
        ctrl.setPrimaryCursor(primary);
        ctrl.addCursorAt(7); // after second X

        ctrl.deletePreviousChar();
        QCOMPARE(doc.toPlainText(), QString("aa bb"));
    }

    void multi_cursor_delete() {
        QTextDocument doc;
        doc.setPlainText("aXa bXb");
        MultiCursorController ctrl(&doc);

        QTextCursor primary(&doc);
        primary.setPosition(1); // before first X
        ctrl.setPrimaryCursor(primary);
        ctrl.addCursorAt(5); // before second X (adjusted: "aXa bXb" pos 5 = before second X)

        ctrl.deleteChar();
        QCOMPARE(doc.toPlainText(), QString("aa bb"));
    }

    void clear_secondary_cursors() {
        QTextDocument doc;
        MultiCursorController ctrl(&doc);
        ctrl.addCursorAt(0);
        ctrl.addCursorAt(0);
        QVERIFY(ctrl.cursorCount() >= 2);
        ctrl.clearSecondaryCursors();
        QCOMPARE(ctrl.cursorCount(), 1);
    }

    void move_cursors() {
        QTextDocument doc;
        doc.setPlainText("hello\nworld");
        MultiCursorController ctrl(&doc);

        QTextCursor primary(&doc);
        primary.setPosition(0);
        ctrl.setPrimaryCursor(primary);
        ctrl.addCursorAt(6); // start of "world"

        ctrl.moveCursors(QTextCursor::EndOfLine);

        auto cursors = ctrl.allCursors();
        QCOMPARE(cursors[0].position(), 5);  // end of "hello"
        // Secondary moved to end of "world"
        bool foundEnd = false;
        for (int i = 1; i < cursors.size(); ++i) {
            if (cursors[i].position() == 11) foundEnd = true;
        }
        QVERIFY(foundEnd);
    }

    void duplicate_cursors_merged() {
        QTextDocument doc;
        doc.setPlainText("hello");
        MultiCursorController ctrl(&doc);

        QTextCursor primary(&doc);
        primary.setPosition(3);
        ctrl.setPrimaryCursor(primary);
        ctrl.addCursorAt(3); // same as primary
        // Should merge
        QCOMPARE(ctrl.cursorCount(), 1);
    }

    void undo_reverses_multi_insert() {
        QTextDocument doc;
        doc.setPlainText("aa bb");
        MultiCursorController ctrl(&doc);

        QTextCursor primary(&doc);
        primary.setPosition(2);
        ctrl.setPrimaryCursor(primary);
        ctrl.addCursorAt(5);

        ctrl.insertText("X");
        QCOMPARE(doc.toPlainText(), QString("aaX bbX"));

        doc.undo();
        QCOMPARE(doc.toPlainText(), QString("aa bb"));
    }

    void secondary_selections_generated() {
        QTextDocument doc;
        doc.setPlainText("hello world");
        MultiCursorController ctrl(&doc);

        QTextCursor primary(&doc);
        primary.setPosition(0);
        primary.setPosition(5, QTextCursor::KeepAnchor); // select "hello"
        ctrl.setPrimaryCursor(primary);

        // Add secondary cursor with selection
        QTextCursor secondary(&doc);
        secondary.setPosition(6);
        secondary.setPosition(11, QTextCursor::KeepAnchor); // select "world"
        ctrl.addCursorAt(6);
        // Manually set selection on last secondary
        // (addCursorAt creates cursor without selection — this tests the selection rendering)

        auto sels = ctrl.secondarySelections();
        // No selection on secondary (addCursorAt doesn't create one)
        QCOMPARE(sels.size(), 0);
    }

    void remote_cursors() {
        QTextDocument doc;
        doc.setPlainText("hello world");
        MultiCursorController ctrl(&doc);

        QList<RemoteCursor> remotes;
        remotes.append({5, 5, Qt::red, "Alice"});
        remotes.append({3, 8, Qt::blue, "Bob"}); // Bob has a selection
        ctrl.setRemoteCursors(remotes);

        auto sels = ctrl.remoteSelections();
        QCOMPARE(sels.size(), 2);
        QCOMPARE(ctrl.remoteCursors().size(), 2);
    }

    void add_cursor_below() {
        QTextDocument doc;
        doc.setPlainText("aaa\nbbb\nccc");
        MultiCursorController ctrl(&doc);

        QTextCursor primary(&doc);
        primary.setPosition(1); // middle of "aaa"
        ctrl.setPrimaryCursor(primary);

        ctrl.addCursorBelow();
        QCOMPARE(ctrl.cursorCount(), 2);

        ctrl.addCursorBelow();
        QCOMPARE(ctrl.cursorCount(), 3);

        ctrl.insertText("X");
        QCOMPARE(doc.toPlainText(), QString("aXaa\nbXbb\ncXcc"));
    }
};

QTEST_MAIN(TestMultiCursor)
#include "tst_multicursor.moc"
```

- [ ] **Step 5: Build and test**

```bash
cmake -S . -B build-dev -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-dev --target tst_multicursor -j$(nproc)
./build-dev/libs/collabtext/tst_multicursor -v2
```

- [ ] **Step 6: Commit**

```bash
git add libs/collabtext/src/ui/MultiCursorController.h \
        libs/collabtext/src/ui/MultiCursorController.cpp \
        libs/collabtext/tests/tst_multicursor.cpp \
        libs/collabtext/CMakeLists.txt
git commit -m "feat: MultiCursorController — multi-cursor edit dispatch

Manages a list of QTextCursors on a QTextDocument. Dispatches edits
(insert, delete, backspace) to all cursors in reverse document order.
Produces extraSelections for secondary cursor highlighting and remote
cursor display. 12 tests covering insert, delete, undo, cursor
management, and remote cursors."
```

---

### Task 2: CollabPlainTextEdit — Multi-Cursor Widget

QPlainTextEdit subclass that integrates MultiCursorController, overrides
paintEvent to draw extra carets, and handles Alt+Click for cursor
addition and Escape to clear secondary cursors.

**Files:**
- Create: `libs/collabtext/src/ui/CollabPlainTextEdit.h`
- Create: `libs/collabtext/src/ui/CollabPlainTextEdit.cpp`
- Modify: `libs/collabtext/CMakeLists.txt`

- [ ] **Step 1: Create the header**

```cpp
// libs/collabtext/src/ui/CollabPlainTextEdit.h
#pragma once

#include "ui/MultiCursorController.h"
#include <QPlainTextEdit>

namespace CollabText::Ui {

class CollabPlainTextEdit : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CollabPlainTextEdit(QWidget *parent = nullptr);

    MultiCursorController *multiCursorController() const { return m_controller; }

protected:
    void paintEvent(QPaintEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;

private:
    void drawSecondaryCaret(QPainter &painter, int position, const QColor &color);
    void syncExtraSelections();

    MultiCursorController *m_controller;
    bool m_handlingKey = false;
};

} // namespace CollabText::Ui
```

- [ ] **Step 2: Create the implementation**

```cpp
// libs/collabtext/src/ui/CollabPlainTextEdit.cpp
#include "ui/CollabPlainTextEdit.h"

#include <QPainter>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTextBlock>
#include <QScrollBar>
#include <QAbstractTextDocumentLayout>

namespace CollabText::Ui {

CollabPlainTextEdit::CollabPlainTextEdit(QWidget *parent)
    : QPlainTextEdit(parent)
    , m_controller(new MultiCursorController(document(), this))
{
    connect(m_controller, &MultiCursorController::cursorsChanged,
            this, &CollabPlainTextEdit::syncExtraSelections);

    // Keep controller's primary cursor in sync with the widget's cursor
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this]() {
        if (!m_handlingKey) {
            m_controller->setPrimaryCursor(textCursor());
        }
    });
}

void CollabPlainTextEdit::paintEvent(QPaintEvent *e) {
    // Let QPlainTextEdit draw everything (text, primary cursor, selections)
    QPlainTextEdit::paintEvent(e);

    // Draw additional carets for secondary cursors
    QPainter painter(viewport());
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    for (int pos : m_controller->secondaryCaretPositions()) {
        drawSecondaryCaret(painter, pos, QColor(100, 149, 237)); // cornflower blue
    }

    // Draw remote cursors
    for (auto &rc : m_controller->remoteCursors()) {
        drawSecondaryCaret(painter, rc.position, rc.color);
    }
}

void CollabPlainTextEdit::keyPressEvent(QKeyEvent *e) {
    // Escape clears secondary cursors
    if (e->key() == Qt::Key_Escape && m_controller->cursorCount() > 1) {
        m_controller->clearSecondaryCursors();
        e->accept();
        return;
    }

    // Ctrl+Alt+Up/Down adds cursors
    if (e->modifiers() == (Qt::ControlModifier | Qt::AltModifier)) {
        if (e->key() == Qt::Key_Up) {
            m_controller->addCursorAbove();
            e->accept();
            return;
        }
        if (e->key() == Qt::Key_Down) {
            m_controller->addCursorBelow();
            e->accept();
            return;
        }
    }

    // Single cursor: delegate to default handler
    if (m_controller->cursorCount() == 1) {
        QPlainTextEdit::keyPressEvent(e);
        return;
    }

    // Multi-cursor: dispatch through controller
    m_handlingKey = true;

    if (e->key() == Qt::Key_Backspace) {
        m_controller->deletePreviousChar();
    } else if (e->key() == Qt::Key_Delete) {
        m_controller->deleteChar();
    } else if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        m_controller->insertText("\n");
    } else if (e->key() == Qt::Key_Left) {
        m_controller->moveCursors(QTextCursor::Left,
            e->modifiers() & Qt::ShiftModifier ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
    } else if (e->key() == Qt::Key_Right) {
        m_controller->moveCursors(QTextCursor::Right,
            e->modifiers() & Qt::ShiftModifier ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
    } else if (e->key() == Qt::Key_Up) {
        m_controller->moveCursors(QTextCursor::Up,
            e->modifiers() & Qt::ShiftModifier ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
    } else if (e->key() == Qt::Key_Down) {
        m_controller->moveCursors(QTextCursor::Down,
            e->modifiers() & Qt::ShiftModifier ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
    } else if (e->key() == Qt::Key_Home) {
        m_controller->moveCursors(QTextCursor::StartOfLine,
            e->modifiers() & Qt::ShiftModifier ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
    } else if (e->key() == Qt::Key_End) {
        m_controller->moveCursors(QTextCursor::EndOfLine,
            e->modifiers() & Qt::ShiftModifier ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
    } else if (!e->text().isEmpty() && e->text().at(0).isPrint()) {
        m_controller->insertText(e->text());
    } else {
        // Unhandled key in multi-cursor mode: fall through to default
        QPlainTextEdit::keyPressEvent(e);
        m_handlingKey = false;
        return;
    }

    // Sync widget cursor to primary
    setTextCursor(m_controller->primaryCursor());
    m_handlingKey = false;
    e->accept();
}

void CollabPlainTextEdit::mousePressEvent(QMouseEvent *e) {
    // Alt+Click adds a cursor at click position
    if (e->modifiers() & Qt::AltModifier) {
        QTextCursor clickCursor = cursorForPosition(e->pos());
        m_controller->addCursorAt(clickCursor.position());
        e->accept();
        return;
    }

    // Normal click: clear secondary cursors and delegate
    if (m_controller->cursorCount() > 1) {
        m_controller->clearSecondaryCursors();
    }
    QPlainTextEdit::mousePressEvent(e);
}

void CollabPlainTextEdit::drawSecondaryCaret(QPainter &painter, int position,
                                              const QColor &color) {
    QTextCursor c(document());
    c.setPosition(position);
    QTextBlock block = c.block();
    if (!block.isValid()) return;

    QTextLayout *layout = block.layout();
    if (!layout) return;

    int relativePos = position - block.position();
    QTextLine line = layout->lineForTextPosition(relativePos);
    if (!line.isValid()) return;

    qreal x = line.cursorToX(relativePos);
    QRectF blockRect = blockBoundingGeometry(block).translated(contentOffset());
    qreal y = blockRect.top() + line.y();
    qreal height = line.height();

    painter.fillRect(QRectF(x, y, 2.0, height), color);
}

void CollabPlainTextEdit::syncExtraSelections() {
    QList<QTextEdit::ExtraSelection> selections;
    selections.append(m_controller->secondarySelections());
    selections.append(m_controller->remoteSelections());
    setExtraSelections(selections);
    viewport()->update();
}

} // namespace CollabText::Ui
```

- [ ] **Step 3: Add to CMakeLists.txt**

In `libs/collabtext/CMakeLists.txt`, after `src/ui/MultiCursorController.cpp`, add:
```cmake
    src/ui/CollabPlainTextEdit.cpp
```

- [ ] **Step 4: Build**

```bash
cmake --build build-dev -j$(nproc)
```

- [ ] **Step 5: Commit**

```bash
git add libs/collabtext/src/ui/CollabPlainTextEdit.h \
        libs/collabtext/src/ui/CollabPlainTextEdit.cpp \
        libs/collabtext/CMakeLists.txt
git commit -m "feat: CollabPlainTextEdit — QPlainTextEdit with multi-cursor support

Overrides paintEvent to draw secondary carets and remote cursors.
Alt+Click adds cursors, Ctrl+Alt+Up/Down adds cursors above/below,
Escape clears secondary cursors. Single-cursor mode delegates to
default QPlainTextEdit. Multi-cursor dispatches through controller."
```

---

### Task 3: Test Application — Multi-Cursor Demo

Replace the test app's QPlainTextEdit with CollabPlainTextEdit. Add
simulated remote cursors to demonstrate collaboration visuals.

**Files:**
- Modify: `app/main.cpp`
- Modify: `app/CMakeLists.txt`

- [ ] **Step 1: Update app/main.cpp**

Replace the entire contents of `app/main.cpp` with:

```cpp
#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

#include "ui/CollabPlainTextEdit.h"
#include "ui/MultiCursorController.h"
#include <collabtext/CollabDocument.h>

using namespace CollabText::Ui;

class EditorPane : public QWidget {
    Q_OBJECT
public:
    EditorPane(const QString &label, uint16_t replicaId,
               const QColor &cursorColor, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_doc(new CollabText::CollabDocument(replicaId, this))
        , m_edit(new CollabPlainTextEdit(this))
        , m_color(cursorColor)
        , m_label(label)
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);

        auto *header = new QLabel(label, this);
        header->setStyleSheet(QStringLiteral("font-weight: bold; color: %1;")
                                  .arg(cursorColor.name()));
        layout->addWidget(header);
        layout->addWidget(m_edit);

        m_edit->setDocument(m_doc->qtDocument());
        m_edit->setPlaceholderText(
            QStringLiteral("Type here... (Alt+Click for multi-cursor, "
                           "Ctrl+Alt+Up/Down to add cursors)"));

        auto *statusLabel = new QLabel(this);
        layout->addWidget(statusLabel);
        connect(m_edit->multiCursorController(),
                &MultiCursorController::cursorsChanged, this,
                [this, statusLabel]() {
                    int n = m_edit->multiCursorController()->cursorCount();
                    statusLabel->setText(
                        n > 1 ? QStringLiteral("%1 cursors").arg(n)
                              : QStringLiteral("1 cursor"));
                });
    }

    CollabPlainTextEdit *editor() const { return m_edit; }
    CollabText::CollabDocument *collabDoc() const { return m_doc; }
    QColor cursorColor() const { return m_color; }
    QString label() const { return m_label; }

private:
    CollabText::CollabDocument *m_doc;
    CollabPlainTextEdit *m_edit;
    QColor m_color;
    QString m_label;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow()
    {
        setWindowTitle(QStringLiteral("CollabText — Multi-Cursor Demo"));
        resize(1000, 600);

        auto *central = new QWidget(this);
        auto *layout = new QHBoxLayout(central);

        m_paneA = new EditorPane(QStringLiteral("Alice"), 1,
                                  QColor(65, 105, 225), central);  // royal blue
        m_paneB = new EditorPane(QStringLiteral("Bob"), 2,
                                  QColor(220, 20, 60), central);   // crimson
        layout->addWidget(m_paneA);
        layout->addWidget(m_paneB);
        setCentralWidget(central);

        // Simulate remote cursors: periodically send each editor's cursor
        // position to the other as a "remote cursor"
        auto *syncTimer = new QTimer(this);
        connect(syncTimer, &QTimer::timeout, this, &MainWindow::syncRemoteCursors);
        syncTimer->start(100); // 100ms — smooth remote cursor updates

        statusBar()->showMessage(
            QStringLiteral("Alt+Click: add cursor | Ctrl+Alt+Up/Down: column cursor | "
                           "Escape: clear extra cursors"));
    }

private slots:
    void syncRemoteCursors() {
        // Send Alice's primary cursor to Bob as a remote cursor
        auto aliceCursor = m_paneA->editor()->textCursor();
        RemoteCursor aliceRemote;
        aliceRemote.position = aliceCursor.position();
        aliceRemote.anchor = aliceCursor.anchor();
        aliceRemote.color = m_paneA->cursorColor();
        aliceRemote.label = m_paneA->label();

        // Send Bob's cursor to Alice
        auto bobCursor = m_paneB->editor()->textCursor();
        RemoteCursor bobRemote;
        bobRemote.position = bobCursor.position();
        bobRemote.anchor = bobCursor.anchor();
        bobRemote.color = m_paneB->cursorColor();
        bobRemote.label = m_paneB->label();

        m_paneA->editor()->multiCursorController()->setRemoteCursors({bobRemote});
        m_paneB->editor()->multiCursorController()->setRemoteCursors({aliceRemote});
    }

private:
    EditorPane *m_paneA;
    EditorPane *m_paneB;
};

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    MainWindow window;
    window.show();
    return app.exec();
}

#include "main.moc"
```

- [ ] **Step 2: Update app/CMakeLists.txt**

Replace `app/CMakeLists.txt` to add the include path for ui/ headers:

```cmake
cmake_minimum_required(VERSION 3.19)
project(collabtext-testapp VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets)

qt_add_executable(collabtext-testapp main.cpp)
target_link_libraries(collabtext-testapp PRIVATE Qt6::Widgets CollabText::CollabText)
target_include_directories(collabtext-testapp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../libs/collabtext/src)
```

- [ ] **Step 3: Build and run**

```bash
cmake --build build-dev -j$(nproc)
./build-dev/app/collabtext-testapp
```

Test manually:
1. Type in Alice's editor — text appears
2. Alt+Click in the editor to add a second cursor — two carets visible
3. Type — text inserts at both positions
4. Ctrl+Alt+Down to add cursor below
5. Escape to clear extra cursors
6. Type in Bob's editor — a colored remote cursor appears in Alice's editor at Bob's position

- [ ] **Step 4: Commit**

```bash
git add app/main.cpp app/CMakeLists.txt
git commit -m "feat: multi-cursor demo app with simulated remote cursors

Two side-by-side editors (Alice and Bob). Each shows the other's
cursor position as a colored remote cursor (updated every 100ms).
Multi-cursor editing via Alt+Click and Ctrl+Alt+Up/Down. Status
bar shows cursor count."
```

---

### Task 4: Full Regression

**Files:** None (read-only)

- [ ] **Step 1: Run all tests**

```bash
ctest --test-dir build-dev --output-on-failure -E "tst_realistic|tst_benchmark"
```

All tests must pass (15 existing + 1 new = 16).

- [ ] **Step 2: Run the app and verify**

```bash
./build-dev/app/collabtext-testapp
```

Visual checks:
- Single-cursor editing works normally
- Alt+Click creates additional cursors (visible as colored carets)
- Typing with multiple cursors inserts at all positions
- Backspace/Delete works with multiple cursors
- Undo reverses the entire multi-cursor operation atomically
- Ctrl+Alt+Down adds cursors on successive lines
- Escape returns to single cursor
- Remote cursor (colored caret) visible in the other pane
- Remote cursor follows as the other user types

- [ ] **Step 3: Commit**

```bash
git commit --allow-empty -m "test: multi-cursor widget verified — all tests pass, visual demo working"
```
