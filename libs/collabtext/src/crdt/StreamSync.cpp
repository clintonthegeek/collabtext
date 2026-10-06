#include "transport/StreamSync.h"
#include <collabtext/Serialization.h>
#include <collabtext/Operations.h>
#include "crdt/StreamSerialization.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <limits>

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
    if (m_started) return;
    auto base = m_shared_folder / "replicas" / m_replica_name / "log" / "streams";
    fs::create_directories(base);
    auto local = m_shared_folder / "local" / m_replica_name;
    fs::create_directories(local / "read-cursors" / "streams");
    auto lease = Storage::ProcessLease::acquire(local / "writer.lock",
                                                 m_writer_cfg.file_ops);
    if (!lease.ok()) { m_last_error = lease.error(); return; }
    m_writer_lease.emplace(std::move(lease.value()));
    for (auto& [name, state] : m_streams) {
        ensure_started_(name);
        if (m_last_error) return;
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
    auto started = s.writer->start();
    if (!started.ok()) { m_last_error = started.error(); return s; }
    fs::create_directories(m_shared_folder / "local" / m_replica_name
                           / "read-cursors" / "streams" / name);
    if (!recover_next_sequence_(name, s)) {
        s.writer.reset();
        return s;
    }
    return s;
}

bool StreamSync::recover_next_sequence_(const std::string& stream, StreamState& state) {
    const auto peer_dir = m_shared_folder / "replicas" / m_replica_name
                          / "log" / "streams" / stream;
    const auto cursor = m_shared_folder / "local" / m_replica_name
                        / "read-cursors" / "streams" / (stream + ".sequence-scan");
    SegmentReader reader(peer_dir, cursor);
    reader.start();
    uint64_t highest = 0;
    for (;;) {
        auto batch = reader.read_new();
        if (batch.gap) {
            m_last_error = batch.gap->with_context("recovering producer sequence for " + stream);
            return false;
        }
        for (const auto& raw : batch.records) {
            auto entry = decode_stream_entry(raw);
            if (!entry) {
                m_last_error = Error(ErrorCode::corrupt_record,
                    "invalid producer record while recovering envelope sequence", stream);
                return false;
            }
            highest = std::max(highest, entry->seq);
        }
        if (batch.records.empty()) break;
        auto accepted = reader.commit(batch.token);
        if (!accepted.ok()) { m_last_error = accepted.error(); return false; }
    }
    if (highest == std::numeric_limits<uint64_t>::max()) {
        m_last_error = Error(ErrorCode::counter_exhausted,
                             "stream envelope sequence exhausted", stream);
        return false;
    }
    state.next_seq = highest + 1;
    state.sequence_recovered = true;
    return true;
}

void StreamSync::register_stream(const std::string& name, StreamType type) {
    if (m_streams.count(name)) return;
    m_streams[name].type = type;
    if (m_started) ensure_started_(name);
}

