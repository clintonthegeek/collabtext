# Remote Cursor Labels Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add floating name tags above remote participant cursors that appear on movement and fade after inactivity.

**Architecture:** A new `CursorLabelWidget` (QWidget child of the viewport) handles rendering and fade animation. `CollabPlainTextEdit` manages a map of identity → label widget, positioning them on each remote cursor update and scroll event. `RemoteCursor` gains an `identityId` field for tracking.

**Tech Stack:** Qt6 Widgets (QPropertyAnimation, QTimer, QPainter), existing CollabPlainTextEdit/MultiCursorController

---

## File Map

| File | Responsibility |
|------|----------------|
| Create: `src/ui/CursorLabelWidget.h` | Floating label widget declaration |
| Create: `src/ui/CursorLabelWidget.cpp` | Rendering, positioning, fade animation |
| Modify: `src/ui/MultiCursorController.h` | Add `identityId` field to `RemoteCursor` |
| Modify: `src/ui/CollabPlainTextEdit.h` | Add label map + `updateCursorLabels()` |
| Modify: `src/ui/CollabPlainTextEdit.cpp` | Label lifecycle, positioning, scroll handling |
| Modify: `app/main.cpp` | Set `identityId` on RemoteCursor in `applyRemoteEphemeral` |
| Modify: `tests/tst_identity_widgets.cpp` | Smoke tests for CursorLabelWidget |
| Modify: `CMakeLists.txt` | Add CursorLabelWidget.cpp |

All paths below are relative to `libs/collabtext/` unless they start with `app/`.

---

## Task 1: Add identityId to RemoteCursor

**Files:**
- Modify: `src/ui/MultiCursorController.h`
- Modify: `app/main.cpp`

- [ ] **Step 1: Add the field to RemoteCursor**

In `src/ui/MultiCursorController.h`, add `identityId` to the `RemoteCursor` struct:

```cpp
struct RemoteCursor {
    uint32_t bytePosition = 0;   // cursor head (byte offset in Buffer::text())
    uint32_t byteAnchor = 0;     // selection anchor (== bytePosition if no selection)
    QColor color;
    QString label;               // participant name
    QString identityId;          // for cursor label widget keying
};
```

- [ ] **Step 2: Set identityId in app/main.cpp**

In `app/main.cpp`, in the `EditorPane::applyRemoteEphemeral()` method (around line 172), add the identityId assignment after setting `rc.label`:

```cpp
    void applyRemoteEphemeral(const EphemeralState &es, const Identity &remoteIdentity) {
        QList<RemoteCursor> cursors;
        for (const auto &cp : es.cursors) {
            RemoteCursor rc;
            rc.bytePosition = m_buffer.resolve_anchor(cp.head);
            rc.byteAnchor = m_buffer.resolve_anchor(cp.anchor);
            rc.color = QColor(QString::fromStdString(remoteIdentity.color));
            rc.label = QString::fromStdString(remoteIdentity.display_name);
            rc.identityId = QString::fromStdString(remoteIdentity.identity_id);
            cursors.append(rc);
        }
        m_edit->multiCursorController()->setRemoteCursors(cursors);
    }
```

- [ ] **Step 3: Build to verify compilation**

Run: `cd /home/clinton/dev/collabtext && cmake --build build-dev -j$(nproc) 2>&1 | tail -5`
Expected: Clean build.

- [ ] **Step 4: Commit**

```bash
git add libs/collabtext/src/ui/MultiCursorController.h app/main.cpp
git commit -m "feat: add identityId field to RemoteCursor"
```

---

## Task 2: CursorLabelWidget

**Files:**
- Create: `src/ui/CursorLabelWidget.h`
- Create: `src/ui/CursorLabelWidget.cpp`
- Modify: `tests/tst_identity_widgets.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add tests to tst_identity_widgets.cpp**

Add `#include "ui/CursorLabelWidget.h"` at the top of the test file. Add these test slots:

