#include "crdt/StreamSync.h"
#include "crdt/StreamSerialization.h"

#include <algorithm>
#include <chrono>

namespace fs = std::filesystem;

namespace CollabText::Crdt {

StreamSync::StreamSync(const fs::path& shared_folder,
                       const std::string& replica_name, WriterConfig cfg)
    : m_shared_folder(shared_folder)
    , m_replica_name(replica_name)
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
                if (s.merged.try_emplace(e->id, *e).second) ++count;
            } else {
                auto existing = s.merged.find(e->id);
                if (existing == s.merged.end()) {
                    s.merged[e->id] = *e;
                    ++count;
                } else if (e->timestamp > existing->second.timestamp) {
                    existing->second = *e;
                    ++count;
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

} // namespace CollabText::Crdt
