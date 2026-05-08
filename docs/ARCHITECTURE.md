# CollabText Architecture

How a plain text document becomes a conflict-free, offline-first
collaborative artifact — and what remains to be built.

**Last updated:** 2026-04-05
**Engine status:** Complete and optimized
**Sync/Editor status:** Specified, not yet implemented

---

## 1. The Big Picture

```
USER EXPERIENCE
  "I open ProjectProposal.md, edit it, close my laptop. My desktop
   had the same file open. When I get home, both sets of changes are
   there. No conflicts. No lost work."

                          ┌─────────────────────────────────────┐
                          │  Virtual Filesystem (KIO / FUSE)    │
                          │  ~/CollabDocs/ProjectProposal.md    │
                          │  (read-only materialized view)      │
                          └──────────────┬──────────────────────┘
                                         │ materializes via
┌────────────────────────────────────────┼──────────────────────┐
│  Qt Editor Application                 │                      │
│  ┌──────────────────┐  ┌──────────────┼───────────────────┐  │
│  │ Multi-cursor     │  │ Chat panel   │  Participant list  │  │
│  │ text widget      │  │ (side stream)│  (presence)        │  │
│  └────────┬─────────┘  └──────┬───────┴───────────────────┘  │
│           │ Qt signals        │ Qt signals                    │
│           │ (presentation)    │ (presentation)                │
├───────────┼───────────────────┼───────────────────────────────┤
│  SyncManager (C++, no Qt dependency)                          │
│  ┌────────────────────────────────────────────────────────┐   │
│  │ ┌──────────────┐  ┌─────────────┐  ┌───────────────┐  │   │
│  │ │ File         │  │ SQLite      │  │ Direct        │  │   │
│  │ │ Transport    │  │ Operation   │  │ Channel       │  │   │
│  │ │ (the floor)  │  │ Store       │  │ Manager       │  │   │
│  │ └──────┬───────┘  └──────┬──────┘  └──────┬────────┘  │   │
│  │        │ always active   │ local cache     │ optional  │   │
│  │        │                 │ + index         │ low-lat   │   │
│  │  ┌─────┴─────────────────┴─────────────────┴───────┐   │   │
│  │  │            Operation Router                      │   │   │
│  │  │  Deduplicates by (replica_id, sequence)          │   │   │
│  │  │  Feeds operations to the CRDT engine             │   │   │
│  │  └─────────────────────┬────────────────────────────┘   │   │
│  └────────────────────────┼────────────────────────────────┘   │
├───────────────────────────┼────────────────────────────────────┤
│  CRDT Engine (C++20, zero external dependencies)               │
│  ┌────────────────────────┼────────────────────────────────┐   │
│  │  Buffer                │                                │   │
│  │  ├─ insert / delete / replace / undo / redo             │   │
│  │  ├─ apply_ops (remote operations, any order)            │   │
│  │  ├─ anchor_at / resolve_anchor (stable positions)       │   │
│  │  ├─ collect_garbage / compact (memory reclamation)      │   │
│  │  └─ text() → current document content                   │   │
│  └─────────────────────────────────────────────────────────┘   │
├────────────────────────────────────────────────────────────────┤
│  Transport (not our code)                                      │
│  Syncthing / Dropbox / NAS / USB stick / anything              │
└────────────────────────────────────────────────────────────────┘
```

---

## 2. What a "Document" Actually Is

A collabtext document is not a file. It is a **folder of operations**.

When you create "ProjectProposal.md" in collabtext, what exists on disk
is a sync folder:

```
~/Sync/collabtext-docs/ProjectProposal/
├── meta/document.json              # Name, encoding, creation date
├── identities/
│   ├── clinton-a7f3b2/profile.json # My identity
│   └── alice-04e1c9/profile.json   # Alice's identity
├── replicas/
│   ├── laptop-3a/                  # My laptop's replica
│   │   ├── ops/00..ff/             # Hash-bucketed operation files
│   │   ├── sequences.json          # Per-hash write counters
│   │   ├── presence.json           # Heartbeat + connection offers
│   │   └── ephemeral.json          # Cursor position, selection
│   ├── desktop-f1/                 # My desktop's replica
│   │   └── ...
│   └── alice-07/                   # Alice's replica
│       └── ...
├── snapshots/                      # Periodic full-state checkpoints
└── local/                          # NOT SYNCED
    └── laptop-3a/
        ├── collabtext.db           # SQLite operation cache + index
        ├── materialized.txt        # Current text (read cache)
        └── channel-state/          # Direct channel bookkeeping
```

