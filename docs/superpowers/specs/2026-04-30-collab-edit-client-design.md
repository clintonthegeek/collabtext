# Remote Collab-Edit Client — Design

**Date:** 2026-04-30
**Status:** Approved, ready for implementation plan
**Related:** `docs/superpowers/specs/2026-04-01-file-sync-and-remote-cursors-design.md`,
`docs/superpowers/specs/2026-04-06-ephemeral-identity-system-design.md`

---

## 1. Problem

The existing `collabtext-testapp` puts two simulated editor panes in one
process against a shared `QTemporaryDir`. That validates round-trip
correctness in-process but tells us nothing about how the engine behaves
across two real machines, two real OS processes, and a real
filesystem-backed transport (Syncthing).

We need a minimal standalone client we can run on each of two machines,
pointed at a Syncthing-mirrored folder, to exercise the CRDT engine
end-to-end as a remote system. The client must be small enough to be
honest about its purpose (a test rig) but architected so it can grow
into the real MVP editor — i.e. the file/sidecar/enrollment model we
ship here is the model we keep.

## 2. Goals

1. **Single-document-per-window editor.** Open one file, edit it, save
   it. File is passed via `File → Open` / `File → New` / CLI arg.
2. **Per-document opt-in collab.** Newly opened files start in
   *Plain* mode (a normal text editor with no CRDT). The user clicks
   `Document → Enable Collab` to enroll the document, which creates a
   persistent sidecar and switches the editor into *Collab* mode.
3. **Sidecar is canonical, file is a snapshot.** Once enrolled, the
   CRDT state in `<file>.collab/` is the source of truth. The original
   file is updated only on `Ctrl+S` (or the equivalent menu action) as
   a snapshot of the current visible text.
4. **Offline-CRDT semantics throughout.** A peer alone in a sidecar
   keeps editing through the CRDT. When Syncthing eventually delivers
   a remote peer's ops, the merge is automatic. There is no "session
   active" gate; the CRDT just runs.
5. **Concurrent enrollment is safe.** Two peers can independently click
   Enable Collab on the same file before Syncthing has synced the
   sidecar. A deterministic seed mechanism makes the resulting merge
   convergent (no double-inserted content).
6. **Reuse existing infrastructure.** `Buffer`, `FileSync`,
   `IdentityStore`, `IdentitySetupDialog`, `PresenceManager`,
   `ParticipantListWidget`, `CollabPlainTextEdit`,
   `MultiCursorController` are reused as-is. New code is a thin
   orchestration layer above them.

## 3. Non-goals (explicit deferrals)

- **Chat, comments, bookmarks** in v1. The transport
  (`StreamSync`) and widgets exist; we leave them un-wired so this v1
  proves text+cursor sync without dragging anchor/persistence
  questions onto the critical path.
- **Save quorum / coordinated save.** `Ctrl+S` writes the snapshot
  unconditionally. If two peers save near-simultaneously, Syncthing
  conflict-renames; the user resolves manually. Documented limitation.
- **Disenrollment ("turn collab off").** Once a document has a
  sidecar, it stays a collab document.
- **Reconciliation of out-of-band edits to the snapshot file.** If a
  user edits `notes.md` in another editor while the collab sidecar
  exists, those edits are ignored on next save (overwritten by the
  CRDT snapshot). A future MVP can add a diff-and-replay path.
- **Multi-document tabs / workspace mode.** One window, one document.
- **Cross-platform polish.** Linux only is tested. Code stays
  portable; we just don't QA Windows/macOS for v1.

## 4. Architecture overview

The client is a single-window Qt6 application with a per-window
`Document` orchestrator. A `Document` is in exactly one of two states:

```
   ┌──────────────┐    Enable Collab     ┌──────────────┐
   │  Plain mode  │ ───────────────────▶ │ Collab mode  │
   │ (file only)  │                      │ (sidecar +   │
   │              │                      │   CRDT)      │
   └──────────────┘                      └──────────────┘
       ▲                                        │
       │                                        │ (no transition
       │                                        │  back in v1)
       └────────────────────────────────────────┘
```

- **Plain mode.** The window opens `notes.md` directly into a plain
  `QPlainTextEdit`. No `Buffer`, no `FileSync`, no presence. Saving
  writes the file in the obvious way. This is the editor a user gets
  for any not-yet-enrolled file.
- **Collab mode.** The window holds a `Buffer`, `FileSync`,
  `PresenceManager`, and a `CollabPlainTextEdit` (the same widget the
  test app uses). Edits flow `QTextDocument → Buffer → FileSync`;
  remote ops flow `FileSync → Buffer → edits_since() → QTextCursor`.
  `Ctrl+S` writes `Buffer::text()` back to the original file path.

