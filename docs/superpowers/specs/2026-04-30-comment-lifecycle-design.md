# Comment Lifecycle — Design

**Date:** 2026-04-30
**Status:** Approved, ready for implementation plan
**Related:** `docs/superpowers/specs/2026-04-15-inline-comments-design.md` (base feature)

---

## 1. Problem

Inline comments shipped without lifecycle actions. Comments accumulate
indefinitely with no way to mark a discussion done or remove a misfire.
StreamSync already supports tombstoning (filtered out of `entries()`)
and LWW merge by timestamp on AnchorKeyed streams; this spec adds the
UI and data-model bits needed to drive both.

## 2. Goals

1. **Resolve / unresolve** a comment. Resolved comments lose their
   editor highlight and are hidden from the active list, accessible via
   a "Show resolved (N)" toggle. Resolving is reversible.
2. **Delete** a comment. Tombstones the entry; comment vanishes
   everywhere on next sync.
3. **High-trust model.** Any participant can resolve, unresolve, or
   delete any comment. No per-user permission checks. A formal
   permissions model, if ever needed, is out of scope here.

## 3. Non-goals

- Threaded replies on comments (separate roadmap item).
- Soft-delete / undo of deletes. Tombstones are final.
- Notifications when someone resolves your comment.
- Resolved-by attribution. We track timestamp, not actor, on the
  resolve transition.

## 4. Data model changes

### 4.1 `Comment` struct

Add one field:

```cpp
struct Comment {
    // ... existing fields ...
    bool resolved = false;
};
```

### 4.2 Payload JSON

Add a `resolved` key:

```json
{
  "author": "alice-a1b2c3",
  "author_name": "Alice",
  "body": "This needs a citation",
  "resolved": false,
  "range": { "start": {...}, "end": {...} }
}
```

`comment_from_entry` reads `resolved` if present, defaults to `false`
when absent (back-compat: existing comments deserialize as
unresolved).

### 4.3 Lifecycle operations

| Op | Wire action |
|----|-------------|
| Resolve | Re-push `Comment` with same `id`, fresh ISO 8601 `timestamp`, `resolved = true`. |
| Unresolve | Same as resolve, with `resolved = false`. |
| Delete | Push `StreamEntry` with same `id`, fresh `timestamp`, `tombstone = true`. Other payload fields can be empty. |

LWW on AnchorKeyed streams ensures the latest timestamp wins on
merge. A delete after a resolve is final (tombstone + later
timestamp). A resolve after a delete loses to the tombstone if its
timestamp is older; if newer, the resurrected comment is fine — but
in practice the deleting user won't see the comment to resurrect, and
the other user only sees it until next sync. Acceptable.

## 5. CommentsPanelWidget changes

### 5.1 `CommentDisplayInfo`

Add one field:

```cpp
struct CommentDisplayInfo {
    QString id;
    QString authorName;
    QString body;
    QString contextSnippet;
    QColor authorColor;
    bool resolved = false;
};
```

### 5.2 New signals

```cpp
signals:
    void addCommentRequested(const QString &body);   // unchanged
    void commentClicked(const QString &commentId);   // unchanged
    void resolveRequested(const QString &commentId);
    void unresolveRequested(const QString &commentId);
    void deleteRequested(const QString &commentId);
```

### 5.3 Item rendering

Replace the plain `QListWidgetItem` text with a custom row widget so
we can host inline buttons:

```
┌────────────────────────────────────────────────────────┐
│ Alice  "Needs citation"                  [Resolve] [✕] │
│        > "the quick brown fox..."                      │
└────────────────────────────────────────────────────────┘
```

For resolved items (visible only when "Show resolved" is expanded):

```
┌────────────────────────────────────────────────────────┐
│ Alice  "Needs citation"  (resolved)    [Unresolve][✕]  │
│        > "the quick brown fox..."                      │
└────────────────────────────────────────────────────────┘
```

Implementation: `QListWidget` with `setItemWidget` per row. The row
widget is a small `QWidget` with an `HBox` containing the existing
labels plus a button column. Buttons are `QToolButton` (compact).

Resolved rows get a stylesheet rule that greys text (e.g.
`color: palette(mid)`) and italicizes the body.

### 5.4 Toggle: "Show resolved (N)"

A `QPushButton` (checkable) above the comment list:

- Hidden when `N == 0`.
- Text: `Show resolved (N)` / `Hide resolved (N)`.
- Toggling rebuilds the visible list from the cached
  `QList<CommentDisplayInfo>`.

The widget caches the last `setComments` argument so toggling is O(N)
without touching the test app.

### 5.5 Public API summary

