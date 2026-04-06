# Ephemeral Identity System Design

A reusable identity, presence, and ephemeral state system for
CollabText. Implements CRDT_SYNC_SPEC sections 3 (Identity), 7
(Presence), and 15.1 (Ephemeral State).

**Date:** 2026-04-06
**Scope:** Core C++ layer (Qt-free) + Qt widget layer
**Deferred:** Ed25519 signing (stubbed), avatar file picker/crop/resize
(stubbed), direct channel offers, custom ephemeral data

---

## 1. Design Principles

- **Core layer has zero toolkit dependencies.** Identity, Presence,
  EphemeralState, and all file I/O are pure C++20 using only the standard
  library and existing CRDT engine types (Anchor, Global). Usable from
  Qt, GTK, curses, or anything else.

- **Qt widgets are embeddable building blocks.** Each widget is a
  self-contained QWidget. Convenience dialogs compose them for common
  flows. Consuming apps own lifecycle and layout.

- **Stubs mark where future work goes.** Ed25519 signing and avatar
  management have real interfaces with trivial implementations. When the
  real code lands, only the function bodies change.

- **Ephemeral state uses CRDT anchors, not byte offsets.** Cursor and
  selection positions reference fragment timestamps in the CRDT, making
  them stable across concurrent edits.

---

## 2. Core Layer — Data Model

All types in `libs/collabtext/src/identity/`, public headers in
`libs/collabtext/include/collabtext/`. Pure C++20.

### 2.1 Identity

Who you are. Persisted locally, projected into document shared folders.

```cpp
struct Identity {
    std::string identity_id;      // "clinton-a7f3b2" — generated once
    std::string display_name;     // free-form UTF-8
    std::string status;           // short IM-style status line (optional)
    std::string bio;              // longer description (optional)
    std::string color;            // "#3b82f6" hex color
    std::string public_key;       // "ed25519:base64..." — empty if unsigned
    std::string updated;          // ISO 8601 timestamp
};
```

`identity_id` format: `<name-slug>-<6-hex-chars>` where the slug is
lowercase ASCII derived from the initial display_name and the hex suffix
comes from a random UUID. Generated once by `IdentityStore::generate()`.

### 2.2 Presence

Per-replica live session state. Written to `presence.json` on every sync
cycle.

```cpp
struct Presence {
    std::string replica_id;
    std::string identity_id;
    std::string device_name;
    bool active = true;
    std::string last_heartbeat;   // ISO 8601, updated every sync cycle
    std::string session_started;  // ISO 8601, set once at session start
    Global version_summary;       // the replica's current vector clock

    // Stubs — populated when direct channels are implemented
    // std::vector<ChannelOffer> channels;
    // Capabilities capabilities;
};
```

Liveness rules (from spec section 7.2):
- **Live:** `active == true` AND `last_heartbeat` within 30 seconds.
- **Stale:** `active == true` AND `last_heartbeat` older than 30 seconds.
- **Departed:** `active == false`.

### 2.3 EphemeralState

Per-replica cursor, selection, viewport, and activity state. Written to
`ephemeral.json`. Overwritten (not appended) on every update.

```cpp
struct EphemeralState {
    uint64_t seq = 0;             // monotonically increasing counter
    std::string timestamp;        // ISO 8601

    struct CursorPair {
        Anchor anchor;            // fixed end of selection
        Anchor head;              // moving end (cursor position)
    };

    std::vector<CursorPair> cursors;
    std::vector<CursorPair> selections;

    std::optional<Anchor> viewport_top;
    std::optional<Anchor> viewport_bottom;

    std::string activity;         // "typing", "selecting", "idle", "away"

    // custom: deferred (free-form JSON namespace, spec section 15.1.5)
};
```

Positions are CRDT Anchors (spec section 15.1.2): `{replica, seq, offset,
bias}`. They reference a specific fragment in the CRDT tree, not a byte
offset. This makes them stable across concurrent edits — no
recalculation needed when remote edits shift text positions.

For a plain cursor with no selection, `anchor == head`. Multiple entries
in the `cursors` array represent multi-cursor editing.

### 2.4 Signing (Stub)

```cpp
// identity/Signing.h

struct SigningKeyPair {
    std::string public_key;   // "ed25519:base64..."
    std::string private_key;  // raw bytes, never synced
};

// Currently returns nullopt.
std::optional<SigningKeyPair> generate_keypair();

// Currently returns empty string.
std::string sign_profile(const Identity &identity,
                         const std::string &private_key);

// Currently returns true (trust everything).
bool verify_profile(const Identity &identity,
                    const std::string &signature);
```