```cpp
    void cursor_label_size_hint() {
        CursorLabelWidget lbl(nullptr);
        lbl.setLabel(QStringLiteral("Alice"), QColor("#3b82f6"));
        QSize hint = lbl.sizeHint();
        QVERIFY(hint.width() > 10);   // text + padding
        QVERIFY(hint.height() > 10);  // text + padding
        QVERIFY(hint.width() < 200);  // reasonable upper bound
    }

    void cursor_label_renders() {
        CursorLabelWidget lbl(nullptr);
        lbl.setLabel(QStringLiteral("Bob"), QColor("#ef4444"));
        lbl.resize(lbl.sizeHint());
        QPixmap pm(lbl.size());
        lbl.render(&pm);
        QVERIFY(!pm.isNull());
    }

    void cursor_label_show_and_fade() {
        CursorLabelWidget lbl(nullptr);
        lbl.setLabel(QStringLiteral("Carol"), QColor("#22c55e"));
        lbl.showAtPosition(QPoint(100, 50));
        QVERIFY(lbl.isVisible());
        // cancelFade should not crash
        lbl.cancelFade();
        QVERIFY(lbl.isVisible());
    }
```

- [ ] **Step 2: Create CursorLabelWidget header**

Create `src/ui/CursorLabelWidget.h`:

```cpp
#pragma once

#include <QPropertyAnimation>
#include <QTimer>
#include <QWidget>

namespace CollabText::Ui {

/// Floating label widget that shows a participant's name above their cursor.
/// Child of the editor viewport. Fades out after inactivity.
class CursorLabelWidget : public QWidget {
    Q_OBJECT
    Q_PROPERTY(qreal opacity READ opacity WRITE setOpacity)
public:
    explicit CursorLabelWidget(QWidget *viewport);

    /// Set the display name and color for this label.
    void setLabel(const QString &name, const QColor &color);

    /// Show the label at the given viewport-relative position.
    /// If flipBelow is true, position below the caret instead of above.
    void showAtPosition(const QPoint &pos, bool flipBelow = false);

    /// Start the fade-out sequence (2s delay, then 500ms animation).
    void scheduleFade();

    /// Cancel any pending or active fade, restore full opacity.
    void cancelFade();

    QSize sizeHint() const override;

    qreal opacity() const { return m_opacity; }
    void setOpacity(qreal opacity);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    void onFadeTimerTimeout();
    void onFadeFinished();

    QString m_name;
    QColor m_color;
    qreal m_opacity = 1.0;
    QTimer m_fadeTimer;
    QPropertyAnimation *m_fadeAnim = nullptr;
};

} // namespace CollabText::Ui
```

- [ ] **Step 3: Create CursorLabelWidget implementation**

Create `src/ui/CursorLabelWidget.cpp`:

```cpp
#include "ui/CursorLabelWidget.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>

namespace CollabText::Ui {

static constexpr int kPadH = 5;
static constexpr int kPadV = 2;
static constexpr int kRadius = 3;
static constexpr int kFontSize = 10;
static constexpr int kFadeDelayMs = 2000;
static constexpr int kFadeDurationMs = 500;

CursorLabelWidget::CursorLabelWidget(QWidget *viewport)
    : QWidget(viewport)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAutoFillBackground(false);

    m_fadeTimer.setSingleShot(true);
    m_fadeTimer.setInterval(kFadeDelayMs);
    connect(&m_fadeTimer, &QTimer::timeout,
            this, &CursorLabelWidget::onFadeTimerTimeout);
}

void CursorLabelWidget::setLabel(const QString &name, const QColor &color) {
    m_name = name;
    m_color = color;
    updateGeometry();
    update();
}

void CursorLabelWidget::showAtPosition(const QPoint &pos, bool flipBelow) {
    cancelFade();
    QSize sz = sizeHint();
    resize(sz);

    int x = pos.x();
    int y = flipBelow ? pos.y() + 2 : pos.y() - sz.height() - 2;

    // Clamp to viewport right edge
    if (parentWidget()) {
        int maxX = parentWidget()->width() - sz.width();
        if (x > maxX) x = qMax(0, maxX);
    }

    move(x, y);
    show();
    raise();
    scheduleFade();
}

void CursorLabelWidget::scheduleFade() {
    m_fadeTimer.start();
}

void CursorLabelWidget::cancelFade() {
    m_fadeTimer.stop();
    if (m_fadeAnim) {
        m_fadeAnim->stop();
        delete m_fadeAnim;
        m_fadeAnim = nullptr;
    }
    setOpacity(1.0);
}

QSize CursorLabelWidget::sizeHint() const {
    QFont font;
    font.setPixelSize(kFontSize);
    font.setBold(true);
    QFontMetrics fm(font);
    int w = fm.horizontalAdvance(m_name) + kPadH * 2;
    int h = fm.height() + kPadV * 2;
    return {w, h};
}

void CursorLabelWidget::setOpacity(qreal opacity) {
    m_opacity = opacity;
    update();
}

void CursorLabelWidget::paintEvent(QPaintEvent *) {
    if (m_name.isEmpty()) return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setOpacity(m_opacity);

    // Background rounded rect
    QColor bg = m_color;
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()), kRadius, kRadius);
    p.fillPath(path, bg);

    // Text
    p.setPen(Qt::white);
    QFont font = p.font();
    font.setPixelSize(kFontSize);
    font.setBold(true);
    p.setFont(font);
    p.drawText(rect(), Qt::AlignCenter, m_name);
}

void CursorLabelWidget::onFadeTimerTimeout() {
    m_fadeAnim = new QPropertyAnimation(this, "opacity", this);
    m_fadeAnim->setDuration(kFadeDurationMs);
    m_fadeAnim->setStartValue(1.0);
    m_fadeAnim->setEndValue(0.0);
    connect(m_fadeAnim, &QPropertyAnimation::finished,
            this, &CursorLabelWidget::onFadeFinished);
    m_fadeAnim->start(QAbstractAnimation::DeleteWhenStopped);
    m_fadeAnim = nullptr; // DeleteWhenStopped handles lifetime
}

void CursorLabelWidget::onFadeFinished() {
    hide();
    m_opacity = 1.0; // reset for next show
}

} // namespace CollabText::Ui
```

