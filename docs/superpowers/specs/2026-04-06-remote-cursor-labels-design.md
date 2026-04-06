# Remote Cursor Labels Design

Floating name tags above remote participant cursors, following the
y-codemirror6 `YRemoteCaretWidget` pattern.

**Date:** 2026-04-06
**Depends on:** Ephemeral identity system (completed)
**Scope:** One new widget + changes to CollabPlainTextEdit and RemoteCursor

---

## 1. Behavior

Each remote cursor gets a small colored tag showing the participant's
display name, positioned just above (or below) the cursor caret.

- **Appears instantly** when the remote cursor moves to a new position.
- **Fades out** after 2 seconds of no movement (opacity animates from
  1.0 to 0.0 over 500ms).
- **Re-appears** on the next cursor movement.
- The cursor caret (2px colored bar) stays visible at all times; only
  the name tag fades.

---

## 2. CursorLabelWidget

A small QWidget that is a child of the editor's viewport. One instance
per remote participant, keyed by identity ID. Reused across cursor
movements (not created/destroyed on each update).

### Visual appearance

- Rounded rectangle background in the participant's identity color.
- White text, small font (10px or so), bold.
- Padding: ~4px horizontal, ~2px vertical.
- Size determined by text content via `QFontMetrics`.

### Positioning

- Placed above the cursor caret, offset upward by the label's height
  plus a 2px gap.
- X position aligned to the caret's X, clamped so the label doesn't
  extend past the viewport's right edge.
- If the label would go above the viewport's top edge (cursor is on
  the first visible line), flip it to below the caret.

### Fade animation

- A `QTimer` (singleShot, 2000ms) starts when the label is shown.
- On timeout, a `QPropertyAnimation` on `windowOpacity` animates from
  1.0 to 0.0 over 500ms.
- On completion of the fade, the widget is hidden (`hide()`).
- Any new cursor movement resets: stop any running animation, set
  opacity to 1.0, show, restart the timer.

### Lifecycle

- Created lazily the first time a remote cursor for that identity
  appears.
- Hidden (not deleted) when the fade completes.
- Deleted when the participant's cursor is removed from the remote
  cursor list (participant departed or went stale).

```cpp
class CursorLabelWidget : public QWidget {
    Q_OBJECT
public:
    explicit CursorLabelWidget(QWidget *viewport);

    void setLabel(const QString &name, const QColor &color);
    void showAtPosition(const QPoint &pos, bool flipBelow = false);
    void fadeOut();
    void cancelFade();

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QString m_name;
    QColor m_color;
    QTimer m_fadeTimer;
    QPropertyAnimation *m_fadeAnim = nullptr;
};
```

---

## 3. Changes to RemoteCursor

Add an `identityId` field so CollabPlainTextEdit can track which label
belongs to which participant:

```cpp
struct RemoteCursor {
    uint32_t bytePosition = 0;
    uint32_t byteAnchor = 0;
    QColor color;
    QString label;          // display name
    QString identityId;     // for label widget keying
};
```

The `identityId` is set by `EditorPane::applyRemoteEphemeral()` from
the Identity struct delivered by the presence system.

---

## 4. Changes to CollabPlainTextEdit

### New members

```cpp
QHash<QString, CursorLabelWidget*> m_cursorLabels; // identityId → label
```

### Updated painting flow

After the existing `drawSecondaryCaret()` loop for remote cursors in
`paintEvent()`, call a new method `updateCursorLabels()` that:

1. Collects the set of identity IDs currently in the remote cursor list.
2. For each remote cursor:
   a. Get or create a `CursorLabelWidget` for that identity ID.
   b. Resolve byte position to a Qt character position (existing code).
   c. Get screen coordinates via `cursorRect(QTextCursor)`.
   d. Determine if the label should flip below (caret near top of
      viewport).
   e. Call `showAtPosition()` with the computed position.
3. For any identity ID in `m_cursorLabels` that is NOT in the current
   remote cursor list: delete the label widget and remove from the map.

### Scroll handling

When the viewport scrolls, labels need repositioning. Connect to the
viewport's scroll events (override `scrollContentsBy()` or connect to
`verticalScrollBar()->valueChanged`) and call `updateCursorLabels()`
to reposition all visible labels.

---

## 5. Changes to the Test App

`EditorPane::applyRemoteEphemeral()` already builds `RemoteCursor`
structs. The only change: set `rc.identityId` from the identity's
`identity_id` field.

---

## 6. Edge Cases

- **Cursor off-screen:** If the resolved cursor position is outside the
  visible viewport, hide the label.
- **Multiple cursors from same identity:** The spec (section 15.1.3)
  says multiple replicas with the same identity collapse visually. The
  label is per-identity, not per-cursor — if an identity has multiple
  cursors, the label follows the first one.
- **Label overlap:** No collision avoidance. Labels from different
  participants may overlap if their cursors are near each other. This
  is acceptable; sophisticated stacking is future work.
- **Viewport resize:** Same as scroll — reposition labels.

---

## 7. Source Files

| File | Change |
|------|--------|
| Create: `src/ui/CursorLabelWidget.h` | Widget class |
| Create: `src/ui/CursorLabelWidget.cpp` | Implementation |
| Modify: `src/ui/MultiCursorController.h` | Add `identityId` to RemoteCursor |
| Modify: `src/ui/CollabPlainTextEdit.h` | Add label map, updateCursorLabels |
| Modify: `src/ui/CollabPlainTextEdit.cpp` | Label positioning + scroll handling |
| Modify: `app/main.cpp` | Set identityId on RemoteCursor |
| Modify: `tests/tst_identity_widgets.cpp` | Smoke test for CursorLabelWidget |
| Modify: `CMakeLists.txt` | Add CursorLabelWidget.cpp |

---

## 8. Testing

- **CursorLabelWidget smoke test:** Create widget, set label, verify
  `sizeHint()` is reasonable, render to pixmap without crash.
- **Show/fade lifecycle:** Create widget, call `showAtPosition()`,
  verify visible. Trigger fade, verify hidden after animation.
- **Manual smoke test:** Run the test app, type in both panes, verify
  labels appear above remote cursors with correct names/colors and
  fade after inactivity.
