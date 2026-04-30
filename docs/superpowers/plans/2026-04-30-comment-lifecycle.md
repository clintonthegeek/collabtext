# Comment Lifecycle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add resolve / unresolve / delete actions to inline comments. Resolved comments lose their editor highlight and hide behind a "Show resolved (N)" toggle. Delete tombstones the comment via StreamSync.

**Architecture:** Extend `Comment` with a `resolved` bool serialized into the existing payload JSON (LWW merge handles state). `CommentsPanelWidget` renders each item as a custom row widget with inline action buttons; resolved items are filtered into a togglable section. The test app (`app/main.cpp`) handles three new signals — re-pushing the Comment for resolve/unresolve, pushing a tombstone StreamEntry for delete — and skips resolved comments when building editor highlights.

**Tech Stack:** C++/Qt6, existing CollabText CRDT and StreamSync.

**Spec:** `docs/superpowers/specs/2026-04-30-comment-lifecycle-design.md`

---

## File Structure

| File | Role |
|------|------|
| `libs/collabtext/src/crdt/Comment.h` | Comment struct gains `resolved`. |
| `libs/collabtext/src/crdt/Comment.cpp` | Serialize and parse `resolved`. |
| `libs/collabtext/tests/tst_comment.cpp` | Two new tests for the field. |
| `libs/collabtext/src/ui/CommentsPanelWidget.h` | New signals, members, `resolved` on `CommentDisplayInfo`. |
| `libs/collabtext/src/ui/CommentsPanelWidget.cpp` | Custom row widget, toggle, rebuild from cache. |
| `app/main.cpp` | Wire signals, handle lifecycle, filter highlights. |

No new files. No CMake changes.

---

## Task 1: Add `resolved` to Comment data model (TDD)

**Files:**
- Modify: `libs/collabtext/src/crdt/Comment.h`
- Modify: `libs/collabtext/src/crdt/Comment.cpp`
- Modify: `libs/collabtext/tests/tst_comment.cpp`

- [ ] **Step 1: Add the failing tests**

In `libs/collabtext/tests/tst_comment.cpp`, add two new private slots to the `TestComment` class (alongside the existing `round_trip`, `payload_contains_range`, `missing_range_returns_nullopt`):

```cpp
    void resolved_round_trip() {
        Comment comment;
        comment.id          = "4-1";
        comment.replica_id  = 4;
        comment.seq         = 1;
        comment.timestamp   = "2026-04-30T09:00:00Z";
        comment.author      = "dave@example.com";
        comment.author_name = "Dave";
        comment.body        = "Looks good now";
        comment.range_start = Anchor(1, 10, Bias::Left);
        comment.range_end   = Anchor(1, 20, Bias::Right);
        comment.resolved    = true;

        StreamEntry entry = comment_to_entry(comment);
        auto result       = comment_from_entry(entry);

        QVERIFY(result.has_value());
        QCOMPARE(result->resolved, true);
    }

    void resolved_defaults_false_when_missing() {
        StreamEntry entry;
        entry.id         = "5-1";
        entry.replica_id = 5;
        entry.seq        = 1;
        entry.timestamp  = "2026-04-30T10:00:00Z";
        entry.payload =
            "{\"author\":\"e@example.com\",\"author_name\":\"Eve\","
            "\"body\":\"old comment\","
            "\"range\":{\"start\":{\"r\":1,\"s\":5,\"b\":\"left\"},"
            "\"end\":{\"r\":1,\"s\":7,\"b\":\"right\"}}}";

        auto result = comment_from_entry(entry);
        QVERIFY(result.has_value());
        QCOMPARE(result->resolved, false);
    }
```

- [ ] **Step 2: Run tests to verify they fail to compile**

Run: `cmake --build build-dev --target tst_comment 2>&1 | tail -20`
Expected: compile error — `Comment` has no member `resolved`.

- [ ] **Step 3: Add `resolved` field to Comment struct**

In `libs/collabtext/src/crdt/Comment.h`, add the field at the end of the struct (after `range_end`):

```cpp
struct Comment {
    std::string id;
    uint16_t    replica_id  = 0;
    uint64_t    seq         = 0;
    std::string timestamp;
    std::string author;
    std::string author_name;
    std::string body;
    Anchor      range_start;
    Anchor      range_end;
    bool        resolved    = false;
};
```

- [ ] **Step 4: Serialize `resolved` in `comment_to_entry`**

In `libs/collabtext/src/crdt/Comment.cpp`, modify `comment_to_entry`. Find the line that builds the `body` field and add `resolved` right after it, before the `range` object:

```cpp
    payload += "\"author\":"       + escape_json(comment.author);
    payload += ",\"author_name\":" + escape_json(comment.author_name);
    payload += ",\"body\":"        + escape_json(comment.body);
    payload += ",\"resolved\":";
    payload += (comment.resolved ? "true" : "false");
    payload += ",\"range\":{";
```

- [ ] **Step 5: Parse `resolved` in `comment_from_entry`**

In `libs/collabtext/src/crdt/Comment.cpp`, the parser uses a `while (auto key = p.next_key())` loop. Add a new branch to handle the `resolved` key alongside the existing `author`/`author_name`/`body`/`range` branches.

First, declare a local at the top of the parsing block (alongside `has_author`/`has_body`/`has_range`):

```cpp
    bool resolved = false;
```

Then inside the key loop, before the trailing `else { if (!p.skip_value()) return std::nullopt; }`:

```cpp
        } else if (*key == "resolved") {
            p.skip_ws();
            if (p.peek() == 't') {
                if (p.pos + 4 > p.src.size()) return std::nullopt;
                if (p.src.substr(p.pos, 4) != "true") return std::nullopt;
                p.pos += 4;
                resolved = true;
            } else if (p.peek() == 'f') {
                if (p.pos + 5 > p.src.size()) return std::nullopt;
                if (p.src.substr(p.pos, 5) != "false") return std::nullopt;
                p.pos += 5;
                resolved = false;
            } else {
                return std::nullopt;
            }
        } else {
```

(The line after the closing brace remains the existing fallback `else { if (!p.skip_value()) ... }`. Wire the new branch in front of it as shown.)

Finally, at the bottom where the `Comment` is constructed, set the field:

```cpp
    comment.range_start = range_start_val;
    comment.range_end   = range_end_val;
    comment.resolved    = resolved;
    return comment;
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `cmake --build build-dev --target tst_comment && ctest --test-dir build-dev -R tst_comment --output-on-failure`
Expected: PASS for all 5 tests (3 existing + 2 new).

- [ ] **Step 7: Commit**

```bash
git add libs/collabtext/src/crdt/Comment.h libs/collabtext/src/crdt/Comment.cpp libs/collabtext/tests/tst_comment.cpp
git commit -m "feat(crdt): Comment.resolved field with payload round-trip"
```

---

## Task 2: Add `resolved` to CommentDisplayInfo and new signals

**Files:**
- Modify: `libs/collabtext/src/ui/CommentsPanelWidget.h`

This task only changes the public API; behavior is wired in Tasks 3-4. The widget must still compile and existing behavior must still work.

- [ ] **Step 1: Add `resolved` to CommentDisplayInfo and the new signals**

In `libs/collabtext/src/ui/CommentsPanelWidget.h`, replace the file's contents with:

```cpp
#pragma once
#include <QColor>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QString>
#include <QWidget>

namespace CollabText::Ui {

struct CommentDisplayInfo {
    QString id;
    QString authorName;
    QString body;
    QString contextSnippet;
    QColor authorColor;
    bool resolved = false;
};

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
    void rebuildList();

    QList<CommentDisplayInfo> m_all;
    bool m_showResolved = false;
    QPushButton *m_toggleResolved;
    QListWidget *m_list;
    QLineEdit *m_input;
};

} // namespace CollabText::Ui
```

- [ ] **Step 2: Build to verify it compiles**

Run: `cmake --build build-dev --target collabtext 2>&1 | tail -20`
Expected: compile errors in `CommentsPanelWidget.cpp` — `m_toggleResolved` undefined, `rebuildList` not declared. (Fixed in Task 3.)

Do NOT commit yet — the widget is mid-refactor. Continue to Task 3.

---

## Task 3: Custom row widget with action buttons

**Files:**
- Modify: `libs/collabtext/src/ui/CommentsPanelWidget.cpp`

This task replaces the simple QLabel-per-row rendering with a row widget that has Resolve/Unresolve and Delete buttons. The "Show resolved" toggle is added in Task 4 — for now, render only non-resolved comments and stub the toggle button as hidden.

- [ ] **Step 1: Rewrite CommentsPanelWidget.cpp**

Replace the entire contents of `libs/collabtext/src/ui/CommentsPanelWidget.cpp` with:

```cpp
#include "ui/CommentsPanelWidget.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidgetItem>
#include <QToolButton>
#include <QVBoxLayout>

