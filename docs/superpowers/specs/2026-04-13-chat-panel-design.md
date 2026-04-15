# Chat Panel — Design

**Date:** 2026-04-13
**Status:** Approved, ready for implementation plan
**Related:** `docs/superpowers/specs/2026-04-13-stream-sync-design.md` (StreamSync transport layer)
**Spec reference:** `docs/CRDT_SYNC_SPEC.md` section 15.2.3

---

## 1. Problem

StreamSync provides a generic transport layer for side-stream data.
Chat is the first consumer. We need a thin data model to build and
parse chat message payloads, and a reusable widget to display and
compose messages in the test app.

## 2. Goals

1. **ChatMessage data model.** A struct and conversion functions that
   bridge between the application's chat concepts (author, body) and
   StreamSync's generic StreamEntry (id, payload).

2. **ChatPanelWidget.** A reusable Qt widget with a scrollable message
   list and a text input. Knows nothing about StreamSync or CRDT — the
   embedding app wires it up.

3. **Test app integration.** Wire StreamSync, ChatMessage, and
   ChatPanelWidget into the two-pane demo so Alice and Bob can chat
   while editing.

## 3. Non-goals

- **Threaded replies (`reply_to`).** Flat chat only. Threading is a
  future enhancement that doesn't affect the transport or widget API.

- **Document anchors (`anchor`).** Messages optionally linked to
  document positions. Deferred — easy to add once flat chat works.

- **Message editing or deletion.** Chat is append-only per the spec.

- **Rich text or markdown rendering.** Plain text bodies only.

## 4. Architecture

Two new components, one modified file:

| Component | Responsibility |
|-----------|---------------|
| `ChatMessage` | Data model: struct + StreamEntry conversion functions. Pure C++, no Qt. |
| `ChatPanelWidget` | UI: scrollable message list + text input. Emits `messageSent`, accepts `addMessage`. No CRDT knowledge. |
| `app/main.cpp` | Wiring: StreamSync ↔ ChatMessage ↔ ChatPanelWidget. |

## 5. ChatMessage

```cpp
struct ChatMessage {
    std::string id;            // "replicaId-seq" (unique, for dedup)
    uint16_t replica_id = 0;   // Lamport component
    uint64_t seq = 0;          // Lamport component
    std::string timestamp;     // ISO 8601
    std::string author;        // identity_id
    std::string author_name;   // display name snapshot at send time
    std::string body;          // plain text
};
```

Two free functions:

```cpp
/// Build a StreamEntry from a ChatMessage.
/// The payload is a JSON object: {"author":"...","author_name":"...","body":"..."}
StreamEntry chat_message_to_entry(const ChatMessage& msg);

/// Parse a StreamEntry's payload into a ChatMessage.
/// Returns nullopt if the payload doesn't contain the required fields.
std::optional<ChatMessage> chat_message_from_entry(const StreamEntry& entry);
```

`chat_message_to_entry` sets `entry.id = msg.id`,
`entry.replica_id = msg.replica_id`, `entry.seq = msg.seq`,
`entry.timestamp = msg.timestamp`, and `entry.payload` to the JSON
encoding of author, author_name, and body.

`chat_message_from_entry` copies id, replica_id, seq, timestamp from
the entry, then parses the payload JSON to extract author, author_name,
and body.

Lives in `libs/collabtext/src/crdt/ChatMessage.h/.cpp`.

### 5.1 Payload format

```json
{"author":"clinton-a7f3b2","author_name":"Clinton","body":"Should we refactor this?"}
```

Uses the same JSON escaping pattern as StreamSerialization. Short keys
would save bytes but readability wins for a low-volume stream.

## 6. ChatPanelWidget

```
┌─────────────────────┐
│ ParticipantList     │
├─────────────────────┤
│ Alice: hello        │
│ Bob: hey there      │
│ Alice: check line 5 │
│                     │
│  (scrollable list)  │
├─────────────────────┤
│ [Type a message...] │
└─────────────────────┘
```

```cpp
class ChatPanelWidget : public QWidget {
    Q_OBJECT
public:
    explicit ChatPanelWidget(QWidget *parent = nullptr);

    /// Append a message to the list. The widget colors the author name
    /// using authorColor. Auto-scrolls to bottom if already at bottom.
    void addMessage(const QString &authorName, const QString &body,
                    const QString &timestamp, const QColor &authorColor);

signals:
    /// Emitted when the user presses Enter in the input field.
    void messageSent(const QString &body);
};
```

### 6.1 Implementation details

- **Message list:** `QListWidget` with custom-styled items. Each item
  shows "AuthorName: body" with the author name in bold and colored.
  Timestamp as tooltip.

- **Input:** `QLineEdit` with placeholder "Type a message...". On
  Return key, emit `messageSent(text)`, clear the input. Ignore empty
  input.