- [ ] **Step 4: Add to CMakeLists.txt**

Add `src/ui/CursorLabelWidget.cpp` to the `add_library(collabtext STATIC ...)` source list in `libs/collabtext/CMakeLists.txt`.

- [ ] **Step 5: Build and run tests**

Run: `cd /home/clinton/dev/collabtext && cmake --build build-dev --target tst_identity_widgets -j$(nproc) 2>&1 | tail -5 && build-dev/libs/collabtext/tst_identity_widgets`
Expected: All tests pass (existing + 3 new).

- [ ] **Step 6: Commit**

```bash
git add libs/collabtext/src/ui/CursorLabelWidget.h \
        libs/collabtext/src/ui/CursorLabelWidget.cpp \
        libs/collabtext/tests/tst_identity_widgets.cpp \
        libs/collabtext/CMakeLists.txt
git commit -m "feat: CursorLabelWidget — floating name tag with fade animation"
```

---

## Task 3: Integrate Labels into CollabPlainTextEdit

**Files:**
- Modify: `src/ui/CollabPlainTextEdit.h`
- Modify: `src/ui/CollabPlainTextEdit.cpp`

- [ ] **Step 1: Update the header**

In `src/ui/CollabPlainTextEdit.h`, add the includes and new members:

```cpp
#pragma once

#include "ui/MultiCursorController.h"
#include <QHash>
#include <QPlainTextEdit>

namespace CollabText::Ui {

class CursorLabelWidget;

class CollabPlainTextEdit : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CollabPlainTextEdit(QWidget *parent = nullptr);

    void setDocument(QTextDocument *document);
    MultiCursorController *multiCursorController() const { return m_controller; }

protected:
    void paintEvent(QPaintEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    void drawSecondaryCaret(QPainter &painter, int position, const QColor &color);
    void syncExtraSelections();
    void updateCursorLabels();

    MultiCursorController *m_controller;
    bool m_handlingKey = false;
    QHash<QString, CursorLabelWidget*> m_cursorLabels;
};

} // namespace CollabText::Ui
```

- [ ] **Step 2: Implement updateCursorLabels and scroll handling**

In `src/ui/CollabPlainTextEdit.cpp`, add the include at the top:

```cpp
#include "ui/CursorLabelWidget.h"
#include <QScrollBar>
```

Add the `scrollContentsBy` override:

```cpp
void CollabPlainTextEdit::scrollContentsBy(int dx, int dy) {
    QPlainTextEdit::scrollContentsBy(dx, dy);
    updateCursorLabels();
}
```

Add the `updateCursorLabels` method:

```cpp
void CollabPlainTextEdit::updateCursorLabels() {
    QString docText = document()->toPlainText();
    QByteArray utf8 = docText.toUtf8();
    int maxPos = document()->characterCount() - 1;
    if (maxPos < 0) maxPos = 0;

    QSet<QString> activeIds;

    for (const auto &rc : m_controller->remoteCursors()) {
        if (rc.identityId.isEmpty()) continue;
        activeIds.insert(rc.identityId);

        // Resolve byte offset to Qt position
        uint32_t clamped = qMin(rc.bytePosition, static_cast<uint32_t>(utf8.size()));
        int qtPos = qMin(QString::fromUtf8(utf8.data(), clamped).length(), maxPos);

        // Get screen coordinates for this position
        QTextCursor tc(document());
        tc.setPosition(qtPos);
        QRect caretRect = cursorRect(tc);

        // Check if cursor is in the visible viewport
        QRect vpRect = viewport()->rect();
        if (!vpRect.intersects(caretRect)) {
            // Cursor off-screen — hide label if it exists
            if (auto *lbl = m_cursorLabels.value(rc.identityId))
                lbl->hide();
            continue;
        }

        // Get or create label widget
        CursorLabelWidget *lbl = m_cursorLabels.value(rc.identityId);
        if (!lbl) {
            lbl = new CursorLabelWidget(viewport());
            m_cursorLabels.insert(rc.identityId, lbl);
        }

        lbl->setLabel(rc.label, rc.color);

        // Position: above the caret, or below if at top edge
        bool flipBelow = (caretRect.top() < lbl->sizeHint().height() + 4);
        QPoint pos(caretRect.left(), flipBelow ? caretRect.bottom() : caretRect.top());
        lbl->showAtPosition(pos, flipBelow);
    }

    // Remove labels for departed participants
    for (auto it = m_cursorLabels.begin(); it != m_cursorLabels.end(); ) {
        if (!activeIds.contains(it.key())) {
            delete it.value();
            it = m_cursorLabels.erase(it);
        } else {
            ++it;
        }
    }
}
```

Modify `syncExtraSelections()` to call `updateCursorLabels()` at the end:

```cpp
void CollabPlainTextEdit::syncExtraSelections() {
    QList<QTextEdit::ExtraSelection> selections;
    selections.append(m_controller->secondarySelections());
    selections.append(m_controller->remoteSelections());
    setExtraSelections(selections);
    viewport()->update();
    updateCursorLabels();
}
```

- [ ] **Step 3: Build the full project**

Run: `cd /home/clinton/dev/collabtext && cmake --build build-dev -j$(nproc) 2>&1 | tail -10`
Expected: Clean build.

- [ ] **Step 4: Run all tests**

Run: `cd /home/clinton/dev/collabtext && cd build-dev && ctest --output-on-failure -j$(nproc) -E tst_benchmark 2>&1 | tail -10`
Expected: All tests pass.

- [ ] **Step 5: Commit**

```bash
git add libs/collabtext/src/ui/CollabPlainTextEdit.h \
        libs/collabtext/src/ui/CollabPlainTextEdit.cpp
git commit -m "feat: integrate cursor labels into CollabPlainTextEdit"
```

---

## Task 4: Manual Smoke Test and Polish

- [ ] **Step 1: Run the test app**

Run: `cd /home/clinton/dev/collabtext && build-dev/app/collabtext-testapp`

Verify:
- Two editor panes with "Alice" (blue) and "Bob" (red)
- Type in the Alice pane → Bob's pane shows Alice's remote cursor with a blue label "Alice" above it
- The label fades out after ~2 seconds of no cursor movement
- Moving Alice's cursor again makes the label reappear
- Scrolling the Bob pane repositions the label correctly
- Labels don't appear for cursors that are off-screen
- Labels flip below the caret when the cursor is at the very top of the viewport

- [ ] **Step 2: Fix any visual issues**

If labels are mispositioned, flickering, or not fading correctly, fix the specific issue. Common adjustments:
- Vertical offset tweaking (the 2px gap in `showAtPosition`)
- Font size adjustment (`kFontSize`)
- Fade timing (`kFadeDelayMs`, `kFadeDurationMs`)

- [ ] **Step 3: Run all tests**

Run: `cd /home/clinton/dev/collabtext && cd build-dev && ctest --output-on-failure -j$(nproc) -E tst_benchmark`
Expected: All tests pass.

- [ ] **Step 4: Commit any fixes**

If fixes were needed:
```bash
git add -u
git commit -m "fix: cursor label positioning and visual polish"
```
