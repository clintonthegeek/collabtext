# StreamSync Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a generic multiplexed side-stream transport layer (StreamSync) and extract shared sync utilities from FileSync.

**Architecture:** Extract bucket hashing, sequence I/O, and line I/O from FileSync into a shared SyncUtils module. Build StreamSync on top of SyncUtils to transport opaque StreamEntry payloads through the same hash-bucketed file structure. Two stream types: AppendOnly (Lamport-ordered, deduped) and AnchorKeyed (LWW-merged, tombstone-deletable).

**Tech Stack:** C++20, Qt6 Test framework, filesystem-based sync (no network)

**Spec:** `docs/superpowers/specs/2026-04-13-stream-sync-design.md`

---

### Task 1: Extract SyncUtils from FileSync

**Files:**
- Create: `libs/collabtext/src/crdt/SyncUtils.h`
- Create: `libs/collabtext/src/crdt/SyncUtils.cpp`
- Modify: `libs/collabtext/src/crdt/FileSync.h`
- Modify: `libs/collabtext/src/crdt/FileSync.cpp`
- Modify: `libs/collabtext/CMakeLists.txt`

- [ ] **Step 1: Create SyncUtils.h**

```cpp
// libs/collabtext/src/crdt/SyncUtils.h
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace CollabText::Crdt::SyncUtils {

/// Format a bucket number (0-255) as a 2-character hex string.
std::string bucket_hex(uint8_t bucket);

/// Compute hash bucket from a Lamport-like (replica_id, seq) pair.
uint8_t hash_bucket_lamport(uint16_t replica_id, uint64_t seq);

/// Compute hash bucket from an arbitrary string key.
uint8_t hash_bucket_string(const std::string& key);

/// Read a sequences.json file. Returns {bucket_hex -> sequence_number}.
std::unordered_map<std::string, uint64_t>
read_sequences(const std::filesystem::path& path);

/// Write a sequences.json file atomically (write .tmp, rename).
void write_sequences(const std::filesystem::path& path,
                     const std::unordered_map<std::string, uint64_t>& seqs);

/// Read raw lines from a file starting after a byte offset.
/// Returns {lines, new_byte_offset}. Skips empty lines.
std::pair<std::vector<std::string>, std::streamsize>
read_lines_after(const std::filesystem::path& path, std::streamsize after_byte);

/// Append data to a file (create if needed). Flushes immediately.
void append_to_bucket(const std::filesystem::path& path, const std::string& data);

} // namespace CollabText::Crdt::SyncUtils
```

- [ ] **Step 2: Create SyncUtils.cpp**

Move the implementations from `FileSync.cpp`. The bodies are identical to the existing FileSync private methods, just in the new namespace.

```cpp
// libs/collabtext/src/crdt/SyncUtils.cpp
#include "crdt/SyncUtils.h"

#include <algorithm>
#include <fstream>

namespace fs = std::filesystem;

namespace CollabText::Crdt::SyncUtils {

std::string bucket_hex(uint8_t bucket) {
    static const char hex[] = "0123456789abcdef";
    return {hex[bucket >> 4], hex[bucket & 0x0F]};
}

uint8_t hash_bucket_lamport(uint16_t replica_id, uint64_t seq) {
    uint32_t h = replica_id * 199;
    return static_cast<uint8_t>((h + seq) % 256);
}

uint8_t hash_bucket_string(const std::string& key) {
    uint32_t h = 0;
    for (char c : key)
        h = h * 31 + static_cast<uint8_t>(c);
    return static_cast<uint8_t>(h % 256);
}

std::unordered_map<std::string, uint64_t>
read_sequences(const fs::path& path) {
    std::unordered_map<std::string, uint64_t> result;
    if (!fs::exists(path)) return result;

    std::ifstream f(path);
    if (!f.is_open()) return result;

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    size_t pos = content.find('{');
    if (pos == std::string::npos) return result;
    ++pos;

    while (pos < content.size()) {
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == ','
               || content[pos] == '\n' || content[pos] == '\r' || content[pos] == '\t'))
            ++pos;
        if (pos >= content.size() || content[pos] == '}') break;
        if (content[pos] != '"') break;
        ++pos;
        size_t key_start = pos;
        while (pos < content.size() && content[pos] != '"') ++pos;
        std::string key = content.substr(key_start, pos - key_start);
        ++pos;
        while (pos < content.size() && content[pos] != ':') ++pos;
        ++pos;
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t'))
            ++pos;
        size_t num_start = pos;
        while (pos < content.size() && content[pos] >= '0' && content[pos] <= '9')
            ++pos;
        if (pos > num_start)
            result[key] = std::stoull(content.substr(num_start, pos - num_start));
    }
    return result;
}

void write_sequences(const fs::path& path,
                     const std::unordered_map<std::string, uint64_t>& seqs) {
    auto tmp_path = path;
    tmp_path += ".tmp";
    {
        std::ofstream f(tmp_path);
        f << '{';
        bool first = true;
        std::vector<std::string> keys;
        keys.reserve(seqs.size());
        for (auto& [k, _] : seqs) keys.push_back(k);
        std::sort(keys.begin(), keys.end());
        for (auto& k : keys) {
            if (!first) f << ',';
            f << '"' << k << '"' << ':' << seqs.at(k);
            first = false;
        }
        f << '}';
    }
    fs::rename(tmp_path, path);
}

std::pair<std::vector<std::string>, std::streamsize>
read_lines_after(const fs::path& path, std::streamsize after_byte) {
    std::vector<std::string> lines;
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {lines, after_byte};

    f.seekg(after_byte);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty()) continue;
        lines.push_back(std::move(line));
    }

    std::streamsize new_offset;
    if (f.eof()) {
        f.clear();
        f.seekg(0, std::ios::end);
        new_offset = f.tellg();
    } else {
        new_offset = f.tellg();
    }
    return {std::move(lines), new_offset};
}

void append_to_bucket(const fs::path& path, const std::string& data) {
    std::ofstream f(path, std::ios::app | std::ios::binary);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    f.flush();
}

} // namespace CollabText::Crdt::SyncUtils
```

