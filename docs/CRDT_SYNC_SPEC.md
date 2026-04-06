# CollabText CRDT Synchronization Specification

A file-sync-first collaborative text editing protocol. Syncthing (or any
file synchronization tool) serves as the reliable floor — always available,
never depends on network configuration, NAT traversal, or peer availability.
Direct channels (TCP, WebSocket, XMPP, etc.) are opportunistic accelerators
that peers negotiate through the synced folder itself. The system is correct
at any latency, on any transport, with any number of peers joining and
leaving at any time.

### Implementation Status (2026-04-05)

| Component | Status |
|-----------|--------|
| CRDT Engine (Buffer, SumTree, Anchors, UndoMap, GC) | **Complete** |
| Operation serialization (encode/decode) | Not implemented |
| SyncManager file transport | Scaffolded, disabled pending serialization |
| SQLite operation store | Not implemented (see section 8.8) |
| Direct channel negotiation | Not implemented |
| Ephemeral state (cursors/selections) | API exists, not syncing |
| Side streams (chat) | Not implemented |
| Qt Editor | Not implemented |
| Virtual filesystem (KIO/FUSE) | Not implemented |

The CRDT engine is the foundation. Everything above it depends on
operation serialization, which is the next piece to build. See
`docs/ARCHITECTURE.md` for the full build order.

---

## 1. Design Principles

1. **Syncthing is the floor.** Every operation is always written to the
   shared folder. Direct channels are optimizations. If every direct channel
   dies, collaboration continues at Syncthing's sync interval (typically
   1-5 seconds on LAN, longer on WAN). No data is ever only in a direct
   channel.

2. **The CRDT is the truth.** The document state is defined entirely by the
   set of operations that have been applied. Any replica holding the same
   set of operations will compute the same document. Operations are
   commutative, associative, and idempotent.

3. **Transport ignorance.** The CRDT layer does not know or care how
   operations arrive. File sync, TCP, carrier pigeon — all valid. The
   transport layer's only job is to eventually deliver every operation to
   every replica.

4. **No coordinator.** There is no leader, no server, no lock. Any replica
   can edit at any time, online or offline. Convergence is guaranteed by the
   CRDT's mathematical properties, not by coordination.

5. **Graceful elevation, graceful collapse.** Peers discover each other via
   the synced folder and attempt direct connections for lower latency.
   Direct channels carry enough state metadata that either side can detect
   failure and resume via the file floor without data loss or duplicate
   application.

---

## 2. Shared Folder Structure

The shared folder is synchronized by Syncthing (or equivalent). Every file
in it is eventually consistent across all devices.

```
collabtext-sync/
├── .stignore                        # Syncthing ignore rules (see §2.1)
├── meta/
│   ├── document.json                # Document metadata (see §2.2)
│   ├── gc-watermark.json            # Garbage collection state (see §10)
│   └── streams.json                 # Side stream registry (see §15)
├── identities/
│   ├── <identity-id>/
│   │   ├── profile.json             # Name, status, bio (see §3)
│   │   └── avatar.png               # Profile picture (see §3.4)
│   └── <identity-id>/
│       └── ...
├── replicas/
│   ├── <replica-id-A>/
│   │   ├── presence.json            # Liveness + connection offers (see §7)
│   │   ├── ephemeral.json           # Cursors, selections, status (see §15.1)
│   │   ├── ops/
│   │   │   ├── 00 .. ff             # Hash-bucketed operation logs (see §4)
│   │   │   └── info                 # Operations on path ["info"]
│   │   ├── streams/
│   │   │   ├── chat/                # Per-stream operation logs (see §15.2)
│   │   │   │   ├── 00 .. ff         # Hash-bucketed, same as ops/
│   │   │   │   └── sequences.json
│   │   │   └── <stream-name>/
│   │   │       └── ...
│   │   ├── sequences.json           # Per-hash write counters (see §5)
│   │   └── version.json             # This replica's current vector clock
│   ├── <replica-id-B>/
│   │   └── ...
│   └── <replica-id-C>/
│       └── ...
├── snapshots/
│   ├── <lamport>-<replica-id>.snapshot   # Periodic state snapshots (see §9)
│   └── latest.json                       # Pointer to newest valid snapshot
└── local/                           # NOT SYNCED (in .stignore)
    └── <own-replica-id>/
        ├── collabtext.db            # SQLite operation cache + index (§8.8)
        ├── read-sequences/
        │   ├── <replica-id-B>.json  # "I've read up to seq N from B's hash XX"
        │   └── <replica-id-C>.json
        ├── materialized.bin         # Current document text (cache)
        ├── pending-ops.log          # Ops written locally but not yet flushed
        └── channel-state/
            ├── <replica-id-B>.json  # Direct channel bookkeeping
            └── <replica-id-C>.json
```

### 2.1 Syncthing Ignore Rules

```
// .stignore
local/
*.tmp
*.part
```

The `local/` directory is per-machine state that must not be synced. It
contains read cursors, the materialized document cache, and direct channel
bookkeeping. Each machine's `local/` directory is independent.

### 2.2 Document Metadata

```json
// meta/document.json
{
  "document_id": "uuid-v4",
  "created": "2026-04-01T12:00:00Z",
  "name": "design-notes.txt",
  "line_ending": "unix",
  "encoding": "utf-8"
}
```

Created once when the collaborative document is initialized. Immutable
after creation.

---

## 3. Identity and Replica Identity

CollabText separates **user identity** (who you are) from **replica
identity** (which editing session this is). A person has one identity
across all devices and documents. Each time they open a document, a new
replica is created.

```
Person (identity)
 ├── Device A
 │    ├── Session 1 (replica deviceA-1)  — closed
 │    └── Session 2 (replica deviceA-2)  — active
 └── Device B
      └── Session 1 (replica deviceB-1)  — active
```

### 3.1 User Identity

A user identity is created once, stored locally on the user's machine,
and projected into every document the user edits. There is no central
account system. Identity is self-asserted.

#### 3.1.1 Local Identity Storage

The identity lives outside any document folder:

```
~/.config/collabtext/
  identity.json          # Profile data
  avatar.png             # Profile picture (optional)
  identity.key           # Ed25519 private key (optional, see §3.1.5)
```

This directory is never synced by Syncthing directly. It is the user's
canonical profile. The editor reads it on startup and copies it into
each document's shared folder.

#### 3.1.2 Profile Format

```json
// ~/.config/collabtext/identity.json
{
  "identity_id": "clinton-a7f3b2",
  "display_name": "Clinton",
  "status": "Drafting the sync spec",
  "bio": "Systems programmer. Obsessed with latency.",
  "color": "#3b82f6",
  "public_key": "ed25519:base64-encoded-public-key",
  "updated": "2026-04-01T12:00:00Z"
}
```

Fields:

- `identity_id`: A globally unique, human-readable identifier. Generated
  once on first launch. Format: `<name-slug>-<6-hex-chars>` where the hex
  suffix is derived from a random UUID. Examples: `clinton-a7f3b2`,
  `alice-04e1c9`. The name slug is lowercase, ASCII, derived from the
  initial `display_name`. The hex suffix ensures uniqueness even if two
  people choose the same name.

- `display_name`: The name shown to other participants. Free-form
  Unicode. Can be changed at any time.

- `status`: A short status line (like an IM status). Optional. Shown
  alongside the user's cursor or in a participant list.

- `bio`: A longer description. Optional. Shown in a profile popover or
  participant detail view.

- `color`: A hex color used to render this user's cursor, selection
  highlights, and chat messages. Chosen by the user or auto-assigned
  on first launch. The editor may override this locally to avoid
  collisions (two participants with the same color), but the stated
  preference is preserved.