void StreamSync::push(const std::string& stream, const StreamEntry& entry) {
    if (!m_started || !m_writer_lease) {
        m_last_error = Error(ErrorCode::io_error, "StreamSync is not started");
        return;
    }
    auto it = m_streams.find(stream);
    if (it == m_streams.end()) return;
    auto& s = ensure_started_(stream);
    auto appended = s.writer->append(encode_stream_entry(entry), entry.seq);
    if (!appended.ok()) {
        m_last_error = appended.error();
        return;
    }
    if (entry.replica_id == m_replica_id && entry.seq >= s.next_seq) {
        if (entry.seq == std::numeric_limits<uint64_t>::max()) {
            m_last_error = Error(ErrorCode::counter_exhausted,
                                 "stream envelope sequence exhausted", stream);
            return;
        }
        s.next_seq = entry.seq + 1;
    }
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
    for (auto& [name, s] : m_streams) {
        (void)name;
        if (!s.writer) continue;
        auto ticked = s.writer->tick(now);
        if (!ticked.ok()) { m_last_error = ticked.error(); return 0; }
    }
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
    for (auto& [name, s] : m_streams) {
        (void)name;
        if (!s.writer) continue;
        auto flushed = s.writer->flush();
        if (!flushed.ok()) { m_last_error = flushed.error(); return; }
    }
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
        if (s.blocked.contains(peer)) continue;
        auto& reader = reader_for_(s, name, peer);
        auto batch = reader.read_new();
        auto& records = batch.records;
        if (records.empty() && batch.gap) {
            m_last_error = batch.gap->with_context("StreamSync " + name + " peer " + peer);
            continue;
        }
        if (records.empty()) {
            auto committed = reader.commit(batch.token);
            if (!committed.ok())
                m_last_error = committed.error().with_context("StreamSync " + name + " peer " + peer);
            continue;
        }
        // Validate this batch before callbacks/merged state can see any entry.
        // Generic streams may carry arbitrary opaque payloads. Only recognized
        // operation families are interpreted here; P07 supplies stream binding.
        for (const auto& record : records) {
            auto entry = decode_stream_entry(record);
            Error error;
            if (!entry) error = Error(ErrorCode::corrupt_record, "invalid stream entry");
            else if (entry->payload.starts_with("CTBO")) {
                auto op = decode_operation(entry->payload);
                if (!op.ok()) error = op.error();
            } else if (entry->payload.starts_with("CTLO")) {
                auto op = decode_idlist_operation(entry->payload);
                if (!op.ok()) error = op.error();
            }
            if (error) {
                m_last_error = error.with_context("StreamSync " + name + " peer " + peer);
                s.blocked.emplace(peer, std::move(batch.records));
                break;
            }
        }
        if (s.blocked.contains(peer)) continue;
        const bool callback_missing = !m_on_inbound;
        if (callback_missing)
            m_last_error = Error(ErrorCode::writer_conflict,
                                 "inbound callback is not registered; records remain uncommitted", name);
        bool callback_failed = false;
        for (auto& r : records) {
            auto e = decode_stream_entry(r);
            if (!e) continue;
            auto existing = s.merged.find(e->id);
            const bool newer_anchor = s.type == StreamType::AnchorKeyed
                && (existing == s.merged.end() || e->timestamp > existing->second.timestamp);
            const bool needs_apply = callback_missing
                || !s.callback_completed.contains(e->id) || newer_anchor;
            if (needs_apply) {
                if (!callback_missing) {
                    try {
                        m_on_inbound(name, e->replica_id, e->payload);
                        s.callback_completed.insert(e->id);
                    } catch (...) {
                        m_last_error = Error(ErrorCode::io_error,
                                             "inbound callback threw; record retained", name);
                        callback_failed = true;
                        break;
                    }
                }
            }
            if (!callback_missing && !callback_failed) {
                if (auto op = decode_operation(e->payload)) {
                    const uint64_t lc = op_identity(op.value()).counter;
                    auto& ack = m_ack_state[e->replica_id];
                    if (lc > ack.max_lamport) {
                        ack.max_lamport = lc;
                        ack.last_observed_at = current_iso_utc_();
                    }
                }
            }
            if (s.type == StreamType::AppendOnly) {
                if (s.merged.try_emplace(e->id, *e).second) {
                    ++count;
                }
            } else {
                if (existing == s.merged.end()) {
                    s.merged[e->id] = *e;
                    ++count;
                } else if (e->timestamp > existing->second.timestamp) {
                    existing->second = *e;
                    ++count;
                }
            }
        }
        if (callback_failed || callback_missing) continue;
        auto committed = reader.commit(batch.token);
        if (!committed.ok()) {
            m_last_error = committed.error().with_context("StreamSync " + name + " peer " + peer);
            continue;
        }
        if (batch.gap) m_last_error = batch.gap->with_context("StreamSync " + name + " peer " + peer);
    }
    return count;
}

