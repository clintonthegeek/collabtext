# Inline Comments Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build range-anchored inline comments with editor highlighting and a comments panel, using StreamSync's AnchorKeyed stream.

**Architecture:** Comment is a thin data model (like ChatMessage) that wraps StreamEntry payloads with range anchors. CollabPlainTextEdit gains a `setCommentHighlights` method that merges comment ranges into extraSelections. CommentsPanelWidget displays comments with context snippets. The test app wires everything through the "comments" AnchorKeyed stream.

**Tech Stack:** C++20, Qt6 Widgets (QListWidget, QLineEdit, QSplitter, ExtraSelections), StreamSync, CRDT Anchors

**Spec:** `docs/superpowers/specs/2026-04-15-inline-comments-design.md`

---

### Task 1: Comment data model

**Files:**
- Create: `libs/collabtext/src/crdt/Comment.h`
- Create: `libs/collabtext/src/crdt/Comment.cpp`
- Create: `libs/collabtext/tests/tst_comment.cpp`
- Modify: `libs/collabtext/CMakeLists.txt`

- [ ] **Step 1: Write the test file**

Create `libs/collabtext/tests/tst_comment.cpp`:

```cpp
#include <QTest>
#include "crdt/Comment.h"

using namespace CollabText::Crdt;

class TestComment : public QObject {
    Q_OBJECT

private slots:
    void round_trip() {
        Comment c;
        c.id = "1-10";
        c.replica_id = 1;
        c.seq = 10;
        c.timestamp = "2026-04-15T10:00:00Z";
        c.author = "alice-a1b2c3";
        c.author_name = "Alice";
        c.body = "This needs a citation";
        c.range_start = Anchor(3, 200, Bias::Left);
        c.range_end = Anchor(3, 245, Bias::Right);

        StreamEntry entry = comment_to_entry(c);
        QVERIFY(!entry.payload.empty());
        QCOMPARE(entry.id, c.id);

        auto decoded = comment_from_entry(entry);
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->id, c.id);
        QCOMPARE(decoded->replica_id, c.replica_id);
        QCOMPARE(decoded->seq, c.seq);
        QCOMPARE(decoded->timestamp, c.timestamp);
        QCOMPARE(decoded->author, c.author);
        QCOMPARE(decoded->author_name, c.author_name);
        QCOMPARE(decoded->body, c.body);
        QCOMPARE(decoded->range_start.replica_id, uint16_t(3));
        QCOMPARE(decoded->range_start.char_value, uint32_t(200));
        QCOMPARE(decoded->range_start.bias, Bias::Left);
        QCOMPARE(decoded->range_end.replica_id, uint16_t(3));
        QCOMPARE(decoded->range_end.char_value, uint32_t(245));
        QCOMPARE(decoded->range_end.bias, Bias::Right);
    }

    void payload_contains_range() {
        Comment c;
        c.id = "1-1";
        c.replica_id = 1;
        c.seq = 1;
        c.timestamp = "2026-04-15T10:00:00Z";
        c.author = "bob-x1y2z3";
        c.author_name = "Bob";
        c.body = "Typo here";
        c.range_start = Anchor(1, 100, Bias::Left);
        c.range_end = Anchor(1, 110, Bias::Right);

        StreamEntry entry = comment_to_entry(c);
        QVERIFY(entry.payload.find("range") != std::string::npos);
        QVERIFY(entry.payload.find("start") != std::string::npos);
        QVERIFY(entry.payload.find("end") != std::string::npos);
    }

    void missing_range_returns_nullopt() {
        StreamEntry entry;
        entry.id = "1-1";
        entry.replica_id = 1;
        entry.seq = 1;
        entry.timestamp = "2026-04-15T10:00:00Z";
        // Payload with author and body but no range
        entry.payload = "{\"author\":\"alice\",\"body\":\"no range\"}";

        auto decoded = comment_from_entry(entry);
        QVERIFY(!decoded.has_value());
    }
};

QTEST_APPLESS_MAIN(TestComment)
#include "tst_comment.moc"
```

- [ ] **Step 2: Create Comment.h**