- `public_key`: Optional Ed25519 public key for identity continuity
  verification (§3.1.5). Not used for encryption or access control.

- `updated`: ISO 8601 timestamp of the last profile modification. Used
  to determine whether a projection in a document folder is stale.

#### 3.1.3 Avatar

The avatar is stored as a file (`~/.config/collabtext/avatar.png`)
rather than inline in JSON. Supported formats: PNG, JPEG, WebP.
Maximum size: 256 KB. Recommended dimensions: 256x256 pixels.

If no avatar is set, the editor generates a default from the
`display_name` and `color` (initials on a colored circle, or similar).

#### 3.1.4 Identity Projection into Documents

When a replica joins a document (creates its replica directory), it
copies its identity into the document's shared folder:

```
collabtext-sync/identities/<identity-id>/
  profile.json      # Copy of ~/.config/collabtext/identity.json
  avatar.png         # Copy of ~/.config/collabtext/avatar.png
```

The projection is updated on every sync cycle if the local identity's
`updated` timestamp is newer than the projected copy. This means a
user can change their display name or avatar and it propagates to all
active documents within seconds.

Multiple devices owned by the same person project the same identity
(same `identity_id`). If the user updates their profile on device A,
device B's local copy is not automatically updated (it's outside the
sync folder). The user must update it manually on each device, or use
a separate sync mechanism for `~/.config/collabtext/` (Syncthing can
do this too — sync the config directory across devices).

The identity directory in the document folder is **written only by the
identity's owner**. Other replicas read it but never modify it. There
are no write conflicts because each identity ID maps to exactly one
writer at a time (or multiple devices with the same content).

#### 3.1.5 Identity Continuity (Optional Signing)

There is no central authority to verify "this identity_id belongs to
this person." A malicious participant could create a replica with
someone else's identity_id and impersonate them.

Optional mitigation: on first launch, the editor generates an Ed25519
keypair. The public key is included in `identity.json`. The private
key stays in `~/.config/collabtext/identity.key` and is never synced.

When projecting a profile into a document:

1. The profile JSON is signed with the private key.
2. The signature is stored alongside the profile:

```json
// collabtext-sync/identities/<identity-id>/profile.json
{
  "identity_id": "clinton-a7f3b2",
  "display_name": "Clinton",
  ...
  "public_key": "ed25519:...",
  "signature": "base64-encoded-signature-of-profile-fields"
}
```

Other replicas verify the signature against the public key. On first
encounter, the public key is trusted on first use (TOFU). On
subsequent encounters, if the public key changes, the editor warns:
"Clinton's identity key has changed" — similar to SSH host key
warnings.

This does not prevent impersonation by someone who has never been seen
before. It prevents impersonation of a known participant. For a
decentralized system with no account server, TOFU is the best
available trust model.

Signing is optional. If `public_key` is absent, the identity is
unsigned and displayed as unverified. The editor may show a small
indicator (like an unverified badge) to distinguish signed from
unsigned identities.

#### 3.1.6 Identity and Chat Attribution

Chat messages (§15.2.3) and comments (§15.2.4) store `author` (the
identity_id) and `author_name` (the display name at the time of
writing). The `author_name` is snapshot into each message because the
identity's display name may change later, and the profile may
eventually be GC'd from the document folder. The message should still
show a name even if the author's profile is gone.

The `author` field (identity_id) is used for grouping, coloring, and
linking to the live profile when available.

### 3.2 Replica Identity

Each editor session is a **replica**. A replica ID is a tuple:

```
replica_id = <device_id>-<session_counter>
```

- `device_id`: A stable per-machine identifier. Derived from hostname or a
  persistent random UUID stored in `local/`. Must survive reboots.
- `session_counter`: Monotonically increasing integer, incremented each time
  the editor opens the document on this device. Stored in
  `local/<own-replica-id>/session_counter`.

The session counter ensures that if the editor crashes and restarts, it gets
a new replica ID rather than reusing one with potentially stale Lamport
clock state. The device ID prefix keeps replica IDs human-readable and
debuggable.

A replica's `presence.json` includes an `identity_id` field that links the
ephemeral session to the persistent user identity. Multiple replicas can
share the same `identity_id` (same person, multiple devices or sessions).

#### Device Visibility Rule

The editor UI presents **one participant per identity**, not one per
replica or per device. When Clinton has two devices editing the same
document, other participants see a single "Clinton" entry in the
participant list. Clinton himself sees his own devices individually.

Specifically:

- **Other identities see:** One participant named "Clinton" with a
  single avatar. If Clinton has multiple active cursors (from multiple
  devices), they all appear in Clinton's color. No device names, no
  device count, no indication that multiple devices are involved. The
  merge is purely visual — the cursors are real, they just aren't
  labeled per-device.

- **Clinton sees his own devices:** The participant list shows his own
  entry expanded into its constituent devices: "ThinkPad" and "Desktop"
  (taken from `device_name` in each replica's `presence.json`). Each
  device's cursor is subtly distinguished (e.g., a small device icon or
  label on the cursor flag). This lets Clinton track which of his own
  devices is where in the document.

This is a **presentation rule**, not a data access rule. The `device_name`
field in `presence.json` is readable by everyone (it's in the synced
folder). The editor simply chooses not to display it to other
identities. This avoids complexity in the storage/sync layer — no
encryption, no per-identity file permissions, no access control within
the shared folder.

If future requirements demand true device privacy (preventing other
participants from even *reading* device names), the `device_name` field
could be encrypted with a key derived from the identity's private key
(§3.1.5), or moved to `local/`. For now, the simpler presentation-layer
approach is sufficient.

### 3.3 Replica ID Reuse Safety

A replica ID must never be reused with a Lamport clock value that could
collide with a previous session. The session counter prevents this: each
new session is a distinct replica with its own Lamport clock starting at 0.

Old replica directories from dead sessions accumulate in `replicas/`. They
are cleaned up by garbage collection (§10).

### 3.4 Replica Registration

On first start, a replica creates its directory:

```
replicas/<replica-id>/
  presence.json     (see §7)
  ops/              (empty directory)
  sequences.json    (empty: {})
  version.json      (empty: {})
```

And projects its identity (if not already present):

```
identities/<identity-id>/
  profile.json
  avatar.png
```

No coordination is needed. Directory creation is atomic on all major
filesystems. If two replicas race to create the same directory (which
cannot happen with unique IDs), mkdir fails harmlessly for the loser.

---

## 4. Operation Format

### 4.1 CRDT Operation Types

Two operation types, modeled on Zed's text CRDT:

#### Edit

```json
{
  "type": "edit",
  "timestamp": { "replica": "device-7", "seq": 42 },
  "version": { "device-3": 100, "device-7": 41 },
  "ranges": [
    { "start": 0, "end": 0 }
  ],
  "new_text": ["hello"],
  "locator": [1152921504606846976]
}
```

Fields:

- `timestamp`: Lamport timestamp. `replica` + `seq` together uniquely
  identify this operation globally. The Lamport clock is incremented
  before every operation: `seq = max(local_seq, max_observed_seq) + 1`.

- `version`: Vector clock at the moment this edit was created. Maps
  replica IDs to the highest sequence number observed from that replica.
  Used by receivers to determine causal readiness (§6.2).

- `ranges`: Byte offset ranges in the document-as-of-`version` that are
  being deleted. An empty range (start == end) means pure insertion.
  Multiple ranges encode multi-cursor edits.

- `new_text`: One string per range. The text to insert at each range's
  start position after the range is deleted. Empty string means pure
  deletion.

- `locator`: Position identifier for the inserted fragment. An array of
  unsigned 64-bit integers, typically length 1-2. Generated by
  `Locator::between(left_neighbor, right_neighbor)` using midpoint
  bisection. Locators provide a total order over all fragments without
  renumbering.