- [ ] **Step 3: Refactor FileSync.h — remove extracted declarations**

Remove these private declarations from `FileSync.h`:
- `static uint8_t hash_bucket(const Lamport& ts);`
- `static std::string bucket_hex(uint8_t bucket);`
- `static std::unordered_map<std::string, uint64_t> read_sequences(const std::filesystem::path& path);`
- `static void write_sequences(const std::filesystem::path& path, const std::unordered_map<std::string, uint64_t>& seqs);`
- `static std::pair<std::vector<Operation>, std::streamsize> read_bucket_file(const std::filesystem::path& path, std::streamsize after_byte);`

Add `#include "crdt/SyncUtils.h"` to FileSync.cpp (not the header — keep it an implementation detail).

- [ ] **Step 4: Refactor FileSync.cpp — delegate to SyncUtils**

Replace `flush_local_ops()`:

```cpp
void FileSync::flush_local_ops() {
    if (m_pending_ops.empty()) return;

    auto ops_dir = m_shared_folder / "replicas" / m_replica_name / "ops";

    std::unordered_map<std::string, std::string> bucket_data;
    for (auto& op : m_pending_ops) {
        Lamport ts;
        if (auto* e = std::get_if<EditOperation>(&op))
            ts = e->timestamp;
        else if (auto* u = std::get_if<UndoOperation>(&op))
            ts = u->timestamp;

        std::string bkt = SyncUtils::bucket_hex(
            SyncUtils::hash_bucket_lamport(ts.replica_id, ts.value));
        bucket_data[bkt] += encode_operation(op) + '\n';
    }
    m_pending_ops.clear();

    for (auto& [bkt, data] : bucket_data) {
        SyncUtils::append_to_bucket(ops_dir / bkt, data);
        m_local_sequences[bkt]++;
    }

    auto seq_path = m_shared_folder / "replicas" / m_replica_name / "sequences.json";
    SyncUtils::write_sequences(seq_path, m_local_sequences);
}
```

Replace `start()` to use `SyncUtils::read_sequences`:

```cpp
void FileSync::start() {
    ensure_directory_structure();
    auto seq_path = m_shared_folder / "replicas" / m_replica_name / "sequences.json";
    m_local_sequences = SyncUtils::read_sequences(seq_path);
    m_started = true;
}
```

Replace `read_remote_ops()` — use `SyncUtils::read_sequences`, `SyncUtils::read_lines_after`, and local decoding:

```cpp
size_t FileSync::read_remote_ops() {
    auto replicas_dir = m_shared_folder / "replicas";
    if (!fs::exists(replicas_dir) || !fs::is_directory(replicas_dir))
        return 0;

    size_t total_applied = 0;

    for (auto& entry : fs::directory_iterator(replicas_dir)) {
        if (!entry.is_directory()) continue;
        std::string peer_name = entry.path().filename().string();
        if (peer_name == m_replica_name) continue;

        auto peer_seq_path = entry.path() / "sequences.json";
        auto peer_seqs = SyncUtils::read_sequences(peer_seq_path);
        if (peer_seqs.empty()) continue;

        auto& read_state = m_peer_read_state[peer_name];

        for (auto& [bkt, seq] : peer_seqs) {
            auto bucket_path = entry.path() / "ops" / bkt;
            if (!fs::exists(bucket_path)) continue;

            std::streamsize already_read = read_state.bucket_bytes[bkt];
            auto file_size = static_cast<std::streamsize>(fs::file_size(bucket_path));
            if (file_size <= already_read) continue;

            auto [lines, new_offset] = SyncUtils::read_lines_after(bucket_path, already_read);
            read_state.bucket_bytes[bkt] = new_offset;

            std::vector<Operation> ops;
            for (auto& line : lines) {
                auto op = decode_operation(line);
                if (op) ops.push_back(std::move(*op));
            }

            if (!ops.empty()) {
                m_buffer.apply_ops(ops);
                total_applied += ops.size();
            }
        }
    }

    if (total_applied > 0 && m_on_remote_ops)
        m_on_remote_ops(total_applied);

    return total_applied;
}
```