Syncthing syncs the operation files, not the document text. Each replica
independently assembles the current text by applying all operations
through the CRDT engine. **Every replica that has seen the same set of
operations produces identical text.** This is the CRDT guarantee.

### When does it become a single file again?

Three ways:

1. **Virtual filesystem (KIO/FUSE).** Mount the collabtext document store
   and it appears as a directory of normal `.md` files. `cat`, `grep`,
   file managers, and preview panes Just Work. Read-only initially; write
   support (injecting edits from external tools) is a future extension.

2. **Export.** The editor can write a plain `.md` file on demand. This is
   a one-way snapshot — the recipient can read it, but edits to the
   exported file don't feed back into the CRDT.

3. **The editor itself.** When the editor is open, it displays the current
   document text assembled from all operations. The user sees and edits a
   normal text document. The CRDT machinery is invisible.

The analogy is Git: a `.git/` directory of objects is the repository;
`git checkout` gives you normal files. The collabtext sync folder is the
repository; the editor (or virtual filesystem) gives you the text file.

---

## 3. The Layers

### 3.1 CRDT Engine — COMPLETE

**Location:** `libs/collabtext/src/crdt/`
**Dependencies:** C++20 standard library only
**Status:** Implemented, tested, optimized (15-31x improvement over
initial version). `IdList` primitive added in β (schema version 3).

The engine is a pure algorithmic core. It takes operations in and
produces operations out. It does not know about files, networks, Qt, or
users. It maintains:

- A **fragment tree** (SumTree, B+ tree with monoid summaries) containing
  the document's text as ordered fragments with fractional-index locators.
- An **undo map** tracking undo/redo parity across all replicas.
- An **origin index** for O(log n) fragment lookup by Lamport timestamp.
- A **causal queue** for deferred application of out-of-order operations.

Key properties:
- Operations are commutative, associative, and idempotent.
- Any replica holding the same set of operations produces the same text.
- Sub-millisecond per-edit latency for documents up to 10K characters.
- Garbage collection reclaims tombstones while respecting undo stacks.

As of schema version 3, the engine provides a second primitive alongside `Buffer`:
**`IdList`** — an ordered-list CRDT over opaque `uint64` elements. It uses the same
`SumTree`/`Locator`/`Anchor`/`UndoMap` machinery as `Buffer` but operates on atomic
elements rather than UTF-16 code units. No text splitting, no multi-char fragments.
Applications that need a structural list (e.g. block ordering in a document) can
compose `IdList` + `Buffer` directly without any changes to the transport layer —
`StreamSync` already handles multiple independent CRDT streams with opaque payloads.

See `docs/CRDT_IDLIST_SPEC.md` for the full specification.

### 3.2 OpStream — Public Transport Boundary

**Specified in:** `docs/CRDT_TRANSPORT_SPEC.md`
**Status:** Specified

`OpStream` is collabtext's public transport-agnostic interface. Any class that
delivers CRDT operations between replicas implements this four-method contract:
`push`, `set_on_inbound`, `lowest_peer_acked_lamport`, `set_on_ack_update`. This
decouples CRDT logic from transport details. `StreamSync` (file-backed, Syncthing-compatible)
ships as the reference implementation. Consumers that need direct-channel transports
(TCP, WebRTC) provide their own `OpStream` implementations; see
`docs/specs/transport-elevation-roadmap.md` for that roadmap track.

### 3.3 SyncManager — NOT YET IMPLEMENTED

**Specified in:** `docs/CRDT_SYNC_SPEC.md`
**Dependencies:** Filesystem watcher, SQLite, optional socket library
**Qt dependency:** None required (filesystem watcher is the only
platform-specific piece; Qt's QFileSystemWatcher is one option, but
inotify/kqueue/ReadDirectoryChanges work too)

The SyncManager bridges the engine and the shared folder. Its
responsibilities:

**File transport (the floor):**
- Write local operations to `replicas/<id>/ops/<hash>/<seq>` files.
- Watch for new operation files from other replicas (filesystem watcher).
- Feed discovered operations to the engine via `Buffer::apply_ops()`.
- Always active. Never disabled. This is the reliability guarantee.

