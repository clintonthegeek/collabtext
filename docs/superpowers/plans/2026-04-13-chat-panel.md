# Chat Panel Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a chat panel that lets users send and receive messages through StreamSync's "chat" AppendOnly stream, displayed in a reusable Qt widget.

**Architecture:** ChatMessage is a thin data model that converts between application chat concepts (author, body) and StreamSync's generic StreamEntry payloads. ChatPanelWidget is a reusable display widget with no CRDT knowledge. The test app wires them together with StreamSync, identity lookup, and the sync timer.

**Tech Stack:** C++20, Qt6 Widgets (QListWidget, QLineEdit, QSplitter), StreamSync

**Spec:** `docs/superpowers/specs/2026-04-13-chat-panel-design.md`

---

### Task 1: ChatMessage data model

**Files:**
- Create: `libs/collabtext/src/crdt/ChatMessage.h`
- Create: `libs/collabtext/src/crdt/ChatMessage.cpp`
- Create: `libs/collabtext/tests/tst_chat_message.cpp`
- Modify: `libs/collabtext/CMakeLists.txt`

- [ ] **Step 1: Write the test file**

Create `libs/collabtext/tests/tst_chat_message.cpp`:

```cpp
#include <QTest>
#include "crdt/ChatMessage.h"

using namespace CollabText::Crdt;

class TestChatMessage : public QObject {
    Q_OBJECT

private slots:
    void round_trip() {
        ChatMessage msg;
        msg.id = "1-42";
        msg.replica_id = 1;
        msg.seq = 42;
        msg.timestamp = "2026-04-13T10:00:00Z";
        msg.author = "alice-a1b2c3";
        msg.author_name = "Alice";
        msg.body = "Hello, world!";

        StreamEntry entry = chat_message_to_entry(msg);
        QCOMPARE(entry.id, msg.id);
        QCOMPARE(entry.replica_id, msg.replica_id);
        QCOMPARE(entry.seq, msg.seq);
        QCOMPARE(entry.timestamp, msg.timestamp);
        QVERIFY(!entry.payload.empty());

        auto decoded = chat_message_from_entry(entry);
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->id, msg.id);
        QCOMPARE(decoded->replica_id, msg.replica_id);
        QCOMPARE(decoded->seq, msg.seq);
        QCOMPARE(decoded->timestamp, msg.timestamp);
        QCOMPARE(decoded->author, msg.author);
        QCOMPARE(decoded->author_name, msg.author_name);
        QCOMPARE(decoded->body, msg.body);
    }

    void payload_contains_expected_fields() {
        ChatMessage msg;
        msg.id = "1-1";
        msg.replica_id = 1;
        msg.seq = 1;
        msg.timestamp = "2026-04-13T10:00:00Z";
        msg.author = "bob-x1y2z3";
        msg.author_name = "Bob";
        msg.body = "test message";

        StreamEntry entry = chat_message_to_entry(msg);
        QVERIFY(entry.payload.find("bob-x1y2z3") != std::string::npos);
        QVERIFY(entry.payload.find("Bob") != std::string::npos);
        QVERIFY(entry.payload.find("test message") != std::string::npos);
    }

    void body_with_special_characters() {
        ChatMessage msg;
        msg.id = "1-2";
        msg.replica_id = 1;
        msg.seq = 2;
        msg.timestamp = "2026-04-13T10:00:00Z";
        msg.author = "alice-a1b2c3";
        msg.author_name = "Alice";
        msg.body = "line1\nline2\ttab and \"quotes\"";

        StreamEntry entry = chat_message_to_entry(msg);
        auto decoded = chat_message_from_entry(entry);
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->body, msg.body);
    }

    void invalid_payload_returns_nullopt() {
        StreamEntry entry;
        entry.id = "1-1";
        entry.replica_id = 1;
        entry.seq = 1;
        entry.timestamp = "2026-04-13T10:00:00Z";
        entry.payload = "not valid json at all";

        auto decoded = chat_message_from_entry(entry);
        QVERIFY(!decoded.has_value());
    }
};

QTEST_APPLESS_MAIN(TestChatMessage)
#include "tst_chat_message.moc"
```

- [ ] **Step 2: Create ChatMessage.h**

```cpp
// libs/collabtext/src/crdt/ChatMessage.h
#pragma once

#include "crdt/StreamSync.h"

#include <optional>
#include <string>

namespace CollabText::Crdt {

struct ChatMessage {
    std::string id;            // "replicaId-seq"
    uint16_t replica_id = 0;
    uint64_t seq = 0;
    std::string timestamp;     // ISO 8601
    std::string author;        // identity_id
    std::string author_name;   // display name snapshot
    std::string body;          // plain text
};

/// Build a StreamEntry from a ChatMessage.
/// Payload: {"author":"...","author_name":"...","body":"..."}
StreamEntry chat_message_to_entry(const ChatMessage& msg);

/// Parse a StreamEntry into a ChatMessage.
/// Returns nullopt if the payload is missing required fields.
std::optional<ChatMessage> chat_message_from_entry(const StreamEntry& entry);

} // namespace CollabText::Crdt
```