```cpp
// libs/collabtext/src/crdt/Comment.h
#pragma once

#include "crdt/Anchor.h"
#include "crdt/StreamSync.h"

#include <optional>
#include <string>

namespace CollabText::Crdt {

struct Comment {
    std::string id;            // "replicaId-seq" (LWW key)
    uint16_t replica_id = 0;
    uint64_t seq = 0;
    std::string timestamp;     // ISO 8601
    std::string author;        // identity_id
    std::string author_name;   // display name snapshot
    std::string body;          // comment text
    Anchor range_start;        // start of commented range
    Anchor range_end;          // end of commented range
};

/// Build a StreamEntry from a Comment.
StreamEntry comment_to_entry(const Comment& c);

/// Parse a StreamEntry into a Comment.
/// Returns nullopt if required fields (author, body, range) are missing.
std::optional<Comment> comment_from_entry(const StreamEntry& entry);

} // namespace CollabText::Crdt
```

- [ ] **Step 3: Create Comment.cpp**

Follow the same pattern as ChatMessage.cpp — local `escape_json`, local parser struct, encode/decode.

```cpp
// libs/collabtext/src/crdt/Comment.cpp
#include "crdt/Comment.h"

#include <cstdio>

namespace CollabText::Crdt {

static std::string escape_json(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
    return out;
}

static std::string encode_anchor(const Anchor& a) {
    std::string out = "{\"r\":";
    out += std::to_string(a.replica_id);
    out += ",\"s\":";
    out += std::to_string(a.char_value);
    out += ",\"b\":";
    out += (a.bias == Bias::Left) ? "\"left\"" : "\"right\"";
    out += '}';
    return out;
}

StreamEntry comment_to_entry(const Comment& c) {
    StreamEntry e;
    e.id = c.id;
    e.replica_id = c.replica_id;
    e.seq = c.seq;
    e.timestamp = c.timestamp;

    std::string p = "{\"author\":";
    p += escape_json(c.author);
    p += ",\"author_name\":";
    p += escape_json(c.author_name);
    p += ",\"body\":";
    p += escape_json(c.body);
    p += ",\"range\":{\"start\":";
    p += encode_anchor(c.range_start);
    p += ",\"end\":";
    p += encode_anchor(c.range_end);
    p += "}}";
    e.payload = std::move(p);

    return e;
}

// ---- Parser ----

namespace {

struct CommentParser {
    std::string_view src;
    size_t pos = 0;

    bool at_end() const { return pos >= src.size(); }
    char peek() const { return at_end() ? '\0' : src[pos]; }
    char advance() { return src[pos++]; }

    void skip_ws() {
        while (!at_end() && (src[pos] == ' ' || src[pos] == '\t' ||
               src[pos] == '\n' || src[pos] == '\r'))
            ++pos;
    }

    bool expect(char c) {
        skip_ws();
        if (peek() == c) { advance(); return true; }
        return false;
    }

    std::optional<std::string> parse_string() {
        skip_ws();
        if (!expect('"')) return std::nullopt;
        std::string out;
        while (!at_end()) {
            char ch = advance();
            if (ch == '"') return out;
            if (ch == '\\') {
                if (at_end()) return std::nullopt;
                char esc = advance();
                switch (esc) {
                    case '"':  out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/'; break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    default: out += esc; break;
                }
            } else {
                out += ch;
            }
        }
        return std::nullopt;
    }

    std::optional<uint64_t> parse_uint() {
        skip_ws();
        if (at_end() || peek() < '0' || peek() > '9') return std::nullopt;
        uint64_t val = 0;
        while (!at_end() && peek() >= '0' && peek() <= '9')
            val = val * 10 + (advance() - '0');
        return val;
    }

    bool skip_value() {
        skip_ws();
        char c = peek();
        if (c == '"') return parse_string().has_value();
        if (c == '{') {
            advance(); int d = 1;
            while (!at_end() && d > 0) {
                char ch = advance();
                if (ch == '{') ++d; else if (ch == '}') --d;
                else if (ch == '"') { while (!at_end()) { ch = advance(); if (ch == '"') break; if (ch == '\\' && !at_end()) advance(); } }
            }
            return d == 0;
        }
        if (c == '[') {
            advance(); int d = 1;
            while (!at_end() && d > 0) {
                char ch = advance();
                if (ch == '[') ++d; else if (ch == ']') --d;
                else if (ch == '"') { while (!at_end()) { ch = advance(); if (ch == '"') break; if (ch == '\\' && !at_end()) advance(); } }
            }
            return d == 0;
        }
        while (!at_end() && peek() != ',' && peek() != '}' && peek() != ']')
            advance();
        return true;
    }

    std::optional<std::string> next_key() {
        skip_ws();
        if (peek() == '}') return std::nullopt;
        if (peek() == ',') advance();
        auto key = parse_string();
        if (!key) return std::nullopt;
        skip_ws();
        if (!expect(':')) return std::nullopt;
        return key;
    }

    std::optional<Anchor> parse_anchor_object() {
        if (!expect('{')) return std::nullopt;
        uint16_t r = 0;
        uint32_t s = 0;
        Bias b = Bias::Left;
        while (auto key = next_key()) {
            if (*key == "r") {
                auto v = parse_uint();
                if (v) r = static_cast<uint16_t>(*v);
            } else if (*key == "s") {
                auto v = parse_uint();
                if (v) s = static_cast<uint32_t>(*v);
            } else if (*key == "b") {
                auto v = parse_string();
                if (v && *v == "right") b = Bias::Right;
            } else {
                skip_value();
            }
        }
        expect('}');
        return Anchor(r, s, b);
    }
};

} // anonymous namespace

std::optional<Comment> comment_from_entry(const StreamEntry& entry) {
    CommentParser p{entry.payload};
    p.skip_ws();
    if (!p.expect('{')) return std::nullopt;

    std::string author, author_name, body;
    bool has_author = false, has_body = false;
    Anchor range_start, range_end;
    bool has_range = false;

    while (auto key = p.next_key()) {
        if (*key == "author") {
            auto v = p.parse_string();
            if (!v) return std::nullopt;
            author = std::move(*v);
            has_author = true;
        } else if (*key == "author_name") {
            auto v = p.parse_string();
            if (!v) return std::nullopt;
            author_name = std::move(*v);
        } else if (*key == "body") {
            auto v = p.parse_string();
            if (!v) return std::nullopt;
            body = std::move(*v);
            has_body = true;
        } else if (*key == "range") {
            if (!p.expect('{')) { p.skip_value(); continue; }
            while (auto rkey = p.next_key()) {
                if (*rkey == "start") {
                    auto a = p.parse_anchor_object();
                    if (a) range_start = *a;
                } else if (*rkey == "end") {
                    auto a = p.parse_anchor_object();
                    if (a) range_end = *a;
                } else {
                    p.skip_value();
                }
            }
            p.expect('}');
            has_range = true;
        } else {
            if (!p.skip_value()) return std::nullopt;
        }
    }

    if (!p.expect('}')) return std::nullopt;
    if (!has_author || !has_body || !has_range) return std::nullopt;

    Comment c;
    c.id = entry.id;
    c.replica_id = entry.replica_id;
    c.seq = entry.seq;
    c.timestamp = entry.timestamp;
    c.author = std::move(author);
    c.author_name = std::move(author_name);
    c.body = std::move(body);
    c.range_start = range_start;
    c.range_end = range_end;
    return c;
}

} // namespace CollabText::Crdt
```

