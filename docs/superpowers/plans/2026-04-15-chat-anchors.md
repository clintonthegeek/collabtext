# Chat Anchors Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add optional CRDT Anchor to chat messages so they link to document positions, displayed as clickable "line N" indicators in the chat panel.

**Architecture:** Add `std::optional<Anchor>` to ChatMessage, serialize it in the payload JSON. ChatPanelWidget gains an `anchorLine` parameter and an `anchorClicked` signal. The test app captures anchors on send, resolves them on display, and scrolls on click.

**Tech Stack:** C++20, Qt6 Widgets, CRDT Anchor (existing)

**Spec:** `docs/superpowers/specs/2026-04-15-chat-anchors-design.md`

---

### Task 1: ChatMessage anchor support

**Files:**
- Modify: `libs/collabtext/src/crdt/ChatMessage.h`
- Modify: `libs/collabtext/src/crdt/ChatMessage.cpp`
- Modify: `libs/collabtext/tests/tst_chat_message.cpp`

- [ ] **Step 1: Add anchor test to tst_chat_message.cpp**

Add this test to the existing `TestChatMessage` class in `libs/collabtext/tests/tst_chat_message.cpp`:

```cpp
    void round_trip_with_anchor() {
        ChatMessage msg;
        msg.id = "1-10";
        msg.replica_id = 1;
        msg.seq = 10;
        msg.timestamp = "2026-04-15T10:00:00Z";
        msg.author = "alice-a1b2c3";
        msg.author_name = "Alice";
        msg.body = "Check this paragraph";
        msg.anchor = Anchor(3, 200, Bias::Left);

        StreamEntry entry = chat_message_to_entry(msg);
        // Verify anchor appears in payload
        QVERIFY(entry.payload.find("anchor") != std::string::npos);

        auto decoded = chat_message_from_entry(entry);
        QVERIFY(decoded.has_value());
        QVERIFY(decoded->anchor.has_value());
        QCOMPARE(decoded->anchor->replica_id, uint16_t(3));
        QCOMPARE(decoded->anchor->char_value, uint32_t(200));
        QCOMPARE(decoded->anchor->bias, Bias::Left);
        // Other fields still intact
        QCOMPARE(decoded->body, msg.body);
        QCOMPARE(decoded->author, msg.author);
    }

    void round_trip_without_anchor_still_works() {
        // Existing messages without anchor should decode with nullopt
        ChatMessage msg;
        msg.id = "1-1";
        msg.replica_id = 1;
        msg.seq = 1;
        msg.timestamp = "2026-04-15T10:00:00Z";
        msg.author = "bob-x1y2z3";
        msg.author_name = "Bob";
        msg.body = "No anchor here";
        // msg.anchor is nullopt by default

        StreamEntry entry = chat_message_to_entry(msg);
        QVERIFY(entry.payload.find("anchor") == std::string::npos);

        auto decoded = chat_message_from_entry(entry);
        QVERIFY(decoded.has_value());
        QVERIFY(!decoded->anchor.has_value());
        QCOMPARE(decoded->body, msg.body);
    }
```

Also add the include for Anchor at the top of the test file:

```cpp
#include "crdt/Anchor.h"
```

- [ ] **Step 2: Run tests to verify new tests fail**

Run: `cmake --build build-dev -j$(($(nproc)-1)) && ctest --test-dir build-dev --output-on-failure -R tst_chat_message`

Expected: Compile error — `ChatMessage` has no member `anchor`.

- [ ] **Step 3: Add anchor field to ChatMessage.h**

In `libs/collabtext/src/crdt/ChatMessage.h`, add the include and field:

Add include after the existing includes:
```cpp
#include "crdt/Anchor.h"
```

Add the anchor field to the struct, after `body`:
```cpp
    std::optional<Anchor> anchor;  // optional document position
```

- [ ] **Step 4: Update chat_message_to_entry in ChatMessage.cpp**

In the `chat_message_to_entry` function, before the closing `}` of the payload string, add anchor serialization:

Replace the payload construction block:
```cpp
    std::string payload = "{";
    payload += "\"author\":"      + escape_json(msg.author);
    payload += ",\"author_name\":" + escape_json(msg.author_name);
    payload += ",\"body\":"        + escape_json(msg.body);
    payload += "}";
```

With:
```cpp
    std::string payload = "{";
    payload += "\"author\":"       + escape_json(msg.author);
    payload += ",\"author_name\":" + escape_json(msg.author_name);
    payload += ",\"body\":"        + escape_json(msg.body);
    if (msg.anchor) {
        payload += ",\"anchor\":{\"r\":";
        payload += std::to_string(msg.anchor->replica_id);
        payload += ",\"s\":";
        payload += std::to_string(msg.anchor->char_value);
        payload += ",\"b\":";
        payload += (msg.anchor->bias == Bias::Left) ? "\"left\"" : "\"right\"";
        payload += "}";
    }
    payload += "}";
```

- [ ] **Step 5: Update chat_message_from_entry in ChatMessage.cpp**

In the `chat_message_from_entry` function, add anchor parsing. In the `while (auto key = p.next_key())` loop, add a new branch after the `body` branch:

```cpp
        } else if (*key == "anchor") {
            // Parse anchor object: {"r":N,"s":N,"b":"left"|"right"}
            if (!p.expect('{')) { p.skip_value(); continue; }
            uint16_t a_rid = 0;
            uint32_t a_cv = 0;
            Bias a_bias = Bias::Left;
            while (auto akey = p.next_key()) {
                if (*akey == "r") {
                    auto v = p.parse_string();
                    if (!v) { // try as bare number
                        // rewind not possible with this parser, so use skip
                        p.skip_value();
                        continue;
                    }
                    a_rid = static_cast<uint16_t>(std::stoul(*v));
                } else if (*akey == "s") {
                    auto v = p.parse_string();
                    if (!v) { p.skip_value(); continue; }
                    a_cv = static_cast<uint32_t>(std::stoul(*v));
                } else if (*akey == "b") {
                    auto v = p.parse_string();
                    if (v && *v == "right") a_bias = Bias::Right;
                } else {
                    p.skip_value();
                }
            }
            p.expect('}');
            anchor_val = Anchor(a_rid, a_cv, a_bias);
            has_anchor = true;
```

Wait — the parser writes numbers without quotes (`"r":3`) but `parse_string` expects quotes. The anchor numbers are bare integers. The parser needs to handle bare numbers for the anchor subobject.

Add a `parse_uint` method to `ChatPayloadParser` (in the anonymous namespace):

```cpp
    std::optional<uint64_t> parse_uint() {
        skip_ws();
        if (at_end() || peek() < '0' || peek() > '9') return std::nullopt;
        uint64_t val = 0;
        while (!at_end() && peek() >= '0' && peek() <= '9')
            val = val * 10 + (advance() - '0');
        return val;
    }
```

Then the anchor parsing in the loop becomes:

```cpp
        } else if (*key == "anchor") {
            if (!p.expect('{')) { p.skip_value(); continue; }
            uint16_t a_rid = 0;
            uint32_t a_cv = 0;
            Bias a_bias = Bias::Left;
            while (auto akey = p.next_key()) {
                if (*akey == "r") {
                    auto v = p.parse_uint();
                    if (v) a_rid = static_cast<uint16_t>(*v);
                } else if (*akey == "s") {
                    auto v = p.parse_uint();
                    if (v) a_cv = static_cast<uint32_t>(*v);
                } else if (*akey == "b") {
                    auto v = p.parse_string();
                    if (v && *v == "right") a_bias = Bias::Right;
                } else {
                    p.skip_value();
                }
            }
            p.expect('}');
            anchor_val = Anchor(a_rid, a_cv, a_bias);
            has_anchor = true;
```

Also add these local variables before the `while` loop in `chat_message_from_entry`:

```cpp
    Anchor anchor_val;
    bool has_anchor = false;
```

And after building the ChatMessage at the end, before `return msg;`:

```cpp
    if (has_anchor)
        msg.anchor = anchor_val;
```

- [ ] **Step 6: Build and run tests**

Run: `cmake --build build-dev -j$(($(nproc)-1)) && ctest --test-dir build-dev --output-on-failure -R tst_chat_message`

Expected: All 6 test cases pass (4 existing + 2 new).

- [ ] **Step 7: Commit**

```
git add libs/collabtext/src/crdt/ChatMessage.h libs/collabtext/src/crdt/ChatMessage.cpp \
       libs/collabtext/tests/tst_chat_message.cpp
git commit -m "feat: optional CRDT Anchor on ChatMessage with serialization"
```

---

### Task 2: ChatPanelWidget anchor display and click

**Files:**
- Modify: `libs/collabtext/src/ui/ChatPanelWidget.h`
- Modify: `libs/collabtext/src/ui/ChatPanelWidget.cpp`

- [ ] **Step 1: Update ChatPanelWidget.h**

Change the `addMessage` signature to include `anchorLine` and add the `anchorClicked` signal:

```cpp
class ChatPanelWidget : public QWidget {
    Q_OBJECT
public:
    explicit ChatPanelWidget(QWidget *parent = nullptr);

    /// Append a message. If anchorLine >= 1, show a clickable "(line N)" link.
    void addMessage(const QString &authorName, const QString &body,
                    const QString &timestamp, const QColor &authorColor,
                    int anchorLine = -1);

signals:
    void messageSent(const QString &body);
    /// Emitted when the user clicks a "(line N)" link in a chat message.
    void anchorClicked(int line);

private:
    QListWidget *m_list;
    QLineEdit *m_input;
};
```

- [ ] **Step 2: Update ChatPanelWidget.cpp**

Replace the `addMessage` implementation:

```cpp
void ChatPanelWidget::addMessage(const QString &authorName, const QString &body,
                                  const QString &timestamp, const QColor &authorColor,
                                  int anchorLine)
{
    QScrollBar *bar = m_list->verticalScrollBar();
    const bool atBottom = bar->value() >= bar->maximum();

    auto *item = new QListWidgetItem(m_list);

    QString html = QString("<b style=\"color:%1\">%2:</b> %3")
        .arg(authorColor.name(), authorName.toHtmlEscaped(), body.toHtmlEscaped());

    if (anchorLine >= 1) {
        html += QString(" <a href=\"line:%1\" style=\"color:#888; font-size:small\">(line %1)</a>")
            .arg(anchorLine);
    }

    auto *label = new QLabel(this);
    label->setTextFormat(Qt::RichText);
    label->setText(html);
    label->setWordWrap(true);
    label->setToolTip(timestamp);

    if (anchorLine >= 1) {
        connect(label, &QLabel::linkActivated, this, [this](const QString &link) {
            // link is "line:N"
            if (link.startsWith("line:")) {
                bool ok = false;
                int line = link.mid(5).toInt(&ok);
                if (ok) emit anchorClicked(line);
            }
        });
    }

    item->setSizeHint(label->sizeHint());
    m_list->setItemWidget(item, label);

    if (atBottom) {
        m_list->scrollToBottom();
    }
}
```

- [ ] **Step 3: Build**

Run: `cmake --build build-dev -j$(($(nproc)-1))`

Expected: Build succeeds. (The test app calls `addMessage` with 4 args — the default `anchorLine = -1` makes this backward compatible.)

- [ ] **Step 4: Commit**

```
git add libs/collabtext/src/ui/ChatPanelWidget.h libs/collabtext/src/ui/ChatPanelWidget.cpp
git commit -m "feat: ChatPanelWidget anchor line display and anchorClicked signal"
```

---

### Task 3: Test app wiring — capture, display, and navigate anchors

**Files:**
- Modify: `app/main.cpp`

- [ ] **Step 1: Include Anchor.h**