**Detection rule on open:** if `<file>.collab/` exists and contains a
valid `manifest.json` (see §5), the document opens in Collab mode and
joins the existing CRDT. Otherwise, Plain mode.

There is no concept of "session active." Presence heartbeats determine
who is *currently live* (for the participant list and remote cursor
overlays) but never gate CRDT operation.

## 5. Sidecar layout

For a document at `/sync/notes.md`, the sidecar is `/sync/notes.md.collab/`,
created as a sibling. Inside:

```
notes.md.collab/
├── manifest.json          ← collab-doc metadata, written once at enrollment
├── seed.txt               ← original file content at enrollment time
├── replicas/<replica>/
│   ├── ops/<bucket>       ← FileSync's existing op log (unchanged)
│   ├── sequences.json     ← FileSync's existing seq counters
│   ├── presence.json      ← PresenceManager's existing presence
│   └── ephemeral.json     ← PresenceManager's existing cursor state
├── meta/                  ← FileSync existing
├── snapshots/             ← FileSync existing (unused in v1)
├── local/                 ← FileSync existing, syncthing-ignored
├── .stignore              ← FileSync existing
```

`manifest.json`:

```json
{
  "schema_version": 1,
  "doc_id": "01HXXXXXXXXXXXXXXXXXXXXXX",
  "enrolled_at": "2026-04-30T14:22:01Z",
  "original_filename": "notes.md",
  "seed_sha256": "<hex digest of seed.txt content>"
}
```

- `doc_id` is a fresh ULID/UUID generated at enrollment. Used as a
  sanity check when joining: refuse to join a sidecar whose `doc_id`
  doesn't match what we expect, in case sidecars get moved around.
- `seed_sha256` lets a joining peer detect a bogus `seed.txt`
  (Syncthing conflict-renamed file, edited by hand, etc.) and refuse
  to load.
- `original_filename` is informational; the client uses the actual
  parent-directory path it was opened from.

The `replicas/` subtree is exactly what `FileSync` already creates.
We're adding `manifest.json` and `seed.txt` as siblings.

## 6. Deterministic seeding

The challenge: peer A and peer B may both click Enable Collab on the
same file before Syncthing has synced the sidecar. We need their
buffers to converge to the same state after Syncthing merges
everything.

**Mechanism:** the seed is a plain text file (`seed.txt`), not an
operation. Both peers, on opening any sidecar, replay the seed
identically into their own `Buffer` using a synthetic seed-replica.

Concretely:

1. The client owns a function `op_for_seed(content) → EditOperation`.
   - Build an empty `Buffer` with `replica_id = 0` (reserved seed id).
   - Call `apply_local_edit({{0, 0}}, {content})` on it, which
     produces a deterministic `EditOperation` (timestamp `Lamport(0,1)`,
     fragments with deterministic locators because they originate from
     a fresh empty buffer with a fixed replica id).
   - Return that operation.
2. When opening a sidecar, the real `Buffer` (`replica_id = my_replica`)
   first calls `apply_ops({op_for_seed(read_seed_txt())})`, then enters
   normal sync. The seed op is never pushed to `FileSync`; each peer
   reconstructs it locally from `seed.txt`.
3. Because both peers compute the same seed op (deterministic replica
   id, deterministic Lamport, deterministic locators), they apply
   identical state. Their subsequent ops live under
   `replicas/<peer>/ops/...` and merge normally.

**Concurrent enrollment race resolution:**

| `seed.txt` files match? | Outcome |
|--|--|
| Identical `seed.txt` bytes (peers had the same file content) | Syncthing sees one `seed.txt` file. Both peers wrote a `manifest.json` to the same path with different `doc_id`s; Syncthing conflict-renames one (e.g. `manifest.sync-conflict-*.json`). **v1 behavior:** the client detects the conflict file at open and surfaces a clear error: *"Manifest conflict: two peers enrolled the same document. Delete the conflicting sidecar on one machine."* The user picks a winner manually. The smaller-`doc_id` automatic tiebreaker is a future enhancement; the comparison helper `doc_id_less` is implemented and unit-tested so the runtime logic can be added without a library change. |
| Different `seed.txt` bytes (peer enrolled stale content) | Syncthing conflict-renames `seed.txt`. The client detects extra `seed.sync-conflict-*.txt` files, refuses to enter Collab mode, surfaces: *"Enrollment conflict: peers enrolled this document with different starting content. Delete the sidecar on the wrong machine and re-sync."* |

