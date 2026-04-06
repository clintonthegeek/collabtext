# CRDT-Editor Integration Patterns — Research Notes

How real collaborative editors keep their CRDT model and display in sync,
and what we're doing wrong.

---

## 1. The Three Approaches

Every collaborative editor must solve the same problem: the CRDT knows
what the text is, and the display widget knows what the user sees. When
a remote edit arrives, the display must update without disrupting the
user's in-progress typing, cursor position, or selection.

The field has converged on three approaches:

### A. Single Model (Zed)

The CRDT buffer IS the document model. There is no separate display
model. The editor reads directly from the buffer on every render.

**Zed's architecture:**
```
Buffer (CRDT) ← local edits
     ↑ remote edits
     ↓ read on render
Editor (reads buffer, renders to screen)
```

- `on_buffer_changed` is literally `cx.notify()` — just "re-render."
  (zed/crates/editor/src/editor.rs:24120)
- The editor never copies text out of the buffer. It reads it in place.
- Cursor positions are Anchors (CRDT-native). They never go stale.
- `edits_since(version)` returns an iterator of `Edit<D>` structs
  describing what changed, so the display map can update incrementally.
  (zed/crates/text/src/text.rs:2638)

**Why it works:** No sync problem because there's nothing to sync.
One model, one source of truth.

**Why we can't do this directly:** Qt's text widgets (QPlainTextEdit,
QTextEdit) require a QTextDocument. We can't make QPlainTextEdit read
from our Buffer directly without rewriting the entire widget from
scratch.

### B. Binding Pattern (Yjs + CodeMirror/ProseMirror)

Two models exist, but changes flow as **incremental deltas**, never
as full document replacements.

**y-codemirror6's architecture:**
```
Y.Text (CRDT) ←→ CodeMirror (editor)
    ↓ observe()           ↑ update()
    delta events          transaction changes
```

**Local edit flow** (y-codemirror6/src/y-sync.js:269-298):
1. User types → CodeMirror fires an `update` with `changes`
2. The binding checks: is this update from Yjs? (via annotation)
3. If NOT from Yjs: convert CodeMirror changes to Yjs delta operations
4. Apply delta to Y.Text inside a Yjs transaction
5. Mark the transaction's origin as `this.conf` (for echo prevention)

**Remote edit flow** (y-codemirror6/src/y-sync.js:236-251):
1. Y.Text fires an `observe` event with a delta
2. The binding checks: is the origin `this.conf`? (is it our own edit?)
3. If NOT our own: convert Yjs delta to CodeMirror change spec
4. Dispatch the changes to CodeMirror with an annotation marking them
   as from Yjs

**The critical detail:** Remote edits are expressed as a **delta**
(retain 5, insert "hello", retain 10, delete 3) — a list of surgical
operations. They are NOT expressed as "here's the new full text."
CodeMirror applies these as incremental changes, and its cursor mapping
handles position adjustment automatically.

**Echo prevention:** An annotation/flag marks changes that came from the
CRDT, so the binding doesn't send them back. Without this, every remote
edit would echo: CRDT → editor → CRDT → editor → ...

**Cursor positions:** Stored as Yjs RelativePositions (equivalent to our
Anchors). Created via `Y.createRelativePositionFromTypeIndex()`, resolved
via `Y.createAbsolutePositionFromRelativePosition()`. This is exactly
what our `anchor_at()` and `resolve_anchor()` do.
(y-codemirror6/src/y-remote-selections.js:170-173)

### C. Delta-from-Version (Xi-Editor)

The CRDT tracks version history and can compute a delta between any two
versions. The frontend requests "what changed since version X?" and
applies the result as incremental edits.

**Xi's architecture:**
```
Engine (CRDT)
  ↓ delta_rev_head(since_version)
  → Delta { Copy(0,5), Insert("hello"), Copy(10,20) }
  → Frontend applies the delta to its display
```

- `Engine::delta_rev_head(rev)` returns a delta from a given revision
  to the current head text. (xi-editor/docs/crdt-details.md)
- The delta is a sequence of `Copy(start, end)` and `Insert(text)`
  operations — surgical, incremental, not a full replacement.
- The frontend tracks which version it last rendered and requests the
  delta since that version.

---

## 2. What We're Doing Wrong

Our current approach:

```
Buffer (CRDT) ←→ QTextDocument (Qt)
    ↑ onContentsChange        ↑ syncBufferToQt
    ↓ (converts positions)    ↓ (REPLACES ENTIRE DOCUMENT)
```

**Problem 1: Full document replacement.** `syncBufferToQt()` replaces
the entire QTextDocument content every time a remote edit arrives. Even
with a diff to find the changed region, this is fundamentally wrong
because:
- It fires QTextDocument signals that interfere with in-progress editing
- It can disrupt Qt's internal input method state
- It requires saving and restoring all cursor positions manually
- Any position conversion error corrupts the entire document

**Problem 2: Position conversion through the wrong model.** We convert
QTextDocument positions (UTF-16 code units) to Buffer byte offsets by
reading the Buffer's text. But the Buffer and QTextDocument can be
temporarily out of sync if a remote edit has been applied to the Buffer
but not yet reflected in the QTextDocument.

**Problem 3: No echo prevention.** When `syncBufferToQt()` modifies the
QTextDocument, `contentsChange` fires. We use `m_syncing` to block
re-entry, but this is fragile — any signal handler we missed, any
queued event, any reentrant call can break the guard.

**Problem 4: No delta from the engine.** When a remote edit arrives, the
Buffer applies it but doesn't tell us WHAT CHANGED. We only get the new
full text. We're forced to diff old and new text ourselves, which is
both slower and more error-prone than getting a delta from the engine.

---

## 3. What We Should Do

### The correct architecture for Qt integration

Follow the **Binding Pattern** (approach B), adapted for our engine:

```
Buffer (CRDT)
  ↑ apply_local_edit()     observe() ↓
  │ returns Operation       returns list of Edit<D>
  │
  ├─→ FileSync.push_local_op(op)
  │
  ↓ when remote ops arrive:
  Buffer.apply_ops(ops) → returns edits as deltas
  ↓
  Apply deltas to QTextDocument as surgical QTextCursor operations
  (with echo-prevention annotation via m_syncing flag)
```

### What the engine needs to provide

**A `edits_since(version)` or `apply_ops_with_edits()` API** that
returns the changes as a list of `{old_range, new_text}` structs in
the current document's coordinate space. This is what Zed provides
(text.rs:2638) and what Xi provides (delta_rev_head).

Without this, we're forced to diff strings — which works for simple
cases but fails when multiple non-adjacent regions change simultaneously
(a diff can't distinguish two separate insertions from one large
replacement).

### How surgical edits preserve cursors

When we apply a remote edit as `QTextCursor::insertText("hello")` at
position 5 (a surgical insert, not a replacement), Qt automatically:
1. Adjusts ALL other QTextCursors registered on the document
2. Shifts cursors after position 5 by 5 characters
3. Leaves cursors before position 5 unchanged
4. Handles the cursor-at-insertion-point case based on
   `keepPositionOnInsert` flag

This is the ENTIRE REASON Qt tracks cursors in
`QTextDocumentPrivate::cursors`. It exists to solve this exact problem.
We just need to USE it by making surgical edits instead of full
replacements.

### How remote cursor positions should work

Following y-codemirror6's pattern:
1. Each replica's cursor position is stored as a CRDT Anchor
   (our `Buffer::anchor_at()`)
2. Anchors are exchanged via ephemeral state (or direct channel)
3. On the receiving side, `Buffer::resolve_anchor()` converts back
   to a byte offset, which is converted to a QTextDocument position
4. Displayed as a QTextEdit::ExtraSelection or painted caret

This is what our code already does for remote cursors, and it's correct.
The bug is in the document sync, not the cursor exchange.

---

## 4. Concrete Next Steps

### Step 1: Add `apply_ops_returning_edits()` to Buffer

```cpp
struct TextEdit {
    uint32_t old_start;   // byte offset in text before this edit
    uint32_t old_end;     // byte offset in text before this edit
    std::string new_text; // replacement text (empty for pure deletion)
};

std::vector<TextEdit> Buffer::apply_ops_returning_edits(
    const std::vector<Operation>& ops);
```

The Buffer already knows what changed — it modifies the fragment tree,
which determines where text was inserted/deleted. We just need to
capture that information and return it.

Alternative: snapshot the text before apply_ops, then diff after. This
is simpler but less precise for multiple non-adjacent changes.