```cpp
class CommentsPanelWidget : public QWidget {
    Q_OBJECT
public:
    explicit CommentsPanelWidget(QWidget *parent = nullptr);
    void setComments(const QList<CommentDisplayInfo> &comments);

signals:
    void addCommentRequested(const QString &body);
    void commentClicked(const QString &commentId);
    void resolveRequested(const QString &commentId);
    void unresolveRequested(const QString &commentId);
    void deleteRequested(const QString &commentId);

private:
    void rebuildList();              // re-renders from cache
    QList<CommentDisplayInfo> m_all; // full set, source of truth
    bool m_showResolved = false;
    QPushButton *m_toggleResolved;
    QListWidget *m_list;
    QLineEdit *m_input;
};
```

## 6. Editor highlight changes

The test app already calls `CollabPlainTextEdit::setCommentHighlights`
once per sync cycle. We change the **test app**, not the editor:
when building the highlight list, **skip resolved comments**.

No editor API change.

## 7. Test app integration

### 7.1 Slot wiring

Connect the three new signals in `MainWindow`:

```cpp
connect(m_commentsPanel, &CommentsPanelWidget::resolveRequested,
        this, [this](const QString& id) { setCommentResolved(id, true); });
connect(m_commentsPanel, &CommentsPanelWidget::unresolveRequested,
        this, [this](const QString& id) { setCommentResolved(id, false); });
connect(m_commentsPanel, &CommentsPanelWidget::deleteRequested,
        this, &MainWindow::deleteComment);
```

### 7.2 `setCommentResolved(id, resolved)`

1. Find the existing `Comment` for `id` in the cached comment list.
2. Build a new `Comment` copy with `resolved = resolved` and a fresh
   ISO 8601 timestamp (use existing `iso8601Now()` helper).
3. `comment_to_entry` and `streamSync.push("comments", entry)`.
4. Trigger a sync cycle (existing pattern from chat send).

### 7.3 `deleteComment(id)`

1. Build a `StreamEntry` with the comment's `id`, fresh timestamp,
   `tombstone = true`. Empty payload is fine.
2. `streamSync.push("comments", entry)`. StreamSync's filter drops it
   from future `entries()` calls on all replicas.

### 7.4 Highlight build

In the comments-display section of `syncCycle`, skip comments with
`resolved == true` when building the highlight tuple list. Resolved
comments still appear in `CommentDisplayInfo` (with `resolved = true`)
so the panel can list them under "Show resolved".

## 8. Edge cases

| Case | Behaviour |
|------|-----------|
| Resolve race (two replicas resolve simultaneously) | LWW: later timestamp wins. Both end up resolved. Idempotent. |
| Delete after resolve | Tombstone wins (newer timestamp). Comment vanishes. |
| Resurrect after delete (clock skew) | If a resolve toggle on replica B with older timestamp arrives at A after A's delete: tombstone keeps it gone. Replica B's user briefly sees a zombie until it syncs and the tombstone wins. Acceptable. |
| Resolved comment range edited | Anchors still update; if user unresolves later, highlight reappears at current resolved range. |
| Toggle state on rebuild | Persisted as a member; survives `setComments` calls within the session. Not persisted across app restarts. |

## 9. Testing

### 9.1 Unit tests (`tst_comment.cpp`)

Add to existing file:

1. **Resolved round-trip.** Build Comment with `resolved = true`,
   convert to entry and back, verify `resolved` survives.
2. **Resolved default.** Parse a payload without `resolved` key,
   verify `Comment.resolved == false`.

### 9.2 Manual smoke test

- Add comment → see highlight + active list entry.
- Click Resolve → highlight disappears on both panes; comment moves
  out of active list; "Show resolved (1)" appears.
- Toggle Show resolved → resolved comment visible, greyed.
- Click Unresolve → highlight returns; comment back in active list.
- Click ✕ on a comment → comment disappears entirely on both panes.

## 10. File inventory

| File | Change |
|------|--------|
| `libs/collabtext/src/crdt/Comment.h` | Add `resolved` field. |
| `libs/collabtext/src/crdt/Comment.cpp` | Serialize/parse `resolved`. |
| `libs/collabtext/src/ui/CommentsPanelWidget.h` | Add `resolved` to `CommentDisplayInfo`; add 3 new signals; member state for toggle + cached list. |
| `libs/collabtext/src/ui/CommentsPanelWidget.cpp` | Custom row widget with buttons; toggle button; resolved styling; rebuildList(). |
| `libs/collabtext/tests/tst_comment.cpp` | Add 2 tests. |
| `app/main.cpp` | Connect 3 new signals; `setCommentResolved` and `deleteComment` slots; skip resolved when building highlights; pass `resolved` into `CommentDisplayInfo`. |

No CMake changes (no new files).

## 11. Success criteria

- New comment round-trip tests pass.
- Full `ctest` suite green.
- Manual smoke (above) all pass on both editor panes.