- [ ] **Step 4: Register in CMakeLists.txt**

Add `src/crdt/Comment.cpp` to `add_library(collabtext STATIC ...)`.
Add `add_crdt_test(tst_comment)` at the bottom.

- [ ] **Step 5: Build and run tests**

Run: `cmake --build build-dev -j$(($(nproc)-1)) && ctest --test-dir build-dev --output-on-failure -R tst_comment`

Expected: 3 test cases pass.

- [ ] **Step 6: Commit**

```
git add libs/collabtext/src/crdt/Comment.h libs/collabtext/src/crdt/Comment.cpp \
       libs/collabtext/tests/tst_comment.cpp libs/collabtext/CMakeLists.txt
git commit -m "feat: Comment data model with range anchors and StreamEntry conversion"
```

---

### Task 2: Comment highlighting in CollabPlainTextEdit

**Files:**
- Modify: `libs/collabtext/src/ui/CollabPlainTextEdit.h`
- Modify: `libs/collabtext/src/ui/CollabPlainTextEdit.cpp`

- [ ] **Step 1: Add setCommentHighlights to header**

In `libs/collabtext/src/ui/CollabPlainTextEdit.h`, add to the public section (after `scrollByteOffsetToTop`):

```cpp
    /// Set comment highlight ranges. Each tuple: (startByteOff, endByteOff, color).
    /// Converts byte offsets to Qt positions and merges into extraSelections.
    void setCommentHighlights(
        const QList<std::tuple<uint32_t, uint32_t, QColor>> &highlights);
```

