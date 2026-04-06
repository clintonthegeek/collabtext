# Multi-Cursor Widget Research Notes

Research into Qt's text editing internals, the Markoff editor's Qt-fork
approach, and Zed's multi-cursor architecture. Goal: design a reusable
multi-cursor text editing layer for collabtext.

---

## 1. Qt Text Widget Architecture

### The three-layer stack

```
Widget:      QTextEdit / QPlainTextEdit       (viewport, scrolling, paint)
Controller:  QWidgetTextControl               (input handling, cursor ops)
Document:    QTextDocument + QTextCursor       (text storage, layout)
```

Both QTextEdit and QPlainTextEdit delegate almost everything to a shared
`QWidgetTextControl`. The widget handles only painting and page-up/down
scrolling. Everything else — key handling, mouse handling, selection,
clipboard, undo — flows through `sendControlEvent()` into the controller.

**Source paths** (in `~/src/qtbase/`):
- `src/widgets/widgets/qplaintextedit.cpp` / `_p.h`
- `src/widgets/widgets/qtextedit.cpp` / `_p.h`
- `src/widgets/widgets/qwidgettextcontrol.cpp` / `_p_p.h`
- `src/gui/text/qtextcursor.cpp` / `_p.h`
- `src/gui/text/qtextdocument.cpp` / `_p.h`

### The single-cursor assumption

`QWidgetTextControlPrivate` has exactly one cursor:

```cpp
QTextCursor cursor;  // THE cursor — singular
```

All keypress handling (`keyPressEvent`), all mouse handling, and all edit
operations operate on this one cursor. There is no loop over multiple
cursors anywhere in the controller.

The paint context carries a single cursor position:

```cpp
ctx.cursorPosition = d->cursor.position();  // one int
```

And QPlainTextEdit's paintEvent draws exactly one blinking caret.

### What DOES support multiple cursors

**QTextDocument tracks ALL cursors.** Every QTextCursor that references a
document is registered in `QTextDocumentPrivate::cursors` (a `QSet`).
When text is inserted or removed, `adjustDocumentChangesAndCursors()`
iterates ALL registered cursors and updates their positions. This is the
foundational infrastructure — if you create 5 QTextCursors on one
document and edit through any of them, the other 4 automatically adjust.

**extraSelections** is the multi-highlight system. Both widgets support:

```cpp
void setExtraSelections(const QList<QTextEdit::ExtraSelection>&);
```

Each ExtraSelection is a {QTextCursor, QTextCharFormat} pair. The paint
loop renders them alongside the primary selection. This is how IDEs do
"find all" highlighting, current-line highlighting, and matching bracket
highlighting. It's the natural extension point for multi-cursor visual
feedback.

### The paint pipeline (QPlainTextEdit)

QPlainTextEdit has its own block-by-block paint loop (bypasses
QTextDocumentLayout::draw). Key steps in `paintEvent()`:

1. Get PaintContext (cursor position + extraSelections)
2. Iterate visible blocks
3. For each block: map selections → QTextLayout::FormatRange
4. Draw block text via `layout->draw()`
5. Draw cursor via `layout->drawCursor()` (single position)

### The key insight

Qt already has the infrastructure for multi-cursor editing at the
**document layer** (QTextCursor auto-adjustment, extraSelections
rendering). What's missing is the **controller layer** — dispatching
keystrokes to multiple cursors and drawing multiple carets.

---

## 2. Markoff's Qt-Fork Approach

### What Markoff is

Markoff is a library within Corbomite (an Obsidian-like note-taking app)
that builds a custom markdown editing/rendering widget. It uses a
QGraphicsScene-based approach where each markdown block (heading, paragraph,
code block, table) is a separate QGraphicsItem.

**Location:** `~/dev/Corbomite/libs/markoff/`

### What was forked

Markoff forked **QWidgetTextControl** — the controller layer — as
`Markoff::TextControl`. The header (`TextControl.h`) is an almost
line-for-line copy of QWidgetTextControl's public API. The private
struct (`TextControl_p.h`) is a flattened version of
`QWidgetTextControlPrivate` — same fields, same methods, converted from
Qt's `QObjectPrivate` inheritance to a plain `std::unique_ptr<>` member.

Key differences from Qt's original:
- `acceptRichText = false` (hardcoded plain text)
- Uses `std::unique_ptr<TextControlPrivate>` instead of Q_D macro
- Dropped some rich-text-specific methods
- Same single-cursor model (`QTextCursor cursor` in the private struct)

### What Markoff teaches us

1. **Forking QWidgetTextControl is the correct approach.** The widget
   layer (QTextEdit/QPlainTextEdit) is simple viewport management. The
   controller layer is where all the interesting logic lives. Markoff
   proves this can be extracted and reused.

2. **The private struct is the real API.** Qt's PIMPL pattern means the
   private header has all the state and methods. Markoff flattened it
   into a plain struct, which is cleaner.

3. **The fork is ~2000 lines of real logic** (excluding boilerplate).
   The controller handles: key dispatch, mouse handling, cursor blinking,
   drag-and-drop, clipboard, input method, context menus. That's the
   wheel we'd be re-inventing without the fork.