#### Undo

```json
{
  "type": "undo",
  "timestamp": { "replica": "device-7", "seq": 43 },
  "version": { "device-3": 100, "device-7": 42 },
  "counts": [
    { "edit": { "replica": "device-7", "seq": 42 }, "count": 1 }
  ]
}
```

Fields:

- `timestamp`, `version`: Same as Edit.

- `counts`: Array of `{edit_id, count}` pairs. Each entry specifies an
  edit operation and how many times it has been undone. Odd count = the
  edit is currently undone (invisible). Even count = the edit is currently
  visible (redo). This supports unlimited undo/redo chains and is
  convergent across replicas.

### 4.2 Operation Serialization

Operations are serialized as single-line JSON (no pretty-printing, no
embedded newlines). Each operation is exactly one line in the log file.
This enables simple append-only writing and line-by-line reading.

Operations within `new_text` that contain literal newlines use `\n` JSON
escaping. The one-operation-per-line invariant is never violated.

### 4.3 Hash Bucketing

Each operation's hash bucket is determined by its Lamport timestamp:

```
bucket = (timestamp.replica_hash * 199 + timestamp.seq) % 256
```

Where `replica_hash` is the polynomial hash of the replica ID string
(same algorithm as DecSync: polynomial evaluation at prime 19 over UTF-8
bytes, mod 256).

This distributes operations across 256 files (`00` through `ff`),
preventing any single file from growing unboundedly large.

The operation is appended to `replicas/<replica-id>/ops/<bucket>`.

---

## 5. Sequence Tracking

Each replica maintains a `sequences.json` file:

```json
// replicas/<replica-id>/sequences.json
{
  "00": 0,
  "0a": 3,
  "b9": 17,
  "ff": 1
}
```

Keys are hash bucket names. Values are write counters. Every time a
replica appends one or more operations to a bucket file, it increments
that bucket's sequence number by 1.

Missing keys imply sequence 0 (no writes to that bucket yet).

### 5.1 Reading New Operations

Each replica tracks what it has already read from every other replica in:

```json
// local/<own-id>/read-sequences/<other-id>.json
{
  "00": 0,
  "0a": 3,
  "b9": 15
}
```

To check for new operations from replica B:

1. Read `replicas/B/sequences.json`.
2. Read `local/<own>/read-sequences/B.json`.
3. For each bucket where B's sequence > own tracked sequence: read the
   entire bucket file `replicas/B/ops/<bucket>`, filter out operations
   already applied (by checking timestamps against the local version
   vector), apply new operations.
4. Update `local/<own>/read-sequences/B.json`.

The "read entire bucket file and filter" step is intentionally simple.
Bucket files are small (bounded by hash distribution and GC). More
sophisticated offset tracking is an optimization that can be added later.

### 5.2 Discovering Replicas

To discover which replicas exist, list the `replicas/` directory. New
subdirectories appear as Syncthing propagates them. No registration
protocol is needed.

Polling interval for new replicas: on each sync cycle (§6.1).

---

## 6. Sync Cycle

### 6.1 The File Sync Loop

Each editor instance runs a background sync loop (separate thread or async
task). The default interval is 1 second. This loop runs regardless of
whether direct channels are active.

```
every 1 second:
  1. FLUSH pending local operations to own ops/ files (§6.3)
  2. SCAN replicas/ for new or updated replicas
  3. For each other replica:
     a. Compare sequences (§5.1)
     b. Read new operations from updated buckets
     c. Apply operations to local CRDT state (§6.2)
     d. Update read-sequences
  4. UPDATE own version.json with current vector clock
  5. UPDATE own presence.json with current liveness (§7)
  6. CHECK for snapshot opportunities (§9)
  7. CHECK for GC opportunities (§10)
```

When a direct channel is active to a peer, that peer's operations arrive
in real-time and are applied immediately. The sync loop still runs, but
step 3 for that peer typically finds nothing new (the direct channel
already delivered everything). This is the "belt and suspenders" guarantee.

### 6.2 Applying Remote Operations

When an operation arrives (from file sync or direct channel):

1. **Duplicate check.** If the operation's Lamport timestamp is already in
   the local operation set, discard it. Operations are uniquely identified
   by (replica_id, seq).

2. **Causal readiness check.** Compare the operation's `version` vector
   against the local version vector. For the operation to be applicable,
   the local state must include every operation that the remote operation
   observed:

   ```
   for each (replica, seq) in operation.version:
     if local_version[replica] < seq:
       operation is NOT ready (queue it)
   ```

   Additionally, exclude the operation's own replica from this check (its
   sequence is expected to be one ahead).

3. **Queue if not ready.** Operations whose dependencies haven't arrived
   are placed in a per-replica queue. After applying any operation,
   re-check queued operations — applying one may satisfy another's
   dependencies.

4. **Apply.** For Edit operations: insert the new text fragment at the
   correct position (determined by the Locator and Lamport timestamp
   ordering). For the ranges being deleted: find the corresponding
   fragments and mark them as deleted (tombstone). For Undo operations:
   update the undo map.

5. **Advance local clock.** Update the local Lamport clock:
   `local_seq = max(local_seq, operation.timestamp.seq) + 1`.
   Update the local version vector:
   `local_version[operation.replica] = max(local_version[operation.replica], operation.timestamp.seq)`.

### 6.3 Flushing Local Operations

Local edits are first written to `local/<own-id>/pending-ops.log` (one
JSON line per operation). This is a local-only file, not synced. It serves
as a write-ahead log.

On each sync cycle (or immediately if no direct channels are active):

1. Read `pending-ops.log`.
2. Group operations by hash bucket.
3. For each bucket: append operations to `replicas/<own-id>/ops/<bucket>`.
4. Increment sequence numbers in `replicas/<own-id>/sequences.json`.
5. Truncate `pending-ops.log`.

Steps 3-4 must be done atomically per bucket (§11).

### 6.4 Debouncing During Typing

During active typing, flushing on every keystroke would thrash the
filesystem and trigger excessive Syncthing scans. The flush is debounced:

- **No direct channels active:** Flush every 100ms or when the pending log
  exceeds 64 operations, whichever comes first.
- **Direct channels active to all known peers:** Flush every 5 seconds or
  when the pending log exceeds 256 operations. The direct channels handle
  real-time delivery; the file flush is just for persistence and
  degradation safety.
- **Mixed (some direct, some file-only):** Flush every 500ms.

---

## 7. Presence and Peer Discovery

Each replica maintains a `presence.json` file in its replica directory:

```json
// replicas/<replica-id>/presence.json
{
  "replica_id": "laptop-3",
  "identity_id": "clinton-a7f3b2",
  "device_name": "Clinton's ThinkPad",
  "active": true,
  "last_heartbeat": "2026-04-01T14:30:00Z",
  "session_started": "2026-04-01T12:00:00Z",
  "version_summary": { "laptop-3": 421, "desktop-1": 300 },

  "channels": [
    {
      "protocol": "tcp",
      "host": "192.168.1.42",
      "port": 9271,
      "priority": 10
    },
    {
      "protocol": "tcp",
      "host": "[fe80::1%eth0]",
      "port": 9271,
      "priority": 10
    },
    {
      "protocol": "ws",
      "url": "ws://192.168.1.42:9272/collab",
      "priority": 20
    },
    {
      "protocol": "xmpp",
      "jid": "collabtext-laptop3@jabber.example.com",
      "priority": 30
    }
  ],

  "capabilities": {
    "crdt_version": 1,
    "compression": ["zstd", "none"],
    "max_batch_size": 1024
  }
}
```

### 7.1 Field Definitions