- [ ] **Step 3: Create ChatMessage.cpp**

The payload JSON uses the same escape/parse helpers as StreamSerialization.cpp. Since those are file-local (`static`) in StreamSerialization.cpp, duplicate the minimal subset needed here (escape and parse for 3 string fields). Alternatively, to avoid duplication, extract a tiny shared JSON helper — but given the project's pattern of self-contained serializers (Serialization.cpp, StreamSerialization.cpp each have their own), follow the same pattern.

```cpp
// libs/collabtext/src/crdt/ChatMessage.cpp
#include "crdt/ChatMessage.h"

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

StreamEntry chat_message_to_entry(const ChatMessage& msg) {
    StreamEntry e;
    e.id = msg.id;
    e.replica_id = msg.replica_id;
    e.seq = msg.seq;
    e.timestamp = msg.timestamp;

    std::string p = "{\"author\":";
    p += escape_json(msg.author);
    p += ",\"author_name\":";
    p += escape_json(msg.author_name);
    p += ",\"body\":";
    p += escape_json(msg.body);
    p += '}';
    e.payload = std::move(p);

    return e;
}

// ---- Minimal JSON string parser ----

static std::string_view skip_ws(std::string_view s) {
    while (!s.empty() && (s[0] == ' ' || s[0] == '\t' || s[0] == '\n' || s[0] == '\r'))
        s.remove_prefix(1);
    return s;
}

static bool consume(std::string_view& s, char c) {
    s = skip_ws(s);
    if (s.empty() || s[0] != c) return false;
    s.remove_prefix(1);
    return true;
}

static std::optional<std::string> parse_string(std::string_view& s) {
    s = skip_ws(s);
    if (s.empty() || s[0] != '"') return std::nullopt;
    s.remove_prefix(1);
    std::string out;
    while (!s.empty() && s[0] != '"') {
        if (s[0] == '\\' && s.size() >= 2) {
            switch (s[1]) {
                case '"':  out += '"'; break;
                case '\\': out += '\\'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                default: out += s[1]; break;
            }
            s.remove_prefix(2);
        } else {
            out += s[0];
            s.remove_prefix(1);
        }
    }
    if (s.empty()) return std::nullopt;
    s.remove_prefix(1); // closing quote
    return out;
}

std::optional<ChatMessage> chat_message_from_entry(const StreamEntry& entry) {
    auto s = std::string_view(entry.payload);
    if (!consume(s, '{')) return std::nullopt;

    ChatMessage msg;
    msg.id = entry.id;
    msg.replica_id = entry.replica_id;
    msg.seq = entry.seq;
    msg.timestamp = entry.timestamp;

    bool got_author = false, got_body = false;

    while (true) {
        s = skip_ws(s);
        if (!s.empty() && s[0] == '}') break;

        auto key = parse_string(s);
        if (!key) return std::nullopt;
        if (!consume(s, ':')) return std::nullopt;

        auto val = parse_string(s);
        if (!val) return std::nullopt;

        if (*key == "author") { msg.author = std::move(*val); got_author = true; }
        else if (*key == "author_name") { msg.author_name = std::move(*val); }
        else if (*key == "body") { msg.body = std::move(*val); got_body = true; }

        s = skip_ws(s);
        if (!s.empty() && s[0] == ',') s.remove_prefix(1);
    }

    if (!got_author || !got_body) return std::nullopt;
    return msg;
}

} // namespace CollabText::Crdt
```

- [ ] **Step 4: Register in CMakeLists.txt**

Add `src/crdt/ChatMessage.cpp` to `add_library(collabtext STATIC ...)`.
Add `add_crdt_test(tst_chat_message)` at the bottom.

- [ ] **Step 5: Build and run tests**

Run: `cmake --build build-dev -j$(($(nproc)-1)) && ctest --test-dir build-dev --output-on-failure -R tst_chat_message`

Expected: 4 test cases pass.

- [ ] **Step 6: Commit**

```
git add libs/collabtext/src/crdt/ChatMessage.h libs/collabtext/src/crdt/ChatMessage.cpp \
       libs/collabtext/tests/tst_chat_message.cpp libs/collabtext/CMakeLists.txt
git commit -m "feat: ChatMessage data model with StreamEntry conversion"
```

---

### Task 2: ChatPanelWidget

