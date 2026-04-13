# Scroll Stability — Design

**Date:** 2026-04-07
**Status:** Approved, ready for implementation plan
**Related:** `docs/superpowers/specs/2026-04-06-ephemeral-identity-system-design.md` (defines `EphemeralState.viewport_top` / `viewport_bottom`)

---

## 1. Problem

When a remote replica inserts text above the local viewport, the visible
text shifts: `QPlainTextEdit` anchors scroll to a pixel Y coordinate, not
a text position. Alice is reading paragraph 7; Bob adds a paragraph at the
top; Alice's viewport now shows paragraph 8 in the place where paragraph 7
used to be. Remote edits *inside* the viewport are fine — it's the ones
*above* that produce the jarring shift.

The existing cursor-preservation work (the one that keeps the local cursor
in the right logical place after remote edits) is the same shape of
problem for a different point — the cursor position — so the fix here
uses the same tool (CRDT Anchors) for a different target (the top and
bottom of the viewport).

## 2. Goals

1. **Preserve scroll position** across remote edits. The visual line that
   was at the top of the viewport before the remote edit stays at the top
   after.
2. **Keep the local cursor visible.** If restoring the scroll would push
   the user's cursor off-screen (e.g., the cursor was below the old top,
   and the new top-line restoration now puts the cursor below the
   viewport), adjust the scroll to keep it visible. Cursor wins on
   conflict.
3. **Populate the ephemeral viewport anchors.** The ephemeral identity
   spec already defines `viewport_top` and `viewport_bottom` on
   `EphemeralState` for future follow-mode. We compute the top anchor
   anyway; compute the bottom too and write both into `EphemeralState`.

## 3. Non-goals

- **Follow-mode on the reading side.** Rendering another user's viewport
  to drive your own scroll. The spec mentions it; that is a separate
  sub-project.
- **Horizontal scroll stability.** `QPlainTextEdit` with default
  `WidgetWidth` wrap has no horizontal scroll.
- **Smooth/animated scroll restoration.** Instant jump back to the
  anchored line, consistent with the existing cursor-position
  preservation.
- **Sub-pixel precision.** Line-level restoration is sufficient. Users
  won't notice a few-pixel drift within a line.
- **Multi-cursor-aware safety net.** `ensureCursorVisible` acts on the
  primary cursor only; secondary carets are not treated as scroll
  targets. This matches existing widget behaviour.

## 4. Architecture

Three layers, each with one clear responsibility. No new files. No CRDT
type leakage into the widget. No Qt type leakage into the `Buffer`.

| Layer | Knows about | New responsibility |
|---|---|---|
| `CollabPlainTextEdit` (widget) | Qt positions, viewport geometry | Three byte-offset helpers |
| `Buffer` (CRDT) | CRDT anchors | Nothing new — uses existing `anchor_at` / `resolve_anchor` |
| `EditorPane` (app) | Both | Orchestrates capture/restore around `applyEditsToQt`; updates cached viewport anchors for `EphemeralState` |

## 5. Widget API

Added to `libs/collabtext/src/ui/CollabPlainTextEdit.h`:

```cpp
/// Byte offset (UTF-8) of the character at the top-left of the viewport.
/// Returns 0 for an empty document. Wrap-aware: returns the byte offset
/// of the visual line at the top, not the containing paragraph.
uint32_t topVisibleByteOffset() const;

/// Byte offset (UTF-8) of the character just past the bottom-right of
/// the viewport. Returns the document's visible byte length if no
/// characters are past it. Used to populate
/// EphemeralState.viewport_bottom for follow-mode.
uint32_t bottomVisibleByteOffset() const;

/// Scroll so that the visual line containing `byteOff` is at the top of
/// the viewport. If `keepCursorVisible` is true and the restoration
/// would put the local cursor off-screen, the viewport is adjusted to
/// keep the cursor visible (Qt's ensureCursorVisible semantics).
void scrollByteOffsetToTop(uint32_t byteOff, bool keepCursorVisible);
```

### 5.1 Implementation sketch

**`topVisibleByteOffset()`**:
- `cursorForPosition(QPoint(0, 0))` → QTextCursor at the top-left pixel
  of the viewport.
- Convert the cursor's `position()` (UTF-16 code unit offset) to a UTF-8
  byte offset using the existing widget-side conversion pattern.

**`bottomVisibleByteOffset()`**:
- `cursorForPosition(QPoint(viewport()->width(), viewport()->height()))`
  — cursor at the bottom-right pixel. If past the end of the document,
  clamp to the document's visible byte length.
- Same UTF-16 → UTF-8 conversion.

**`scrollByteOffsetToTop(byteOff, keepCursorVisible)`**:
- Convert `byteOff` (UTF-8) to a UTF-16 position.
- Build a temporary `QTextCursor` at that position.
- Use `cursorRect(tempCursor).top()` to find the pixel Y of the target
  line relative to the current scroll.
- Adjust `verticalScrollBar()->value()` by the delta (in line units) to
  put that line at the top.