- `active`: Set to `true` while the editor is running. Set to `false` on
  graceful shutdown. If the editor crashes, this remains `true` but the
  heartbeat goes stale (§7.2).

- `last_heartbeat`: ISO 8601 timestamp, updated on every sync cycle. Used
  by other replicas to detect stale presence.

- `version_summary`: The replica's current vector clock. Published here so
  other replicas can estimate sync lag without reading operation files.
  Also used by the GC protocol (§10).

- `channels`: Ordered list of connection offers, lowest `priority` number
  first (highest priority). Each entry specifies a protocol and the
  information needed to connect. A replica advertises every address it can
  be reached at. Multiple entries for the same protocol at different
  addresses (IPv4, IPv6, link-local) are encouraged.

- `capabilities`: Protocol version and feature negotiation. Peers with
  incompatible `crdt_version` must not exchange operations directly (they
  can still coexist via the file floor if the version difference is
  forward-compatible).

### 7.2 Liveness Detection

A replica is considered **live** if:
- `active` is `true`, AND
- `last_heartbeat` is within the last 30 seconds.

A replica is considered **stale** if:
- `active` is `true`, AND
- `last_heartbeat` is older than 30 seconds.

Stale replicas are probably crashed. Their operations are still valid and
will be applied. Their presence is ignored for channel negotiation but
their replica directory is not cleaned up (that's GC's job, §10).

A replica is considered **departed** if:
- `active` is `false`.

Departed replicas have shut down gracefully. Their operations are still
valid. Their channel offers are ignored. Their directory is eligible for
GC after all operations have been absorbed.

### 7.3 Heartbeat Frequency

Presence is updated on every sync cycle (default 1 second). The heartbeat
timestamp is cheap to write (one small JSON file). Syncthing propagates it
within its own scan interval.

---

## 8. Transport Elevation and Degradation

### 8.1 The Transport Stack

```
┌─────────────────────────────────┐
│  CRDT Engine                    │  Receives operations, applies them.
│  (transport-ignorant)           │  Does not know or care about source.
├─────────────────────────────────┤
│  Operation Router               │  Deduplicates. Routes ops to CRDT.
│                                 │  Accepts ops from any transport.
├────────────┬────────────────────┤
│  Direct    │  File Transport    │  Parallel transports.
│  Channels  │  (the floor)       │  Direct channels are optional.
│  (TCP/WS/  │                    │  File transport is always active.
│   XMPP)    │  reads/writes to   │
│            │  replicas/<id>/    │
├────────────┴────────────────────┤
│  SQLite Operation Store         │  Local indexed cache (§8.8).
│  local/<id>/collabtext.db       │  Accelerates catch-up, startup,
│  (never synced)                 │  and direct channel negotiation.
└─────────────────────────────────┘
```

Both transports feed into the same Operation Router. The router
deduplicates by Lamport timestamp (replica_id + seq). An operation
delivered by a direct channel and later read from the file floor is simply
discarded on the second arrival.

### 8.2 Channel Negotiation

When a replica discovers another replica's `presence.json` with channel
offers:

1. **Filter.** Discard offers with incompatible `crdt_version` or
   unsupported protocols.

2. **Sort.** Order remaining offers by `priority` (lowest number first).

3. **Attempt connection.** Try each offer in priority order. For TCP/WS:
   open a socket. For XMPP: send a presence probe. Timeout: 3 seconds per
   attempt.

4. **Handshake.** On successful connection, exchange a handshake message
   (§8.3).

5. **Activate.** If handshake succeeds, the direct channel is active.
   Operations are streamed bidirectionally in real-time.

6. **Fallback.** If all offers fail, do nothing. File sync continues as
   normal. Retry channel negotiation on the next sync cycle.

Connection attempts are rate-limited: at most once per 10 seconds per
peer to avoid hammering unreachable hosts.

### 8.3 Direct Channel Handshake

The handshake is the same regardless of transport protocol:

```json
// Sent by both sides simultaneously (or initiator first, responder second)
{
  "msg": "collabtext-hello",
  "version": 1,
  "replica_id": "laptop-3",
  "document_id": "uuid-of-document",
  "vector_clock": { "laptop-3": 421, "desktop-1": 300, "phone-2": 50 },
  "capabilities": {
    "compression": ["zstd", "none"],
    "max_batch_size": 1024
  }
}
```

On receiving the peer's handshake:

1. **Validate `document_id`.** Must match. Reject otherwise.

2. **Compare vector clocks.** Determine which operations the peer is
   missing. For each replica R in the local vector clock where
   `local_version[R] > peer_version[R]`: the peer needs operations from R
   with sequence numbers in `(peer_version[R], local_version[R]]`.

3. **Send catch-up batch.** Send all operations the peer is missing. These
   come from the local operation store (not from disk — the CRDT engine
   holds them in memory). If the catch-up is large, stream in batches of
   `max_batch_size`.

4. **Enter streaming mode.** After catch-up, both sides stream new
   operations as they occur.

The handshake vector clock exchange is what makes degradation seamless: if
the direct channel dies, the file floor picks up from wherever the vector
clocks left off. No operations are lost because they were always being
written to the file floor in parallel.

### 8.4 Direct Channel Message Format

After handshake, the channel carries framed messages:

```json
{"msg": "ops", "ops": [ <op1>, <op2>, ... ]}
{"msg": "stream-ops", "stream": "chat", "ops": [ <op1>, ... ]}
{"msg": "ephemeral", "state": { ... }}
{"msg": "ack", "vector_clock": { ... }}
{"msg": "ping"}
{"msg": "pong"}
```

- `ops`: A batch of document CRDT operations. Delivered in causal order
  when possible, but out-of-order delivery is safe (the CRDT handles it).

- `stream-ops`: A batch of side stream operations (§15.2). The `stream`
  field names the stream. Same delivery guarantees as `ops`.

- `ephemeral`: Cursor positions, selections, and other transient state
  (§15.1). Sent on every local cursor/selection change. No persistence
  guarantees — if it's lost, the next one overwrites it anyway.

- `ack`: Periodically sent (every 5 seconds or every 100 operations) to
  inform the peer of the sender's current vector clock. This lets the peer
  update its understanding of what the sender has seen, which is used for
  GC (§10). The `ack` is also written to
  `local/<own>/channel-state/<peer>.json` for recovery after crash.

- `ping`/`pong`: Keepalive. Sent every 5 seconds if no other messages.
  If no `pong` received within 10 seconds, the channel is considered dead.

### 8.5 Graceful Degradation

A direct channel is abandoned when:

- Keepalive timeout (10 seconds without pong).
- Transport error (connection reset, write failure).
- Peer's `presence.json` goes stale (heartbeat > 30 seconds old).

On channel death:

1. **No action needed for correctness.** The file floor has every
   operation. The sync loop's next cycle will detect any operations that
   the direct channel might have been in the middle of delivering.

2. **Update local state.** Remove the channel from the active channel set.
   Optionally write the last-known peer vector clock to
   `local/<own>/channel-state/<peer>.json` so the next connection attempt
   can skip redundant catch-up.

3. **Continue sync loop.** The 1-second file sync loop was always running.
   Operations that were flowing over the direct channel are also in the
   file floor. The peer will see them on its next scan.

4. **Retry elevation.** On the next sync cycle, re-read the peer's
   `presence.json` and attempt reconnection if the peer is still live.

### 8.6 Graceful Elevation

When a direct channel is established to a peer who was previously
file-only:

1. **Handshake exchanges vector clocks** (§8.3). This reveals exactly what
   each side has. Any operations delivered by the file floor during the
   gap between the last direct channel session and now are accounted for.

2. **Catch-up is fast.** The gap is bounded by Syncthing's sync latency
   (typically a few seconds). The catch-up batch is small.