### Markoff's selection system

Markoff has a separate `SelectionManager` class for cross-block selection
(selecting across multiple QGraphicsItems). This is a different problem
from multi-cursor — it handles the case where a drag-selection spans
multiple block items in the scene. Each block delegates mouse events to
the SelectionManager.

---

## 3. Zed's Multi-Cursor Architecture

### The data model

Zed stores all cursors in `SelectionsCollection`:

```rust
struct SelectionsCollection {
    disjoint: Arc<[Selection<Anchor>]>,  // committed, non-overlapping selections
    pending: Option<PendingSelection>,    // active mouse-drag selection
    next_selection_id: usize,
    select_mode: SelectMode,              // Character, Word, Line
}
```

Each `Selection<Anchor>` has:
```rust
struct Selection<T> {
    id: usize,
    start: T,          // anchor position
    end: T,            // head position
    reversed: bool,    // head < anchor?
    goal: SelectionGoal,  // saved column for vertical movement
}
```

Selections are parameterized by position type — can be `Anchor` (stable
CRDT position), `Point` (line/column), or `Offset` (byte offset). The
`all::<Offset>()` method resolves anchors to concrete positions.

### How multi-cursor edits work

The critical pattern (from `replace_selections()`):

```rust
// 1. Get ALL selections as concrete offsets
let old_selections = self.selections.all_adjusted(&snapshot);

// 2. Create anchors at each selection's head BEFORE editing
let anchors = old_selections.iter().map(|s| snapshot.anchor_after(s.head()));

// 3. Apply ALL edits at once as a batch
buffer.edit(
    old_selections.iter().map(|s| (s.start..s.end, text.clone())),
    autoindent_mode,
    cx,
);

// 4. Update selections to the saved anchors (automatically adjusted)
self.change_selections(|s| s.select_anchors(anchors));
```

The key insight: **edits are batched and applied in one transaction.**
The buffer handles offset adjustment for multiple overlapping edits.
After the edit, stable anchors (which automatically adjust) tell each
cursor where it ended up.

For `handle_input()` (typing with multiple cursors), the same pattern
but with auto-close bracket handling per selection:

```rust
let selections = self.selections.all_adjusted(&snapshot);
let mut edits = Vec::new();
let mut new_selections = Vec::with_capacity(selections.len());
for (selection, autoclose_region) in ... {
    // per-cursor logic (bracket pairs, auto-indent)
    edits.push((selection.start..selection.end, text));
    new_selections.push(new_selection);
}
// apply all edits at once
buffer.edit(edits, ...);
```

### How multiple carets are rendered

In `element.rs`, the editor element iterates all selections to draw:

```rust
for selection in &selections {
    // Draw selection highlight (if start != end)
    // Draw caret at head position
}
```

Remote collaborator cursors are drawn separately using the same
mechanism but with different colors and participant labels.

### The abstraction boundary

Zed's clean separation:
- **SelectionsCollection**: owns the list of selections, handles
  merging overlapping selections, resolves anchors to positions
- **Editor**: dispatches operations to all selections, applies edits
  as a batch, renders via element
- **Buffer (text crate)**: handles position adjustment automatically
  via CRDT anchors — this is exactly what our engine does with
  `Buffer::anchor_at()`

---

## 4. Design for collabtext Multi-Cursor Widget

### What we need

A reusable layer that can be used by:
1. A collabtext plain-text editor widget
2. Markoff (markdown, QGraphicsScene-based)
3. A Kate-style advanced editor (syntax highlighting, folding)
4. Remote cursor display for collaboration

### The proposed architecture

```
┌─────────────────────────────────────────────┐
│  Widget (QPlainTextEdit derivative, or      │
│  QGraphicsScene item, or custom)            │
│  - Calls MultiCursorController on input     │
│  - Paints carets + selections from state    │
└────────────────┬────────────────────────────┘
                 │
┌────────────────┼────────────────────────────┐
│  MultiCursorController                      │
│  - Owns list of cursors (QTextCursor[])     │
│  - Dispatches edits to all cursors          │
│  - Handles add/remove cursor (Ctrl+D, etc.) │
│  - Produces extraSelections for rendering   │
│  - Qt-dependent (uses QTextCursor)          │
└────────────────┬────────────────────────────┘
                 │
┌────────────────┼────────────────────────────┐
│  QTextDocument                              │
│  - Automatic cursor position adjustment     │
│  - Undo/redo grouping                       │
│  - Standard Qt text storage                 │
└─────────────────────────────────────────────┘
```

### MultiCursorController — the key new class