- **Auto-scroll:** If the list is scrolled to the bottom when a new
  message arrives, scroll to show it. If the user has scrolled up
  (reading history), don't auto-scroll — they're reading.

- **No message limit.** For the test app's scale, unbounded is fine.

Lives in `libs/collabtext/src/ui/ChatPanelWidget.h/.cpp`.

## 7. Test app integration

### 7.1 StreamSync instance

MainWindow creates one `StreamSync` instance shared across both panes
(chat is global, not per-editor). Registers "chat" as AppendOnly.
Calls `StreamSync::start()` on construction.

### 7.2 Layout

The right sidebar becomes a `QSplitter` (vertical):
- Top: `ParticipantListWidget` (existing)
- Bottom: `ChatPanelWidget` (new)

The splitter gives the user control over how much space each gets.

### 7.3 Sending

Connect `ChatPanelWidget::messageSent` to a slot that:
1. Builds a `ChatMessage` with the local identity's id, display_name,
   a body from the signal, an auto-incrementing seq, and current
   timestamp.
2. Converts to `StreamEntry` via `chat_message_to_entry`.
3. Pushes to `StreamSync::push("chat", entry)`.

The message appears locally immediately (StreamSync merges into its
own view on push).

### 7.4 Receiving

In `syncCycle`, after `StreamSync::poll()`:
1. Call `m_streamSync.entries("chat")` to get the full sorted list.
2. Compare count to `m_lastChatCount`. If new entries exist, convert
   each new one via `chat_message_from_entry`.
3. Look up author's color from `IdentityProjector`.
4. Call `ChatPanelWidget::addMessage` for each new message.
5. Update `m_lastChatCount`.

### 7.5 Seq counter

Each pane needs its own monotonically increasing seq for chat messages.
Use a simple `uint64_t m_chatSeq = 0` on MainWindow, incremented on
each send. The id is `std::to_string(replica_id) + "-" + std::to_string(seq)`.

Since the test app has two panes (two replicas) sharing one MainWindow,
we need to know which pane sent the message. The `messageSent` signal
doesn't carry this. Solution: MainWindow connects both panes'
hypothetical chat sends, but actually the ChatPanelWidget is shared
(one panel, two editors). The active pane's identity is used as the
author. Simpler: track a "last active pane" by connecting to each
editor's `focusIn` or `cursorPositionChanged`.

Actually, even simpler: since chat is global and the panel is shared,
just alternate identity based on which editor was last focused. Store
`m_activePaneIdentity` and update it on editor focus changes.

## 8. Edge cases

| Case | Behaviour |
|------|-----------|
| Empty message body | Input ignores Enter on empty text. |
| Very long message | No truncation. QListWidget handles word wrap. |
| Message from unknown identity | Use author_name from the message (it's a snapshot). Color defaults to gray. |
| Rapid sending | Each message gets a unique seq. StreamSync deduplicates by id. |
| Chat panel scrolled up while new messages arrive | Don't auto-scroll. User is reading history. |

## 9. Testing

New test file: `libs/collabtext/tests/tst_chat_message.cpp`.

1. **ChatMessage round-trip.** Build a ChatMessage, convert to
   StreamEntry via `chat_message_to_entry`, convert back via
   `chat_message_from_entry`. Verify all fields match.

2. **Payload encoding.** Verify the StreamEntry payload contains
   `author`, `author_name`, `body` as JSON fields.

3. **Body with special characters.** Message body containing newlines,
   quotes, backslashes. Verify round-trip preserves them.

4. **Invalid payload.** Call `chat_message_from_entry` with a
   StreamEntry whose payload is not valid chat JSON. Verify it returns
   nullopt.

Widget gets manual smoke test only (type in chat, verify it shows on
both sides with correct colors).

## 10. File inventory

| File | Change |
|------|--------|
| `libs/collabtext/src/crdt/ChatMessage.h` | **New.** Struct + conversion function declarations. |
| `libs/collabtext/src/crdt/ChatMessage.cpp` | **New.** Implementation. |
| `libs/collabtext/src/ui/ChatPanelWidget.h` | **New.** Widget class. |
| `libs/collabtext/src/ui/ChatPanelWidget.cpp` | **New.** Implementation. |
| `libs/collabtext/tests/tst_chat_message.cpp` | **New.** Data model tests. |
| `libs/collabtext/CMakeLists.txt` | Register new sources + test. |
| `app/main.cpp` | Wire StreamSync, ChatPanelWidget, sync cycle, identity tracking. |

## 11. Success criteria

- All new ChatMessage tests pass.
- Full `ctest` suite remains green.
- Manual smoke: type a message in the chat panel, see it appear. Both
  Alice and Bob can send, messages show with correct colors and names,
  messages persist across poll cycles.