3. **Transparent to the user.** The editor's responsiveness improves
   (operations from this peer now arrive in milliseconds instead of
   seconds). No user action is required.

### 8.7 Why Syncthing Is the Floor, Not a Conduit

Syncthing excels at:
- NAT traversal and relay discovery (BEP protocol)
- Robust file conflict detection
- Efficient delta sync for changed files
- Zero-configuration peer discovery (via Syncthing's own device IDs)

Syncthing does not provide:
- A message-passing API
- Sub-second delivery guarantees
- Ordered message streams
- Bidirectional channels

The design exploits Syncthing's strengths (reliable, eventually-consistent
file delivery with great connectivity) while using direct channels for
what Syncthing cannot do (low-latency bidirectional streaming). The two
layers are complementary, not competing.

### 8.8 SQLite Local Operation Store

The shared folder's file layout — many small files in hash-bucketed
directories — is optimized for Syncthing's delta sync. But reading
hundreds of files for startup catch-up or direct channel negotiation is
slow. A local SQLite database bridges this gap.

**Location:** `local/<replica-id>/collabtext.db` (inside the non-synced
`local/` directory). This database is never synced by Syncthing. It is a
local cache that can be deleted and rebuilt from the operation files at
any time.

**Schema (conceptual):**

```sql
CREATE TABLE operations (
    replica_id   INTEGER NOT NULL,
    sequence     INTEGER NOT NULL,
    data         BLOB NOT NULL,       -- Serialized operation
    PRIMARY KEY (replica_id, sequence)
);

CREATE TABLE snapshots (
    id           INTEGER PRIMARY KEY,
    lamport      INTEGER NOT NULL,
    replica_id   INTEGER NOT NULL,
    data         BLOB NOT NULL,       -- Serialized snapshot
    created      TEXT NOT NULL
);

CREATE TABLE metadata (
    key          TEXT PRIMARY KEY,
    value        TEXT NOT NULL          -- JSON: vector clock, etc.
);
```

**Ingestion:** On startup, the SyncManager scans each replica's ops
directory for files not yet in SQLite (comparing sequence numbers against
the database). New files are deserialized and inserted. This is a one-time
catch-up; incremental ingestion via filesystem watcher handles ongoing
updates.

**Query patterns:**
- "What ops from replica B after sequence 47?" → indexed range scan.
- "Give me all ops I need to catch up peer X" → vector clock comparison
  against the metadata table, then indexed reads.
- "Load the latest snapshot" → single row lookup.

**Direct channel integration:** When a direct channel is active,
incoming operations go into SQLite immediately (and are fed to the
engine). They are flushed to operation files in the background. If the
channel drops, the file floor picks up from the last flushed sequence.
The dual-write guarantee (file floor always written) is maintained by
the background flush — SQLite is a write-ahead buffer, not a
replacement for the files.

**Why not replace the files entirely?** Because the files are the
transport. Syncthing syncs files, not SQLite rows. The files must exist
for other devices to receive operations. SQLite is a local performance
optimization layered on top of the file transport, not an alternative to
it.

**Rebuild:** If `collabtext.db` is deleted or corrupted, the SyncManager
rebuilds it by scanning the operation files. No data is lost because the
files are the source of truth.

---

## 9. State Snapshots

### 9.1 Purpose

Over time, the operation log grows. A new replica joining the document
would need to replay the entire history from the beginning. Snapshots
provide a checkpoint: a serialized CRDT state at a known version, from
which a new replica can start and then replay only the operations after
the snapshot.

### 9.2 Snapshot Format

```json
// snapshots/latest.json
{
  "snapshot_file": "4210-laptop-3.snapshot",
  "version": { "laptop-3": 4210, "desktop-1": 3800, "phone-2": 900 },
  "created": "2026-04-01T15:00:00Z",
  "created_by": "laptop-3"
}
```

The snapshot file itself (`snapshots/<lamport>-<replica>.snapshot`) is a
binary or JSON serialization of the complete CRDT state:

- All fragments (text content, Locator positions, Lamport timestamps,
  visibility flags, deletion lists).
- The undo map.
- The current vector clock.

The format is opaque to the transport layer. The CRDT engine defines its
own serialization.

### 9.3 When to Snapshot

A replica creates a snapshot when:

- The operation log has grown by more than 10,000 operations since the
  last snapshot, OR
- The total size of all operation files across all replicas exceeds 1 MB
  since the last snapshot, OR
- The replica is shutting down gracefully (to help future sessions start
  faster).

Snapshots are cheap to create (serialize in-memory state). They are
written atomically (write to temp file, then rename).

### 9.4 Snapshot Consistency

A snapshot is taken at a specific version vector. It is consistent with
all operations at or below that version. The `latest.json` pointer is
updated only after the snapshot file is fully written.

Multiple replicas may create snapshots concurrently. This is harmless:
each snapshot is a valid checkpoint. `latest.json` may briefly point to
different snapshots on different machines as Syncthing propagates, but
any snapshot is usable — a new replica simply replays more operations if
it reads an older snapshot.

### 9.5 New Replica Bootstrapping

When a new editor session joins (new replica ID, empty local state):

1. Read `snapshots/latest.json`.
2. Load the referenced snapshot file. Deserialize the CRDT state.
3. Read all operations from all replicas' `ops/` directories.
4. Filter to operations not covered by the snapshot's version vector.
5. Apply those operations (§6.2).
6. The document is now current.

If no snapshot exists (brand new document), start with empty state and
replay all operations from the beginning.

---

## 10. Garbage Collection

### 10.1 The Problem

Operation logs grow indefinitely. Dead replicas leave behind directories
that are never updated. Without cleanup, the shared folder grows without
bound.

### 10.2 What Can Be Garbage Collected

1. **Operations older than the latest snapshot** — once every live replica
   has advanced past a snapshot's version, the operations predating that
   snapshot are redundant (any new replica will load the snapshot instead
   of replaying them).

2. **Dead replica directories** — once all of a dead replica's operations
   have been incorporated into a snapshot, its `replicas/<id>/` directory
   can be removed.

3. **Old snapshots** — once a newer snapshot exists and all replicas have
   advanced past the older snapshot, the older snapshot file can be
   removed.

### 10.3 The GC Watermark

Garbage collection requires knowing what every live replica has seen.
This is tracked in `meta/gc-watermark.json`:

```json
// meta/gc-watermark.json
{
  "minimum_version": { "laptop-3": 200, "desktop-1": 150 },
  "last_updated": "2026-04-01T15:30:00Z",
  "participants": ["laptop-3", "desktop-1", "phone-2"]
}
```

`minimum_version` is the component-wise minimum of all live replicas'
`version_summary` from their `presence.json`. An operation is GC-safe if
its Lamport timestamp is covered by `minimum_version`.

### 10.4 GC Protocol

Any replica can initiate GC. The protocol is coordination-free:

1. **Compute minimum version.** Read all live replicas' `presence.json`
   files. For each replica R in `version_summary`, take the minimum across
   all live replicas:

   ```
   min_version[R] = min(presence[P].version_summary[R]
                        for all live P)
   ```

2. **Check snapshot coverage.** The latest snapshot's version must be ≤
   `min_version` (component-wise). If not, GC cannot proceed — not all
   replicas have advanced past the snapshot.

3. **Update watermark.** Write `meta/gc-watermark.json` with
   `min_version`. This is advisory — other replicas can verify independently.

4. **Purge operations.** For each replica directory, for each bucket file:
   read all operations, discard those whose Lamport timestamp is ≤ the
   watermark, rewrite the file with only the surviving operations. Update
   sequence numbers.

5. **Purge dead replica directories.** If a replica is departed (§7.2),
   and all of its operations are below the watermark, remove its
   `replicas/<id>/` directory.