Remove the old static method implementations (`hash_bucket`, `bucket_hex`, `read_sequences`, `write_sequences`, `read_bucket_file`) from the bottom of FileSync.cpp.

- [ ] **Step 5: Add SyncUtils.cpp to CMakeLists.txt**

In `libs/collabtext/CMakeLists.txt`, add `src/crdt/SyncUtils.cpp` to the `add_library(collabtext STATIC ...)` list, after `src/crdt/FileSync.cpp`.

- [ ] **Step 6: Build and run tst_filesync**

Run: `cmake --build build-dev -j$(($(nproc)-1)) && ctest --test-dir build-dev --output-on-failure -R tst_filesync`

Expected: Build succeeds, `tst_filesync` passes (all 9 test cases). This confirms the extraction is transparent.

- [ ] **Step 7: Commit**

```
git add libs/collabtext/src/crdt/SyncUtils.h libs/collabtext/src/crdt/SyncUtils.cpp \
       libs/collabtext/src/crdt/FileSync.h libs/collabtext/src/crdt/FileSync.cpp \
       libs/collabtext/CMakeLists.txt
git commit -m "refactor: extract SyncUtils from FileSync for shared bucket/sequence I/O"
```

---

### Task 2: StreamEntry struct and StreamSerialization

**Files:**
- Create: `libs/collabtext/src/crdt/StreamSync.h` (StreamEntry struct only for now)
- Create: `libs/collabtext/src/crdt/StreamSerialization.h`
- Create: `libs/collabtext/src/crdt/StreamSerialization.cpp`
- Create: `libs/collabtext/tests/tst_stream_sync.cpp`
- Modify: `libs/collabtext/CMakeLists.txt`

- [ ] **Step 1: Create StreamSync.h with StreamEntry struct**

```cpp
// libs/collabtext/src/crdt/StreamSync.h
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace CollabText::Crdt {

struct StreamEntry {
    std::string id;           // unique identifier for dedup
    uint16_t replica_id = 0;  // Lamport component (append-only ordering)
    uint64_t seq = 0;         // Lamport component (append-only ordering)
    std::string timestamp;    // ISO 8601 (LWW key for anchor-keyed)
    std::string payload;      // opaque JSON — consumer's problem
    bool tombstone = false;   // anchor-keyed only
};

} // namespace CollabText::Crdt
```

- [ ] **Step 2: Create StreamSerialization.h**

```cpp
// libs/collabtext/src/crdt/StreamSerialization.h
#pragma once

#include "crdt/StreamSync.h"

#include <optional>
#include <string>
#include <string_view>

namespace CollabText::Crdt {

/// Encode a StreamEntry as a single-line JSON string (no trailing newline).
/// Format: {"id":"...","r":N,"s":N,"ts":"...","p":"...","t":bool}
std::string encode_stream_entry(const StreamEntry& entry);

/// Decode a single-line JSON string into a StreamEntry.
/// Returns nullopt on parse failure.
std::optional<StreamEntry> decode_stream_entry(std::string_view json);

} // namespace CollabText::Crdt
```

- [ ] **Step 3: Create StreamSerialization.cpp**

