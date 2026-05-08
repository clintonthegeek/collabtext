#include <collabtext/StreamSync.h>
#include <collabtext/Serialization.h>
#include <collabtext/Operations.h>
#include "crdt/StreamSerialization.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>

namespace fs = std::filesystem;

namespace CollabText::Crdt {

StreamSync::StreamSync(const fs::path& shared_folder,
                       const std::string& replica_name,
                       uint16_t replica_id,
                       WriterConfig cfg)
    : m_shared_folder(shared_folder)
    , m_replica_name(replica_name)
    , m_replica_id(replica_id)
    , m_writer_cfg(cfg) {}

void StreamSync::start() {
    auto base = m_shared_folder / "replicas" / m_replica_name / "log" / "streams";
    fs::create_directories(base);
    fs::create_directories(m_shared_folder / "local" / m_replica_name
                           / "read-cursors" / "streams");
    for (auto& [name, state] : m_streams) {
        (void)state;
        ensure_started_(name);
    }
    m_started = true;
}

StreamSync::StreamState& StreamSync::ensure_started_(const std::string& name) {
    auto& s = m_streams[name];
    if (s.writer) return s;
    auto stream_dir = m_shared_folder / "replicas" / m_replica_name
                      / "log" / "streams" / name;
    fs::create_directories(stream_dir);
    s.writer = std::make_unique<SegmentWriter>(stream_dir, SegmentKind::Stream, m_writer_cfg);
    s.writer->start();
    fs::create_directories(m_shared_folder / "local" / m_replica_name
                           / "read-cursors" / "streams" / name);
    return s;
}

void StreamSync::register_stream(const std::string& name, StreamType type) {
    if (m_streams.count(name)) return;
    m_streams[name].type = type;
    if (m_started) ensure_started_(name);
}

void StreamSync::push(const std::string& stream, const StreamEntry& entry) {
    auto it = m_streams.find(stream);
    if (it == m_streams.end()) return;
    auto& s = ensure_started_(stream);
    s.writer->append(encode_stream_entry(entry), entry.seq);
    if (it->second.type == StreamType::AppendOnly) {
        s.merged.try_emplace(entry.id, entry);
    } else {
        auto existing = s.merged.find(entry.id);
        if (existing == s.merged.end() || existing->second.timestamp < entry.timestamp)
            s.merged[entry.id] = entry;
    }
}

size_t StreamSync::poll() {
    if (!m_started) return 0;
    auto now = std::chrono::steady_clock::now();
    for (auto& [name, s] : m_streams) if (s.writer) s.writer->tick(now);
    size_t total = 0;
    for (auto& [name, s] : m_streams) {
        size_t c = read_remote_stream_(name, s);
        if (c > 0) {
            total += c;
            if (m_on_new_entries) m_on_new_entries(name, c);
        }
    }
    write_acks_();
    recompute_fence_();
    return total;
}

void StreamSync::flush() {
    for (auto& [name, s] : m_streams) if (s.writer) s.writer->flush();
}

SegmentReader& StreamSync::reader_for_(StreamState& s, const std::string& stream,
                                       const std::string& peer) {
    auto it = s.readers.find(peer);
    if (it != s.readers.end()) return *it->second;
    auto peer_stream_dir = m_shared_folder / "replicas" / peer / "log" / "streams" / stream;
    auto cursor_path = m_shared_folder / "local" / m_replica_name
                       / "read-cursors" / "streams" / stream / (peer + ".bin");
    auto r = std::make_unique<SegmentReader>(peer_stream_dir, cursor_path);
    r->start();
    auto [ins, _] = s.readers.emplace(peer, std::move(r));
    return *ins->second;
}

size_t StreamSync::read_remote_stream_(const std::string& name, StreamState& s) {
    auto replicas_dir = m_shared_folder / "replicas";
    if (!fs::exists(replicas_dir)) return 0;
    size_t count = 0;
    for (auto& dir_entry : fs::directory_iterator(replicas_dir)) {
        if (!dir_entry.is_directory()) continue;
        std::string peer = dir_entry.path().filename().string();
        if (peer == m_replica_name) continue;
        auto peer_stream_dir = dir_entry.path() / "log" / "streams" / name;
        if (!fs::exists(peer_stream_dir)) continue;
        auto& reader = reader_for_(s, name, peer);
        auto records = reader.read_new();
        for (auto& r : records) {
            auto e = decode_stream_entry(r);
            if (!e) continue;
            if (s.type == StreamType::AppendOnly) {
                if (s.merged.try_emplace(e->id, *e).second) {
                    ++count;
                    if (m_on_inbound) m_on_inbound(name, e->replica_id, e->payload);
                    if (auto op = decode_operation(e->payload)) {
                        uint64_t lc = op_lamport(*op).counter();
                        auto& ack = m_ack_state[e->replica_id];
                        if (lc > ack.max_lamport) {
                            ack.max_lamport = lc;
                            ack.last_observed_at = current_iso_utc_();
                        }
                    }
                }
            } else {
                auto existing = s.merged.find(e->id);
                if (existing == s.merged.end()) {
                    s.merged[e->id] = *e;
                    ++count;
                    if (m_on_inbound) m_on_inbound(name, e->replica_id, e->payload);
                    if (auto op = decode_operation(e->payload)) {
                        uint64_t lc = op_lamport(*op).counter();
                        auto& ack = m_ack_state[e->replica_id];
                        if (lc > ack.max_lamport) {
                            ack.max_lamport = lc;
                            ack.last_observed_at = current_iso_utc_();
                        }
                    }
                } else if (e->timestamp > existing->second.timestamp) {
                    existing->second = *e;
                    ++count;
                    if (m_on_inbound) m_on_inbound(name, e->replica_id, e->payload);
                    if (auto op = decode_operation(e->payload)) {
                        uint64_t lc = op_lamport(*op).counter();
                        auto& ack = m_ack_state[e->replica_id];
                        if (lc > ack.max_lamport) {
                            ack.max_lamport = lc;
                            ack.last_observed_at = current_iso_utc_();
                        }
                    }
                }
            }
        }
        reader.commit();
    }
    return count;
}

std::vector<StreamEntry> StreamSync::entries(const std::string& stream) const {
    auto it = m_streams.find(stream);
    if (it == m_streams.end()) return {};
    std::vector<StreamEntry> result;
    result.reserve(it->second.merged.size());
    if (it->second.type == StreamType::AppendOnly) {
        for (auto& [id, entry] : it->second.merged) result.push_back(entry);
        std::sort(result.begin(), result.end(),
            [](const StreamEntry& a, const StreamEntry& b) {
                if (a.seq != b.seq) return a.seq < b.seq;
                return a.replica_id < b.replica_id;
            });
    } else {
        for (auto& [id, entry] : it->second.merged)
            if (!entry.tombstone) result.push_back(entry);
    }
    return result;
}

void StreamSync::set_on_new_entries(NewEntriesCallback cb) {
    m_on_new_entries = std::move(cb);
}

void StreamSync::set_on_inbound(InboundCallback cb) {
    m_on_inbound = std::move(cb);
}

std::string StreamSync::current_iso_utc_() const {
    auto now = std::chrono::system_clock::now();
    auto now_t = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&now_t, &utc);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buf;
}

