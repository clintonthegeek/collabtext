# StreamSync — Generic Side-Stream Transport

**Date:** 2026-04-13
**Status:** Approved, ready for implementation plan
**Related:** `docs/CRDT_SYNC_SPEC.md` (sections 15.2.1–15.2.4)

---

## 1. Problem

The CRDT sync spec defines persistent side streams (chat, comments,
bookmarks) that share the same file-based transport as document
operations — hash-bucketed append-only logs, per-bucket sequence
tracking, periodic polling. The engine currently implements this for
document operations only (`FileSync`). There is no transport layer for
side-stream data.

Chat is the first consumer, but comments and bookmarks are defined in
the spec and will follow. Building a generic stream transport now means
each future consumer is just a data model + widget on top.

## 2. Goals

1. **Generic multiplexed stream transport.** One `StreamSync` instance
   manages all named streams for a replica. Streams are registered
   dynamically.

2. **Two stream types.** `AppendOnly` (chat, activity log): entries are
   independent, ordered by Lamport timestamp, never edited or deleted.
   `AnchorKeyed` (comments, bookmarks): entries keyed by unique ID,
   LWW-merged by timestamp, tombstone-deletable.

3. **Extract shared sync utilities.** `FileSync` and `StreamSync` share
   the same low-level mechanics (bucket hashing, sequence files, bucket
   I/O). Extract these into a `SyncUtils` module so neither class
   duplicates them.

4. **No CRDT dependency.** `StreamSync` does not depend on `Buffer` or
   `Operations`. It transports opaque payloads; consumers handle
   semantics.

## 3. Non-goals

- **Stream registry file (`meta/streams.json`).** The spec defines this
  for discovery. We defer it — consumers register streams in code for
  now. The file can be added later without changing StreamSync's API.

- **GC of stream entries.** Document operation GC is already complex.
  Stream entries accumulate on disk. This is fine for the foreseeable
  scale; GC is a separate sub-project.

- **Chat widget or any UI.** This spec covers the transport layer only.
  The chat data model and widget are sub-project 2.

## 4. Architecture

Three new files, one refactored file:

| Component | Responsibility |
|-----------|---------------|
| `SyncUtils` | Shared utilities: bucket hashing, hex formatting, sequence file I/O, raw line I/O for bucket files. |
| `StreamSync` | Multiplexed stream transport: register streams, push local entries, poll remote entries, return merged views. |
| `StreamSerialization` | Encode/decode `StreamEntry` to/from JSON lines. |
| `FileSync` (refactored) | Delegates to `SyncUtils` for bucket/sequence I/O. No API changes. |

## 5. SyncUtils

Extracted from `FileSync`'s private methods into a standalone
header/implementation pair. All functions are in the
`CollabText::Crdt::SyncUtils` namespace (or a struct with static
methods).

```cpp
namespace SyncUtils {

/// Format a bucket number (0–255) as a 2-character hex string.
std::string bucket_hex(uint8_t bucket);

/// Compute hash bucket from a Lamport-like (replica_id, seq) pair.
/// Used by FileSync (document ops) and StreamSync (append-only streams).
uint8_t hash_bucket_lamport(uint16_t replica_id, uint64_t seq);

/// Compute hash bucket from an arbitrary string key.
/// Used by StreamSync for anchor-keyed streams.
uint8_t hash_bucket_string(const std::string& key);

/// Read a sequences.json file. Returns {bucket_hex -> sequence_number}.
/// Returns empty map if the file doesn't exist or can't be parsed.
std::unordered_map<std::string, uint64_t>
read_sequences(const std::filesystem::path& path);

/// Write a sequences.json file atomically (write to .tmp, rename).
void write_sequences(const std::filesystem::path& path,
                     const std::unordered_map<std::string, uint64_t>& seqs);

/// Read raw lines from a bucket file starting after a byte offset.
/// Returns {lines, new_byte_offset}. Skips empty lines.
std::pair<std::vector<std::string>, std::streamsize>
read_lines_after(const std::filesystem::path& path, std::streamsize after_byte);

/// Append data to a bucket file (create if needed).
void append_to_bucket(const std::filesystem::path& path, const std::string& data);

} // namespace SyncUtils
```

### 5.1 Hash functions

`hash_bucket_lamport` is the existing `FileSync::hash_bucket` logic:

```cpp
uint8_t hash_bucket_lamport(uint16_t replica_id, uint64_t seq) {
    uint32_t h = replica_id * 199;
    return static_cast<uint8_t>((h + seq) % 256);
}
```

`hash_bucket_string` uses a polynomial hash matching the spec's §4.3
description for string keys:

```cpp
uint8_t hash_bucket_string(const std::string& key) {
    uint32_t h = 0;
    for (char c : key) h = h * 31 + static_cast<uint8_t>(c);
    return static_cast<uint8_t>(h % 256);
}
```

## 6. StreamEntry

```cpp
struct StreamEntry {
    std::string id;           // unique identifier for dedup
                              //   append-only: "replicaId-seq"
                              //   anchor-keyed: application-defined (e.g., UUID)
    uint16_t replica_id = 0;  // Lamport component (append-only ordering)
    uint64_t seq = 0;         // Lamport component (append-only ordering)
    std::string timestamp;    // ISO 8601 (used for LWW in anchor-keyed)
    std::string payload;      // opaque JSON — consumer's problem
    bool tombstone = false;   // anchor-keyed only: marks entry as deleted
};
```

StreamSync parses only the common fields. Everything type-specific
(body, author, anchor, reply_to for chat; range, resolved for comments)
lives in `payload` as opaque JSON that passes through untouched.

### 6.1 Serialization format

One JSON object per line in bucket files, mirroring FileSync's
line-delimited encoding:

```json
{"id":"1-42","r":1,"s":42,"ts":"2026-04-13T10:00:00Z","p":"{\"body\":\"hello\"}","t":false}
```

Short field names (`r`, `s`, `ts`, `p`, `t`) keep bucket files compact.
`encode_stream_entry` / `decode_stream_entry` live in
`StreamSerialization.h/.cpp`.

## 7. StreamSync

```cpp
class StreamSync {
public:
    enum class StreamType { AppendOnly, AnchorKeyed };

    StreamSync(const std::filesystem::path& shared_folder,
               const std::string& replica_name);

    /// Ensure directory structure exists. Call once before poll().
    void start();

    /// Register a named stream with the given type.
    /// Must be called before push() or entries() for that stream.
    void register_stream(const std::string& name, StreamType type);

    /// Write a local entry to a named stream.
    void push(const std::string& stream, const StreamEntry& entry);

    /// Sync cycle: flush pending writes, read remote entries.
    /// Returns total new entries ingested across all streams.
    size_t poll();

    /// Get the merged view of a stream's entries.
    ///
    /// AppendOnly: all entries from all replicas, deduped by id,
    /// sorted by Lamport order (seq ascending, replica_id as tiebreak).
    ///
    /// AnchorKeyed: grouped by id, latest timestamp wins (LWW),
    /// tombstoned entries excluded from the result.
    std::vector<StreamEntry> entries(const std::string& stream) const;

    /// Callback fired after poll() ingests new entries.
    using NewEntriesCallback =
        std::function<void(const std::string& stream, size_t count)>;
    void set_on_new_entries(NewEntriesCallback cb);
};
```

### 7.1 Internal state

Per registered stream, StreamSync maintains:

- **`StreamType`** — determines hash function and merge semantics.
- **`std::vector<StreamEntry> m_pending`** — local entries not yet
  flushed to disk.
- **`std::unordered_map<std::string, uint64_t> m_local_sequences`** —
  per-bucket write counters for this stream.
- **`std::unordered_map<std::string, StreamEntry> m_entries`** — the
  merged entry set (all replicas). For append-only this is keyed by
  `id` for dedup. For anchor-keyed this is keyed by `id` with LWW
  applied.
- **Per-peer read state** — `{peer_name -> {bucket_hex -> bytes_read}}`
  per stream, same pattern as FileSync.

### 7.2 Storage layout

```
replicas/<replica-name>/streams/<stream-name>/
  00 .. ff              # hash-bucketed, newline-delimited JSON entries
  sequences.json        # {bucket_hex: write_count}
```

`start()` creates `streams/` directories for all registered streams.
`register_stream()` called after `start()` creates the directory lazily.

### 7.3 Push flow

1. Compute hash bucket:
   - AppendOnly: `hash_bucket_lamport(entry.replica_id, entry.seq)`
   - AnchorKeyed: `hash_bucket_string(entry.id)`
2. Append to `m_pending[stream]`.

### 7.4 Poll flow

1. **Flush** — For each stream with pending entries, group by bucket,
   `append_to_bucket()`, update `m_local_sequences`, write
   `sequences.json`.
2. **Read remote** — For each peer, for each registered stream, read
   peer's `sequences.json`, compare with our read state, read new
   lines from changed buckets, decode, merge into `m_entries`.
3. **Merge** — Append-only: insert if `id` not seen. Anchor-keyed:
   insert or replace if timestamp is newer; remove if tombstoned.
4. Fire `NewEntriesCallback` for each stream that got new entries.

### 7.5 entries() semantics