```cpp
// libs/collabtext/src/crdt/StreamSerialization.cpp
#include "crdt/StreamSerialization.h"

#include <charconv>

namespace CollabText::Crdt {

// Reuse the same JSON string escaping pattern as Serialization.cpp.
static std::string escape_json(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
    return out;
}

std::string encode_stream_entry(const StreamEntry& e) {
    std::string out = "{\"id\":";
    out += escape_json(e.id);
    out += ",\"r\":" + std::to_string(e.replica_id);
    out += ",\"s\":" + std::to_string(e.seq);
    out += ",\"ts\":";
    out += escape_json(e.timestamp);
    out += ",\"p\":";
    out += escape_json(e.payload);
    if (e.tombstone)
        out += ",\"t\":true";
    out += '}';
    return out;
}

// ---- Minimal JSON parser (fixed schema) ----

static std::string_view skip_ws(std::string_view s) {
    while (!s.empty() && (s[0] == ' ' || s[0] == '\t' || s[0] == '\n' || s[0] == '\r'))
        s.remove_prefix(1);
    return s;
}

static bool consume(std::string_view& s, char c) {
    s = skip_ws(s);
    if (s.empty() || s[0] != c) return false;
    s.remove_prefix(1);
    return true;
}

static std::optional<std::string> parse_string(std::string_view& s) {
    s = skip_ws(s);
    if (s.empty() || s[0] != '"') return std::nullopt;
    s.remove_prefix(1);
    std::string out;
    while (!s.empty() && s[0] != '"') {
        if (s[0] == '\\' && s.size() >= 2) {
            switch (s[1]) {
                case '"':  out += '"'; break;
                case '\\': out += '\\'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    if (s.size() >= 6) {
                        unsigned val = 0;
                        for (int i = 2; i < 6; ++i) {
                            val <<= 4;
                            char ch = s[i];
                            if (ch >= '0' && ch <= '9') val += ch - '0';
                            else if (ch >= 'a' && ch <= 'f') val += 10 + ch - 'a';
                            else if (ch >= 'A' && ch <= 'F') val += 10 + ch - 'A';
                        }
                        out += static_cast<char>(val);
                        s.remove_prefix(6);
                        continue;
                    }
                    break;
                }
                default: out += s[1]; break;
            }
            s.remove_prefix(2);
        } else {
            out += s[0];
            s.remove_prefix(1);
        }
    }
    if (s.empty()) return std::nullopt;
    s.remove_prefix(1); // closing quote
    return out;
}

static std::optional<uint64_t> parse_uint(std::string_view& s) {
    s = skip_ws(s);
    if (s.empty() || s[0] < '0' || s[0] > '9') return std::nullopt;
    uint64_t val = 0;
    while (!s.empty() && s[0] >= '0' && s[0] <= '9') {
        val = val * 10 + (s[0] - '0');
        s.remove_prefix(1);
    }
    return val;
}

static std::optional<bool> parse_bool(std::string_view& s) {
    s = skip_ws(s);
    if (s.size() >= 4 && s.substr(0, 4) == "true") {
        s.remove_prefix(4);
        return true;
    }
    if (s.size() >= 5 && s.substr(0, 5) == "false") {
        s.remove_prefix(5);
        return false;
    }
    return std::nullopt;
}

std::optional<StreamEntry> decode_stream_entry(std::string_view json) {
    auto s = json;
    if (!consume(s, '{')) return std::nullopt;

    StreamEntry e;
    bool got_id = false;

    while (true) {
        s = skip_ws(s);
        if (!s.empty() && s[0] == '}') break;

        auto key = parse_string(s);
        if (!key) return std::nullopt;
        if (!consume(s, ':')) return std::nullopt;

        if (*key == "id") {
            auto val = parse_string(s);
            if (!val) return std::nullopt;
            e.id = std::move(*val);
            got_id = true;
        } else if (*key == "r") {
            auto val = parse_uint(s);
            if (!val) return std::nullopt;
            e.replica_id = static_cast<uint16_t>(*val);
        } else if (*key == "s") {
            auto val = parse_uint(s);
            if (!val) return std::nullopt;
            e.seq = *val;
        } else if (*key == "ts") {
            auto val = parse_string(s);
            if (!val) return std::nullopt;
            e.timestamp = std::move(*val);
        } else if (*key == "p") {
            auto val = parse_string(s);
            if (!val) return std::nullopt;
            e.payload = std::move(*val);
        } else if (*key == "t") {
            auto val = parse_bool(s);
            if (!val) return std::nullopt;
            e.tombstone = *val;
        } else {
            // Skip unknown value — consume string, number, or bool
            s = skip_ws(s);
            if (!s.empty() && s[0] == '"') {
                parse_string(s);
            } else if (!s.empty() && (s[0] >= '0' && s[0] <= '9')) {
                parse_uint(s);
            } else {
                parse_bool(s);
            }
        }

        s = skip_ws(s);
        if (!s.empty() && s[0] == ',') s.remove_prefix(1);
    }

    if (!got_id) return std::nullopt;
    return e;
}

} // namespace CollabText::Crdt
```

- [ ] **Step 4: Write the serialization round-trip test**

Create `libs/collabtext/tests/tst_stream_sync.cpp`:

```cpp
// libs/collabtext/tests/tst_stream_sync.cpp
#include <QTest>
#include <QTemporaryDir>

#include "crdt/StreamSync.h"
#include "crdt/StreamSerialization.h"

using namespace CollabText::Crdt;

class TestStreamSync : public QObject {
    Q_OBJECT

private slots:
    void stream_entry_serialization_round_trip() {
        StreamEntry e;
        e.id = "1-42";
        e.replica_id = 1;
        e.seq = 42;
        e.timestamp = "2026-04-13T10:00:00Z";
        e.payload = R"({"body":"hello world","author":"alice"})";
        e.tombstone = false;

        std::string json = encode_stream_entry(e);
        auto decoded = decode_stream_entry(json);
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->id, e.id);
        QCOMPARE(decoded->replica_id, e.replica_id);
        QCOMPARE(decoded->seq, e.seq);
        QCOMPARE(decoded->timestamp, e.timestamp);
        QCOMPARE(decoded->payload, e.payload);
        QCOMPARE(decoded->tombstone, e.tombstone);
    }

    void stream_entry_serialization_with_tombstone() {
        StreamEntry e;
        e.id = "comment-uuid-1";
        e.replica_id = 2;
        e.seq = 100;
        e.timestamp = "2026-04-13T11:00:00Z";
        e.payload = "";
        e.tombstone = true;

        std::string json = encode_stream_entry(e);
        auto decoded = decode_stream_entry(json);
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->id, e.id);
        QCOMPARE(decoded->tombstone, true);
    }

    void stream_entry_payload_with_special_chars() {
        StreamEntry e;
        e.id = "1-1";
        e.replica_id = 1;
        e.seq = 1;
        e.timestamp = "2026-04-13T10:00:00Z";
        e.payload = R"({"body":"line1\nline2\ttab\"quoted\""})";

        std::string json = encode_stream_entry(e);
        auto decoded = decode_stream_entry(json);
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->payload, e.payload);
    }
};

QTEST_MAIN(TestStreamSync)
#include "tst_stream_sync.moc"
```