The `doc_id` tiebreaker requires a new tiny module (~30 LOC)
`SidecarManifest` (read/write/compare). Manifests are written
atomically; any peer that sees two competing manifests (or a manifest
with a smaller doc_id than its own) demotes to the smaller.

> **Practical note:** in the steady state, only the very first
> enrollment writes manifests. After that, `manifest.json` is
> read-only for all replicas. The race window is the seconds between
> two simultaneous Enable Collab clicks before Syncthing converges.

## 7. App shape & UI

### 7.1 Window layout

```
┌────────────────────────────────────────────────────────────┐
│ File   Document   Help                                     │
├──────────────────────────────────────────┬─────────────────┤
│                                          │ Participants    │
│                                          │ ┌─────────────┐ │
│                                          │ │ ● Alice     │ │
│      [text editor]                       │ │ ● Bob       │ │
│                                          │ └─────────────┘ │
│                                          │                 │
│                                          │  (Plain mode:   │
│                                          │   panel hidden) │
├──────────────────────────────────────────┴─────────────────┤
│ <status: file path | mode | replica> | [Enable Collab]     │
└────────────────────────────────────────────────────────────┘
```

- The participant list is hidden in Plain mode and shown in Collab
  mode.
- The `Enable Collab` button on the status bar is the same action as
  `Document → Enable Collab`. It's hidden once the doc is in Collab
  mode.
- Title bar shows: `<filename> [<mode>] — collabedit` (e.g.
  `notes.md [collab] — collabedit`). Modified marker (`*`) is shown
  when the in-memory text differs from what's on disk in the snapshot
  file.

### 7.2 Menus

**File menu:**

| Item | Plain mode | Collab mode |
|------|------------|-------------|
| New | Open empty unsaved Plain doc | Same |
| Open... | `QFileDialog::getOpenFileName`; opens in mode determined by sidecar presence | Same |
| Save | Write QPlainTextEdit text to file | Write `Buffer::text()` to file |
| Save As... | Pick a new path; treats as Plain mode (no sidecar follows) | Disabled in v1 (would mean disenrollment) |
| Close | Close window | Depart presence, write final snapshot, close |
| Quit | Close all windows | Depart all presences, close |

**Document menu:**

| Item | Plain mode | Collab mode |
|------|------------|-------------|
| Enable Collab | Visible. Triggers enrollment flow. | Hidden / disabled. |

**Help menu:**

| Item | Behavior |
|------|----------|
| About | Shows app version, replica name, identity display name |

### 7.3 Identity setup

On first launch, if `IdentityStore::load()` returns nullopt, present
`IdentitySetupDialog` (existing widget). Persisted to
`~/.config/collabtext/` per `IdentityStore`'s default. Subsequent
launches reuse it.

**Replica name:** computed once on first launch as
`<identity-id-prefix-8>-<hostname>`, persisted alongside identity.
Used as `FileSync` and `PresenceManager` replica name. Example:
`a1b2c3d4-thinkpad`. Two installs on the same host with the same
identity get the same replica name (correct: the same human, same
device). Two devices for the same human get distinct names.

### 7.4 Enable Collab flow (UX)

