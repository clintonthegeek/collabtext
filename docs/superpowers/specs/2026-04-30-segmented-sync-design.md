# Segmented Append-Only Sync — Design Spec

**Date:** 2026-04-30
**Status:** Design (approved)
**Scope:** CRDT ops, side streams, presence/ephemeral
**Out of scope (this plan):** snapshots, distributed GC integration, binary
op encoding, vendored zstd, virtual filesystem, direct channels
**Breakage:** clean break; sidecars from `format_version == 1` are not read

---

## 1. Motivation

The current sidecar layout produces several hundred files totalling ≥100 KB
for a few dozen words of editing. Profile:

- 256 hash buckets per stream per replica (`replicas/<id>/ops/{00..ff}` and
  `replicas/<id>/streams/<name>/{00..ff}`), even when only a few are touched.
- `sequences.json` rewritten on every flush.
- `presence.json` and `ephemeral.json` rewritten every 500 ms via
  atomic-rename, which every block-diffing transport (Syncthing, rsync, S3
  versioning, IPFS) treats as a brand-new object.
- JSON-line ops are 5–10x larger than necessary.
- No compaction story — sealed data and live data share one file.

CollabText is committed to being transport-agnostic with Syncthing as the
floor (see `docs/CRDT_SYNC_SPEC.md` §1). The on-disk shape today is hostile
to that floor and pessimal for everything above it.

This spec redesigns the synced layout around **append-only segment logs
with sealed immutable bodies**. Local edits land in a single open tail
file per stream; size or idle thresholds seal the tail into an immutable
zstd-compressed segment; presence and ephemeral state collapse into one
LWW file written only on real change.

## 2. Design Principles

1. **One open file per stream per replica.** Appends only. Block-diffed
   cheaply by Syncthing.
2. **Sealed segments are immutable and content-stable.** Once written,
   never rewritten. Transferred once, never re-uploaded.
3. **No synced metadata files for sequence tracking.** Ordering and
   "what's been read" are recoverable from filenames and file sizes.
   Per-peer read cursors live in `local/`.
4. **One atomic-rename file in the synced root** (`state.json`), throttled
   and only-on-real-change.
5. **Plaintext on the open tail, zstd on sealed segments.** A crash leaves
   a human-readable file; sealed segments are dense.
6. **Format-versioned segment headers.** Future binary encoding,
   alternative compression, snapshot integration drop in behind a version
   bump without changing the file naming or lifecycle.

## 3. On-Disk Layout

```
<sidecar>/
├── manifest.json                  # format_version: 2 (was 1)
├── seed.txt                       # unchanged
├── replicas/<replica-id>/
│   ├── state.json                 # presence + ephemeral, LWW, throttled
│   ├── log/
│   │   ├── ops/
│   │   │   ├── 0000000001.seg.zst # sealed, immutable
│   │   │   ├── 0000000002.seg.zst
│   │   │   └── 0000000003.open    # current tail, plaintext, append-only
│   │   └── streams/<name>/
│   │       ├── 0000000001.seg.zst
│   │       └── 0000000002.open
│   └── REPLICA                    # 16-byte file, format version + replica id
└── local/<replica-id>/            # NOT synced (.stignore)
    ├── read-cursors/
    │   ├── ops/<peer-id>.bin
    │   └── streams/<name>/<peer-id>.bin
    ├── seal-state.bin             # local segment counter, in-flight seal info
    └── decode-cache/              # optional: decompressed sealed segments
```

`.stignore` keeps `local/`, `*.tmp`, and `*.part` unsynced (unchanged).

### 3.1 Why this shape

- **One open file per stream** — Syncthing block-diffs append-only writes
  efficiently. Tens of small appends transfer as block deltas, not
  rewrites.
- **Sealed segments are content-final** — Syncthing transfers each once
  and never touches it again. Cumulative bytes-on-wire grows linearly with
  *new* content, not with edit count.
- **No synced `sequences.json`** — sealed-vs-open is encoded in filename
  suffix (`.seg.zst` vs `.open`); ordering is encoded in the monotonic
  prefix; per-peer read state lives in `local/` exclusively.
- **`state.json` is the only atomic-rename file in the synced root** —
  throttled to ≥250 ms and ≥1 keepalive per 25 s.

### 3.2 File-count budget

For a session producing N bytes of payload across S streams:

```
files_created ≈ S + ceil(total_payload_bytes / 64KiB)
files_modified per cycle ≤ S + 1     (S open tails + state.json)
```