```cpp
class MultiCursorController {
public:
    // Cursor management
    void addCursorAt(int position);
    void addCursorAtNextMatch(const QString& text); // Ctrl+D
    void removeCursor(int index);
    void clearSecondaryCursors(); // Escape — back to one cursor
    int cursorCount() const;

    // Edit dispatch (called by widget on keypress)
    void insertText(const QString& text);
    void deleteChar();       // Delete key
    void deletePrevChar();   // Backspace
    void moveCursors(QTextCursor::MoveOperation op,
                     QTextCursor::MoveMode mode);

    // State for rendering
    QList<QTextCursor> cursors() const;
    QList<QTextEdit::ExtraSelection> extraSelections() const;
    QList<QRect> caretRects() const;

    // Remote cursors (collaboration)
    void setRemoteCursors(const QList<RemoteCursor>& cursors);
    QList<QTextEdit::ExtraSelection> remoteExtraSelections() const;

private:
    QTextDocument* m_document;
    QTextCursor m_primary;               // the "main" cursor
    QList<QTextCursor> m_secondary;      // additional cursors
    QList<RemoteCursor> m_remoteCursors; // from other replicas
};
```

### How edit dispatch works

Following Zed's pattern: iterate all cursors **in reverse document
order** (highest position first), apply the edit to each, let Qt's
cursor adjustment handle the position shifts.

```cpp
void MultiCursorController::insertText(const QString& text) {
    // Sort cursors by position, descending
    auto cursors = allCursorsSorted();
    std::reverse(cursors.begin(), cursors.end());

    m_document->undoStack()->beginMacro("multi-cursor insert");
    for (auto& cursor : cursors) {
        cursor.insertText(text);
    }
    m_document->undoStack()->endMacro();
}
```

Reverse order is critical: editing at higher positions first means
lower-position cursors don't shift. Qt's cursor adjustment handles the
remaining cases.

### How rendering works

The widget's paintEvent:
1. Gets `extraSelections()` from MultiCursorController (secondary
   cursor selections + remote cursor highlights)
2. Calls `setExtraSelections()` on the widget
3. Draws additional carets at each cursor position (the widget's
   built-in caret draws the primary cursor; secondary and remote
   carets are drawn manually in paintEvent)

### Where collabtext's engine fits

For collaboration:
1. Local edits go through MultiCursorController → QTextDocument
2. The widget also feeds edits to collabtext's Buffer (via the
   SyncManager)
3. Remote edits arrive via FileSync → Buffer::apply_ops()
4. Remote cursor positions arrive via ephemeral.json
5. Remote cursors are displayed via MultiCursorController's
   remote cursor list → extraSelections

The QTextDocument and collabtext Buffer are kept in sync:
- Local edit → QTextDocument (for display) + Buffer (for CRDT)
- Remote edit → Buffer (for CRDT) → replace QTextDocument content

### What can be reused across widget types

| Component | Qt-dependent? | Reusable? |
|-----------|--------------|-----------|
| MultiCursorController | Yes (QTextCursor) | Across all QTextDocument-based widgets |
| Edit dispatch (reverse-order batch) | Yes | Same |
| Extra selections generation | Yes | Same |
| Caret rendering | Widget-specific | Must be implemented per widget type |
| Key binding → cursor operation mapping | No | Fully reusable |
| Remote cursor management | Partially | Core is Qt-free, rendering is Qt |

---

## 5. Key Findings

### What Qt gives us for free
- Multiple QTextCursors on one document with automatic position adjustment
- extraSelections for arbitrary highlight ranges
- QPlainTextEdit's block-by-block paint (overridable for custom caret drawing)
- QWidgetTextControl as a forkable controller with all input handling

### What we must build
- **MultiCursorController**: cursor list management, edit fan-out, selection merging
- **Custom paintEvent override**: drawing multiple carets (Qt only draws one)
- **Ctrl+D / Alt+Click**: cursor addition from keyboard and mouse
- **Remote cursor integration**: mapping collabtext Anchors to QTextCursor positions

### The Markoff approach (fork QWidgetTextControl) is viable
Markoff proves this works. For our multi-cursor case, we'd fork and
modify the controller to:
1. Replace `QTextCursor cursor` with `QList<QTextCursor> cursors`
2. Modify `keyPressEvent` to dispatch to all cursors
3. Modify `getPaintContext` to include all cursor positions
4. Add Ctrl+D / Alt+Click handling in mouse events

### Zed's batch-edit pattern is the correct approach
Iterate selections in reverse document order, apply edits in one
transaction. Qt's cursor adjustment handles the rest. This is exactly
what `QTextDocumentPrivate::adjustDocumentChangesAndCursors()` was
designed for.

---

## 6. Recommended Build Order

1. **MultiCursorController** (pure logic, no widget) — cursor list, edit
   dispatch, extraSelections generation. Test with QTextDocument directly.

2. **CollabPlainTextEdit** — QPlainTextEdit subclass that uses
   MultiCursorController instead of the built-in single cursor. Override
   paintEvent to draw multiple carets.

3. **Remote cursor integration** — Map collabtext Anchors to positions,
   display via extraSelections with per-user colors.

4. **Reuse in Markoff** — Port MultiCursorController to Markoff's
   TextControl (the forked QWidgetTextControl).

5. **Kate integration** — Investigate KTextEditor's multi-cursor support
   (Kate already has some) and whether MultiCursorController can feed
   into it.