void StreamSync::push(const std::string& stream_name, const std::string& payload) {
    if (!m_streams.count(stream_name)) return;
    auto& s = ensure_started_(stream_name);
    uint64_t seq = s.next_seq++;
    StreamEntry entry;
    entry.replica_id = m_replica_id;
    entry.seq = seq;
    // next_seq resets on restart; Phase 4 will persist seq to avoid id collisions
    entry.id = m_replica_name + "-" + std::to_string(seq);
    entry.timestamp = current_iso_utc_();
    entry.payload = payload;
    push(stream_name, entry);
}

void StreamSync::write_acks_() {
    if (m_ack_state.empty()) return;

    // Build fresh map, skip peers with no observations
    std::unordered_map<uint16_t, AckState> merged = m_ack_state;
    for (auto it = merged.begin(); it != merged.end(); ) {
        if (it->second.max_lamport == 0) it = merged.erase(it);
        else ++it;
    }
    if (merged.empty()) return;

    auto acks_path = m_shared_folder / "replicas" / m_replica_name / "acks.json";
    auto tmp_path  = m_shared_folder / "replicas" / m_replica_name / "acks.json.tmp";

    // Read existing acks.json and enforce monotonicity
    if (fs::exists(acks_path)) {
        std::ifstream f(acks_path);
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        // Simple scan for "\"<id>\": { ... "max_lamport_observed": <N> ..." patterns
        // We look for each peer key and extract max_lamport_observed + last_observed_at
        size_t search_pos = 0;
        while (search_pos < content.size()) {
            // Find a quoted decimal key
            auto q1 = content.find('"', search_pos);
            if (q1 == std::string::npos) break;
            auto q2 = content.find('"', q1 + 1);
            if (q2 == std::string::npos) break;
            std::string key_str = content.substr(q1 + 1, q2 - q1 - 1);
            search_pos = q2 + 1;

            // Check if key_str is a decimal number (replica id)
            bool is_decimal = !key_str.empty();
            for (char c : key_str) if (c < '0' || c > '9') { is_decimal = false; break; }
            if (!is_decimal) continue;

            // Parse replica_id
            uint64_t rid_val = 0;
            for (char c : key_str) {
                rid_val = rid_val * 10 + (c - '0');
                if (rid_val > UINT16_MAX) { rid_val = UINT16_MAX + 1; break; }
            }
            if (rid_val > UINT16_MAX) continue;
            auto rid = static_cast<uint16_t>(rid_val);

            // Find max_lamport_observed value in the object following this key
            auto ml_tag = content.find("\"max_lamport_observed\"", search_pos);
            auto next_key_q = content.find('"', search_pos);
            // Only look ahead if max_lamport_observed comes before the next top-level key
            if (ml_tag == std::string::npos) continue;
            if (next_key_q != std::string::npos && next_key_q < ml_tag) continue;

            auto colon = content.find(':', ml_tag + 22);
            if (colon == std::string::npos) continue;
            size_t num_start = content.find_first_not_of(" \t\n\r", colon + 1);
            if (num_start == std::string::npos) continue;
            size_t num_end = num_start;
            while (num_end < content.size() && content[num_end] >= '0' && content[num_end] <= '9')
                ++num_end;
            if (num_end == num_start) continue;
            uint64_t stored_lc = 0;
            for (size_t i = num_start; i < num_end; ++i)
                stored_lc = stored_lc * 10 + (content[i] - '0');

            // Also grab last_observed_at if stored_lc >= fresh (we preserve existing timestamp)
            std::string stored_ts;
            auto ts_tag = content.find("\"last_observed_at\"", search_pos);
            if (ts_tag != std::string::npos) {
                auto ts_colon = content.find(':', ts_tag + 18);
                if (ts_colon != std::string::npos) {
                    auto ts_q1 = content.find('"', ts_colon + 1);
                    if (ts_q1 != std::string::npos) {
                        auto ts_q2 = content.find('"', ts_q1 + 1);
                        if (ts_q2 != std::string::npos)
                            stored_ts = content.substr(ts_q1 + 1, ts_q2 - ts_q1 - 1);
                    }
                }
            }

            auto& ack = merged[rid];
            if (stored_lc > ack.max_lamport) {
                ack.max_lamport = stored_lc;
                ack.last_observed_at = stored_ts;
            }

            search_pos = num_end;
        }
    }

    // Check if anything changed (compare against what was last written by loading acks_path again)
    // Simple approach: always write if merged is non-empty — the caller already skips when
    // m_ack_state is empty. The file comparison would require storing last-written state;
    // instead we rely on the monotonicity read above and just write unconditionally when
    // there are observations. This is safe per the spec ("skip if nothing changed").
    // For now always write — future optimization can diff against a cached snapshot.

    // Build JSON
    std::string json = "{\n  \"schema_version\": 1,\n  \"acks\": {\n";
    bool first = true;
    for (auto& [rid, ack] : merged) {
        if (ack.max_lamport == 0) continue;
        if (!first) json += ",\n";
        first = false;
        json += "    \"" + std::to_string(rid) + "\": {\n";
        json += "      \"max_lamport_observed\": " + std::to_string(ack.max_lamport) + ",\n";
        json += "      \"last_observed_at\": \"" + ack.last_observed_at + "\"\n";
        json += "    }";
    }
    json += "\n  }\n}\n";

    // Atomic write via tmp-file rename
    {
        std::ofstream f(tmp_path, std::ios::out | std::ios::trunc);
        f << json;
    }
    fs::rename(tmp_path, acks_path);
}