namespace CollabText::Ui {

CommentsPanelWidget::CommentsPanelWidget(QWidget *parent)
    : QWidget(parent)
    , m_toggleResolved(new QPushButton(this))
    , m_list(new QListWidget(this))
    , m_input(new QLineEdit(this))
{
    m_list->setWordWrap(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);

    m_input->setPlaceholderText("Add comment on selection...");

    auto *header = new QLabel("Comments", this);
    QFont f = header->font();
    f.setBold(true);
    header->setFont(f);

    m_toggleResolved->setCheckable(true);
    m_toggleResolved->setVisible(false);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(header);
    layout->addWidget(m_toggleResolved);
    layout->addWidget(m_list, 1);
    layout->addWidget(m_input);

    connect(m_input, &QLineEdit::returnPressed, this, [this]() {
        const QString text = m_input->text().trimmed();
        if (!text.isEmpty()) {
            emit addCommentRequested(text);
            m_input->clear();
        }
    });

    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        const QString id = item->data(Qt::UserRole).toString();
        emit commentClicked(id);
    });

    connect(m_toggleResolved, &QPushButton::toggled, this, [this](bool checked) {
        m_showResolved = checked;
        rebuildList();
    });
}

void CommentsPanelWidget::setComments(const QList<CommentDisplayInfo> &comments)
{
    m_all = comments;
    rebuildList();
}

void CommentsPanelWidget::rebuildList()
{
    m_list->clear();

    int resolvedCount = 0;
    for (const CommentDisplayInfo &info : m_all)
        if (info.resolved) ++resolvedCount;

    if (resolvedCount == 0) {
        m_toggleResolved->setVisible(false);
        m_toggleResolved->setChecked(false);
        m_showResolved = false;
    } else {
        m_toggleResolved->setVisible(true);
        m_toggleResolved->setText(
            (m_showResolved ? "Hide resolved (" : "Show resolved (")
            + QString::number(resolvedCount) + ")");
    }

    for (const CommentDisplayInfo &info : m_all) {
        if (info.resolved && !m_showResolved) continue;

        auto *item = new QListWidgetItem(m_list);
        item->setData(Qt::UserRole, info.id);

        const QString authorColorHex = info.authorColor.name();
        const QString textColor = info.resolved ? "#888" : authorColorHex;
        const QString fontStyle = info.resolved ? "font-style:italic;color:#888;" : "";
        const QString resolvedTag = info.resolved
            ? QStringLiteral(" <span style=\"color:#888\">(resolved)</span>")
            : QString();

        QString html = QString("<b style=\"color:%1\">%2</b>%3 <span style=\"%4\">%5</span>")
            .arg(textColor,
                 info.authorName.toHtmlEscaped(),
                 resolvedTag,
                 fontStyle,
                 info.body.toHtmlEscaped());

        if (!info.contextSnippet.isEmpty()) {
            html += QString("<br><span style=\"color:#888\">&gt; %1</span>")
                .arg(info.contextSnippet.toHtmlEscaped());
        }

        auto *row = new QWidget(m_list);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(2, 2, 2, 2);
        rowLayout->setSpacing(4);

        auto *label = new QLabel(row);
        label->setTextFormat(Qt::RichText);
        label->setText(html);
        label->setWordWrap(true);

        auto *resolveBtn = new QToolButton(row);
        resolveBtn->setText(info.resolved ? "Unresolve" : "Resolve");
        resolveBtn->setAutoRaise(true);

        auto *deleteBtn = new QToolButton(row);
        deleteBtn->setText("✕");
        deleteBtn->setAutoRaise(true);
        deleteBtn->setToolTip("Delete comment");

        rowLayout->addWidget(label, 1);
        rowLayout->addWidget(resolveBtn);
        rowLayout->addWidget(deleteBtn);

        const QString id = info.id;
        const bool wasResolved = info.resolved;
        connect(resolveBtn, &QToolButton::clicked, this, [this, id, wasResolved]() {
            if (wasResolved) emit unresolveRequested(id);
            else             emit resolveRequested(id);
        });
        connect(deleteBtn, &QToolButton::clicked, this, [this, id]() {
            emit deleteRequested(id);
        });

        item->setSizeHint(row->sizeHint());
        m_list->setItemWidget(item, row);
    }
}

} // namespace CollabText::Ui
```

- [ ] **Step 2: Build to verify it compiles**

Run: `cmake --build build-dev --target collabtext 2>&1 | tail -20`
Expected: PASS.

- [ ] **Step 3: Run unit tests**

Run: `ctest --test-dir build-dev --output-on-failure`
Expected: all green (no test changes since Task 1).

- [ ] **Step 4: Commit**

```bash
git add libs/collabtext/src/ui/CommentsPanelWidget.h libs/collabtext/src/ui/CommentsPanelWidget.cpp
git commit -m "feat(ui): comment row widget with resolve/delete buttons + toggle"
```

(Note: this commit covers both Task 2's header changes and Task 3's implementation, because the header was left in a non-compiling state. Tasks 2 and 3 are conceptually separate but share a single commit.)

---

## Task 4: Wire test app — resolve, unresolve, delete

**Files:**
- Modify: `app/main.cpp`

- [ ] **Step 1: Connect new signals**

In `app/main.cpp`, after the existing `commentClicked` connection block (around line 643, just before the `chatPanel anchorClicked` connect), add three new connections:

```cpp
        connect(m_commentsPanel, &CommentsPanelWidget::resolveRequested,
                this, [this](const QString &id) { setCommentResolved(id, true); });

        connect(m_commentsPanel, &CommentsPanelWidget::unresolveRequested,
                this, [this](const QString &id) { setCommentResolved(id, false); });

        connect(m_commentsPanel, &CommentsPanelWidget::deleteRequested,
                this, &MainWindow::deleteComment);