- **AppendOnly**: Collect all values from `m_entries[stream]`, sort by
  `(seq, replica_id)`. This is Lamport order — lower seq first, with
  replica_id as a deterministic tiebreak for concurrent entries.

- **AnchorKeyed**: Collect all non-tombstoned values from
  `m_entries[stream]`, sorted by timestamp (newest first) or by
  insertion order — the consumer decides display order.

## 8. FileSync refactoring

`FileSync` delegates to `SyncUtils` for the shared mechanics. No public
API changes — callers are unaffected.

| FileSync private method | Replaced by |
|------------------------|-------------|
| `hash_bucket(const Lamport&)` | `SyncUtils::hash_bucket_lamport(ts.replica_id, ts.value)` |
| `bucket_hex(uint8_t)` | `SyncUtils::bucket_hex(bucket)` |
| `read_sequences(path)` | `SyncUtils::read_sequences(path)` |
| `write_sequences(path, seqs)` | `SyncUtils::write_sequences(path, seqs)` |
| `read_bucket_file(path, after)` | `SyncUtils::read_lines_after(path, after)` + local `decode_operation()` |
| (inline append in flush) | `SyncUtils::append_to_bucket(path, data)` |

`read_bucket_file` is the only one that changes shape: it currently
returns decoded `vector<Operation>`. After refactoring, FileSync calls
`read_lines_after()` to get raw strings, then decodes each line with
`decode_operation()` itself.

## 9. Edge cases

| Case | Behaviour |
|------|-----------|
| Push to unregistered stream | Assertion failure (programming error). |
| Remote replica has a stream we don't have registered | Ignored silently. We only read streams we've registered. |
| Duplicate entry (same id) in append-only | Deduped by id. Second copy is silently dropped. |
| LWW tie (same timestamp) in anchor-keyed | Deterministic tiebreak by `id` string comparison. |
| Tombstone for nonexistent entry in anchor-keyed | Tombstone is stored; if the entry arrives later it's immediately suppressed. |
| Empty payload | Valid. Some entries may carry no payload. |
| poll() before start() | Returns 0, same as FileSync. |

## 10. Testing

New test file: `libs/collabtext/tests/tst_stream_sync.cpp`.

1. **Append-only round-trip.** Two StreamSync instances sharing a temp
   folder. Replica A pushes 5 chat entries, replica B polls and reads
   them. Verify all 5 arrive, in Lamport order, with correct payloads.

2. **Append-only dedup.** Push the same entry twice (same id). Verify
   `entries()` returns it once.

3. **Anchor-keyed LWW merge.** Push two entries with the same `id` but
   different timestamps. Verify `entries()` returns the one with the
   later timestamp.

4. **Anchor-keyed tombstone.** Push an entry, then push a tombstone with
   the same `id` and a later timestamp. Verify `entries()` excludes it.

5. **Multi-stream isolation.** Register "chat" (append-only) and
   "comments" (anchor-keyed). Push entries to both. Verify
   `entries("chat")` contains no comment entries and vice versa.

6. **StreamEntry serialization round-trip.** Encode a StreamEntry,
   decode it, verify all fields match.

7. **FileSync regression.** The existing `tst_filesync` tests pass
   unchanged after the SyncUtils extraction.

## 11. File inventory

| File | Change |
|------|--------|
| `libs/collabtext/src/crdt/SyncUtils.h` | **New.** Shared utility declarations. |
| `libs/collabtext/src/crdt/SyncUtils.cpp` | **New.** Shared utility implementations (extracted from FileSync). |
| `libs/collabtext/src/crdt/StreamSync.h` | **New.** StreamSync class + StreamEntry struct. |
| `libs/collabtext/src/crdt/StreamSync.cpp` | **New.** StreamSync implementation. |
| `libs/collabtext/src/crdt/StreamSerialization.h` | **New.** encode/decode StreamEntry. |
| `libs/collabtext/src/crdt/StreamSerialization.cpp` | **New.** Implementation. |
| `libs/collabtext/src/crdt/FileSync.h` | **Modify.** Remove private utility declarations. |
| `libs/collabtext/src/crdt/FileSync.cpp` | **Modify.** Delegate to SyncUtils; simplify read_bucket_file. |
| `libs/collabtext/tests/tst_stream_sync.cpp` | **New.** Tests 1–6 above. |
| `libs/collabtext/CMakeLists.txt` | Register new sources + test target. |

No changes to Buffer, Operations, Anchor, or any UI code.

## 12. Success criteria

- All new StreamSync tests pass.
- Existing `tst_filesync` passes unchanged (SyncUtils extraction is
  transparent).
- Full `ctest` suite remains green.
- No new dependencies introduced.