- [ ] **Step 5: Register new sources and test in CMakeLists.txt**

In `libs/collabtext/CMakeLists.txt`:

Add to `add_library(collabtext STATIC ...)`:
```
    src/crdt/StreamSerialization.cpp
```

Add at the bottom:
```
add_crdt_test(tst_stream_sync)
```

- [ ] **Step 6: Build and run the serialization test**

Run: `cmake --build build-dev -j$(($(nproc)-1)) && ctest --test-dir build-dev --output-on-failure -R tst_stream_sync`

Expected: 3 test cases pass.

- [ ] **Step 7: Commit**

```
git add libs/collabtext/src/crdt/StreamSync.h \
       libs/collabtext/src/crdt/StreamSerialization.h \
       libs/collabtext/src/crdt/StreamSerialization.cpp \
       libs/collabtext/tests/tst_stream_sync.cpp \
       libs/collabtext/CMakeLists.txt
git commit -m "feat: StreamEntry struct and StreamSerialization encode/decode"
```

---

### Task 3: StreamSync class — registration, push, flush, and poll

**Files:**
- Modify: `libs/collabtext/src/crdt/StreamSync.h` (add StreamSync class)
- Create: `libs/collabtext/src/crdt/StreamSync.cpp`
- Modify: `libs/collabtext/tests/tst_stream_sync.cpp` (add round-trip test)
- Modify: `libs/collabtext/CMakeLists.txt`

- [ ] **Step 1: Write the append-only round-trip test**

Add to `tst_stream_sync.cpp`:

```cpp
    void append_only_round_trip() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        std::filesystem::path shared = tmp.path().toStdString();

        StreamSync syncA(shared, "replica-A");
        StreamSync syncB(shared, "replica-B");
        syncA.register_stream("chat", StreamSync::StreamType::AppendOnly);
        syncB.register_stream("chat", StreamSync::StreamType::AppendOnly);
        syncA.start();
        syncB.start();

        // A pushes 5 entries
        for (uint64_t i = 1; i <= 5; ++i) {
            StreamEntry e;
            e.id = "1-" + std::to_string(i);
            e.replica_id = 1;
            e.seq = i;
            e.timestamp = "2026-04-13T10:00:0" + std::to_string(i) + "Z";
            e.payload = R"({"body":"msg)" + std::to_string(i) + R"("})";
            syncA.push("chat", e);
        }
        syncA.poll();  // flush to disk

        // B polls and reads
        size_t applied = syncB.poll();
        QCOMPARE(applied, size_t(5));

        auto entries = syncB.entries("chat");
        QCOMPARE(entries.size(), size_t(5));

        // Verify Lamport ordering (seq ascending)
        for (size_t i = 0; i < entries.size(); ++i) {
            QCOMPARE(entries[i].seq, uint64_t(i + 1));
        }

        // Verify payloads survived
        QVERIFY(entries[0].payload.find("msg1") != std::string::npos);
        QVERIFY(entries[4].payload.find("msg5") != std::string::npos);
    }
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build-dev -j$(($(nproc)-1)) 2>&1 | tail -5`

Expected: Compile error — `StreamSync` class not defined yet.

- [ ] **Step 3: Add StreamSync class declaration to StreamSync.h**

Append to `libs/collabtext/src/crdt/StreamSync.h`, after the `StreamEntry` struct:

```cpp
class StreamSync {
public:
    enum class StreamType { AppendOnly, AnchorKeyed };

    StreamSync(const std::filesystem::path& shared_folder,
               const std::string& replica_name);

    void start();
    void register_stream(const std::string& name, StreamType type);
    void push(const std::string& stream, const StreamEntry& entry);
    size_t poll();
    std::vector<StreamEntry> entries(const std::string& stream) const;

    using NewEntriesCallback =
        std::function<void(const std::string& stream, size_t count)>;
    void set_on_new_entries(NewEntriesCallback cb);

private:
    struct StreamState {
        StreamType type;
        std::vector<StreamEntry> pending;
        std::unordered_map<std::string, uint64_t> local_sequences;
        std::unordered_map<std::string, StreamEntry> merged; // keyed by id
        struct PeerReadState {
            std::unordered_map<std::string, std::streamsize> bucket_bytes;
        };
        std::unordered_map<std::string, PeerReadState> peer_read_state;
    };

    void flush_stream(const std::string& name, StreamState& state);
    size_t read_remote_stream(const std::string& name, StreamState& state);

    std::filesystem::path m_shared_folder;
    std::string m_replica_name;
    std::unordered_map<std::string, StreamState> m_streams;
    NewEntriesCallback m_on_new_entries;
    bool m_started = false;
};
```