### Step 2: Apply edits as surgical QTextCursor operations

```cpp
void EditorPane::applyRemoteEdits(const std::vector<TextEdit>& edits) {
    m_syncing = true;
    // Apply in reverse order (like multi-cursor edits) so positions
    // remain valid for subsequent edits
    for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
        int qtStart = byteOffsetToQtPos(it->old_start);
        int qtEnd = byteOffsetToQtPos(it->old_end);
        QTextCursor cursor(m_qtDoc);
        cursor.setPosition(qtStart);
        if (qtEnd > qtStart)
            cursor.setPosition(qtEnd, QTextCursor::KeepAnchor);
        cursor.insertText(QString::fromStdString(it->new_text));
    }
    m_syncing = false;
}
```

Each edit is a surgical operation. Qt adjusts all other cursors
automatically. No save/restore needed. No full document replacement.

### Step 3: Position conversion uses the correct text

Since edits are applied incrementally (not as a full replacement), the
QTextDocument and Buffer stay in sync after each surgical edit. Position
conversion in `onContentsChange` always has consistent state.

---

## 5. Key Lessons From the Field

### From Zed
- **Single model is ideal.** The CRDT buffer IS the document. No sync.
- **`edits_since(version)`** is the critical API. It tells subscribers
  what changed without requiring them to diff.
- **Anchors are the only safe position representation.** Integer offsets
  go stale; anchors don't.

### From y-codemirror6
- **Deltas, not replacements.** Remote changes are expressed as
  retain/insert/delete operations, applied as CodeMirror transactions.
- **Echo prevention via annotation.** Changes from the CRDT are marked
  so the binding doesn't send them back.
- **Cursor positions are relative positions** (Yjs equivalent of our
  Anchors), exchanged via the Awareness protocol.

### From Xi-Editor
- **The two-process architecture was a mistake.** Raph Levien: "I now
  firmly believe that the process separation between front-end and core
  was not a good idea." The async boundary made everything harder.
- **`delta_rev_head(rev)`** returns a delta from any revision to current
  — the engine computes the diff, not the frontend.
- **Transforms map coordinates between versions.** The engine handles
  coordinate space translation internally.

### From CodeMirror's author (Marijn Haverbeke)
- **Don't integrate the CRDT as the core data model.** Use it as an
  external component that drives collaborative editing.
- **The binding should be a thin layer** that translates between the
  CRDT's representation and the editor's representation.
- **CodeMirror's transaction model** makes this easy: changes are
  first-class objects that can be constructed and dispatched.

---

## 6. What This Means for collabtext

Our engine is correct. Our tests prove convergence. The Buffer, FileSync,
Serialization, and Anchor system all work.

The bug is in the integration layer — specifically, in `syncBufferToQt()`.
We're replacing the entire QTextDocument instead of applying surgical
edits. This is the wrong pattern.

The fix is NOT another patch to `syncBufferToQt()`. The fix is to add
a delta/edits API to the Buffer and rewrite the integration layer to
use surgical edits following the y-codemirror6 binding pattern.

This is an engine change (adding the edits API) plus an integration
change (rewriting the Qt binding). It is not a simple fix. But it is
the correct fix, and it's what every successful collaborative editor
does.

---

## Sources

- [y-codemirror.next source](https://github.com/yjs/y-codemirror.next) — local at ~/src/y-codemirror6/
- [Zed CRDT blog post](https://zed.dev/blog/crdts)
- [Zed editor source](https://github.com/zed-industries/zed) — local at ~/src/zed/
- [Xi-Editor CRDT details](https://xi-editor.io/docs/crdt-details.html)
- [Xi-Editor retrospective](https://raphlinus.github.io/xi/2020/06/27/xi-retrospective.html)
- [CRDTs & Positions in CodeMirror 6](https://discuss.codemirror.net/t/crdts-positions-in-codemirror-6/2571)
- [Are CRDTs suitable for shared editing?](https://blog.kevinjahns.de/are-crdts-suitable-for-shared-editing)
- [Yjs CodeMirror docs](https://docs.yjs.dev/ecosystem/editor-bindings/codemirror)
- [Yjs ProseMirror docs](https://docs.yjs.dev/ecosystem/editor-bindings/prosemirror)
- [collaborative-editor binding library](https://github.com/streamich/collaborative-editor)