6. **Purge old snapshots.** Remove snapshot files whose version is ≤ the
   watermark (except the latest).

### 10.5 GC Safety

GC is safe because:

- Operations are only purged when every live replica has already
  incorporated them (via `min_version`).
- A new replica joining during GC reads the latest snapshot (which
  includes all purged operations' effects) and then replays surviving
  operations.
- If GC races with a new replica that hasn't announced its presence yet,
  the new replica will load the snapshot and be fine. It never needed the
  purged operations.

### 10.6 GC Frequency

GC runs at most once per minute, triggered by any replica that notices
the operation log size exceeds a threshold (default: 10,000 total
operations across all replicas, or 2 MB total file size).

### 10.7 Scaling GC to N Replicas

With N replicas, the GC watermark is the component-wise minimum of N
vector clocks. A single slow or offline replica can block GC entirely (its
stale `version_summary` holds back the minimum). Mitigation:

- If a replica's `last_heartbeat` is older than 1 hour, it is excluded
  from the GC watermark computation. Its operations are still preserved
  (they're in the snapshot), but it no longer prevents cleanup.
- When the stale replica returns, it bootstraps from the latest snapshot
  like a new replica (§9.5).

---

## 11. Atomic Writes and Crash Safety

### 11.1 Write Strategy

All writes to the shared folder use the **write-to-temp-then-rename**
pattern:

1. Write content to `<target>.tmp` (or a randomly-named temp file in the
   same directory).
2. Call `fsync()` on the file descriptor.
3. Rename `<target>.tmp` to `<target>`.

On POSIX systems, rename is atomic: the file is either fully visible
with the new content or not visible at all. Syncthing will never see a
partially-written file.

On Windows, `MoveFileEx` with `MOVEFILE_REPLACE_EXISTING` provides the
same guarantee (in Qt, `QSaveFile` encapsulates this).

### 11.2 Append-Only Operation Files

Operation bucket files are append-only in normal operation. Appending to
a file is not atomic on most filesystems — a crash mid-append can leave a
partial last line. Defense:

- After appending, `fsync()` the file.
- On read, if the last line is not valid JSON, discard it. The operation
  it contained is still in the local `pending-ops.log` or in the CRDT
  engine's in-memory state and will be re-flushed on next cycle.
- Readers must tolerate truncated trailing lines.

GC is the only operation that rewrites (rather than appends to) bucket
files. GC uses the full write-to-temp-then-rename pattern.

### 11.3 Sequence File Atomicity

`sequences.json` is always written atomically (write-to-temp-then-rename).
If a crash occurs between appending operations and updating sequences,
the sequence number is stale. On next startup:

- The replica counts lines in each bucket file and reconciles with
  `sequences.json`.
- Alternatively, it simply increments the sequence and re-flushes.
  Readers tolerate re-reading already-seen operations (duplicates are
  discarded by timestamp).

### 11.4 Crash Recovery

On startup after a crash:

1. Read `local/<own>/pending-ops.log`. These are operations that were
   created locally but may not have been flushed to `replicas/`. Re-flush
   them.

2. Load the latest snapshot (§9.5).

3. Replay all operations from all replicas (§6.1).

4. Resume normal operation with a new replica ID (incremented session
   counter).

Local state (`materialized.bin`, `read-sequences/`) is a cache. If it's
corrupt or missing, it's rebuilt from the snapshot + operations. No data
is lost.

---

## 12. Scaling to N Replicas

### 12.1 Directory Scan Cost

Each sync cycle scans `replicas/` for subdirectories and reads
`sequences.json` from each. With N replicas:

- N directory listings (trivial)
- N `sequences.json` reads (small JSON files, typically < 1 KB)
- Up to 256 bucket file reads per replica with changes (only changed
  buckets are read)

This is O(N) per sync cycle. For 5-10 simultaneous editors, this is
negligible. For 50+, the scan interval can be increased.

### 12.2 Vector Clock Size

Vector clocks grow with the number of distinct replica IDs ever created.
With session counters incrementing replica IDs, the vector clock can grow
larger than the number of simultaneously active editors.

Mitigation:
- After GC, replica IDs whose operations have been fully absorbed into a
  snapshot can be removed from vector clocks.
- Snapshots define a new "epoch" — the vector clock in the snapshot
  replaces all prior history.

In practice, with a few dozen total sessions (across all devices, all
time), vector clocks remain small (< 1 KB serialized).

### 12.3 Conflict Resolution at Scale

The CRDT's conflict resolution is pairwise: every pair of concurrent
edits is resolved by Lamport timestamp ordering. This is transitive and
associative. Adding a third, fourth, or fifth replica does not change the
resolution of any pairwise conflict. All replicas converge to the same
document regardless of the order in which they receive operations.

### 12.4 Bandwidth

With N replicas, each replica reads from N-1 others. Total file I/O
scales as O(N^2) across all replicas (each of N replicas reads from N-1).
For real-time collaboration with 5 editors, this means 20 read paths per
sync cycle — well within any filesystem's capability.

Direct channels reduce this: if all peers are connected directly, the
file sync loop finds nothing new and the reads are effectively free (just
comparing sequence numbers).

---

## 13. Document Lifecycle

### 13.1 Creating a New Collaborative Document

1. Create the shared folder structure (§2).
2. Write `meta/document.json`.
3. Create the first replica directory.
4. Write the initial document content as the first Edit operation (or
   leave empty for a blank document).
5. Share the folder via Syncthing (add to Syncthing's folder list,
   share with devices).

### 13.2 Joining an Existing Document

1. Accept the Syncthing folder share.
2. Wait for initial sync to complete.
3. Open the document in the editor. The editor detects the
   `collabtext-sync/` structure and enters collaborative mode.
4. Bootstrap from snapshot + operations (§9.5).
5. Create a new replica directory and begin participating.

### 13.3 Leaving a Document

1. Set `active: false` in `presence.json`.
2. Flush all pending operations.
3. Optionally create a snapshot.
4. Close the editor.

The replica directory remains for other replicas to read. GC will
eventually clean it up after all operations are absorbed.

### 13.4 Solo Editing

If only one replica is active, the system degenerates to a simple
append-only edit log. No conflict resolution is triggered. The overhead
is one JSON line per edit to the operation log, flushed periodically.
This is acceptable for normal editing and provides a free undo history.

---

## 14. Security Considerations

### 14.1 Trust Model

All replicas in the shared folder are trusted. There is no authentication
at the CRDT layer — anyone with access to the Syncthing share can create a
replica and edit the document. Access control is delegated to Syncthing's
device authorization.

### 14.2 Direct Channel Security

Direct channel offers in `presence.json` are visible to all replicas.
Connections should use TLS or equivalent transport security. The handshake
should verify that the connecting peer's `document_id` matches.

Channel offers should not be treated as trusted network endpoints by
anything other than CollabText. Do not expose connection offers publicly.

### 14.3 Denial of Service

A malicious replica could flood the operation log with garbage operations.
The CRDT will faithfully apply them. Mitigation is social (revoke
Syncthing device access) or administrative (exclude replica from GC
watermark and let its operations be purged).

---

## 15. Side Streams

The document CRDT (§4-§6) handles text operations. Side streams carry
everything else: cursor positions, selections, chat messages, comments,
annotations, and any future data that should travel alongside the document.

Side streams fall into two categories with fundamentally different
properties:

| Property | Ephemeral | Persistent |
|----------|-----------|------------|
| Survives session end | No | Yes |
| Needs operation log | No | Yes |
| Needs GC | No | Yes (with text ops) |
| Conflict resolution | Last-write-wins (overwrite) | Stream-specific |
| Delivery guarantee | Best-effort | Reliable (same as text) |
| File floor behavior | Overwrite a single file | Append-only log |
| Examples | Cursors, selections, typing indicator, viewport | Chat, comments, annotations, bookmarks |

### 15.1 Ephemeral State

Ephemeral state is per-replica, overwritten (not appended), and
meaningless after the replica departs. It is the natural extension of
`presence.json` — live session state that other replicas display but
do not persist.

#### 15.1.1 The Ephemeral File

Each replica maintains `replicas/<replica-id>/ephemeral.json`:

```json
{
  "seq": 14207,
  "timestamp": "2026-04-01T14:32:01.337Z",

  "cursors": [
    {
      "anchor": { "replica": "laptop-3", "seq": 400, "offset": 12, "bias": "right" },
      "head":   { "replica": "laptop-3", "seq": 400, "offset": 12, "bias": "right" }
    }
  ],

  "selections": [
    {
      "anchor": { "replica": "desktop-1", "seq": 50, "offset": 0, "bias": "left" },
      "head":   { "replica": "desktop-1", "seq": 50, "offset": 27, "bias": "right" }
    }
  ],

  "viewport": {
    "top":    { "replica": "laptop-3", "seq": 380, "offset": 0, "bias": "left" },
    "bottom": { "replica": "laptop-3", "seq": 412, "offset": 0, "bias": "left" }
  },

  "activity": "typing",

  "custom": {}
}
```

#### 15.1.2 Positions as Anchors, Not Byte Offsets

Cursor and selection positions are expressed as **CRDT Anchors**, not byte
offsets. An Anchor references the Lamport timestamp of the operation that
created a piece of text, plus a byte offset within that insertion, plus a
bias (left or right).

```json
{
  "replica": "laptop-3",
  "seq": 400,
  "offset": 12,
  "bias": "right"
}
```

This means: "the position 12 bytes into the text fragment that was
inserted by replica `laptop-3` at sequence number 400, biased to the
right."

Why anchors instead of byte offsets:

- **Byte offsets shift.** When a remote edit inserts 5 characters before
  your cursor at byte 100, your cursor should be at byte 105. With a byte
  offset, every remote edit requires recalculation. With an anchor, the
  position is stable — the anchor points to a specific fragment in the
  CRDT, which never moves.

- **Anchors resolve automatically.** To convert an anchor to a byte offset
  for display, the CRDT engine walks the fragment tree and sums visible
  fragment lengths. This is O(log n) and already happens during normal
  rendering.

- **Concurrent edits can't corrupt anchors.** If the fragment an anchor
  points to is deleted, the anchor still resolves (to the position where
  the fragment was, biased left or right depending on `bias`). Anchors
  degrade gracefully.

#### 15.1.3 Multiple Cursors and Selections

The `cursors` and `selections` arrays support multi-cursor editing. Each
entry has:

- `anchor`: The fixed end of the selection (where the user started
  clicking/selecting).
- `head`: The moving end (where the cursor currently is).

For a plain cursor with no selection, `anchor == head`.

Multiple entries represent multiple cursors (as in "add cursor above/below"
or column selection mode).

When rendering remote cursors, the editor collects ephemeral state from
all replicas sharing the same `identity_id` and presents them as one
participant's cursors, all in that identity's color. The device
distinction is not shown to other participants (§3.2, Device Visibility
Rule). The cursor owner sees their own per-device cursors labeled by
device name.

#### 15.1.4 Activity Indicator

The `activity` field is a simple string enum:

- `"typing"`: The user is actively typing (keystrokes within the last 2
  seconds).
- `"selecting"`: The user is making a selection (mouse drag or shift+arrow).
- `"idle"`: The user has the document focused but isn't actively editing.
- `"away"`: The editor window is not focused.

This drives UI indicators (e.g., showing a pulsing cursor for active
typists, dimming idle participants).

#### 15.1.5 Custom Ephemeral Data

The `custom` object is a free-form JSON namespace for application-specific
ephemeral data. Examples:

- Scroll-linked following (a "follow me" flag)
- Momentary highlights ("look at this line")
- Debug/diagnostic state

No schema is enforced. Unknown keys are preserved by readers and ignored
if not understood.

#### 15.1.6 Ephemeral Update Frequency

**Over direct channels:** The `ephemeral` message (§8.4) is sent on every
cursor/selection change — typically on every keystroke and mouse movement.
This is lightweight (small JSON, no persistence overhead). Direct channels
can easily handle hundreds of ephemeral updates per second.

**Over the file floor:** `ephemeral.json` is overwritten on every sync
cycle (default 1 second), or more frequently during active typing (every
200ms). Syncthing propagates the file. The `seq` field is a monotonically
increasing counter that lets readers detect stale data without comparing
timestamps character by character.

**Staleness:** Ephemeral state from a replica whose `presence.json`
heartbeat is stale (>30 seconds) should be hidden from the UI. Cursors
from departed or crashed replicas should not be displayed.

#### 15.1.7 No Persistence, No GC

Ephemeral state is overwritten, not appended. There is no history.
When a replica departs, its `ephemeral.json` remains on disk but is
ignored (stale heartbeat). GC of dead replica directories (§10.4 step 5)
removes it along with everything else in the replica directory.

### 15.2 Persistent Side Streams

Persistent side streams carry data that must survive sessions: chat
messages, comments attached to document positions, annotations,
bookmarks. They use the same infrastructure as document operations
(hash-bucketed append-only logs, sequence tracking, GC) but are logically
separate.

#### 15.2.1 Stream Registry

```json
// meta/streams.json
{
  "streams": {
    "chat": {
      "type": "append-only",
      "created": "2026-04-01T12:00:00Z"
    },
    "comments": {
      "type": "anchor-keyed",
      "created": "2026-04-01T12:05:00Z"
    },
    "bookmarks": {
      "type": "anchor-keyed",
      "created": "2026-04-01T13:00:00Z"
    }
  }
}
```

Streams are registered in `meta/streams.json`. Each stream has a name
and a type that determines its merge semantics:

- **`append-only`**: Messages are independent. No conflicts are possible.
  Every message is retained. Ordered by Lamport timestamp for display.
  Used for: chat, activity log.

- **`anchor-keyed`**: Entries are keyed by a unique ID and reference
  document positions via Anchors. Entries with the same key are
  LWW-merged (latest timestamp wins). Entries can be "deleted" by
  setting a tombstone. Used for: comments, annotations, bookmarks.

New stream types can be defined by applications. Unknown types are
preserved but not displayed.

#### 15.2.2 Stream Storage

Each stream has its own operation log per replica, following the same
hash-bucketing and sequence tracking as document operations:

```
replicas/<replica-id>/streams/<stream-name>/
  00 .. ff              # Hash-bucketed entries
  sequences.json        # Per-hash write counters
```

The hash is computed from the entry's key (for `anchor-keyed` streams)
or Lamport timestamp (for `append-only` streams), using the same
polynomial hash as document operations (§4.3).

Stream entries are read, applied, and GC'd by the same sync cycle
machinery as document operations (§6.1). Each stream has its own
sequence tracking in `local/<own>/read-sequences/<peer>.json`, nested
under a `streams` key:

```json
// local/<own>/read-sequences/<peer>.json
{
  "ops": { "00": 42, "a1": 7 },
  "streams": {
    "chat": { "0f": 3, "b2": 1 },
    "comments": { "44": 2 }
  }
}
```

#### 15.2.3 Chat Stream

The chat stream is an `append-only` stream. Each entry is a single
message:

```json
{
  "type": "chat",
  "id": { "replica": "laptop-3", "seq": 500 },
  "timestamp": "2026-04-01T14:35:00Z",
  "author": "clinton-a7f3b2",
  "author_name": "Clinton",
  "body": "Should we refactor this section?",
  "reply_to": null,
  "anchor": null
}
```

Fields:

- `id`: Lamport timestamp. Uniquely identifies this message. Used for
  ordering and deduplication.

- `author`: The identity_id (§3.1) of the person who posted. Used for
  grouping, coloring, and linking to the live profile.

- `author_name`: Human-readable name, snapshot from the author's
  profile at the time of posting. Stored in the message because the
  author's profile may change or be GC'd by the time the message is
  read.

- `body`: Message text. Supports whatever markup the application wants
  (plain text, markdown, etc.).

- `reply_to`: Optional Lamport timestamp of another chat message. Enables
  threaded replies.

- `anchor`: Optional CRDT Anchor referencing a document position. When
  present, the chat message is contextually linked to a location in the
  document. The UI can show "Clinton commented near line 42" and
  navigate to the anchor on click.

Chat messages are never edited or deleted (append-only). If editing or
deletion is desired, use an `anchor-keyed` stream instead and model
messages as mutable entries.

#### 15.2.4 Comments Stream

The comments stream is an `anchor-keyed` stream. Each entry is a comment
attached to a document range:

```json
{
  "type": "comment",
  "id": "comment-uuid-1",
  "timestamp": "2026-04-01T14:40:00Z",
  "author": "alice-04e1c9",
  "author_name": "Alice",
  "range": {
    "start": { "replica": "laptop-3", "seq": 200, "offset": 0, "bias": "left" },
    "end":   { "replica": "laptop-3", "seq": 200, "offset": 45, "bias": "right" }
  },
  "body": "This paragraph needs a citation.",
  "resolved": false,
  "deleted": false
}
```

Fields:

- `id`: A stable unique ID (UUID). This is the key for LWW merging. Two
  entries with the same `id` are the same comment; the one with the later
  `timestamp` wins.

- `range`: A pair of CRDT Anchors defining the document range this
  comment is attached to. As text is edited around the comment, the
  anchors track the range automatically. If the commented text is deleted,
  the anchors collapse to a point (the anchors still resolve, but
  `start == end`). The UI can show "this comment's text was deleted" and
  offer to discard or re-anchor it.

- `resolved`: Boolean. When a comment is resolved (e.g., the suggested
  change was made), it can be marked resolved rather than deleted. The
  UI may hide resolved comments or show them dimmed.

- `deleted`: Boolean tombstone. When true, the comment is logically
  deleted. It remains in the stream for convergence (other replicas
  may have concurrent edits to the same comment) but is not displayed.

#### 15.2.5 Bookmarks / Annotations

The bookmarks stream is another `anchor-keyed` stream. Structure is
similar to comments but with different fields:

```json
{
  "type": "bookmark",
  "id": "bookmark-uuid-1",
  "timestamp": "2026-04-01T15:00:00Z",
  "author": "clinton-a7f3b2",
  "anchor": { "replica": "laptop-3", "seq": 300, "offset": 0, "bias": "left" },
  "label": "TODO: revisit this",
  "color": "#ff6600",
  "deleted": false
}
```

Applications can define additional `anchor-keyed` streams for any
anchored metadata: highlights, review markers, error annotations, etc.

#### 15.2.6 Custom Streams

Applications can create new streams by adding entries to
`meta/streams.json`. The only requirement is choosing a type
(`append-only` or `anchor-keyed`) and a unique name.

The stream machinery (hash bucketing, sequence tracking, sync cycle,
GC) is generic. It operates on opaque JSON entries keyed by stream name
and type semantics. The application defines the entry schema.

### 15.3 Stream Delivery Over Direct Channels

When a direct channel is established, the handshake (§8.3) is extended
to include stream state:

```json
{
  "msg": "collabtext-hello",
  "version": 1,
  "replica_id": "laptop-3",
  "document_id": "uuid-of-document",
  "vector_clock": { "laptop-3": 421, "desktop-1": 300 },
  "stream_versions": {
    "chat": { "laptop-3": 15, "desktop-1": 12 },
    "comments": { "laptop-3": 3 }
  },
  "capabilities": {
    "compression": ["zstd", "none"],
    "max_batch_size": 1024,
    "streams": ["chat", "comments", "bookmarks"]
  }
}
```

- `stream_versions`: Per-stream vector clocks. These are separate from
  the document vector clock because stream operations have their own
  Lamport sequences.

- `capabilities.streams`: Which streams this replica participates in. A
  replica that doesn't support a particular stream simply doesn't send
  or receive its operations. The stream's data is still in the file floor
  for replicas that do support it.

After handshake, stream operations flow as `stream-ops` messages (§8.4).
Ephemeral state flows as `ephemeral` messages. Both are multiplexed on
the same channel alongside document `ops`.

### 15.4 Stream GC

Persistent stream entries are garbage-collected together with document
operations. The GC watermark (§10.3) is extended with per-stream minimum
versions:

```json
// meta/gc-watermark.json
{
  "minimum_version": { "laptop-3": 200, "desktop-1": 150 },
  "stream_minimum_versions": {
    "chat": { "laptop-3": 10, "desktop-1": 8 },
    "comments": { "laptop-3": 2 }
  },
  "last_updated": "2026-04-01T15:30:00Z",
  "participants": ["laptop-3", "desktop-1", "phone-2"]
}
```

For `append-only` streams (chat), entries below the stream watermark are
purged. They are gone — chat history before the watermark is not
recoverable. If chat history preservation is important, increase the GC
threshold or snapshot chat separately.

For `anchor-keyed` streams (comments, bookmarks), only tombstoned entries
(`deleted: true`) below the watermark are purged. Live entries are never
purged by GC — they are active document metadata. Modifying them creates
new entries that replace the old ones via LWW.

### 15.5 Scalability of Side Streams

Side streams are lightweight:

- **Ephemeral:** One small JSON file per replica, overwritten. O(N) reads
  per sync cycle. No growth over time.

- **Chat:** Append-only, but chat volume is orders of magnitude lower
  than edit volume. A busy collaborative session might produce 100 chat
  messages per hour. At ~200 bytes per message, that's 20 KB/hour.

- **Comments:** Anchor-keyed with LWW. Entry count equals the number of
  active comments, which is bounded by human attention. A heavily
  commented document might have 50-100 comments.

- **Custom streams:** Bounded by application design. The infrastructure
  scales the same as document operations.

The sync cycle processes streams in the same pass as document operations,
adding negligible overhead per stream.

---

## 16. Summary of Invariants

1. Every operation is written to the file floor before or simultaneously
   with direct channel delivery. The file floor is never skipped.

2. Every operation is uniquely identified by (replica_id, seq). Duplicate
   delivery is harmless.

3. Every operation carries a vector clock version. An operation is only
   applied when all its dependencies are satisfied.

4. Every replica's `sequences.json` is monotonically increasing per
   bucket. Readers can detect new operations by comparing sequence
   numbers.

5. Snapshots are consistent checkpoints. A snapshot plus all subsequent
   operations yields the current document state.

6. GC only purges operations that are covered by a snapshot AND whose
   version is below the minimum version of all live replicas. No live
   replica will ever need a purged operation.

7. The CRDT converges regardless of operation delivery order, timing, or
   transport. The document state is a deterministic function of the
   operation set.

8. Ephemeral state is always overwritten, never appended. Stale ephemeral
   data (from departed or crashed replicas) is ignored, not processed.

9. Persistent side stream entries follow the same reliability guarantees
   as document operations: written to the file floor, deduplicated by ID,
   GC'd only when all live replicas have incorporated them.

10. Cursor and selection positions are expressed as CRDT Anchors, never
    byte offsets. Anchors are stable across concurrent edits.
