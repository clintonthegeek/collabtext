#include "crdt/StreamSync.h"
#include "crdt/StreamSerialization.h"
#include "crdt/SyncUtils.h"

#include <algorithm>

namespace fs = std::filesystem;

namespace CollabText::Crdt {

StreamSync::StreamSync(const fs::path& shared_folder, const std::string& replica_name)
    : m_shared_folder(shared_folder), m_replica_name(replica_name) {}

void StreamSync::start() {
    auto streams_dir = m_shared_folder / "replicas" / m_replica_name / "streams";
    fs::create_directories(streams_dir);
    for (auto& [name, state] : m_streams) {
        auto stream_dir = streams_dir / name;
        fs::create_directories(stream_dir);
        state.local_sequences = SyncUtils::read_sequences(stream_dir / "sequences.json");
    }
    m_started = true;
}

void StreamSync::register_stream(const std::string& name, StreamType type) {
    if (m_streams.count(name)) return;
    m_streams[name].type = type;
    if (m_started) {
        auto stream_dir = m_shared_folder / "replicas" / m_replica_name / "streams" / name;
        fs::create_directories(stream_dir);
        m_streams[name].local_sequences = SyncUtils::read_sequences(stream_dir / "sequences.json");
    }
}

void StreamSync::push(const std::string& stream, const StreamEntry& entry) {
    auto it = m_streams.find(stream);
    if (it == m_streams.end()) return;
    it->second.pending.push_back(entry);
    // Merge into local view immediately
    if (it->second.type == StreamType::AppendOnly) {
        it->second.merged.try_emplace(entry.id, entry);
    } else {
        auto existing = it->second.merged.find(entry.id);
        if (existing == it->second.merged.end() || existing->second.timestamp < entry.timestamp)
            it->second.merged[entry.id] = entry;
    }
}

size_t StreamSync::poll() {
    if (!m_started) return 0;
    for (auto& [name, state] : m_streams)
        flush_stream(name, state);
    size_t total = 0;
    for (auto& [name, state] : m_streams) {
        size_t count = read_remote_stream(name, state);
        if (count > 0) {
            total += count;
            if (m_on_new_entries) m_on_new_entries(name, count);
        }
    }
    return total;
}

std::vector<StreamEntry> StreamSync::entries(const std::string& stream) const {
    auto it = m_streams.find(stream);
    if (it == m_streams.end()) return {};
    std::vector<StreamEntry> result;
    result.reserve(it->second.merged.size());
    if (it->second.type == StreamType::AppendOnly) {
        for (auto& [id, entry] : it->second.merged)
            result.push_back(entry);
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

void StreamSync::flush_stream(const std::string& name, StreamState& state) {
    if (state.pending.empty()) return;
    auto stream_dir = m_shared_folder / "replicas" / m_replica_name / "streams" / name;
    std::unordered_map<std::string, std::string> bucket_data;
    for (auto& entry : state.pending) {
        uint8_t bucket = (state.type == StreamType::AppendOnly)
            ? SyncUtils::hash_bucket_lamport(entry.replica_id, entry.seq)
            : SyncUtils::hash_bucket_string(entry.id);
        bucket_data[SyncUtils::bucket_hex(bucket)] += encode_stream_entry(entry) + '\n';
    }
    state.pending.clear();
    for (auto& [bkt, data] : bucket_data) {
        SyncUtils::append_to_bucket(stream_dir / bkt, data);
        state.local_sequences[bkt]++;
    }
    SyncUtils::write_sequences(stream_dir / "sequences.json", state.local_sequences);
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
        auto peer_seqs = SyncUtils::read_sequences(peer_stream_dir / "sequences.json");
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
                    if (state.merged.try_emplace(entry->id, *entry).second) ++count;
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