1. User opens `notes.md` (Plain mode).
2. User edits. (May or may not save; doesn't matter for enrollment.)
3. User clicks `Enable Collab`.
4. Confirmation dialog: *"Enable collaborative editing on
   `notes.md`? This creates `notes.md.collab/` next to the file. The
   sidecar must be inside a Syncthing-shared folder for remote peers
   to join."*
5. On confirm:
   - Save the current QPlainTextEdit text to `notes.md` first (so
     the sidecar's `seed.txt` is a known on-disk state).
   - Create `notes.md.collab/` with `manifest.json` and `seed.txt`.
   - Construct `Buffer`, apply the seed op, hook up `FileSync` and
     `PresenceManager`.
   - Replace the QPlainTextEdit with `CollabPlainTextEdit`, populate
     it from `Buffer::text()`, wire up signals (per the existing
     test app's pattern in `EditorPane`).
6. Window transitions to Collab mode.

### 7.5 Open existing collab doc flow

1. User opens `notes.md`.
2. Client detects `notes.md.collab/manifest.json`.
3. Validate: schema_version == 1, seed.txt sha256 matches manifest.
   - On mismatch: show error, fall back to Plain mode (read-only
     warning).
4. Construct `Buffer` (with this client's replica id), apply seed op,
   apply any existing remote ops in `replicas/*/ops/...`.
5. Window opens directly in Collab mode. The original file content on
   disk is not consulted (the sidecar is canonical).

## 8. Lifecycle flows

### 8.1 Sync timer

A single `QTimer` at 100ms cadence (matching the existing test app)
drives the per-cycle work in Collab mode:

1. `FileSync::poll()` → applies remote ops, surgically updates
   `QTextDocument` via `edits_since()`.
2. Write own `presence.json` (heartbeat).
3. Write own `ephemeral.json` (cursor + viewport anchors).
4. Read remote ephemerals; render remote cursors via
   `MultiCursorController`.
5. Update participant list.

In Plain mode the timer is stopped.

### 8.2 Save (Ctrl+S)

**Plain mode:** write `QPlainTextEdit::toPlainText()` to the file path.

**Collab mode:** write `Buffer::text()` to the file path, atomically
(write to `notes.md.tmp`, rename). The CRDT state in the sidecar is
unaffected.

If two peers save near-simultaneously, Syncthing conflict-renames one
copy. The CRDT is unaffected; the file is just a snapshot. Documented
limitation.

### 8.3 Close / Quit

In Collab mode: call `PresenceManager::depart()` to mark our
presence inactive. Run one final `FileSync::poll()` to flush pending
ops. Write a final snapshot to the file (best-effort).

### 8.4 Peer joins / leaves (steady state)

Already handled by existing `FileSync` + `PresenceManager`. New peer
appears in `replicas/<their-name>/`, their ops show up in our next
poll, their presence appears in the participant list. When their
heartbeat ages out (>30s, per `PresenceManager::is_live`), they fade
from the participant list. CRDT state is unchanged on their leave.

## 9. Error cases

| Case | Behavior |
|------|----------|
| Open file that doesn't exist | Plain mode, empty buffer, save creates it |
| Open file in dir we can't write to | Plain mode, save shows error |
| Open file whose sidecar has invalid `manifest.json` (parse fail or schema mismatch) | Show error: *"Sidecar exists but couldn't be loaded: <reason>. Open in Plain mode (read-only)?"* with read-only fallback. |
| `seed.txt` SHA mismatch with manifest | Same fallback |
| `seed.sync-conflict-*.txt` file present | Show enrollment-conflict error (§6); read-only |
| Manifest sync-conflict file visible at open | Refuse to open in Collab mode; surface a clear error (§6, "Identical seed bytes" row). Automatic doc_id-tiebreaker rebase is a future enhancement. |
| FileSync I/O failure mid-poll | Log warning, continue; next poll retries. Documented best-effort. |
| Window closed during Enable Collab | Sidecar may be partially created. We make `manifest.json` the LAST file written; absence of `manifest.json` means "not enrolled," and a partial sidecar is recovered by re-running Enable Collab (idempotent: skip steps whose outputs already exist). |

## 10. Components / file inventory

**New files:**

| File | Purpose |
|------|---------|
| `app/collabedit/CMakeLists.txt` | Build target `collabedit` linking against `Qt6::Widgets` and `CollabText::CollabText`. |
| `app/collabedit/main.cpp` | `QApplication` setup, identity bootstrap, command-line file arg, instantiates `MainWindow`. |
| `app/collabedit/MainWindow.h/.cpp` | Top-level window, menu bar, status bar, owns the `Document`. |
| `app/collabedit/Document.h/.cpp` | State machine (Plain/Collab), holds either `QPlainTextEdit` or the Collab pane. Methods: `open(path)`, `enableCollab()`, `save()`, `close()`. |
| `app/collabedit/CollabPane.h/.cpp` | The Collab-mode editor pane: owns `Buffer`, `FileSync`, `PresenceManager`, the sync timer, and a `CollabPlainTextEdit`. Distilled from `EditorPane` in the existing test app — drop gremlin/follow-mode/comments/chat. |
| `libs/collabtext/src/crdt/SidecarManifest.h` | Read/write/validate `manifest.json`. Compute `doc_id` comparison for the race-tiebreaker. |
| `libs/collabtext/src/crdt/SidecarManifest.cpp` | Implementation. |
| `libs/collabtext/src/crdt/SeedOp.h` and `SeedOp.cpp` | `op_for_seed(const std::string& content) → EditOperation`. Self-contained. |
| `libs/collabtext/tests/tst_seed_op.cpp` | Determinism + round-trip tests. |
| `libs/collabtext/tests/tst_sidecar_manifest.cpp` | Schema, sha mismatch, doc_id compare. |

**Modified files:**

| File | Change |
|------|--------|
| `CMakeLists.txt` (root) | (no change) — `add_subdirectory(app)` already includes the new subdir once we update `app/CMakeLists.txt`. |
| `app/CMakeLists.txt` | New file (currently this dir lacks one — it's `app/CMakeLists.txt` is implicit via `add_subdirectory(app)` invoking `app/CMakeLists.txt`). After we add `app/collabedit/`, we either: (a) make the existing `app/CMakeLists.txt` only contain the testapp and add a sibling `add_subdirectory(collabedit)`, OR (b) restructure into `app/testapp/` + `app/collabedit/`. We do **(a)** — minimal disruption: create `app/CMakeLists.txt` that does `add_subdirectory(testapp)` (move existing into a `testapp/` subdir) and `add_subdirectory(collabedit)`. |
| `app/testapp/CMakeLists.txt`, `app/testapp/main.cpp` | Move from `app/` (rename only). |
| `libs/collabtext/CMakeLists.txt` | Register new sources (`SidecarManifest.cpp`, `SeedOp.cpp`) and tests. |

**Why `SeedOp` lives in the library:** it's a CRDT-level concern (it
manipulates `Buffer` and `EditOperation`). Future apps will need it
too.

**Why `SidecarManifest` lives in the library:** it's a small but
non-trivial schema with format validation, used by every collab-doc
client we ever build.

## 11. Testing

### 11.1 Unit tests

Added to `libs/collabtext/tests/`:

1. **`tst_seed_op.cpp`:**
   - `op_for_seed("hello").timestamp == Lamport(0, 1)`.
   - `op_for_seed("hello") == op_for_seed("hello")` (deterministic).
   - `op_for_seed(text)` applied to two empty real-replica Buffers
     (with different replica ids) produces the same `Buffer::text()`
     in both.
   - Empty seed: `op_for_seed("")` does not crash; resulting Buffer
     has empty visible text.

2. **`tst_sidecar_manifest.cpp`:**
   - Round-trip JSON: write → read → equal.
   - Reject schema_version != 1.
   - Detect SHA mismatch.
   - Compare two manifests: smaller `doc_id` wins (string compare on
     the canonical-form ULID).

### 11.2 Manual remote test (the actual goal)

Two-machine smoke:

1. Set up `~/sync/` on machine A and B, syncthing-mirrored.
2. On A: `collabedit ~/sync/notes.md`. Type a few lines. Save.
3. On A: `Document → Enable Collab`. Confirm sidecar creation.
4. Wait ~5s for Syncthing to propagate `notes.md.collab/`.
5. On B: `collabedit ~/sync/notes.md`. Verify it opens directly into
   Collab mode (sidecar detected), with A's text visible.
6. Type on both sides simultaneously. Verify edits converge with no
   duplication or loss.
7. Verify A's cursor appears on B's window with A's name label, and
   vice versa.
8. Verify participant list shows both Alice and Bob with green dots.
9. Save on B. Verify `notes.md` on A updates after Syncthing
   propagation (within ~10s).
10. Quit B. Verify Bob's dot fades from A's participant list within
    30s.

### 11.3 Concurrent-enrollment race smoke

1. Disable network sharing temporarily (or use offline mode).
2. On A: edit `notes.md`. Enable Collab.
3. On B: edit `notes.md` (same starting content). Enable Collab.
4. Re-enable sync.
5. Verify exactly one `manifest.json` survives; both peers' content
   converges to the union of edits; no double-inserted seed text.

### 11.4 Existing tests stay green

`ctest` for `libs/collabtext` passes unchanged. The new tests are
additive.

## 12. Success criteria

- Unit tests for `SeedOp` and `SidecarManifest` pass.
- Existing `ctest` suite stays green.
- Builds clean with the existing CMake preset (`build-dev/`).
- Two-machine manual smoke (§11.2) passes end-to-end.
- The concurrent-enrollment race smoke (§11.3) converges to a single
  manifest with no duplicated seed content.
- The existing `collabtext-testapp` still builds and runs after the
  `app/` reorganization.

## 13. Open questions deferred to implementation

- Exact `doc_id` format: ULID (Crockford base32) is preferred for
  lexicographic ordering. Implementation may fall back to UUIDv4 if a
  ULID lib isn't already available — UUIDv4 strings still compare
  deterministically as strings, so the tiebreaker logic is identical.
- Atomic write helper: reuse `PresenceManager::atomic_write` if its
  visibility allows; otherwise add a small file-utility header.
- Whether `Buffer::apply_local_edit` on a fresh `replica_id=0` buffer
  is *exactly* deterministic (locators included): verified by the
  first unit test in §11.1. If determinism breaks for non-trivial
  inputs (e.g. multi-fragment seeds), we add a small dedicated
  `Buffer::seed_with(content)` method that constructs the op
  by-hand. Expected outcome: just works.
