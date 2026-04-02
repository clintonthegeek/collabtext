# File-Based Sync and Remote Cursors

## Goal

Replace the current in-process signal wiring between the two test harness
panes with file-based CRDT sync via SyncManager, and add remote cursor +
selection rendering. Each pane becomes a fully independent CRDT replica
communicating only through a shared folder on disk — identical to two
separate processes (or machines synced via Syncthing).

## Architecture

```
MainWindow
+-- PeerPane A
|   +-- QPlainTextEdit
|   +-- CollabDocument (YDoc A, QTextDocument A)
|   +-- SyncManager A (replica "peer-a", shared folder /tmp/collabtext-test/)
+-- PeerPane B
    +-- QPlainTextEdit
    +-- CollabDocument (YDoc B, QTextDocument B)
    +-- SyncManager B (replica "peer-b", shared folder /tmp/collabtext-test/)
```

No signal connections between A and B. Communication is exclusively
through the shared folder. Peer A writes CRDT updates and ephemeral
state to `replicas/peer-a/`, Peer B reads them, and vice versa.

### Shared Folder Layout

```
/tmp/collabtext-test/
  .stignore
  meta/
  replicas/
    peer-a/
      ops/
        0.bin, 1.bin, ...     # CRDT update blobs
      seq                     # Monotonic write counter
      ephemeral.json          # Cursor position + metadata
    peer-b/
      ops/
      seq
      ephemeral.json
  local/                      # Not synced (.stignore'd)
    peer-a/
      cursors/
        peer-b               # Read-up-to cursor for peer-b's ops
    peer-b/
      cursors/
        peer-a
```

## Component Changes

### SyncManager

Two new capabilities added to the existing class:

**Writing ephemeral state.** New method `setEphemeralState(const QJsonObject &)`.
Called by PeerPane to set the state that gets written on each sync cycle.
SyncManager writes it to `replicas/<id>/ephemeral.json` atomically via
QSaveFile. The JSON includes a `last_heartbeat` ISO timestamp added by
SyncManager automatically.

**Reading remote ephemeral state.** New method `readRemoteEphemerals()`
called during the sync cycle. Reads all other replicas' `ephemeral.json`
files, checks the `last_heartbeat` for staleness (> 5 seconds = stale,
cursor hidden), and emits a new signal for each live peer:

```cpp
signal: void remoteEphemeralChanged(const QString &replicaId,
                                     const QJsonObject &state);
```

**Updated sync cycle:**

```
syncCycle():
  1. flushLocalUpdates()       // existing — write CRDT ops to disk
  2. readRemoteUpdates()       // existing — read peers' CRDT ops
  3. writeEphemeral()          // new — write cursor + heartbeat
  4. readRemoteEphemerals()    // new — read peers' cursors, emit signals
```

No changes to the existing CRDT sync path.

### CollabDocument

Minimal additions. Two convenience methods that forward to YrsDocument:

- `QByteArray stickyIndexAt(int position, int8_t assoc)` — creates a
  YStickyIndex at the given QTextDocument offset.
- `int resolveSticky(const QByteArray &encoded)` — resolves a serialized
  YStickyIndex back to a QTextDocument offset. Returns -1 on failure.

### PeerPane (test harness)

Becomes the orchestrator. Owns both a CollabDocument and a SyncManager.

**Cursor capture.** Connects to `QPlainTextEdit::cursorPositionChanged`.
On each change, creates two YStickyIndex blobs (cursor head and selection
anchor) and stores them. On each sync cycle, SyncManager writes them to
the ephemeral file.

**Remote cursor rendering.** Connects to
`SyncManager::remoteEphemeralChanged`. For each remote peer:

1. Decodes the sticky index blobs from the ephemeral JSON.
2. Resolves them to QTextDocument offsets via `CollabDocument::resolveSticky()`.
3. Builds `QTextEdit::ExtraSelection` entries:
   - If anchor == head (no selection): a thin colored cursor line. Done
     by selecting the single character at the cursor position and giving
     it a left-border via QTextCharFormat.
   - If anchor != head (selection): a translucent colored background
     between anchor and head.
4. Calls `setExtraSelections()` on the QPlainTextEdit.

**Cursor label painting.** Subclasses or overrides `paintEvent` on the
QPlainTextEdit (via an event filter or thin subclass). After the base
paint, draws a small colored rectangle with the peer's name above each
remote cursor position, using `cursorRect(QTextCursor)` to find screen
coordinates.

### Ephemeral JSON Format

```json
{
  "name": "Peer A",
  "color": "#3b82f6",
  "cursor_head": "<base64 sticky index>",
  "cursor_anchor": "<base64 sticky index>",
  "activity": "typing",
  "last_heartbeat": "2026-04-01T21:30:00.000Z"
}
```

- `cursor_head` and `cursor_anchor` are base64-encoded serialized
  YStickyIndex blobs from `ysticky_index_encode()`.
- `last_heartbeat` is written by SyncManager automatically.
- `activity` is set by PeerPane based on recent keystrokes: "typing" if
  keys within last 2 seconds, "idle" otherwise.
- `name` and `color` are set once at construction. In the test harness,
  hardcoded to "Peer A" / blue and "Peer B" / green.

### MainWindow

Simplified. Creates the shared folder (a QTemporaryDir or fixed path in
/tmp), creates two PeerPanes with different replica IDs pointing at the
same folder, and starts both SyncManagers. No signal wiring between
panes. Status bar shows the shared folder path.

## Sync Timing

- Sync cycle interval: 500ms (tighter than the spec's 1 second, for a
  snappier demo — the floor is designed to work at any interval).
- Cursor updates arrive at the sync cycle rate. With 500ms cycles,
  remote cursors update roughly twice per second. Adequate for
  demonstrating the concept.

## Removed

The direct in-process signal wiring between Peer A and Peer B
(`connect(m_peerA->collabDoc(), &CollabDocument::updateReady, ...)`)
is removed entirely. All communication goes through the file system.

## Not In Scope

- Direct channel transport (TCP/WebSocket) — future work.
- Identity system (profiles, avatars) — future work.
- Chat/comments streams — future work.
- Integration with Markoff — future work.