- If `keepCursorVisible`: call `QPlainTextEdit::ensureCursorVisible()`.
  Qt's implementation scrolls minimally to show the primary cursor,
  correctly overriding our top-line restoration when the cursor would
  otherwise be off-screen.

### 5.2 UTF-16 ↔ UTF-8 conversion

The widget already converts between the two for cursor-label rendering
(see `CollabPlainTextEdit::updateCursorLabels` and `paintEvent`). The
same pattern (`QString::fromUtf8(utf8.data(), byteOff).length()` for
UTF-8 → UTF-16, and truncation via UTF-8 slicing for the reverse)
applies here. Keep the helpers local to `CollabPlainTextEdit.cpp`.

## 6. App-side orchestration

Added to `app/main.cpp` on `EditorPane`:

```cpp
/// Apply remote edits while preserving the local viewport scroll and
/// the local cursor visibility. Also refreshes the cached viewport
/// anchors used to populate EphemeralState on the next presence flush.
void applyEditsPreservingScroll(const std::vector<TextEdit> &edits) {
    if (edits.empty()) return;

    // Capture viewport anchors before the tree mutates.
    uint32_t topByteOff = m_edit->topVisibleByteOffset();
    Anchor topAnchor = m_buffer.anchor_at(topByteOff, Bias::Left);

    uint32_t bottomByteOff = m_edit->bottomVisibleByteOffset();
    Anchor bottomAnchor = m_buffer.anchor_at(bottomByteOff, Bias::Left);

    applyEditsToQt(edits);

    // Restore scroll.
    uint32_t newTopByteOff = m_buffer.resolve_anchor(topAnchor);
    m_edit->scrollByteOffsetToTop(newTopByteOff, /*keepCursorVisible=*/true);

    // Cache the anchors for the next EphemeralState flush.
    m_viewportTopAnchor = topAnchor;
    m_viewportBottomAnchor = bottomAnchor;
}
```

Every remote-edit path (`poll()`, gremlin timer, any future syncer) calls
`applyEditsPreservingScroll(edits)` instead of `applyEditsToQt(edits)`
directly. The raw `applyEditsToQt` stays as the private UTF-16 mutation
helper; `applyEditsPreservingScroll` becomes the public entry point.

### 6.1 Cached anchor members

Added to `EditorPane`:

```cpp
std::optional<Anchor> m_viewportTopAnchor;
std::optional<Anchor> m_viewportBottomAnchor;
```

These are updated in two places:

1. `applyEditsPreservingScroll` (post-remote-edit).
2. A new slot reacting to local scroll, wired to the existing
   `CollabPlainTextEdit::scrollContentsBy` override. The widget already
   emits nothing here; we'll add a `viewportScrolled()` signal emitted
   from that override, and the app recomputes both anchors on receipt.

This gives the invariant: after any event that could move the viewport
(local scroll or remote edit), the cached anchors reflect the current
viewport within one event loop tick.

### 6.2 Feeding `EphemeralState`

`PresenceManager::flush()` (or whatever path serializes
`EphemeralState`) reads `m_viewportTopAnchor` and
`m_viewportBottomAnchor` into the outgoing state on each presence
update. If they're `nullopt` (widget hasn't been shown yet), the fields
are omitted from the serialized JSON.

### 6.3 Ephemeral JSON serialization

Already implemented. `libs/collabtext/src/identity/Identity.cpp`
(lines 124-132 for encode, 561-568 for decode) already emits and reads
`viewport_top` / `viewport_bottom` as optional anchor objects. This
design does not touch `Identity.cpp`; populating the fields at the
call site (§6.2) is sufficient.

## 7. Edge cases

| Case | Behaviour |
|---|---|
| Top anchor's character is deleted by a remote edit | `resolve_anchor` returns the position where the character used to be (§Buffer.cpp:367-393). Scroll restores to the adjacent surviving line. No jump. |
| Empty document at capture | `topVisibleByteOffset()` returns 0 → `Anchor::min()` → `resolve_anchor` returns 0 → scroll to top. No-op. |
| Viewport is at the document end | Bottom anchor captures `visible_length()`; resolves to the new `visible_length()`. The top anchor is still a real character, and restoration puts that line at the top. |
| User is mid-drag on the scrollbar | Capture happens at the current scroll position regardless of drag state. No special handling — the dragged position just becomes the "before" state. |
| Local cursor is in multi-cursor mode | `ensureCursorVisible` only considers the primary cursor. Secondary carets may land off-screen. This matches existing widget behaviour and is out of scope. |
| Remote edit is entirely inside the viewport | Top-line anchor position doesn't move → `scrollByteOffsetToTop` is a no-op → the edit just appears in place. Desirable. |
| Remote edit happens concurrently with local typing | The new `applyEditsPreservingScroll` is only called from the remote-edit path; local edits don't go through it. `m_handlingKey` or similar existing guards prevent re-entrancy. |
| Document has zero lines visible (e.g., collapsed) | `cursorForPosition(QPoint(0,0))` returns a valid cursor at position 0. Anchor captures at 0. Harmless. |