Add to the includes at the top of `app/main.cpp`:

```cpp
#include "crdt/Anchor.h"
```

- [ ] **Step 2: Capture anchor on send**

In `onChatMessageSent`, after setting `msg.body` and before pushing to StreamSync, add anchor capture:

```cpp
        // Capture document anchor from the active editor's cursor
        EditorPane *activePane = m_paneB->editor()->hasFocus() ? m_paneB : m_paneA;
        auto cursor = activePane->editor()->textCursor();
        int qtPos = cursor.hasSelection() ? cursor.selectionStart() : cursor.position();
        uint32_t byteOff = activePane->qtPosToByteOffset(qtPos);
        msg.anchor = activePane->buffer().anchor_at(byteOff, Bias::Left);
```

Note: `EditorPane::qtPosToByteOffset` and `EditorPane::buffer()` are already public methods.

- [ ] **Step 3: Display anchor line number when receiving**

In `syncCycle`, modify the chat display loop. Replace:

```cpp
            m_chatPanel->addMessage(
                QString::fromStdString(msg->author_name),
                QString::fromStdString(msg->body),
                QString::fromStdString(msg->timestamp),
                color);
```

With:

```cpp
            int anchorLine = -1;
            if (msg->anchor) {
                // Resolve anchor against pane A's buffer (both have the same text)
                uint32_t byteOff = m_paneA->buffer().resolve_anchor(*msg->anchor);
                // Count newlines up to byteOff to get 1-based line number
                std::string text = m_paneA->buffer().text();
                int line = 1;
                for (uint32_t i = 0; i < byteOff && i < text.size(); ++i) {
                    if (text[i] == '\n') ++line;
                }
                anchorLine = line;
            }
            m_chatPanel->addMessage(
                QString::fromStdString(msg->author_name),
                QString::fromStdString(msg->body),
                QString::fromStdString(msg->timestamp),
                color,
                anchorLine);
```

- [ ] **Step 4: Handle anchor click — scroll to line**

Connect the `anchorClicked` signal. In the MainWindow constructor, after the existing `connect(m_chatPanel, &ChatPanelWidget::messageSent, ...)` line, add:

```cpp
        connect(m_chatPanel, &ChatPanelWidget::anchorClicked,
                this, [this](int line) {
            // Convert 1-based line number to byte offset
            std::string text = m_paneA->buffer().text();
            uint32_t byteOff = 0;
            int currentLine = 1;
            for (size_t i = 0; i < text.size() && currentLine < line; ++i) {
                if (text[i] == '\n') ++currentLine;
                byteOff = static_cast<uint32_t>(i + 1);
            }
            // Scroll the last-focused editor pane
            EditorPane *pane = m_paneB->editor()->hasFocus() ? m_paneB : m_paneA;
            pane->editor()->scrollByteOffsetToTop(byteOff, /*keepCursorVisible=*/false);
        });
```

- [ ] **Step 5: Build and manual test**

Run: `cmake --build build-dev -j$(($(nproc)-1))`

Expected: Build succeeds.

Launch: `./build-dev/app/collabtext-testapp`

Manual smoke test:
1. Type enough text to have 20+ lines
2. Click at line 15 in Alice's editor
3. Type a chat message, press Enter
4. Verify the message shows "(line 15)" as a clickable link
5. Scroll the editor away from line 15
6. Click "(line 15)" in the chat
7. Verify the editor scrolls back to line 15

- [ ] **Step 6: Commit**

```
git add app/main.cpp
git commit -m "feat: capture, display, and navigate chat anchors in test app"
```

---

### Task 4: Full test suite verification

**Files:** None (verification only)

- [ ] **Step 1: Run full test suite**

Run: `ctest --test-dir build-dev --output-on-failure -j$(($(nproc)-1)) -E tst_benchmark`

Expected: All tests pass, including `tst_chat_message` (6 tests).

- [ ] **Step 2: Verify clean working tree**

Run: `git status`

Expected: Clean.