**SQLite operation store (performance layer):**
- Local-only database in `local/<id>/collabtext.db`. Never synced.
- Indexes operations by `(replica_id, sequence)` for instant lookup.
- Replaces directory scanning for catch-up queries ("what ops from
  replica B after sequence 47?").
- On startup: ingest any operation files not yet in the database.
- On file watcher event: ingest new files into SQLite, then feed to
  engine.
- On direct channel: write ops to SQLite immediately; flush to operation
  files in the background.
- Enables fast startup: load snapshot + replay only ops after snapshot
  from SQLite, instead of scanning the entire ops directory tree.

**Why SQLite?** The shared folder's hash-bucketed file layout is
optimized for Syncthing (many small files = efficient delta sync). But
reading hundreds of files per second for catch-up or startup is slow.
SQLite gives indexed reads with a single file. The two are complementary:
files for transport, SQLite for local performance.

**Presence and ephemeral state:**
- Write `presence.json` with heartbeat timestamp and connection offers.
- Write `ephemeral.json` with cursor position, selection, typing status.
- Read other replicas' presence/ephemeral files for UI display.
- Mark remote cursors stale after 5 seconds without heartbeat.

**Direct channel manager (optional accelerator):**
- Discover peers via `presence.json` connection offers.
- Negotiate WebSocket/TCP connections for sub-second operation delivery.
- Exchange vector clocks on handshake for efficient catch-up.
- Stream operations bidirectionally in real-time.
- Fall back to file transport on channel failure — transparent to engine.
- Operations always written to file floor in parallel (dual-write).

**Side streams:**
- Chat messages, comments, bookmarks — each is its own operation log in
  `replicas/<id>/streams/<name>/`.
- Same CRDT guarantees as the main document stream.
- Chat is the initial use case (group chat side panel in the editor).

**Serialization:**
- Encode/decode Operations, snapshots, and vector clocks for file storage
  and wire transmission.
- Not yet implemented — this is the first thing to build.

### 3.4 Virtual Filesystem — NOT YET IMPLEMENTED

**New component.** See `docs/specs/virtual-filesystem-design.md` (to be
written).

Exposes collabtext document stores as directories of plain text files.
Two implementations planned:

**KIO worker (KDE):** Registers a `collabtext://` protocol. KDE file
managers, Dolphin, Kate, and any KIO-aware application can browse and
read documents natively. Fastest path for KDE users.

**FUSE mount (universal):** Mounts a collabtext document store at an
arbitrary path. Any application on any desktop environment sees normal
files. Broader compatibility, slightly more setup.

Both implementations are thin: enumerate document folders, call
`Buffer::text()` to materialize content on read. Initially read-only.
Write support (creating operations from external file modifications) is a
future extension that would enable editing collabtext documents from any
text editor.

### 3.5 Qt Editor — NOT YET IMPLEMENTED

**Specified in:** `docs/superpowers/specs/2026-04-01-file-sync-and-remote-cursors-design.md`,
`docs/LOW_LATENCY_INPUT.md`, `docs/GPU_AND_VSYNC_OPTIONS.md`

The editor is pure presentation. It does not contain CRDT logic, sync
logic, or file I/O. It talks to the SyncManager, which talks to the
engine.

**Multi-cursor text widget:**
- Custom widget (QPlainTextEdit derivative or bespoke) that renders
  remote cursors and selections as colored overlays.
- Each remote replica's cursor position comes from `ephemeral.json`
  (resolved via `Buffer::resolve_anchor()`).
- Stale cursors (>5s without heartbeat) fade out.
- Low-latency input pipeline: `repaint()` path for synchronous
  keyboard-to-screen rendering (see `LOW_LATENCY_INPUT.md`).

**Chat side panel:**
- Consumes the "chat" side stream from SyncManager.
- Displays messages with identity (name, color, avatar) from
  `identities/<id>/profile.json`.
- Message input sends operations to the chat side stream.

**Participant list:**
- Shows active replicas from presence heartbeats.
- Displays identity info (name, avatar, status, cursor color).
- Indicates connection quality (file-sync-only vs. direct channel).

**Identity management:**
- First-launch: create identity (name, color, avatar).
- Stored in `~/.config/collabtext/identity.json`.
- Projected into each document's `identities/` folder on join.

---

## 4. Build Order

The layers have clear dependencies. Build bottom-up:

### Phase 1: Serialization

The engine exists but operations are in-memory C++ structs. Before
SyncManager can write them to files or send them over channels, we need
`encode()` / `decode()` for:

- `EditOperation` (the main operation type)
- `UndoOperation`
- `Global` (vector clock)
- Snapshots (full document state for fast bootstrap)

Binary format specified in `CRDT_SYNC_SPEC.md` section 4. This is
straightforward serialization work — no design decisions remaining.

### Phase 2: SyncManager — File Transport

The minimum viable sync loop:
1. On local edit: serialize operation, write to ops file.
2. On file watcher event: read new ops file, deserialize, feed to engine.
3. Maintain `sequences.json` counters.
4. Write `presence.json` heartbeats.
5. Write `ephemeral.json` with cursor state.

This gives us working multi-device collaboration via Syncthing. No
SQLite, no direct channels — just the file floor. The simplest thing
that could possibly work.

### Phase 3: SQLite Operation Store

Add `local/<id>/collabtext.db` as a local-only indexed cache:
- Ingest ops files on startup and incrementally.
- Serve catch-up queries by `(replica_id, sequence)`.
- Cache snapshot state for fast startup.
- Replace directory scanning in the sync loop.

This makes startup fast (load snapshot from SQLite, not from scanning
hundreds of files) and prepares the ground for direct channels.

### Phase 4: Minimal Qt Editor

A working text editor with:
- Multi-cursor text widget (local editing + remote cursor display).
- Integration with SyncManager for operation flow.
- Participant list showing presence.
- Identity setup on first launch.

No chat panel, no direct channels, no virtual filesystem yet. Just
editing and seeing remote cursors.

### Phase 5: Direct Channels

WebSocket (or TCP) transport for sub-second delivery:
- Presence-based peer discovery.
- Vector clock handshake for catch-up.
- Streaming mode for real-time operation delivery.
- Graceful degradation to file floor on failure.
- SQLite as the operation buffer for channel catch-up.

This is where the editor starts feeling "real-time" when peers are on
the same network.

### Phase 6: Chat Side Panel

Side stream support in SyncManager + chat UI in the editor:
- Chat operations serialized and synced like document operations.
- Messages displayed with identity info.
- Same offline-first guarantees as the main document.

### Phase 7: Virtual Filesystem

KIO worker and/or FUSE mount for document materialization:
- Read-only initially.
- Enumerate document stores, materialize on read.
- Write support as a future extension.

---

## 5. What the Engine Provides (and What It Doesn't)

| Concern | Engine's job | SyncManager's job | Editor's job |
|---------|-------------|-------------------|-------------|
| Text merging | Convergent CRDT ops | -- | -- |
| Operation delivery | -- | File I/O, channels | -- |
| Operation storage | In-memory tree | SQLite + files | -- |
| Serialization | -- | Encode/decode | -- |
| Cursor tracking | Anchor primitives | Ephemeral state sync | Rendering |
| Undo/redo | Parity-based UndoMap | Broadcast undo ops | UI binding |
| Garbage collection | Tombstone removal | Watermark coordination | -- |
| Presence | -- | Heartbeat files | Display |
| Identity | -- | Profile projection | UI + setup |
| Chat | -- | Side stream sync | Chat panel UI |
| File materialization | `text()` | -- | Export / VFS |

The engine is deliberately minimal and dependency-free. Everything above
it can be swapped, reimplemented, or used from non-Qt contexts (a CLI
tool, a web interface via WASM, a mobile app) without touching the core.

---

## 6. Key Design Decisions and Their Rationale

**Why file-sync as the floor?**
Because it's always available. Syncthing works through NAT, across
continents, on intermittent connections, with zero configuration. No
server to run, no account to create, no port to open. Every other
transport is an optimization on top.

**Why SQLite locally?**
Because the file layout (many small files, hash-bucketed) is optimized
for Syncthing's delta sync, not for local reads. SQLite gives indexed
access to operations without changing the sync-friendly file layout.
It's a local cache, not a replacement for the files.

**Why separate engine from SyncManager?**
Because the engine's correctness properties (convergence, commutativity,
idempotency) must not depend on any transport assumption. If the engine
required SQLite, or files, or a network, those dependencies would
compromise the mathematical guarantee. The engine takes operations in
and produces operations out. Period.

**Why plain text only?**
Because the offline-first file-sync niche is primarily about prose:
notes, manuscripts, documentation, research. Rich text (bold, links,
annotations) is a future extension, not a prerequisite. Getting plain
text right — with undo, GC, anchors, and convergence — is the
foundation everything else builds on.

**Why C++ with no framework dependency?**
Because the engine is infrastructure, not application code. A Qt editor,
a GTK editor, a terminal tool, a WASM module, or an embedded system
should all be able to use the same engine. Zero dependencies means zero
constraints on where it runs.
