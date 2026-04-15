#include "crdt/FileSync.h"
#include "crdt/Serialization.h"
#include "crdt/SyncUtils.h"

#include <fstream>

namespace fs = std::filesystem;

namespace CollabText::Crdt {

FileSync::FileSync(Buffer& buffer,
                   const fs::path& shared_folder,
                   const std::string& replica_name)
    : m_buffer(buffer)
    , m_shared_folder(shared_folder)
    , m_replica_name(replica_name)
{
}

void FileSync::start() {
    ensure_directory_structure();
    // Load our existing sequence counters
    auto seq_path = m_shared_folder / "replicas" / m_replica_name / "sequences.json";
    m_local_sequences = SyncUtils::read_sequences(seq_path);
    m_started = true;
}

void FileSync::push_local_op(const Operation& op) {
    m_pending_ops.push_back(op);
}

size_t FileSync::poll() {
    if (!m_started) return 0;
    flush_local_ops();
    return read_remote_ops();
}

void FileSync::set_on_remote_ops(RemoteOpsCallback cb) {
    m_on_remote_ops = std::move(cb);
}

// ---- Private implementation ----

void FileSync::ensure_directory_structure() {
    auto replica_dir = m_shared_folder / "replicas" / m_replica_name;
    fs::create_directories(replica_dir / "ops");
    fs::create_directories(m_shared_folder / "meta");
    fs::create_directories(m_shared_folder / "snapshots");

    // Ensure local (non-synced) directory
    auto local_dir = m_shared_folder / "local" / m_replica_name;
    fs::create_directories(local_dir);

    // Ensure .stignore
    auto stignore = m_shared_folder / ".stignore";
    if (!fs::exists(stignore)) {
        std::ofstream f(stignore);
        f << "local/\n*.tmp\n*.part\n";
    }
}

void FileSync::flush_local_ops() {
    if (m_pending_ops.empty()) return;

    auto ops_dir = m_shared_folder / "replicas" / m_replica_name / "ops";

    // Group ops by bucket
    std::unordered_map<std::string, std::string> bucket_data;
    for (auto& op : m_pending_ops) {
        Lamport ts;
        if (auto* e = std::get_if<EditOperation>(&op))
            ts = e->timestamp;
        else if (auto* u = std::get_if<UndoOperation>(&op))
            ts = u->timestamp;

        std::string bkt = SyncUtils::bucket_hex(SyncUtils::hash_bucket_lamport(ts.replica_id, ts.value));
        bucket_data[bkt] += encode_operation(op) + '\n';
    }
    m_pending_ops.clear();

    // Append to bucket files and update sequence counters
    for (auto& [bkt, data] : bucket_data) {
        auto bucket_path = ops_dir / bkt;
        SyncUtils::append_to_bucket(bucket_path, data);
        m_local_sequences[bkt]++;
    }

    // Write sequences.json atomically
    auto seq_path = m_shared_folder / "replicas" / m_replica_name / "sequences.json";
    SyncUtils::write_sequences(seq_path, m_local_sequences);
}

size_t FileSync::read_remote_ops() {
    auto replicas_dir = m_shared_folder / "replicas";
    if (!fs::exists(replicas_dir) || !fs::is_directory(replicas_dir))
        return 0;

    size_t total_applied = 0;

    for (auto& entry : fs::directory_iterator(replicas_dir)) {
        if (!entry.is_directory()) continue;
        std::string peer_name = entry.path().filename().string();
        if (peer_name == m_replica_name) continue;  // skip self

        // Read peer's sequence counters
        auto peer_seq_path = entry.path() / "sequences.json";
        auto peer_seqs = SyncUtils::read_sequences(peer_seq_path);
        if (peer_seqs.empty()) continue;

        auto& read_state = m_peer_read_state[peer_name];

        // Check each bucket for new data
        for (auto& [bkt, seq] : peer_seqs) {
            auto bucket_path = entry.path() / "ops" / bkt;
            if (!fs::exists(bucket_path)) continue;

            std::streamsize already_read = read_state.bucket_bytes[bkt];
            auto file_size = static_cast<std::streamsize>(fs::file_size(bucket_path));
            if (file_size <= already_read) continue;

            // Read new operations from this bucket
            auto [lines, new_offset] = SyncUtils::read_lines_after(bucket_path, already_read);
            read_state.bucket_bytes[bkt] = new_offset;

            if (!lines.empty()) {
                std::vector<Operation> ops;
                ops.reserve(lines.size());
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
    }

    if (total_applied > 0 && m_on_remote_ops)
        m_on_remote_ops(total_applied);

    return total_applied;
}


} // namespace CollabText::Crdt