Add a member variable in the private section (after `m_cursorLabels`):

```cpp
    QList<QTextEdit::ExtraSelection> m_commentSelections;
```

- [ ] **Step 2: Implement setCommentHighlights**

Add to `libs/collabtext/src/ui/CollabPlainTextEdit.cpp`:

```cpp
void CollabPlainTextEdit::setCommentHighlights(
        const QList<std::tuple<uint32_t, uint32_t, QColor>> &highlights) {
    m_commentSelections.clear();
    for (const auto &[startByte, endByte, color] : highlights) {
        int qtStart = byteOffsetToQtPos(startByte);
        int qtEnd = byteOffsetToQtPos(endByte);
        if (qtStart == qtEnd) continue;  // collapsed range, skip

        QTextEdit::ExtraSelection sel;
        QColor bg = color;
        bg.setAlpha(50);
        sel.format.setBackground(bg);
        QTextCursor cursor(document());
        cursor.setPosition(qtStart);
        cursor.setPosition(qtEnd, QTextCursor::KeepAnchor);
        sel.cursor = cursor;
        m_commentSelections.append(sel);
    }
    syncExtraSelections();
}
```

- [ ] **Step 3: Update syncExtraSelections to include comment highlights**

In the existing `syncExtraSelections` method, add `m_commentSelections` to the list:

Replace:
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

With:
```cpp
void CollabPlainTextEdit::syncExtraSelections() {
    QList<QTextEdit::ExtraSelection> selections;
    selections.append(m_controller->secondarySelections());
    selections.append(m_controller->remoteSelections());
    selections.append(m_commentSelections);
    setExtraSelections(selections);
    viewport()->update();
    updateCursorLabels();
}
```

- [ ] **Step 4: Build**

Run: `cmake --build build-dev -j$(($(nproc)-1))`

Expected: Build succeeds.

- [ ] **Step 5: Commit**

```
git add libs/collabtext/src/ui/CollabPlainTextEdit.h libs/collabtext/src/ui/CollabPlainTextEdit.cpp
git commit -m "feat: setCommentHighlights on CollabPlainTextEdit via extraSelections"
```

---

### Task 3: CommentsPanelWidget

**Files:**
- Create: `libs/collabtext/src/ui/CommentsPanelWidget.h`
- Create: `libs/collabtext/src/ui/CommentsPanelWidget.cpp`
- Modify: `libs/collabtext/CMakeLists.txt`

- [ ] **Step 1: Create CommentsPanelWidget.h**

```cpp
// libs/collabtext/src/ui/CommentsPanelWidget.h
#pragma once

#include <QColor>
#include <QLineEdit>
#include <QListWidget>
#include <QString>
#include <QWidget>

namespace CollabText::Ui {

struct CommentDisplayInfo {
    QString id;
    QString authorName;
    QString body;
    QString contextSnippet;  // first ~40 chars of commented text
    QColor authorColor;
};

class CommentsPanelWidget : public QWidget {
    Q_OBJECT
public:
    explicit CommentsPanelWidget(QWidget *parent = nullptr);

    /// Replace the full comment list.
    void setComments(const QList<CommentDisplayInfo> &comments);

signals:
    /// User pressed Enter with non-empty text (and presumably a text selection).
    void addCommentRequested(const QString &body);

    /// User clicked a comment in the list.
    void commentClicked(const QString &commentId);

private:
    QListWidget *m_list;
    QLineEdit *m_input;
};

} // namespace CollabText::Ui
```

- [ ] **Step 2: Create CommentsPanelWidget.cpp**