## 8. Testing

Three layers, three kinds of tests:

1. **Widget unit tests** — new file `libs/collabtext/tests/tst_collab_plain_text_edit.cpp`. No existing widget test covers `CollabPlainTextEdit` directly (only `tst_identity_widgets.cpp` covers the identity widgets). Populate a widget with a known text, scroll programmatically, verify:
   - `topVisibleByteOffset()` returns the expected offset for the known scroll position.
   - `bottomVisibleByteOffset()` returns an offset strictly greater than top for a non-empty document, equals visible length when scrolled to end.
   - `scrollByteOffsetToTop(off)` lands on the right visual line.
   - `scrollByteOffsetToTop(off, keepCursorVisible=true)` with a cursor above the target correctly adjusts scroll so the cursor is visible.

   Requires `QTest::qWaitForWindowExposed` for the widget's layout to be
   computed; otherwise `cursorForPosition` returns nonsense. Keep the
   document synthetic and wrapping predictable (fixed monospace font).

2. **App-level integration test** (`libs/collabtext/tests/tst_scroll_stability.cpp`, new file). Two `Buffer`s, one widget, one `QTextDocument`. Scenario:
   - Populate with 100 lines.
   - Scroll the widget to line 50.
   - Build a remote edit that inserts "X\n" 10 times at offset 0 (10 lines above the viewport).
   - Call `applyEditsPreservingScroll(edits)`.
   - Assert: the byte offset at `QPoint(0,0)` after the call resolves to the same original content as it did before.

   Also: a scenario where the top anchor's line is deleted remotely —
   verify the restoration picks the next surviving line and doesn't crash.

3. **Manual smoke** in `app/main.cpp` — two panes, scroll pane A to
   mid-document, type on pane B above pane A's scroll position, watch
   pane A stay put while the text shifts around it. Existing gremlin
   stress test (which already runs many remote edits) will also exercise
   the new path.

## 9. File inventory

| File | Change |
|---|---|
| `libs/collabtext/src/ui/CollabPlainTextEdit.h` | Add `topVisibleByteOffset`, `bottomVisibleByteOffset`, `scrollByteOffsetToTop` declarations; add `viewportScrolled()` signal. |
| `libs/collabtext/src/ui/CollabPlainTextEdit.cpp` | Implement the three methods; emit `viewportScrolled()` from the existing `scrollContentsBy` override. |
| `app/main.cpp` | Add `applyEditsPreservingScroll`, `m_viewportTopAnchor`, `m_viewportBottomAnchor`; wire `viewportScrolled` signal to a slot that recomputes anchors; replace direct `applyEditsToQt` calls; populate `EphemeralState.viewport_top/bottom` when flushing presence. |
| `libs/collabtext/tests/tst_collab_plain_text_edit.cpp` | **New file.** Widget unit tests for the three new methods. |
| `libs/collabtext/tests/tst_scroll_stability.cpp` | **New file.** App-level integration test. |
| `libs/collabtext/CMakeLists.txt` | Register the two new test targets. |

No new CRDT API. No changes to `Buffer.h` / `Buffer.cpp`. No changes to
`Anchor.h`.

## 10. Success criteria

- All new unit and integration tests pass.
- Full `ctest` suite remains green (24/24).
- Manual smoke: typing in one pane while another pane is scrolled mid-document does not make the scrolled pane jump, except when the local cursor would otherwise scroll off, in which case it adjusts only as much as necessary.
- `EphemeralState.viewport_top` and `viewport_bottom` appear in the on-disk presence file and reflect the current viewport.
- No measurable performance regression in `tst_benchmark` beyond run-to-run noise.

## 11. Implementation notes (2026-04-13)

Implementation completed 2026-04-07. All 9 plan tasks committed, full
`ctest` suite green (26/26), working tree clean.

### Known limitations (resolved 2026-04-13)

1. **Scroll-restore drift — FIXED.** Replaced the iterative
   pixel-delta convergence loop with a deterministic one-shot
   computation using `QTextBlock::firstLineNumber()` +
   `QTextLayout::lineForTextPosition()`. The scrollbar value is now set
   directly to the exact visual line number — no font-metric
   assumptions, no iteration, no truncation drift. The integration test
   tolerance was tightened from `abs(drift) < 50` bytes to
   `abs(drift) < 20` bytes (within one visual line).

### Deferred from the original spec

- **Follow-mode reading side.** `EphemeralState.viewport_top` and
  `viewport_bottom` are now populated and serialized to `ephemeral.json`
  on every scroll and remote-edit cycle. The *writing* side is complete.
  The *reading* side (rendering another replica's viewport to drive your
  own scroll, or showing a minimap indicator) is a separate sub-project.

- **Horizontal scroll stability.** Not relevant while
  `QPlainTextEdit::LineWrapMode` is `WidgetWidth` (the default, which
  disables horizontal scrolling).