- [ ] **Step 4: Create StreamSync.cpp**

```cpp
// libs/collabtext/src/crdt/StreamSync.cpp
#include "crdt/StreamSync.h"
#include "crdt/StreamSerialization.h"
#include "crdt/SyncUtils.h"

#include <algorithm>

namespace fs = std::filesystem;

namespace CollabText::Crdt {

StreamSync::StreamSync(const fs::path& shared_folder,
                       const std::string& replica_name)
    : m_shared_folder(shared_folder)
    , m_replica_name(replica_name)
{
}

void StreamSync::start() {
    auto streams_dir = m_shared_folder / "replicas" / m_replica_name / "streams";
    fs::create_directories(streams_dir);

    // Load existing sequences for each registered stream
    for (auto& [name, state] : m_streams) {
        auto stream_dir = streams_dir / name;
        fs::create_directories(stream_dir);
        auto seq_path = stream_dir / "sequences.json";
        state.local_sequences = SyncUtils::read_sequences(seq_path);
    }

    m_started = true;
}

void StreamSync::register_stream(const std::string& name, StreamType type) {
    if (m_streams.count(name)) return;
    m_streams[name].type = type;

    // If already started, create the directory and load sequences
    if (m_started) {
        auto stream_dir = m_shared_folder / "replicas" / m_replica_name / "streams" / name;
        fs::create_directories(stream_dir);
        auto seq_path = stream_dir / "sequences.json";
        m_streams[name].local_sequences = SyncUtils::read_sequences(seq_path);
    }
}

void StreamSync::push(const std::string& stream, const StreamEntry& entry) {
    auto it = m_streams.find(stream);
    if (it == m_streams.end()) return;
    it->second.pending.push_back(entry);

    // Also merge into our own view immediately
    auto& merged = it->second.merged;
    if (it->second.type == StreamType::AppendOnly) {
        merged.try_emplace(entry.id, entry);
    } else {
        auto existing = merged.find(entry.id);
        if (existing == merged.end() || existing->second.timestamp < entry.timestamp) {
            merged[entry.id] = entry;
        }
    }
}

size_t StreamSync::poll() {
    if (!m_started) return 0;

    // Flush all streams
    for (auto& [name, state] : m_streams)
        flush_stream(name, state);

    // Read remote entries for all streams
    size_t total = 0;
    for (auto& [name, state] : m_streams) {
        size_t count = read_remote_stream(name, state);
        if (count > 0) {
            total += count;
            if (m_on_new_entries)
                m_on_new_entries(name, count);
        }
    }
    return total;
}

std::vector<StreamEntry> StreamSync::entries(const std::string& stream) const {
    auto it = m_streams.find(stream);
    if (it == m_streams.end()) return {};

    const auto& state = it->second;
    std::vector<StreamEntry> result;
    result.reserve(state.merged.size());

    if (state.type == StreamType::AppendOnly) {
        for (auto& [id, entry] : state.merged)
            result.push_back(entry);
        // Lamport order: seq ascending, replica_id as tiebreak
        std::sort(result.begin(), result.end(),
            [](const StreamEntry& a, const StreamEntry& b) {
                if (a.seq != b.seq) return a.seq < b.seq;
                return a.replica_id < b.replica_id;
            });
    } else {
        // AnchorKeyed: exclude tombstoned entries
        for (auto& [id, entry] : state.merged) {
            if (!entry.tombstone)
                result.push_back(entry);
        }
    }

    return result;
}

void StreamSync::set_on_new_entries(NewEntriesCallback cb) {
    m_on_new_entries = std::move(cb);
}

// ---- Private ----

void StreamSync::flush_stream(const std::string& name, StreamState& state) {
    if (state.pending.empty()) return;

    auto stream_dir = m_shared_folder / "replicas" / m_replica_name / "streams" / name;

    std::unordered_map<std::string, std::string> bucket_data;
    for (auto& entry : state.pending) {
        uint8_t bucket;
        if (state.type == StreamType::AppendOnly)
            bucket = SyncUtils::hash_bucket_lamport(entry.replica_id, entry.seq);
        else
            bucket = SyncUtils::hash_bucket_string(entry.id);

        std::string bkt = SyncUtils::bucket_hex(bucket);
        bucket_data[bkt] += encode_stream_entry(entry) + '\n';
    }
    state.pending.clear();

    for (auto& [bkt, data] : bucket_data) {
        SyncUtils::append_to_bucket(stream_dir / bkt, data);
        state.local_sequences[bkt]++;
    }

    auto seq_path = stream_dir / "sequences.json";
    SyncUtils::write_sequences(seq_path, state.local_sequences);
}

size_t StreamSync::read_remote_stream(const std::string& name, StreamState& state) {
    auto replicas_dir = m_shared_folder / "replicas";
    if (!fs::exists(replicas_dir)) return 0;

    size_t count = 0;

    for (auto& dir_entry : fs::directory_iterator(replicas_dir)) {
        if (!dir_entry.is_directory()) continue;
        std::string peer = dir_entry.path().filename().string();
        if (peer == m_replica_name) continue;

        auto peer_stream_dir = dir_entry.path() / "streams" / name;
        if (!fs::exists(peer_stream_dir)) continue;

        auto peer_seq_path = peer_stream_dir / "sequences.json";
        auto peer_seqs = SyncUtils::read_sequences(peer_seq_path);
        if (peer_seqs.empty()) continue;

        auto& read_state = state.peer_read_state[peer];

        for (auto& [bkt, seq] : peer_seqs) {
            auto bucket_path = peer_stream_dir / bkt;
            if (!fs::exists(bucket_path)) continue;

            std::streamsize already_read = read_state.bucket_bytes[bkt];
            auto file_size = static_cast<std::streamsize>(fs::file_size(bucket_path));
            if (file_size <= already_read) continue;

            auto [lines, new_offset] = SyncUtils::read_lines_after(bucket_path, already_read);
            read_state.bucket_bytes[bkt] = new_offset;

            for (auto& line : lines) {
                auto entry = decode_stream_entry(line);
                if (!entry) continue;

                if (state.type == StreamType::AppendOnly) {
                    if (state.merged.try_emplace(entry->id, *entry).second)
                        ++count;
                } else {
                    auto existing = state.merged.find(entry->id);
                    if (existing == state.merged.end()) {
                        state.merged[entry->id] = *entry;
                        ++count;
                    } else if (entry->timestamp > existing->second.timestamp) {
                        existing->second = *entry;
                        ++count;
                    }
                }
            }
        }
    }

    return count;
}

} // namespace CollabText::Crdt
```