For "a few dozen words" (S=1 ops + maybe S=1 chat, total payload <64 KiB):
**≤6 files created, ≤2 modified per cycle.**

## 4. Segment Format

Open and sealed segments share the same on-disk record stream. Sealing is
"stop appending, fsync, rename, then write a zstd-wrapped copy."

### 4.1 Open segment (`<N>.open`)

Append-only, plaintext, line-delimited:

```
<base64-payload>\n
<base64-payload>\n
...
```

Payload is the existing JSON-encoded `Operation` or `StreamEntry`,
base64-encoded so any record is exactly one line and no JSON escaping
crosses record boundaries. A torn write at the end of the file is
recoverable by truncating to the last `\n`.

Plaintext-on-tail is deliberate: a crash mid-write leaves a file readable
by humans and dumb recovery tools.

### 4.2 Sealed segment (`<N>.seg.zst`)

A single zstd frame containing a fixed header followed by the same
line-stream payload as the open segment.

```
[ 4 bytes ] magic           "CTSG"
[ 1 byte  ] format version  0x01
[ 1 byte  ] stream kind     0x01=ops, 0x02=stream
[ 2 bytes ] flags           reserved, 0
[ 8 bytes ] first lamport   little-endian, lowest seq in segment
[ 8 bytes ] last lamport    little-endian, highest seq in segment
[ 4 bytes ] record count    little-endian
[ 32 bytes] sha256 of payload below
---- payload ----
<base64-payload>\n
<base64-payload>\n
...
```

The whole `header + payload` blob is zstd-frame-encoded as a single frame.
The reader streams the frame, validates magic and version, then reads
lines exactly like it does for an open segment.

### 4.3 Why these fields

- **Magic + version** — fail-fast on corruption or future format upgrades.
- **Stream kind** — debug/tooling discriminator; the data path uses the
  enclosing directory.
