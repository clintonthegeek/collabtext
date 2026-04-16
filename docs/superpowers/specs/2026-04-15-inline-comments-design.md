# Inline Comments — Design

**Date:** 2026-04-15
**Status:** Approved, ready for implementation plan
**Related:** `docs/superpowers/specs/2026-04-13-stream-sync-design.md` (StreamSync transport)
**Spec reference:** `docs/CRDT_SYNC_SPEC.md` section 15.2.4

---

## 1. Problem

Collaborators need to annotate specific document ranges with comments
that track edits automatically. The StreamSync AnchorKeyed stream type
is built and tested but has no consumer. Inline comments are the first
anchor-keyed feature.

## 2. Goals

1. **Comment data model.** A struct and StreamEntry conversion functions
   for comments with range anchors (start + end), body, and author.

2. **Editor highlighting.** Commented ranges shown as semi-transparent
   background highlights in the editor using QPlainTextEdit's
   `extraSelections` mechanism.

3. **CommentsPanelWidget.** A separate panel listing comments with
   author, body, and quoted context. Click a comment to scroll to its
   range.

4. **Test app integration.** Wire the "comments" AnchorKeyed stream
   through StreamSync. Users select text, type a comment, and it
   appears as a highlight in both editors.

## 3. Non-goals

- **Resolve/delete lifecycle.** StreamSync handles LWW merge and
  tombstones. The UI for resolving or deleting comments is a future
  enhancement.

- **Threaded replies on comments.** Flat comments only.

- **Gutter markers.** Highlights show the commented range directly.

## 4. Comment data model

```cpp
struct Comment {
    std::string id;            // "replicaId-seq" (LWW key)
    uint16_t replica_id = 0;
    uint64_t seq = 0;
    std::string timestamp;     // ISO 8601 (LWW ordering)
    std::string author;        // identity_id
    std::string author_name;   // display name snapshot
    std::string body;          // comment text
    Anchor range_start;        // start of commented range
    Anchor range_end;          // end of commented range
};
```

### 4.1 Conversion functions

```cpp
StreamEntry comment_to_entry(const Comment& c);
std::optional<Comment> comment_from_entry(const StreamEntry& entry);
```

`comment_to_entry` sets `entry.id = c.id`, copies replica_id/seq/
timestamp, and builds a payload JSON:

```json
{
  "author": "alice-a1b2c3",
  "author_name": "Alice",
  "body": "This needs a citation",
  "range": {
    "start": {"r": 1, "s": 200, "b": "left"},
    "end": {"r": 1, "s": 245, "b": "right"}
  }
}
```

`comment_from_entry` parses the payload, returns nullopt if author,
body, or range is missing/malformed.

Lives in `libs/collabtext/src/crdt/Comment.h/.cpp`. Follows the same
JSON escaping and parsing pattern as ChatMessage.cpp.

## 5. Editor highlighting

Comments are rendered as `QTextEdit::ExtraSelection` entries with a
semi-transparent background color. This reuses the same mechanism
already used for multi-cursor selections in `CollabPlainTextEdit`.

### 5.1 API addition to CollabPlainTextEdit

```cpp
/// Set the comment highlight ranges. Each entry is a (startByteOff,
/// endByteOff, color) tuple. The widget converts byte offsets to Qt
/// positions and builds ExtraSelections with the given background color.
void setCommentHighlights(
    const QList<std::tuple<uint32_t, uint32_t, QColor>> &highlights);
```

This is called from the test app after resolving comment anchors.
The widget converts byte offsets to Qt positions and merges them into
the existing `extraSelections` (alongside multi-cursor selections).

### 5.2 Implementation

`setCommentHighlights` stores the highlights in a member
`m_commentSelections`. `syncExtraSelections` (already called whenever
cursors change) is updated to append `m_commentSelections` to the
extra selections list.

Each highlight becomes an ExtraSelection with:
- `QTextCharFormat::setBackground(color.lighter(180))` — light
  semi-transparent tint
- Cursor spanning the byte range (converted to Qt positions using
  the existing `byteOffsetToQtPos` helper)

## 6. CommentsPanelWidget

```cpp
class CommentsPanelWidget : public QWidget {
    Q_OBJECT
public:
    explicit CommentsPanelWidget(QWidget *parent = nullptr);

    /// Replace the full comment list. Each entry: (id, authorName,
    /// body, contextSnippet, authorColor).
    void setComments(const QList<CommentDisplayInfo> &comments);

signals:
    /// User wants to add a comment (clicked the "Add Comment" button
    /// or pressed a shortcut with text selected).
    void addCommentRequested(const QString &body);

    /// User clicked a comment in the list.
    void commentClicked(const QString &commentId);
};
```

### 6.1 CommentDisplayInfo

```cpp
struct CommentDisplayInfo {
    QString id;
    QString authorName;
    QString body;
    QString contextSnippet;  // first ~40 chars of the commented text
    QColor authorColor;
};
```