- [ ] **Step 5: Add StreamSync.cpp to CMakeLists.txt**

In `libs/collabtext/CMakeLists.txt`, add `src/crdt/StreamSync.cpp` to the `add_library(collabtext STATIC ...)` list, after `src/crdt/StreamSerialization.cpp`.

- [ ] **Step 6: Build and run the round-trip test**

Run: `cmake --build build-dev -j$(($(nproc)-1)) && ctest --test-dir build-dev --output-on-failure -R tst_stream_sync`

Expected: 4 test cases pass (3 serialization + 1 round-trip).

- [ ] **Step 7: Commit**

```
git add libs/collabtext/src/crdt/StreamSync.h \
       libs/collabtext/src/crdt/StreamSync.cpp \
       libs/collabtext/tests/tst_stream_sync.cpp \
       libs/collabtext/CMakeLists.txt
git commit -m "feat: StreamSync class with append-only push/poll/entries"
```

---

### Task 4: Append-only dedup and anchor-keyed merge tests

**Files:**
- Modify: `libs/collabtext/tests/tst_stream_sync.cpp`

- [ ] **Step 1: Add append-only dedup test**

```cpp
    void append_only_dedup() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        std::filesystem::path shared = tmp.path().toStdString();

        StreamSync syncA(shared, "replica-A");
        StreamSync syncB(shared, "replica-B");
        syncA.register_stream("chat", StreamSync::StreamType::AppendOnly);
        syncB.register_stream("chat", StreamSync::StreamType::AppendOnly);
        syncA.start();
        syncB.start();

        StreamEntry e;
        e.id = "1-1";
        e.replica_id = 1;
        e.seq = 1;
        e.timestamp = "2026-04-13T10:00:00Z";
        e.payload = R"({"body":"hello"})";

        // Push the same entry twice
        syncA.push("chat", e);
        syncA.push("chat", e);
        syncA.poll();

        syncB.poll();
        auto entries = syncB.entries("chat");
        QCOMPARE(entries.size(), size_t(1));
    }
```

- [ ] **Step 2: Add anchor-keyed LWW merge test**