When Ed25519 is implemented: `generate_keypair()` creates a real pair,
`sign_profile()` signs the profile fields, `verify_profile()` checks the
signature against the public key with TOFU semantics (trust on first use,
warn on key change).

---

## 3. Core Layer — Services

### 3.1 IdentityStore

Manages the local identity at a configurable directory (defaults to
`~/.config/collabtext/`). Injectable path for testing.

```cpp
class IdentityStore {
public:
    explicit IdentityStore(std::filesystem::path config_dir);

    std::optional<Identity> load() const;
    void save(const Identity &identity);
    Identity generate(const std::string &display_name);

    std::filesystem::path avatar_path() const;
    std::vector<uint8_t> load_avatar() const;
    void save_avatar(const std::vector<uint8_t> &data);

    std::filesystem::path signing_key_path() const;

private:
    std::filesystem::path m_config_dir;
};
```

`generate()` creates a new Identity with:
- `identity_id`: slugified display_name + "-" + 6 random hex chars
- `color`: randomly chosen from a curated palette
- `updated`: current UTC timestamp
- All other fields empty/default

`save()` writes `identity.json` atomically (write to temp, rename).
Creates the directory if it doesn't exist.

`save_avatar()` validates size <= 256KB but does not validate format or
resize. The consuming app or a future avatar management layer handles
that.

### 3.2 IdentityProjector

Copies local identity into a document's shared folder. Reads other
participants' projected identities.

```cpp
class IdentityProjector {
public:
    explicit IdentityProjector(std::filesystem::path shared_folder);

    void project(const Identity &identity);
    void project_avatar(const std::string &identity_id,
                        const std::vector<uint8_t> &data);

    std::vector<Identity> read_all() const;
    std::optional<Identity> read(const std::string &identity_id) const;

private:
    std::filesystem::path m_shared_folder;
};
```

`project()` writes to `<shared_folder>/identities/<identity_id>/profile.json`.
Only updates if the local identity's `updated` timestamp is newer than
the existing projection (avoids unnecessary writes that would trigger
Syncthing sync cycles).

`read_all()` enumerates `<shared_folder>/identities/*/profile.json`.

### 3.3 PresenceManager

Writes local presence and ephemeral state. Reads remote replicas' state.
No timer, no event loop — the caller drives the cadence.

```cpp
class PresenceManager {
public:
    PresenceManager(std::filesystem::path shared_folder,
                    std::string replica_id,
                    std::string identity_id);

    void write_presence(const Presence &presence);
    void write_ephemeral(const EphemeralState &state);

    std::vector<std::pair<std::string, Presence>>
        read_remote_presences() const;

    std::vector<std::pair<std::string, EphemeralState>>
        read_remote_ephemerals() const;

    static bool is_live(const Presence &p);
    static bool is_stale(const Presence &p);
    static bool is_departed(const Presence &p);

    void depart();

private:
    std::filesystem::path m_shared_folder;
    std::string m_replica_id;
    std::string m_identity_id;
};
```

`write_presence()` writes to
`<shared_folder>/replicas/<replica_id>/presence.json`. The caller is
responsible for setting `last_heartbeat` to the current time before
calling (or PresenceManager can stamp it — implementation detail).

`write_ephemeral()` writes to
`<shared_folder>/replicas/<replica_id>/ephemeral.json`. Increments `seq`
automatically.

`read_remote_presences()` and `read_remote_ephemerals()` enumerate
`<shared_folder>/replicas/*/` directories, skipping the local replica_id.
Each return value pairs the replica_id string with the parsed struct.

`depart()` sets `active = false` in presence.json for graceful shutdown.

---

## 4. JSON Serialization

Free functions in `identity/Json.h`:

```cpp
std::string to_json(const Identity &);
Identity identity_from_json(const std::string &);

std::string to_json(const Presence &);
Presence presence_from_json(const std::string &);

std::string to_json(const EphemeralState &);
EphemeralState ephemeral_from_json(const std::string &);

std::string anchor_to_json(const Anchor &);
Anchor anchor_from_json(const std::string &);
```

Hand-rolled minimal JSON writer and parser. The objects are flat or
one-level nested — no need for a general-purpose JSON library. This
keeps the zero-dependency constraint.

Malformed JSON from remote replicas (corrupt file, partial write) is
handled by returning `std::nullopt` or a default-constructed struct.
Callers skip entries they can't parse — same resilience model as the
CRDT engine's causal queue (tolerate garbage, don't crash).