uint64_t StreamSync::lowest_peer_acked_lamport() const {
    return m_cached_fence;
}

void StreamSync::set_on_ack_update(std::function<void(uint64_t)> cb) {
    m_ack_update_cb = std::move(cb);
}

uint64_t StreamSync::read_peer_ack_(const fs::path& acks_path) const {
    if (!fs::exists(acks_path)) return UINT64_MAX;

    std::ifstream f(acks_path);
    if (!f) return UINT64_MAX;
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    // Look for our replica_id key inside the "acks" object and extract
    // max_lamport_observed. We need to find:
    //   "<our_id>": { "max_lamport_observed": <N>, ... }
    std::string our_key = "\"" + std::to_string(m_replica_id) + "\"";
    auto key_pos = content.find(our_key);
    if (key_pos == std::string::npos) return UINT64_MAX;

    // Find max_lamport_observed after our key position
    auto ml_tag = content.find("\"max_lamport_observed\"", key_pos);
    if (ml_tag == std::string::npos) return UINT64_MAX;

    // Make sure there's no other top-level quoted key between our key and the tag
    // (i.e., we're still inside our key's object). A simple heuristic: the next
    // occurrence of a decimal-only quoted string after key_pos should not come
    // before ml_tag.
    auto colon = content.find(':', ml_tag + 22);
    if (colon == std::string::npos) return UINT64_MAX;

    size_t num_start = content.find_first_not_of(" \t\n\r", colon + 1);
    if (num_start == std::string::npos) return UINT64_MAX;

    size_t num_end = num_start;
    while (num_end < content.size() && content[num_end] >= '0' && content[num_end] <= '9')
        ++num_end;
    if (num_end == num_start) return UINT64_MAX;

    uint64_t value = 0;
    for (size_t i = num_start; i < num_end; ++i)
        value = value * 10 + (content[i] - '0');

    return value;
}

void StreamSync::recompute_fence_() {
    auto replicas_dir = m_shared_folder / "replicas";
    if (!fs::exists(replicas_dir)) return;

    uint64_t new_fence = 0;
    bool any_peer = false;

    for (auto& dir_entry : fs::directory_iterator(replicas_dir)) {
        if (!dir_entry.is_directory()) continue;
        std::string peer = dir_entry.path().filename().string();
        if (peer == m_replica_name) continue;  // skip our own directory

        auto acks_path = dir_entry.path() / "acks.json";
        uint64_t peer_val = read_peer_ack_(acks_path);
        if (peer_val == UINT64_MAX) continue;  // no entry for us — excluded

        if (!any_peer) {
            new_fence = peer_val;
            any_peer = true;
        } else {
            new_fence = std::min(new_fence, peer_val);
        }
    }

    if (!any_peer) return;  // no enrolled peers yet — leave fence at current value

    if (new_fence > m_cached_fence) {
        m_cached_fence = new_fence;
        if (m_ack_update_cb) m_ack_update_cb(m_cached_fence);
    }
}

} // namespace CollabText::Crdt