```cpp
    void anchor_keyed_lww_merge() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        std::filesystem::path shared = tmp.path().toStdString();

        StreamSync syncA(shared, "replica-A");
        StreamSync syncB(shared, "replica-B");
        syncA.register_stream("comments", StreamSync::StreamType::AnchorKeyed);
        syncB.register_stream("comments", StreamSync::StreamType::AnchorKeyed);
        syncA.start();
        syncB.start();

        // A pushes an entry
        StreamEntry e1;
        e1.id = "comment-1";
        e1.replica_id = 1;
        e1.seq = 1;
        e1.timestamp = "2026-04-13T10:00:00Z";
        e1.payload = R"({"body":"first version"})";
        syncA.push("comments", e1);
        syncA.poll();

        // B pushes a newer version of the same entry
        StreamEntry e2;
        e2.id = "comment-1";
        e2.replica_id = 2;
        e2.seq = 1;
        e2.timestamp = "2026-04-13T11:00:00Z";  // later timestamp
        e2.payload = R"({"body":"updated version"})";
        syncB.push("comments", e2);
        syncB.poll();

        // A polls — should see the updated version (LWW)
        syncA.poll();
        auto entries = syncA.entries("comments");
        QCOMPARE(entries.size(), size_t(1));
        QVERIFY(entries[0].payload.find("updated version") != std::string::npos);
    }
```

- [ ] **Step 3: Add anchor-keyed tombstone test**

```cpp
    void anchor_keyed_tombstone() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        std::filesystem::path shared = tmp.path().toStdString();

        StreamSync syncA(shared, "replica-A");
        StreamSync syncB(shared, "replica-B");
        syncA.register_stream("comments", StreamSync::StreamType::AnchorKeyed);
        syncB.register_stream("comments", StreamSync::StreamType::AnchorKeyed);
        syncA.start();
        syncB.start();

        // A pushes an entry
        StreamEntry e;
        e.id = "comment-1";
        e.replica_id = 1;
        e.seq = 1;
        e.timestamp = "2026-04-13T10:00:00Z";
        e.payload = R"({"body":"to be deleted"})";
        syncA.push("comments", e);
        syncA.poll();

        syncB.poll();
        QCOMPARE(syncB.entries("comments").size(), size_t(1));

        // A pushes a tombstone for the same entry
        StreamEntry tomb;
        tomb.id = "comment-1";
        tomb.replica_id = 1;
        tomb.seq = 2;
        tomb.timestamp = "2026-04-13T12:00:00Z";
        tomb.tombstone = true;
        syncA.push("comments", tomb);
        syncA.poll();

        // B polls — entry should be excluded
        syncB.poll();
        QCOMPARE(syncB.entries("comments").size(), size_t(0));
    }
```

- [ ] **Step 4: Add multi-stream isolation test**

```cpp
    void multi_stream_isolation() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        std::filesystem::path shared = tmp.path().toStdString();

        StreamSync syncA(shared, "replica-A");
        StreamSync syncB(shared, "replica-B");
        syncA.register_stream("chat", StreamSync::StreamType::AppendOnly);
        syncA.register_stream("comments", StreamSync::StreamType::AnchorKeyed);
        syncB.register_stream("chat", StreamSync::StreamType::AppendOnly);
        syncB.register_stream("comments", StreamSync::StreamType::AnchorKeyed);
        syncA.start();
        syncB.start();

        StreamEntry chat_entry;
        chat_entry.id = "1-1";
        chat_entry.replica_id = 1;
        chat_entry.seq = 1;
        chat_entry.timestamp = "2026-04-13T10:00:00Z";
        chat_entry.payload = R"({"body":"chat msg"})";
        syncA.push("chat", chat_entry);

        StreamEntry comment_entry;
        comment_entry.id = "comment-1";
        comment_entry.replica_id = 1;
        comment_entry.seq = 2;
        comment_entry.timestamp = "2026-04-13T10:00:01Z";
        comment_entry.payload = R"({"body":"comment text"})";
        syncA.push("comments", comment_entry);

        syncA.poll();
        syncB.poll();

        auto chat_entries = syncB.entries("chat");
        auto comment_entries = syncB.entries("comments");
        QCOMPARE(chat_entries.size(), size_t(1));
        QCOMPARE(comment_entries.size(), size_t(1));
        QVERIFY(chat_entries[0].payload.find("chat msg") != std::string::npos);
        QVERIFY(comment_entries[0].payload.find("comment text") != std::string::npos);
    }
```

- [ ] **Step 5: Build and run all stream sync tests**

Run: `cmake --build build-dev -j$(($(nproc)-1)) && ctest --test-dir build-dev --output-on-failure -R tst_stream_sync`

Expected: 8 test cases pass.

- [ ] **Step 6: Commit**

```
git add libs/collabtext/tests/tst_stream_sync.cpp
git commit -m "test: append-only dedup, anchor-keyed LWW/tombstone, multi-stream isolation"
```

---

### Task 5: Full test suite verification

**Files:** None (verification only)

- [ ] **Step 1: Run full test suite (excluding benchmark)**

Run: `ctest --test-dir build-dev --output-on-failure -j$(($(nproc)-1)) -E tst_benchmark`

Expected: All tests pass, including:
- `tst_filesync` (9 tests — regression check for SyncUtils extraction)
- `tst_stream_sync` (8 tests — new StreamSync tests)
- All other existing tests unchanged

- [ ] **Step 2: Verify file structure is clean**

Run: `git status`

Expected: Clean working tree, all changes committed.