### 4.1 JSON Schemas

**identity.json / profile.json:**

```json
{
  "identity_id": "clinton-a7f3b2",
  "display_name": "Clinton",
  "status": "Drafting the sync spec",
  "bio": "Systems programmer. Obsessed with latency.",
  "color": "#3b82f6",
  "public_key": "",
  "signature": "",
  "updated": "2026-04-06T12:00:00Z"
}
```

`public_key` and `signature` are present but empty (signing stub).

**presence.json:**

```json
{
  "replica_id": "laptop-3",
  "identity_id": "clinton-a7f3b2",
  "device_name": "Clinton's ThinkPad",
  "active": true,
  "last_heartbeat": "2026-04-06T14:30:00.337Z",
  "session_started": "2026-04-06T12:00:00Z",
  "version_summary": {"laptop-3": 421, "desktop-1": 300},
  "channels": [],
  "capabilities": {"crdt_version": 1}
}
```

**ephemeral.json:**

```json
{
  "seq": 14207,
  "timestamp": "2026-04-06T14:32:01.337Z",
  "cursors": [
    {
      "anchor": {"replica": "laptop-3", "seq": 400, "offset": 12, "bias": "right"},
      "head":   {"replica": "laptop-3", "seq": 400, "offset": 12, "bias": "right"}
    }
  ],
  "selections": [],
  "viewport": {
    "top":    {"replica": "laptop-3", "seq": 380, "offset": 0, "bias": "left"},
    "bottom": {"replica": "laptop-3", "seq": 412, "offset": 0, "bias": "left"}
  },
  "activity": "typing",
  "custom": {}
}
```

---

## 5. Qt Widget Layer

All widgets in `libs/collabtext/src/ui/`. Reusable by any Qt application.

### 5.1 Embeddable Widgets