```

- [ ] **Step 2: Add `setCommentResolved` and `deleteComment` slots**

In the `private slots:` section of `MainWindow`, add the two methods after `onAddComment` (around line 695):

```cpp
    void setCommentResolved(const QString &id, bool resolved) {
        auto entries = m_streamSync->entries("comments");
        for (const auto &entry : entries) {
            if (entry.id != id.toStdString()) continue;
            auto c = comment_from_entry(entry);
            if (!c) return;
            c->resolved  = resolved;
            c->timestamp = now_iso8601();
            m_streamSync->push("comments", comment_to_entry(*c));
            return;
        }
    }

    void deleteComment(const QString &id) {
        StreamEntry tomb;
        tomb.id        = id.toStdString();
        tomb.timestamp = now_iso8601();
        tomb.tombstone = true;
        m_streamSync->push("comments", tomb);
    }
```

- [ ] **Step 3: Skip resolved comments when building highlights, pass `resolved` into display info**

In `app/main.cpp`, in the `// Sync comments` block of `syncCycle()` (around line 770-810), change the per-comment loop body so resolved comments do not contribute highlights but still appear in `commentDisplayList`. Replace the existing loop body (the section starting `for (const auto &entry : commentEntries) {` through the `commentDisplayList.append(info);` line) with:

```cpp
        for (const auto &entry : commentEntries) {
            auto c = comment_from_entry(entry);
            if (!c) continue;

            uint32_t startByte = m_paneA->buffer().resolve_anchor(c->range_start);
            uint32_t endByte = m_paneA->buffer().resolve_anchor(c->range_end);

            QColor color(Qt::yellow);
            auto maybeId = m_projector.read(c->author);
            if (maybeId)
                color = QColor(QString::fromStdString(maybeId->color));

            if (!c->resolved) {
                highlightsA.append({startByte, endByte, color});
                highlightsB.append({startByte, endByte, color});
            }

            std::string text = m_paneA->buffer().text();
            std::string snippet;
            if (startByte < text.size()) {
                uint32_t len = std::min(endByte - startByte, uint32_t(40));
                snippet = text.substr(startByte, len);
                if (endByte - startByte > 40) snippet += "...";
            }

            CommentDisplayInfo info;
            info.id = QString::fromStdString(c->id);
            info.authorName = QString::fromStdString(c->author_name);
            info.body = QString::fromStdString(c->body);
            info.contextSnippet = QString::fromStdString(snippet);
            info.authorColor = color;
            info.resolved = c->resolved;
            commentDisplayList.append(info);
        }
```

- [ ] **Step 4: Build the app**

Run: `cmake --build build-dev 2>&1 | tail -20`
Expected: PASS. No new warnings.

- [ ] **Step 5: Run all tests**

Run: `ctest --test-dir build-dev --output-on-failure`
Expected: all green.

- [ ] **Step 6: Commit**

```bash
git add app/main.cpp
git commit -m "feat: wire comment resolve/unresolve/delete in test app"
```

---

## Manual smoke test (post-implementation)

After the last commit, the user runs the test app and verifies:

1. Add a comment → highlight appears on both panes; comment in active list.
2. Click **Resolve** → highlight vanishes on both panes; comment moves out of active list; "Show resolved (1)" button appears.
3. Click **Show resolved (1)** → resolved comment appears greyed/italicized with "Unresolve" button.
4. Click **Unresolve** → highlight returns; comment back in active list; toggle button hides.
5. Click **✕** on a comment → comment vanishes from both panes (and stays gone after the next sync cycle).
6. Add 2 comments, resolve 1, delete the other → exactly one resolved comment remains; toggle button reads "Show resolved (1)".

If any step fails, the bug is local to the test app (Task 4) or the widget (Tasks 2-3); the data model (Task 1) is covered by unit tests.