### 6.2 Layout

```
┌─────────────────────┐
│ ParticipantList     │
├─────────────────────┤
│ Chat panel          │
├─────────────────────┤
│ Comments:           │
│ ┌─────────────────┐ │
│ │ Alice: "Needs   │ │
│ │ citation"       │ │
│ │ > "the quick...'│ │
│ └─────────────────┘ │
│ ┌─────────────────┐ │
│ │ Bob: "Typo here"│ │
│ │ > "teh word..." │ │
│ └─────────────────┘ │
│ [Comment input...  ] │
└─────────────────────┘
```

- **Comment list:** `QListWidget` with styled items. Each shows
  author name (bold, colored), comment body, and a quoted snippet of
  the commented text (gray, truncated).
- **Input:** `QLineEdit` with placeholder "Add comment on selection...".
  Enter key emits `addCommentRequested` if there's an active text
  selection in the editor. Disabled or shows a hint when no text is
  selected.

Lives in `libs/collabtext/src/ui/CommentsPanelWidget.h/.cpp`.

## 7. Test app integration

### 7.1 StreamSync registration

Register a "comments" AnchorKeyed stream on the existing
`m_streamSync` instance in MainWindow.

### 7.2 Adding a comment

Connect `CommentsPanelWidget::addCommentRequested` to a slot:
1. Get active editor's text selection (start/end byte offsets).
2. If no selection, ignore.
3. Create a `Comment` with range anchors from
   `buffer.anchor_at(startByte, Bias::Left)` and
   `buffer.anchor_at(endByte, Bias::Right)`.
4. Convert to StreamEntry, push to StreamSync "comments" stream.

### 7.3 Displaying comments

In `syncCycle`, after chat sync:
1. Get `m_streamSync->entries("comments")`.
2. For each comment, resolve range anchors against the buffer to get
   current byte offsets.
3. Build highlight list and call `setCommentHighlights` on both
   editor panes.
4. Build `CommentDisplayInfo` list (extract context snippet from
   document text between the resolved range) and call
   `setComments` on the panel.
5. Track `m_lastCommentCount` to avoid re-processing unchanged lists
   (or just rebuild each cycle — comments are low volume).

### 7.4 Click to navigate

Connect `commentClicked(id)` to a slot that looks up the comment's
resolved range_start byte offset and scrolls the last-focused editor
to that position.

### 7.5 Sidebar layout

Add CommentsPanelWidget as a third widget in the vertical splitter,
below the chat panel. Stretch factors: participants 1, chat 2,
comments 2.

## 8. Edge cases

| Case | Behaviour |
|------|-----------|
| Commented text deleted | Anchors collapse to a point. Highlight disappears (start == end). Comment still shows in panel with "(text deleted)" context. |
| No text selected when adding | Input is ignored. |
| Multiple comments on overlapping ranges | Each gets its own highlight color (cycle through a palette or use author color). ExtraSelections stack visually. |
| Comment from unknown identity | Use author_name from the comment. Color defaults to gray. |

## 9. Testing

### 9.1 Unit tests (`tst_comment.cpp`)

1. **Round-trip.** Build Comment with range anchors, convert to entry
   and back, verify all fields including range_start/range_end.
2. **Payload encoding.** Verify payload JSON contains author, body,
   range object with start and end.
3. **Missing range returns nullopt.** Entry with payload lacking range
   object returns nullopt from `comment_from_entry`.

### 9.2 Manual smoke test

Select text in editor, type a comment, verify highlight appears on
both panes, comment shows in panel, click comment scrolls to range.

## 10. File inventory

| File | Change |
|------|--------|
| `libs/collabtext/src/crdt/Comment.h` | **New.** Comment struct + conversion functions. |
| `libs/collabtext/src/crdt/Comment.cpp` | **New.** Implementation. |
| `libs/collabtext/src/ui/CommentsPanelWidget.h` | **New.** Widget + CommentDisplayInfo struct. |
| `libs/collabtext/src/ui/CommentsPanelWidget.cpp` | **New.** Implementation. |
| `libs/collabtext/src/ui/CollabPlainTextEdit.h` | **Modify.** Add `setCommentHighlights`. |
| `libs/collabtext/src/ui/CollabPlainTextEdit.cpp` | **Modify.** Implement highlighting, merge with existing extraSelections. |
| `libs/collabtext/tests/tst_comment.cpp` | **New.** Data model tests. |
| `libs/collabtext/CMakeLists.txt` | Register new sources + test. |
| `app/main.cpp` | Wire comments stream, panel, highlights, navigation. |

## 11. Success criteria

- Comment round-trip tests pass.
- Full `ctest` suite green.
- Manual smoke: select text, add comment, see highlight in both
  editors, see comment in panel, click to navigate.
