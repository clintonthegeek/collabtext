# Chat Anchors — Design

**Date:** 2026-04-15
**Status:** Approved, ready for implementation plan
**Related:** `docs/superpowers/specs/2026-04-13-chat-panel-design.md` (chat fundamentals)

---

## 1. Problem

Chat messages exist in isolation from the document. When Alice says
"this paragraph needs work," Bob has to guess which paragraph she
means. Linking chat messages to document positions lets the
conversation reference specific locations that track edits
automatically via CRDT Anchors.

## 2. Goals

1. **Automatic anchor capture.** Every chat message captures the
   sender's cursor position as a CRDT Anchor. If the sender has a
   text selection, the selection start is used instead (the intention
   is "I'm commenting on this").

2. **"Near line N" display.** Anchored messages show a clickable
   "near line N" indicator in the chat panel.

3. **Click to navigate.** Clicking the indicator scrolls the editor
   to the anchored position.

4. **Backward compatible.** Messages without anchors (from before this
   feature, or from a build that doesn't support anchors) display
   normally with no indicator.

## 3. Non-goals

- **Selection range anchors.** Anchoring both start and end of a
  selection to highlight a range in the document. A single-point
  anchor is sufficient for "near line N" navigation. Range anchors
  are inline-comments territory.

- **Anchor staleness indicators.** Showing when an anchor's text has
  been heavily edited or deleted since the message was sent.

## 4. ChatMessage changes

Add an optional anchor field:

```cpp
struct ChatMessage {
    std::string id;
    uint16_t replica_id = 0;
    uint64_t seq = 0;
    std::string timestamp;
    std::string author;
    std::string author_name;
    std::string body;
    std::optional<Anchor> anchor;  // NEW: document position
};
```

### 4.1 Payload format

When an anchor is present, the payload JSON includes an `anchor`
object:

```json
{
  "author": "alice-a1b2c3",
  "author_name": "Alice",
  "body": "This paragraph needs work",
  "anchor": {"r": 1, "s": 200, "b": "left"}
}
```

Fields: `r` = replica_id, `s` = char_value (Lamport seq), `b` = bias
("left" or "right"). When anchor is absent, the key is omitted
entirely.

### 4.2 Conversion function changes

`chat_message_to_entry`: If `msg.anchor` has a value, serialize it
as the `anchor` object in the payload. Otherwise omit it.

`chat_message_from_entry`: Parse the optional `anchor` object from
the payload. If absent or malformed, set `msg.anchor = std::nullopt`
(not a failure — the message is still valid).

## 5. ChatPanelWidget changes

### 5.1 addMessage signature

```cpp
void addMessage(const QString &authorName, const QString &body,
                const QString &timestamp, const QColor &authorColor,
                int anchorLine = -1);
```

New parameter: `anchorLine`. When >= 0, append " (line N)" to the
message as a clickable link. When -1 (default), no anchor indicator.
This keeps the widget document-agnostic — the caller computes the
line number from the byte offset.

### 5.2 Anchor click signal

```cpp
signals:
    void messageSent(const QString &body);
    void anchorClicked(int line);  // NEW
```

Emitted when the user clicks the "line N" link. The embedding app
converts the line number back to a byte offset (or stores the mapping)
and scrolls the editor.

### 5.3 Rendering

The "line N" part is rendered as a styled link within the message's
rich text HTML:

```html
<b style="color:#4165E1">Alice:</b> This paragraph needs work
<a href="line:42" style="color:#888; font-size:small">(line 42)</a>
```

The QLabel's `linkActivated` signal fires when clicked, which the
widget maps to `anchorClicked(line)`.

## 6. Test app wiring

### 6.1 Sending

In `onChatMessageSent`, after building the ChatMessage:

1. Get the active editor's text cursor.
2. Compute the byte offset: if there's a selection, use the selection
   start. Otherwise use the cursor position.
3. Call `Buffer::anchor_at(byteOffset, Bias::Left)` to create a CRDT
   Anchor. (The MainWindow doesn't own a Buffer, but the active pane
   does — use `m_paneA->buffer()` or `m_paneB->buffer()` based on
   focus.)
4. Set `msg.anchor = anchor`.

### 6.2 Receiving

When displaying new chat entries in `syncCycle`:

1. If `msg.anchor` has a value, resolve it against a Buffer:
   `buffer.resolve_anchor(*msg.anchor)` to get the current byte
   offset.
2. Convert byte offset to a line number: count newlines in the
   document text up to that offset, + 1.
3. Pass the line number to `addMessage`.
4. Store a mapping from line number to byte offset for click handling
   (or just recompute on click).

### 6.3 Click handling

Connect `ChatPanelWidget::anchorClicked(int line)` to a slot that
converts the 1-based line number to a byte offset (find the Nth
newline in the document text) and calls
`scrollByteOffsetToTop(byteOffset, false)` on the last-focused
editor pane. Line numbers are approximate ("near line N") so minor
drift between display time and click time is acceptable.

## 7. Edge cases

| Case | Behaviour |
|------|-----------|
| Anchor resolves past document end | Clamp to document length. |
| Anchor from deleted text | `resolve_anchor` returns the position where text was. Navigation lands on the nearest surviving text. |
| Old message without anchor | `anchorLine = -1`, no link shown. Backward compatible. |
| Empty document when sending | `anchor_at(0)` returns `Anchor::min()`. Resolves to 0. "line 1" shown. |
| Click anchor while no editor has focus | Default to pane A. |

## 8. Testing

### 8.1 Unit tests (modify `tst_chat_message.cpp`)

1. **Round-trip with anchor.** ChatMessage with an anchor, convert to
   entry and back. Verify anchor fields (replica_id, char_value, bias)
   survive.

2. **Round-trip without anchor.** Existing test — verify it still works
   (anchor is nullopt after round-trip).

3. **Payload with anchor.** Verify the payload JSON contains the
   `anchor` object with `r`, `s`, `b` fields.

### 8.2 Manual smoke test

Send a chat message while the cursor is at a known position. Verify
"line N" appears. Click it. Verify the editor scrolls to that area.

## 9. File inventory

| File | Change |
|------|--------|
| `libs/collabtext/src/crdt/ChatMessage.h` | **Modify.** Add `std::optional<Anchor> anchor` to struct. |
| `libs/collabtext/src/crdt/ChatMessage.cpp` | **Modify.** Serialize/parse anchor in payload. |
| `libs/collabtext/src/ui/ChatPanelWidget.h` | **Modify.** Add `anchorLine` param to `addMessage`, add `anchorClicked` signal. |
| `libs/collabtext/src/ui/ChatPanelWidget.cpp` | **Modify.** Render anchor link, emit signal on click. |
| `libs/collabtext/tests/tst_chat_message.cpp` | **Modify.** Add anchor round-trip test. |
| `app/main.cpp` | **Modify.** Capture anchor on send, resolve on display, scroll on click. |

No new files.

## 10. Success criteria

- Anchor round-trip test passes.
- Existing ChatMessage tests still pass.
- Full `ctest` suite green.
- Manual smoke: send message with cursor at line 30, see "(line 30)"
  in chat, click it, editor scrolls to line 30.