**AvatarWidget** — a small square QWidget that renders either a loaded
image (scaled, rounded to circle) or the generated fallback (colored
circle with initials from display_name, using the identity's color).
Used in IdentityEditor, ParticipantListWidget, and cursor popovers.

```cpp
class AvatarWidget : public QWidget {
    Q_OBJECT
public:
    explicit AvatarWidget(QWidget *parent = nullptr);

    void setIdentity(const std::string &display_name,
                     const std::string &color);
    void setImage(const std::vector<uint8_t> &data);
    void clearImage();  // revert to initials fallback

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;
};
```

**IdentityEditor** — a form panel (QWidget, no buttons) for editing an
Identity. Contains: QLineEdit for display_name, QLineEdit for status,
small QTextEdit for bio, color picker button (opens QColorDialog, shows
swatch), AvatarWidget with placeholder "click to set" (stub — no file
picker yet).

```cpp
class IdentityEditor : public QWidget {
    Q_OBJECT
public:
    explicit IdentityEditor(QWidget *parent = nullptr);

    void setIdentity(const Identity &identity);
    Identity identity() const;

signals:
    void identityChanged();
};
```

**ParticipantListWidget** — vertical list of connected participants.
Each entry shows: AvatarWidget + display_name + status +
PresenceIndicator. Entries keyed by identity_id (multiple replicas with
the same identity collapse into one entry showing device count).
Stale entries (heartbeat >30s) fade and disappear.

```cpp
class ParticipantListWidget : public QWidget {
    Q_OBJECT
public:
    explicit ParticipantListWidget(QWidget *parent = nullptr);

    void updateParticipants(
        const std::vector<Identity> &identities,
        const std::vector<Presence> &presences);

signals:
    void participantClicked(const QString &identityId);
};
```

**PresenceIndicator** — a tiny colored dot. Green = typing/selecting,
yellow = idle, gray = away/stale. For inline use in participant entries,
tab bars, status bars.

```cpp
class PresenceIndicator : public QWidget {
    Q_OBJECT
public:
    explicit PresenceIndicator(QWidget *parent = nullptr);

    void setActivity(const std::string &activity);  // "typing", "idle", etc.
    void setStale(bool stale);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;
};
```

### 5.2 Convenience Dialogs

**IdentitySetupDialog** — first-launch wizard. Embeds an IdentityEditor.
"Welcome to CollabText" header. OK button generates and saves the
identity via IdentityStore. Shown when `IdentityStore::load()` returns
nullopt. Returns the created Identity on accept.

```cpp
class IdentitySetupDialog : public QDialog {
    Q_OBJECT
public:
    explicit IdentitySetupDialog(IdentityStore &store,
                                 QWidget *parent = nullptr);

    Identity identity() const;
};
```

**IdentityPreferencesPage** — settings panel. Embeds an IdentityEditor,
pre-populated from IdentityStore::load(). Save button calls
IdentityStore::save(). Designed to drop into any QTabWidget or
preferences dialog.

```cpp
class IdentityPreferencesPage : public QWidget {
    Q_OBJECT
public:
    explicit IdentityPreferencesPage(IdentityStore &store,
                                     QWidget *parent = nullptr);

signals:
    void identitySaved(const Identity &identity);
};
```

---

## 6. SyncManager Integration

SyncManager becomes a thin Qt adapter. It owns the QTimer and emits
signals, but file I/O and data structures are in the core layer.

### 6.1 Updated Interface

```cpp
class SyncManager : public QObject {
    Q_OBJECT
public:
    SyncManager(CrdtEngine *engine, const Identity &identity,
                const std::string &replica_id, QObject *parent = nullptr);

    void start(const QString &sharedFolder);
    void stop();

    void setEphemeralState(const EphemeralState &state);

signals:
    void remoteEphemeralChanged(const QString &replicaId,
                                const EphemeralState &state,
                                const Identity &identity);
    void presenceChanged(const std::vector<Presence> &livePeers);
    void syncError(const QString &message);
};
```

### 6.2 Updated Sync Cycle

```
syncCycle():
  1. flushLocalUpdates()                           // existing
  2. readRemoteUpdates()                           // existing
  3. m_presence.write_presence(local_presence)      // core PresenceManager
  4. m_presence.write_ephemeral(m_ephemeral_state)  // core PresenceManager
  5. auto remotes = m_presence.read_remote_presences()
  6. auto ephemerals = m_presence.read_remote_ephemerals()
  7. for each live remote:
       identity = m_projector.read(remote.identity_id)
       emit remoteEphemeralChanged(replica_id, ephemeral, identity)
  8. emit presenceChanged(live_presences)
```

### 6.3 Test App Changes

The current `MainWindow::syncRemoteCursor()` direct in-process wiring is
replaced by the ephemeral file path:

- Each `EditorPane` generates an Identity (via IdentityStore pointed at a
  temp directory) and passes it to SyncManager.
- On cursor change, EditorPane captures an `EphemeralState` with CRDT
  anchors (via `Buffer::anchor_at()`) and calls
  `SyncManager::setEphemeralState()`.
- SyncManager writes `ephemeral.json` to disk on each sync cycle.
- The other pane's SyncManager reads it, looks up the Identity, and emits
  `remoteEphemeralChanged`.
- The receiving EditorPane resolves anchors via `Buffer::resolve_anchor()`
  and feeds RemoteCursor structs to MultiCursorController.
- Cursor data flows through files on disk, identical to two separate
  processes syncing via Syncthing.

---

## 7. Source Tree

```
libs/collabtext/
  include/collabtext/
    Identity.h              # Identity, Presence, EphemeralState structs
    IdentityStore.h         # local identity management
    IdentityProjector.h     # projection into shared folders
    PresenceManager.h       # presence + ephemeral file I/O
    Signing.h               # Ed25519 stubs
  src/identity/
    Identity.cpp            # JSON serialization for all structs
    IdentityStore.cpp
    IdentityProjector.cpp
    PresenceManager.cpp
    Signing.cpp             # stub implementations
  src/ui/
    AvatarWidget.h/cpp
    IdentityEditor.h/cpp
    ParticipantListWidget.h/cpp
    PresenceIndicator.h/cpp
    IdentitySetupDialog.h/cpp
    IdentityPreferencesPage.h/cpp
    CollabPlainTextEdit.h/cpp   # existing
    MultiCursorController.h/cpp # existing
```

The core identity files (`src/identity/`, `include/collabtext/`) have no
Qt includes. They are compiled into the same `collabtext` library as the
CRDT engine. The ui/ files depend on Qt Widgets and the core identity
types.

---

## 8. What This Enables

With this system in place, the remaining roadmap items become
straightforward:

- **Remote cursor labels** (roadmap step 2): The identity's display_name
  and color are available at cursor render time. The label widget reads
  them from the Identity struct delivered by remoteEphemeralChanged.

- **Participant list** (roadmap step 3): ParticipantListWidget is built
  in this spec. Wire it to SyncManager::presenceChanged.

- **Chat panel** (roadmap step 5): Chat messages carry `author`
  (identity_id) and `author_name`. The identity system provides both.

- **Scroll stability** (roadmap step 4): EphemeralState already carries
  viewport anchors. The receiving editor can use them for follow-mode.
