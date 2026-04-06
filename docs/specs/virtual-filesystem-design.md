# Virtual Filesystem — Design Spec

**Date:** 2026-04-05
**Status:** Design
**Dependencies:** Serialization (engine ops encode/decode), SyncManager
(file transport at minimum)

---

## 1. Motivation

A collabtext document is a folder of CRDT operations, not a plain text
file. This is correct for sync but creates friction: users can't `cat`,
`grep`, or browse their documents with standard tools. File managers show
a tree of `ops/` directories and `sequences.json` files instead of
"ProjectProposal.md".

A virtual filesystem solves this by presenting collabtext document stores
as directories of normal text files. The user sees `~/CollabDocs/` with
readable `.md` files. Under the hood, each file is materialized on-demand
by calling `Buffer::text()` on the corresponding CRDT document.

---

## 2. User Experience

### What the user sees

```
~/CollabDocs/
├── ProjectProposal.md      (23 KB, last modified 2 minutes ago)
├── meeting-notes.md         (4 KB, last modified yesterday)
└── research/
    ├── literature-review.md (67 KB, last modified 3 days ago)
    └── methodology.md       (12 KB, last modified 1 hour ago)
```

These are normal files as far as any application is concerned:
- `cat ~/CollabDocs/ProjectProposal.md` prints the document.
- `grep -r "deadline" ~/CollabDocs/` searches across all documents.
- Dolphin/Nautilus/Finder shows previews and file sizes.
- Kate/VS Code/vim can open them read-only.
- Markdown previewers render them.

### What actually exists

```
~/Sync/collabtext-store/
├── ProjectProposal/
│   ├── meta/document.json
│   ├── replicas/...
│   └── local/...
├── meeting-notes/
│   ├── meta/document.json
│   └── ...
└── research/
    ├── literature-review/
    │   └── ...
    └── methodology/
        └── ...
```

The virtual filesystem maps each document folder to a single file,
using the `name` field from `meta/document.json` as the filename.
Subdirectories in the store map to subdirectories in the virtual view.

---

## 3. Implementation: Two Paths

### 3.1 KIO Worker (KDE)

A KIO worker plugin registers the `collabtext://` protocol. KDE
applications (Dolphin, Kate, Konsole, any KIO-aware app) can access
documents via URLs like `collabtext:///ProjectProposal.md`.

**Advantages:**
- Native KDE integration (sidebar entries, file dialogs, previews).
- No mount point needed — accessed via URL protocol.
- Runs in-process with the file manager (fast).
- Can provide metadata (document participants, last edit time) via
  KIO's UDSEntry fields.

**Disadvantages:**
- KDE-only. Doesn't help GNOME, XFCE, or terminal-only users.
- Requires KDE Frameworks as a build dependency.

**Key KIO methods to implement:**

```
listDir(url)      → Enumerate document folders, emit UDSEntry per doc
stat(url)         → Return file size (visible_length), mtime, permissions
get(url)          → Materialize document text via Buffer::text()
mimetype(url)     → Return text/plain or text/markdown based on extension
```

The worker loads the CRDT engine, reads operation files (or SQLite cache)
for the requested document, applies all operations, and returns the
materialized text. For frequently accessed documents, the materialized
text can be cached in `local/<id>/materialized.txt` with a version
vector to detect staleness.

### 3.2 FUSE Mount (Universal)

A FUSE filesystem mounts the collabtext store at an arbitrary path
(e.g., `~/CollabDocs/`). Every application on the system sees normal
files.

**Advantages:**
- Desktop-environment-agnostic. Works everywhere FUSE works (Linux,
  macOS via macFUSE, FreeBSD).
- Any application can read the files — no special protocol support.
- Mount on login, forget about it.

**Disadvantages:**
- Requires FUSE installation (usually available, not always installed).
- Mount/unmount lifecycle to manage (systemd unit or login script).
- Slightly higher overhead than KIO (kernel round-trip per read).

**Key FUSE operations to implement:**

```
getattr(path)     → File size, mtime, permissions (read-only: 0444)
readdir(path)     → List document folders as files
open(path)        → Load engine state for document
read(path, buf)   → Copy from materialized text into buffer
release(path)     → Unload engine state
```

**Caching strategy:** Materialize each document on first `open()` and
cache until `release()`. On subsequent reads, serve from cache. Detect
staleness by watching the sync folder for new operation files (inotify)
and re-materializing when changes arrive. This means the file content
updates live as remote edits arrive — a `tail -f` would see the document
grow.

---

## 4. Shared Design

Both implementations share the same core logic. The virtual filesystem
library provides:

```cpp
namespace CollabText::VFS {

struct DocumentInfo {
    std::string name;        // From meta/document.json
    std::string path;        // Relative path in store (for subdirs)
    size_t size;             // Buffer::visible_length()
    time_t mtime;            // Most recent operation timestamp
    size_t participants;     // Number of replicas with recent presence
};

class DocumentStore {
public:
    explicit DocumentStore(const std::filesystem::path& store_root);

    /// List all documents in the store.
    std::vector<DocumentInfo> list() const;

    /// Materialize a document's text.
    std::string materialize(const std::string& name) const;

    /// Check if any document has changed since last call.
    bool has_changes() const;
};

}  // namespace CollabText::VFS
```

This is a plain C++ class with no Qt or FUSE dependency. The KIO worker
and FUSE mount are thin adapters over it.

**Dependencies:** The DocumentStore needs to load operations from files
(or SQLite). This requires the serialization layer. Without
serialization, the virtual filesystem cannot function.

---

## 5. Read-Only vs. Read-Write

### Initial implementation: Read-only

Files in the virtual filesystem are read-only (permissions 0444).
Attempting to write produces EACCES. This is sufficient for:
- Browsing and previewing documents
- Searching across documents (grep, ripgrep, etc.)
- Opening in editors for reading
- Rendering markdown previews
- Copying text to other applications

### Future extension: Read-write

Write support would allow any text editor to modify collabtext
documents. The virtual filesystem would:
1. Detect the write (FUSE `write()` or KIO `put()`).
2. Diff the new content against the current materialized text.
3. Convert the diff into CRDT edit operations.
4. Feed the operations to the engine and write them to the sync folder.

This is significantly more complex than read-only:
- Diff-to-operations conversion must handle insertions, deletions, and
  replacements correctly.
- Concurrent modifications (another replica editing while the external
  editor writes) must be handled.
- The external editor's save model (overwrite entire file) must be
  reconciled with the CRDT's incremental model.

The read-write extension is not planned for the initial implementation
but the architecture supports it.

---

## 6. Lifecycle

### FUSE

```
# Mount on login (systemd user service or shell profile)
collabtext-mount ~/Sync/collabtext-store ~/CollabDocs

# Documents visible at ~/CollabDocs/
cat ~/CollabDocs/ProjectProposal.md

# Unmount
fusermount -u ~/CollabDocs
```

### KIO

```
# No mount needed — use the protocol URL
kioclient cat collabtext:///ProjectProposal.md

# In Dolphin: navigate to collabtext:///
# In Kate: open collabtext:///ProjectProposal.md
```

---

## 7. Build Order

1. **Serialization** (prerequisite) — Operation encode/decode.
2. **DocumentStore** — Core materialization library (C++, no deps).
3. **FUSE mount** — Adapter using libfuse3.
4. **KIO worker** — Adapter using KDE Frameworks.

The FUSE mount is higher priority (universal) unless the editor is
KDE-only, in which case KIO may come first for tighter integration.