```cpp
// libs/collabtext/src/ui/CommentsPanelWidget.cpp
#include "ui/CommentsPanelWidget.h"

#include <QLabel>
#include <QListWidgetItem>
#include <QScrollBar>
#include <QVBoxLayout>

namespace CollabText::Ui {

CommentsPanelWidget::CommentsPanelWidget(QWidget *parent)
    : QWidget(parent)
    , m_list(new QListWidget(this))
    , m_input(new QLineEdit(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    auto *header = new QLabel(QStringLiteral("<b>Comments</b>"), this);
    header->setContentsMargins(4, 2, 4, 2);
    layout->addWidget(header);

    m_list->setWordWrap(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(m_list, 1);

    m_input->setPlaceholderText(QStringLiteral("Add comment on selection..."));
    layout->addWidget(m_input);

    connect(m_input, &QLineEdit::returnPressed, this, [this]() {
        QString text = m_input->text().trimmed();
        if (text.isEmpty()) return;
        m_input->clear();
        emit addCommentRequested(text);
    });

    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        QString id = item->data(Qt::UserRole).toString();
        if (!id.isEmpty()) emit commentClicked(id);
    });
}

void CommentsPanelWidget::setComments(const QList<CommentDisplayInfo> &comments) {
    m_list->clear();
    for (const auto &c : comments) {
        auto *item = new QListWidgetItem(m_list);
        item->setData(Qt::UserRole, c.id);

        QString contextHtml;
        if (!c.contextSnippet.isEmpty()) {
            contextHtml = QStringLiteral(
                "<br><span style=\"color:#888; font-size:small\">&gt; %1</span>")
                .arg(c.contextSnippet.toHtmlEscaped());
        }

        QString html = QStringLiteral(
            "<b style=\"color:%1\">%2:</b> %3%4")
            .arg(c.authorColor.name(),
                 c.authorName.toHtmlEscaped(),
                 c.body.toHtmlEscaped(),
                 contextHtml);

        auto *label = new QLabel(html, m_list);
        label->setTextFormat(Qt::RichText);
        label->setWordWrap(true);
        label->setContentsMargins(4, 4, 4, 4);
        // Make label clicks propagate to the item
        label->setAttribute(Qt::WA_TransparentForMouseEvents);

        item->setSizeHint(label->sizeHint());
        m_list->setItemWidget(item, label);
    }
}

} // namespace CollabText::Ui
```

- [ ] **Step 3: Register in CMakeLists.txt**

Add `src/ui/CommentsPanelWidget.cpp` to `add_library(collabtext STATIC ...)`, after `src/ui/ChatPanelWidget.cpp`.

- [ ] **Step 4: Build**

Run: `cmake --build build-dev -j$(($(nproc)-1))`

Expected: Build succeeds.

- [ ] **Step 5: Commit**

```
git add libs/collabtext/src/ui/CommentsPanelWidget.h libs/collabtext/src/ui/CommentsPanelWidget.cpp \
       libs/collabtext/CMakeLists.txt
git commit -m "feat: CommentsPanelWidget with comment list and input"
```

---

### Task 4: Test app integration

**Files:**
- Modify: `app/main.cpp`

- [ ] **Step 1: Add includes**

Add at the top of `app/main.cpp`:

```cpp
#include "crdt/Comment.h"
#include "ui/CommentsPanelWidget.h"
```

- [ ] **Step 2: Register "comments" stream**

After the existing `m_streamSync->register_stream("chat", ...)` line, add:

```cpp
        m_streamSync->register_stream("comments", StreamSync::StreamType::AnchorKeyed);
```

- [ ] **Step 3: Add CommentsPanelWidget to sidebar**

After the existing `m_chatPanel` setup in the sidebar splitter, add:

```cpp
        m_commentsPanel = new CommentsPanelWidget(sidebar);
        sidebar->addWidget(m_commentsPanel);
```

Update the stretch factors (3 widgets now):
```cpp
        sidebar->setStretchFactor(0, 1);  // participants
        sidebar->setStretchFactor(1, 2);  // chat
        sidebar->setStretchFactor(2, 2);  // comments
```

- [ ] **Step 4: Wire addCommentRequested**

After the chat panel connections, add:

```cpp
        connect(m_commentsPanel, &CommentsPanelWidget::addCommentRequested,
                this, &MainWindow::onAddComment);
```

- [ ] **Step 5: Add onAddComment slot**

Add to `private slots:`:

```cpp
    void onAddComment(const QString &body) {
        EditorPane *pane = m_paneB->editor()->hasFocus() ? m_paneB : m_paneA;
        auto cursor = pane->editor()->textCursor();
        if (!cursor.hasSelection()) return;  // no selection, ignore

        int qtStart = cursor.selectionStart();
        int qtEnd = cursor.selectionEnd();
        uint32_t byteStart = pane->qtPosToByteOffset(qtStart);
        uint32_t byteEnd = pane->qtPosToByteOffset(qtEnd);

        const auto &id = pane->identity();

        ++m_commentSeq;
        Comment c;
        c.id = "c-" + std::to_string(m_commentSeq);
        c.replica_id = 0;
        c.seq = m_commentSeq;
        c.timestamp = now_iso8601();
        c.author = id.identity_id;
        c.author_name = id.display_name;
        c.body = body.toStdString();
        c.range_start = pane->buffer().anchor_at(byteStart, Bias::Left);
        c.range_end = pane->buffer().anchor_at(byteEnd, Bias::Right);

        m_streamSync->push("comments", comment_to_entry(c));
    }
```

- [ ] **Step 6: Add comment sync to syncCycle**

At the end of `syncCycle`, after the chat sync block, add:

```cpp
        // Sync comments
        auto commentEntries = m_streamSync->entries("comments");
        QList<std::tuple<uint32_t, uint32_t, QColor>> highlightsA, highlightsB;
        QList<CommentDisplayInfo> commentDisplayList;

        for (const auto &entry : commentEntries) {
            auto c = comment_from_entry(entry);
            if (!c) continue;

            uint32_t startByte = m_paneA->buffer().resolve_anchor(c->range_start);
            uint32_t endByte = m_paneA->buffer().resolve_anchor(c->range_end);

            QColor color(Qt::yellow);
            auto maybeId = m_projector.read(c->author);
            if (maybeId)
                color = QColor(QString::fromStdString(maybeId->color));

            highlightsA.append({startByte, endByte, color});
            highlightsB.append({startByte, endByte, color});

            // Build context snippet from document text
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
            commentDisplayList.append(info);
        }

        m_paneA->editor()->setCommentHighlights(highlightsA);
        m_paneB->editor()->setCommentHighlights(highlightsB);
        m_commentsPanel->setComments(commentDisplayList);
```

- [ ] **Step 7: Wire commentClicked for navigation**

In the MainWindow constructor, after the `addCommentRequested` connection:

```cpp
        connect(m_commentsPanel, &CommentsPanelWidget::commentClicked,
                this, [this](const QString &commentId) {
            // Find the comment and scroll to its range
            auto entries = m_streamSync->entries("comments");
            for (const auto &entry : entries) {
                if (entry.id == commentId.toStdString()) {
                    auto c = comment_from_entry(entry);
                    if (!c) break;
                    uint32_t byteOff = m_paneA->buffer().resolve_anchor(c->range_start);
                    EditorPane *pane = m_paneB->editor()->hasFocus() ? m_paneB : m_paneA;
                    pane->editor()->scrollByteOffsetToTop(byteOff, false);
                    break;
                }
            }
        });
```

- [ ] **Step 8: Add member variables**

Add to MainWindow's `private:` section:

```cpp
    CommentsPanelWidget *m_commentsPanel;
    uint64_t m_commentSeq = 0;
```

- [ ] **Step 9: Build and manual test**

Run: `cmake --build build-dev -j$(($(nproc)-1))`

Expected: Build succeeds.

Launch: `./build-dev/app/collabtext-testapp`

Manual smoke test:
1. Type or paste enough text for 20+ lines
2. Select a few words in Alice's editor
3. Type a comment in the comments input, press Enter
4. Verify: the selected range gets a colored highlight in both editors
5. Verify: the comment appears in the comments panel with author, body, and context snippet
6. Click the comment in the panel — editor scrolls to the range

- [ ] **Step 10: Commit**

```
git add app/main.cpp
git commit -m "feat: wire inline comments into test app with highlights and panel"
```

---

### Task 5: Full test suite verification

**Files:** None (verification only)

- [ ] **Step 1: Run full test suite**

Run: `ctest --test-dir build-dev --output-on-failure -j$(($(nproc)-1)) -E tst_benchmark`

Expected: All tests pass, including `tst_comment` (3 tests).

- [ ] **Step 2: Verify clean working tree**

Run: `git status`

Expected: Clean.