- **Flags** — reserved for future per-segment toggles (e.g., "binary
  encoding enabled" for Q5 option B in the design discussion).
- **First/last Lamport + record count** — peers can plan catch-up
  efficiently and snapshot/GC code (future) can decide segment coverage
  without decompressing the body.
- **sha256 of payload** — integrity check. Validated on decode; segment
  rejected if mismatched.

### 4.4 Sealing procedure

Each replica owns its own `log/<stream>/` so there is exactly one writer
per directory. Sealing is single-process, no cross-process race:

1. Stop new appends to `<N>.open`. New appends now land in `<N+1>.open`.
2. fsync `<N>.open`.
3. Decode the line stream of `<N>.open` to compute Lamport range + count
   + sha256 over the line bytes. (Cheap; we already have the in-memory
   ring.)
4. Write `<N>.seg.zst.tmp`: header + payload, zstd-compressed, fsync.
5. `rename(<N>.seg.zst.tmp, <N>.seg.zst)`.
6. `unlink(<N>.open)`.

Steps 4–6 are crash-safe in this order:

- Crash before 5: peers see only `<N>.open`. Recovery on next start
  retries the seal.
- Crash between 5 and 6: peers may briefly see both `<N>.seg.zst` and
  `<N>.open`. Readers always prefer `.seg.zst` for any N that has both
  (see §5.2). Recovery on next start unlinks the stale `<N>.open`.
- Stale `.tmp` from a crash before 5: cleaned up on startup.

### 4.5 Seal triggers

The writer seals the open segment when, on any `tick()`:

- open-segment size ≥ **64 KiB**, **or**
- newest-record age ≥ **30 s** **and** segment is non-empty, **or**
- `flush()` was called with a `force_seal=true` argument (used by
  shutdown).

Constants are defined as `static constexpr` in the writer; tests can
parameterize them via constructor injection.

## 5. Write and Read Paths

### 5.1 Writer

A new `SegmentWriter` owns one stream's `log/<name>/` directory. One
writer per (replica, stream).

```cpp
class SegmentWriter {
public:
    SegmentWriter(std::filesystem::path stream_dir,
                  StreamKind kind,
                  WriterConfig config = WriterConfig{});

    // Recovery: scan stream_dir, resume highest-N .open or start new.
    void start();

    // Append one record (caller passes raw payload bytes; writer
    // base64-encodes and adds a trailing \n).
    void append(std::string_view payload);

    // Drive flush + seal decisions. Caller passes monotonic time.
    void tick(std::chrono::steady_clock::time_point now);

    // Force fsync of in-memory ring + open segment.
    void flush();

    // Force seal of the current open segment (used by shutdown).
    void close();

    // Stats accessor for observability.
    SegmentStats stats() const;

private:
    bool should_flush_(std::chrono::steady_clock::time_point now) const;
    bool should_seal_(std::chrono::steady_clock::time_point now) const;
    void seal_current_();
    void open_next_();
};

struct WriterConfig {
    size_t flush_bytes        = 1024;       // ≥1 KiB pending → flush
    std::chrono::milliseconds flush_idle{250};
    size_t seal_bytes         = 64 * 1024;  // 64 KiB
    std::chrono::seconds seal_idle{30};
    int    zstd_level         = 3;
};
```

Append behavior:

- `append()` writes into an in-memory ring buffer. Does **not** touch
  disk.
- `tick()` flushes the ring to the open `.open` file when ≥1 KiB pending
  *or* ≥250 ms since first pending byte.
- `tick()` seals the current `.open` when the size or idle thresholds in
  §4.5 are met.
- `flush()` is what `CollabPane::shutdown()` and the save path call;
  guarantees fsync to disk before returning.
- `close()` is what shutdown calls; flushes, then seals.

### 5.2 Reader

A new `SegmentReader` owns one peer's `log/<name>/` view. One reader per
(local replica, peer, stream).

```cpp
class SegmentReader {
public:
    SegmentReader(std::filesystem::path peer_stream_dir,
                  std::filesystem::path local_cursor_path);

    // Recovery: load cursor file (or zero if missing).
    void start();

    // Returns base64-decoded payloads not yet consumed.
    // Advances cursor in memory. Caller must call commit() to persist.
    std::vector<std::string> read_new();

    // Atomic-rename the cursor file with the in-memory state.
    void commit();
};
```

Cursor file (binary, 24 bytes, atomic-rename via `.tmp`):

```
[ 8 bytes ] last_sealed_segment_consumed   N: all .seg.zst with id ≤ N done
[ 8 bytes ] open_segment_id                  M: id of currently-tracked .open
[ 8 bytes ] open_segment_bytes_read          byte offset into <M>.open
```

Read algorithm (one pass through one peer-stream):

1. List `peer_stream_dir`. Partition entries into sealed map
   `{N → path}` and at most one open segment.
2. For each sealed N where N > `last_sealed_segment_consumed`,
   ascending:
   - Stream-decompress, validate header magic + version + sha256.
   - Yield each base64-decoded line as a payload.
   - On success, set `last_sealed_segment_consumed = N`.
   - On failure, skip and log; cursor not advanced for this N (the
     segment may be in-flight from the writer or transport).
3. If an open segment `<M>.open` exists:
   - If `M ≤ last_sealed_segment_consumed`, the writer sealed it
     between polls (and step 2 already consumed it). Set
     `open_segment_id = 0`, `open_segment_bytes_read = 0`, skip.
   - If `M != open_segment_id` (writer rolled over to a new open segment
     while we weren't looking), reset `open_segment_bytes_read = 0`
     and set `open_segment_id = M`.
   - Open the file at `open_segment_bytes_read`. Read complete lines
     only; any partial trailing line is a write-in-flight and is left.
   - Yield decoded payloads. Update `open_segment_bytes_read = new_offset`.
4. Caller calls `commit()`; cursor file is atomic-renamed.

Reading is idempotent across resumes: if `commit()` did not run, the next
read repeats the work from the prior cursor — applied ops are
deduplicated by the CRDT engine via Lamport identity.

### 5.3 FileSync and StreamSync

`FileSync` and `StreamSync` keep their public APIs (`push_local_op`,
`poll`, `set_on_remote_ops` etc.) and become thin wrappers:

- One `SegmentWriter` per local stream (`ops`, plus one per registered
  side stream).
- One `SegmentReader` per (peer, stream) pair, lazily created on first
  observation of `replicas/<peer>/log/<stream>/`.
- Old hash-bucketing helpers (`hash_bucket_lamport`, `hash_bucket_string`,
  `bucket_hex`, sequences read/write) are removed.

### 5.4 Cross-replica ordering

Hash bucketing is gone. Within a single replica's stream, records are
strictly monotonic on disk (one writer). Cross-replica ordering remains
the CRDT engine's responsibility via Lamport timestamps at apply time.
The transport just delivers; the engine orders.

## 6. Presence and Ephemeral State

### 6.1 Combine into one file

`presence.json` and `ephemeral.json` collapse into
`replicas/<id>/state.json`. They are written on the same cadence by the
same code path and read together; the split was historical.

```json
{
  "schema": 1,
  "presence": {
    "identity_id": "clinton-a7f3b2",
    "device_name": "laptop-3a",
    "active": true,
    "last_heartbeat": "2026-04-30T15:42:01Z"
  },
  "ephemeral": {
    "seq": 4821,
    "timestamp": "2026-04-30T15:42:01Z",
    "cursor": { "anchor": "...", "head": "..." },
    "selection": null,
    "viewport": { "top": "...", "bottom": "..." },
    "follow": null,
    "color": "#3b82f6"
  }
}
```

Atomic-rename (`state.json.tmp` → `state.json`) preserved — Syncthing
handles renames; the cost is a full small-file resync on each rename, but
write frequency drops far enough that this is no longer the dominant
churn.

### 6.2 Write rules

1. **On real change** (cursor moved, selection changed, viewport
   scrolled, follow toggled, active flipped): write immediately, but
   rate-limited to ≥250 ms since last write.
2. **Heartbeat-only**: if no real change has happened in 25 s, write one
   keepalive (only `last_heartbeat` and `presence.timestamp` change).
   Liveness window is 30 s; 25 s gives a 5 s skew margin.
3. **Floor**: never more than one rewrite per 250 ms.
4. **Ceiling**: at most one rewrite per 25 s in pure-idle steady state.

Idle peers rewrite ~2.4 times per minute (was ~120). Actively-typing peers
rewrite ~4 times per second (was ~2 — but with two files; net I/O cuts in
half, file count visible to Syncthing per rewrite drops from 2 to 1).

### 6.3 API change

`PresenceManager` keeps the same external shape:

- `write_presence(Presence)` and `write_ephemeral(EphemeralState)` become
  internal updates against an in-memory combined state; an internal
  throttler decides when to actually rewrite `state.json`.
- New `flush_state()` forces an immediate rewrite (used by shutdown and
  save).
- `read_remote_presences()` and `read_remote_ephemerals()` keep their
  return shapes by projecting fields out of each peer's `state.json`.
- `depart()` flips `active=false` and forces a flush.

The data model (`Identity::Presence`, `Identity::EphemeralState`) and all
downstream signals (`remoteEphemeralChanged`, etc.) are unchanged.

## 7. Recovery

On `SegmentWriter::start()`:

1. List `log/<stream>/`.
2. Find highest sealed N. Find any `.tmp` file; delete.
3. Find any `.open` file. If its id is ≤ highest sealed N, the writer
   crashed mid-seal; delete the `.open`.
4. If a `.open` with id > highest sealed N exists, reopen append-mode;
   verify last byte is `\n` (truncate trailing partial line if not).
5. Else open a new `.open` with id = (highest sealed N) + 1.

On `SegmentReader::start()`:

1. Load cursor file. If missing or smaller than 24 bytes, treat as zero.
2. Future reads start from this state.

On `PresenceManager::start()`:

1. Read existing `state.json` if present (so we don't drop heartbeat
   continuity across restarts).
2. Subsequent writes are throttled per §6.2.

## 8. Testing

### 8.1 Unit tests

Library-level (`libs/collabtext/tests/`), Qt-free, in temp dirs:

- `test_segment_format` — round-trip encode/decode of a sealed segment;
  byte-layout regression; reject bad magic, bad version, bad sha256.
- `test_segment_writer` — append + tick batches; seal at size; seal at
  idle; recovery from torn `.open` (truncate to last `\n`); recovery
  when `.seg.zst.tmp` is left; recovery when `.open` exists with id ≤
  highest sealed; fsync ordering.
- `test_segment_reader` — read fresh sealed segments in order; read
  open-tail records; advance cursor across `commit()`; resume across a
  seal boundary; ignore partial trailing line; reject corrupt sealed
  segment without advancing cursor.
- `test_file_sync_segments` — full FileSync round-trip with two
  simulated replicas writing into the same shared dir; reads converge;
  file-count under fixed-workload assertion (e.g. 200 ops produce ≤8
  files per replica).
- `test_stream_sync_segments` — same for StreamSync.
- `test_state_throttling` — fake clock, drive presence/ephemeral
  updates, assert exact write counts under each rule (immediate on
  change, floor at 250 ms, keepalive at 25 s, depart forces flush).

### 8.2 Integration tests

- `test_collabpane_segments` (Qt-aware, headless): two `CollabPane`
  instances on a shared sidecar dir, gremlin runs for 2 simulated
  minutes, assert total file count in `replicas/<id>/log/` matches the
  formula in §3.2.

### 8.3 Manual

- Run gremlin for 2 minutes on a fresh sidecar against a real Syncthing
  folder; eyeball watch-event count drops vs master.

### 8.4 Performance budget

Verifiable claims (not aspirations):

- File count for "a few dozen words" session: ≤6 created, ≤2 modified.
- Bytes-on-disk for that same session: ≥10x reduction vs current.
- Cross-replica visibility: open-tail reader sees new ops within one
  poll cycle (500 ms today; unchanged).

## 9. Migration and Compatibility

Clean break.

- `manifest.json.format_version`: 1 → 2.
- On open, `format_version == 1` is rejected with an error pointing at
  the re-enrollment path: save the file as plaintext, delete the
  sidecar, run `Document::enableCollab()`, which builds a fresh
  v2 sidecar from the saved file.
- New sidecars get `format_version == 2`.

No read-old-format code path. Pre-1.0 dev-stage; one user; this is
acceptable per Q1 (clean slate) of brainstorming.

## 10. Observability

A `SegmentStats` struct accumulated by `SegmentWriter` and
`SegmentReader`, aggregated by `FileSync` / `StreamSync` and surfaced via
a debug-menu item in `CollabPane`:

- segments sealed / opened
- bytes written to open tails
- bytes written to sealed segments (post-zstd)
- segments read per peer
- sealed-segment decode failures (sha256 mismatch, bad header)
- `state.json` rewrites per minute
- fsync count

Off by default in release; gated behind a compile-time flag or a
runtime debug toggle.

## 11. Risks

1. **Open-segment readers seeing partial writes** — mitigated by reading
   complete lines only and the appender flushing whole base64-encoded
   lines. Verified by `test_segment_reader`.
2. **Crash mid-seal** — mitigated by the rename-after-fsync ordering and
   `.tmp` cleanup on startup. Verified by `test_segment_writer`.
3. **Clock skew breaking the 25 s keepalive** — heartbeat throttler runs
   off a monotonic timer; only the ISO-8601 timestamp inside the file
   uses system clock. Up to 5 s skew is absorbed by the 30 s liveness
   window. Larger skew already breaks `is_live` today; out of scope.
4. **Zstd as a new dep** — added via `find_package(zstd REQUIRED)` to
   `libs/collabtext/CMakeLists.txt`. Vendored fallback out of scope;
   fail-fast at configure time.
5. **Race between writer finishing seal and reader observing `.seg.zst`
   first** — peers preferring `.seg.zst` over `.open` for the same N
   handles this; both files briefly coexisting is harmless.

## 12. Out of Scope (this plan)

These are explicitly deferred to follow-up plans, but the layout was
designed to accommodate them with no further breakage:

- **Snapshots** — `snapshots/<lamport>-<replica>.snapshot` files. The
  `first_lamport` / `last_lamport` fields in segment headers let a
  snapshot writer compute coverage and a GC pass delete sealed segments
  whose entire range is below the watermark.
- **Distributed GC integration** — already implemented for ops in-engine;
  wiring it to delete-sealed-segments-and-old-snapshots is a separate
  plan.
- **Binary op encoding** — flips the `flags` bit in the segment header,
  changes what's inside the lines, no other change.
- **Direct channels** — `state.json` is the natural place to publish a
  channel offer; mechanical work, not coupled to this plan.
- **Virtual filesystem** — independent of the wire format; reads
  materialized text from `Buffer::text()` regardless.

## 13. Implementation Order (preview)

The plan will be detailed in the writing-plans step; sketch:

1. Add `find_package(zstd)` + zstd wrapper utility.
2. `SegmentFormat` (header struct, encode/decode).
3. `SegmentWriter` (open-tail appends, seal procedure, recovery).
4. `SegmentReader` (cursor file, sealed-then-open read pass).
5. Rewire `FileSync` to use writer + per-peer readers; delete bucketing
   code.
6. Rewire `StreamSync` symmetrically.
7. Collapse `PresenceManager` to combined `state.json` + throttler.
8. Wire `CollabPane::shutdown()` → flush + close on all writers + flush
   state.
9. Bump `manifest.json.format_version` to 2; reject v1 with a clear
   error.
10. Tests at each step (TDD per superpowers conventions).
11. Manual gremlin/Syncthing eyeballing as the last step.