const std::vector<std::string>& StreamSync::blocked_records(const std::string& stream,
                                                           const std::string& peer) const {
    static const std::vector<std::string> empty;
    auto s = m_streams.find(stream);
    if (s == m_streams.end()) return empty;
    auto batch = s->second.blocked.find(peer);
    return batch == s->second.blocked.end() ? empty : batch->second;
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
    if (m_last_error || !s.sequence_recovered) return;
    if (s.next_seq == 0 || s.next_seq == std::numeric_limits<uint64_t>::max()) {
        m_last_error = Error(ErrorCode::counter_exhausted,
                             "stream envelope sequence exhausted", stream_name);
        return;
    }
    uint64_t seq = s.next_seq;
    StreamEntry entry;
    entry.replica_id = m_replica_id;
    entry.seq = seq;
    // next_seq resets on restart; Phase 4 will persist seq to avoid id collisions
    entry.id = m_replica_name + "-" + std::to_string(seq);
    entry.timestamp = current_iso_utc_();
    entry.payload = payload;
    push(stream_name, entry);
    if (!m_last_error) s.next_seq = seq + 1;
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

    // Read existing acks.json and enforce monotonicity
    if (fs::exists(acks_path)) {
        std::ifstream f(acks_path, std::ios::binary | std::ios::ate);
        if (!f) return;
        const auto file_size = f.tellg();
        if (file_size < 0 || file_size > 1024 * 1024) return;
        std::string content(static_cast<size_t>(file_size), '\0');
        f.seekg(0);
        f.read(content.data(), static_cast<std::streamsize>(content.size()));
        if (!f && !content.empty()) return;
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
            bool counter_overflow = false;
            for (size_t i = num_start; i < num_end; ++i)
            {
                const uint64_t digit = static_cast<uint64_t>(content[i] - '0');
                if (stored_lc > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
                    counter_overflow = true;
                    break;
                }
                stored_lc = stored_lc * 10 + digit;
            }
            if (counter_overflow) continue;

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

    if (fs::exists(acks_path)) {
        std::ifstream existing(acks_path, std::ios::binary | std::ios::ate);
        if (existing) {
            const auto size = existing.tellg();
            if (size >= 0 && static_cast<uint64_t>(size) == json.size()) {
                std::string old(static_cast<size_t>(size), '\0');
                existing.seekg(0);
                existing.read(old.data(), static_cast<std::streamsize>(old.size()));
                if (existing && old == json) return;
            }
        }
    }
    Storage::FileOpCounts counts;
    auto written = Storage::replace_atomically(acks_path, json,
        m_writer_cfg.step_hook, m_writer_cfg.file_ops, &counts);
    if (!written.ok()) m_last_error = written.error();
}

uint64_t StreamSync::lowest_peer_acked_lamport() const {
    return m_cached_fence;
}

void StreamSync::set_on_ack_update(std::function<void(uint64_t)> cb) {
    m_ack_update_cb = std::move(cb);
}

std::optional<uint64_t> decode_legacy_peer_ack(std::string_view content,
                                               uint16_t replica_id) {
    constexpr size_t kMaxAckBytes = 1u * 1024u * 1024u;
    if (content.size() > kMaxAckBytes) return std::nullopt;
    const std::string our_key = "\"" + std::to_string(replica_id) + "\"";
    const auto key_pos = content.find(our_key);
    if (key_pos == std::string_view::npos) return std::nullopt;
    const auto ml_tag = content.find("\"max_lamport_observed\"", key_pos + our_key.size());
    const auto object_end = content.find('}', key_pos + our_key.size());
    if (ml_tag == std::string_view::npos || object_end == std::string_view::npos
        || ml_tag > object_end) return std::nullopt;
    const auto colon = content.find(':', ml_tag + 22);
    if (colon == std::string_view::npos) return std::nullopt;
    size_t pos = colon + 1;
    while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t'
                                    || content[pos] == '\n' || content[pos] == '\r')) ++pos;
    const size_t start = pos;
    uint64_t value = 0;
    while (pos < content.size() && content[pos] >= '0' && content[pos] <= '9') {
        const uint64_t digit = static_cast<uint64_t>(content[pos] - '0');
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10)
            return std::nullopt;
        value = value * 10 + digit;
        ++pos;
    }
    if (pos == start) return std::nullopt;
    if (pos < content.size() && content[pos] != ',' && content[pos] != '}'
        && content[pos] != ' ' && content[pos] != '\t' && content[pos] != '\n'
        && content[pos] != '\r') return std::nullopt;
    return value;
}

uint64_t StreamSync::read_peer_ack_(const fs::path& acks_path) const {
    if (!fs::exists(acks_path)) return UINT64_MAX;

    std::ifstream f(acks_path, std::ios::binary | std::ios::ate);
    if (!f) return UINT64_MAX;
    const auto file_size = f.tellg();
    if (file_size < 0 || file_size > 1024 * 1024) return UINT64_MAX;
    std::string content(static_cast<size_t>(file_size), '\0');
    f.seekg(0);
    f.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (!f && !content.empty()) return UINT64_MAX;
    return decode_legacy_peer_ack(content, m_replica_id).value_or(UINT64_MAX);
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