**Files:**
- Create: `libs/collabtext/src/ui/ChatPanelWidget.h`
- Create: `libs/collabtext/src/ui/ChatPanelWidget.cpp`
- Modify: `libs/collabtext/CMakeLists.txt`

- [ ] **Step 1: Create ChatPanelWidget.h**

```cpp
// libs/collabtext/src/ui/ChatPanelWidget.h
#pragma once

#include <QColor>
#include <QLineEdit>
#include <QListWidget>
#include <QWidget>

namespace CollabText::Ui {

class ChatPanelWidget : public QWidget {
    Q_OBJECT
public:
    explicit ChatPanelWidget(QWidget *parent = nullptr);

    /// Append a message to the list. Auto-scrolls to bottom if already
    /// at bottom. The author name is displayed in bold with authorColor.
    void addMessage(const QString &authorName, const QString &body,
                    const QString &timestamp, const QColor &authorColor);

signals:
    /// Emitted when the user presses Enter with non-empty text.
    void messageSent(const QString &body);

private:
    QListWidget *m_list;
    QLineEdit *m_input;
};

} // namespace CollabText::Ui
```

- [ ] **Step 2: Create ChatPanelWidget.cpp**

```cpp
// libs/collabtext/src/ui/ChatPanelWidget.cpp
#include "ui/ChatPanelWidget.h"

#include <QScrollBar>
#include <QVBoxLayout>

namespace CollabText::Ui {

ChatPanelWidget::ChatPanelWidget(QWidget *parent)
    : QWidget(parent)
    , m_list(new QListWidget(this))
    , m_input(new QLineEdit(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    m_list->setWordWrap(true);
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    m_list->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(m_list, 1);

    m_input->setPlaceholderText(QStringLiteral("Type a message..."));
    layout->addWidget(m_input);

    connect(m_input, &QLineEdit::returnPressed, this, [this]() {
        QString text = m_input->text().trimmed();
        if (text.isEmpty()) return;
        m_input->clear();
        emit messageSent(text);
    });
}

void ChatPanelWidget::addMessage(const QString &authorName, const QString &body,
                                  const QString &timestamp, const QColor &authorColor) {
    // Check if scrolled to bottom before adding
    auto *bar = m_list->verticalScrollBar();
    bool wasAtBottom = (bar->value() >= bar->maximum() - 4);

    auto *item = new QListWidgetItem(m_list);
    QString html = QStringLiteral("<b style=\"color:%1\">%2:</b> %3")
        .arg(authorColor.name(),
             authorName.toHtmlEscaped(),
             body.toHtmlEscaped().replace('\n', QStringLiteral("<br>")));
    auto *label = new QLabel(html, m_list);
    label->setWordWrap(true);
    label->setToolTip(timestamp);
    label->setTextFormat(Qt::RichText);
    label->setContentsMargins(4, 2, 4, 2);
    item->setSizeHint(label->sizeHint());
    m_list->setItemWidget(item, label);

    if (wasAtBottom) {
        m_list->scrollToBottom();
    }
}

} // namespace CollabText::Ui
```

- [ ] **Step 3: Register in CMakeLists.txt**

Add `src/ui/ChatPanelWidget.cpp` to `add_library(collabtext STATIC ...)`, after the existing `src/ui/ParticipantListWidget.cpp` line.

- [ ] **Step 4: Build**

Run: `cmake --build build-dev -j$(($(nproc)-1))`

Expected: Build succeeds. No test for the widget — it's a thin display widget tested manually in Task 3.

- [ ] **Step 5: Commit**

```
git add libs/collabtext/src/ui/ChatPanelWidget.h libs/collabtext/src/ui/ChatPanelWidget.cpp \
       libs/collabtext/CMakeLists.txt
git commit -m "feat: ChatPanelWidget with message list and input"
```

---

### Task 3: Test app integration

**Files:**
- Modify: `app/main.cpp`

- [ ] **Step 1: Add includes**

Add these includes to the top of `app/main.cpp`:

```cpp
#include <QSplitter>
#include "crdt/StreamSync.h"
#include "crdt/ChatMessage.h"
#include "ui/ChatPanelWidget.h"
```

- [ ] **Step 2: Modify MainWindow constructor — layout changes**

Replace the right sidebar section. Currently (around line 590-592):

```cpp
        m_participantList = new ParticipantListWidget(central);
        m_participantList->setFixedWidth(200);
        layout->addWidget(m_participantList);
```

Replace with:

```cpp
        // Right sidebar: participant list + chat panel in a vertical splitter
        auto *sidebar = new QSplitter(Qt::Vertical, central);
        sidebar->setFixedWidth(250);

        m_participantList = new ParticipantListWidget(sidebar);
        sidebar->addWidget(m_participantList);

        m_chatPanel = new ChatPanelWidget(sidebar);
        sidebar->addWidget(m_chatPanel);

        // Give chat panel more space than participant list
        sidebar->setStretchFactor(0, 1);
        sidebar->setStretchFactor(1, 3);

        layout->addWidget(sidebar);
```

- [ ] **Step 3: Add StreamSync initialization to MainWindow constructor**

After the sidebar setup and before `setCentralWidget(central)`, add:

```cpp
        // Stream sync for chat
        m_streamSync = new StreamSync(sharedFolder, "main");
        m_streamSync->register_stream("chat", StreamSync::StreamType::AppendOnly);
        m_streamSync->start();
```

Note: `StreamSync` is not a QObject, so we manage it as a raw pointer (or unique_ptr). Since MainWindow owns it, clean up in destructor or use a member unique_ptr. Simplest: store as a member and delete in destructor. Actually even simpler: allocate on the heap with `new` and the MainWindow destructor will handle it when the window is destroyed. Or just use a `std::unique_ptr<StreamSync>` member.

- [ ] **Step 4: Connect chat panel messageSent signal**

After the StreamSync initialization, connect the chat signal:

```cpp
        // Track which editor was last focused for chat authorship
        m_activeIdentity = &aliceId;
        connect(m_paneA->editor(), &QPlainTextEdit::cursorPositionChanged,
                this, [this]() { m_activeIdentity = &m_paneA->identity(); });
        connect(m_paneB->editor(), &QPlainTextEdit::cursorPositionChanged,
                this, [this]() { m_activeIdentity = &m_paneB->identity(); });

        connect(m_chatPanel, &ChatPanelWidget::messageSent,
                this, &MainWindow::onChatMessageSent);
```

- [ ] **Step 5: Add the onChatMessageSent slot**

Add to MainWindow's `private slots:` section:

```cpp
    void onChatMessageSent(const QString &body) {
        ++m_chatSeq;
        ChatMessage msg;
        msg.replica_id = 0;  // "main" replica for chat
        msg.seq = m_chatSeq;
        msg.id = "0-" + std::to_string(m_chatSeq);
        msg.timestamp = now_iso8601();
        msg.author = m_activeIdentity->identity_id;
        msg.author_name = m_activeIdentity->display_name;
        msg.body = body.toStdString();

        m_streamSync->push("chat", chat_message_to_entry(msg));
    }
```

- [ ] **Step 6: Add chat sync to syncCycle**

At the end of `syncCycle()`, after `updateParticipants()`, add:

```cpp
        // Sync chat
        m_streamSync->poll();
        auto chatEntries = m_streamSync->entries("chat");
        size_t chatCount = chatEntries.size();
        for (size_t i = m_lastChatCount; i < chatCount; ++i) {
            auto msg = chat_message_from_entry(chatEntries[i]);
            if (!msg) continue;
            QColor color(Qt::gray);
            auto maybeId = m_projector.read(msg->author);
            if (maybeId)
                color = QColor(QString::fromStdString(maybeId->color));
            m_chatPanel->addMessage(
                QString::fromStdString(msg->author_name),
                QString::fromStdString(msg->body),
                QString::fromStdString(msg->timestamp),
                color);
        }
        m_lastChatCount = chatCount;
```

- [ ] **Step 7: Add member variables**

Add to MainWindow's `private:` section:

```cpp
    ChatPanelWidget *m_chatPanel;
    StreamSync *m_streamSync = nullptr;
    const Identity *m_activeIdentity = nullptr;
    uint64_t m_chatSeq = 0;
    size_t m_lastChatCount = 0;
```

Also add a destructor to clean up StreamSync:

```cpp
    ~MainWindow() override { delete m_streamSync; }
```

- [ ] **Step 8: Build and run**

Run: `cmake --build build-dev -j$(($(nproc)-1))`

Expected: Build succeeds.

Then launch: `./build-dev/app/collabtext-testapp`

Manual smoke test:
1. Type a message in the chat input, press Enter
2. Verify the message appears in the chat panel with the correct author name and color
3. Click in the other editor pane, send another message — verify it shows as the other identity
4. Verify messages persist across poll cycles (they stay in the list)

- [ ] **Step 9: Commit**

```
git add app/main.cpp
git commit -m "feat: wire chat panel into test app with StreamSync"
```

---

### Task 4: Full test suite verification

**Files:** None (verification only)

- [ ] **Step 1: Run full test suite**

Run: `ctest --test-dir build-dev --output-on-failure -j$(($(nproc)-1)) -E tst_benchmark`

Expected: All tests pass, including:
- `tst_chat_message` (4 tests — new)
- `tst_stream_sync` (8 tests — from StreamSync)
- `tst_filesync` (9 tests — regression)
- All other existing tests

- [ ] **Step 2: Verify clean working tree**

Run: `git status`

Expected: Clean.
