#include "crdt/FileSync.h"
#include "crdt/Serialization.h"

#include <algorithm>
#include <fstream>
#include <sstream>

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
    m_local_sequences = read_sequences(seq_path);
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

        std::string bkt = bucket_hex(hash_bucket(ts));
        bucket_data[bkt] += encode_operation(op) + '\n';
    }
    m_pending_ops.clear();

    // Append to bucket files and update sequence counters
    for (auto& [bkt, data] : bucket_data) {
        auto bucket_path = ops_dir / bkt;

        // Append (create if needed)
        std::ofstream f(bucket_path, std::ios::app | std::ios::binary);
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        f.flush();

        m_local_sequences[bkt]++;
    }

    // Write sequences.json atomically
    auto seq_path = m_shared_folder / "replicas" / m_replica_name / "sequences.json";
    write_sequences(seq_path, m_local_sequences);
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
        auto peer_seqs = read_sequences(peer_seq_path);
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
            auto [ops, new_offset] = read_bucket_file(bucket_path, already_read);
            read_state.bucket_bytes[bkt] = new_offset;

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

uint8_t FileSync::hash_bucket(const Lamport& ts) {
    // Polynomial hash of replica_id, matching CRDT_SYNC_SPEC §4.3
    // (simplified: replica_id is uint16_t, not a string)
    uint32_t replica_hash = ts.replica_id * 199;
    return static_cast<uint8_t>((replica_hash + ts.value) % 256);
}

std::string FileSync::bucket_hex(uint8_t bucket) {
    static const char hex[] = "0123456789abcdef";
    return {hex[bucket >> 4], hex[bucket & 0x0F]};
}

std::unordered_map<std::string, uint64_t>
FileSync::read_sequences(const fs::path& path) {
    std::unordered_map<std::string, uint64_t> result;
    if (!fs::exists(path)) return result;

    std::ifstream f(path);
    if (!f.is_open()) return result;

    // Minimal JSON object parser for {"key": number, ...}
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    size_t pos = content.find('{');
    if (pos == std::string::npos) return result;
    ++pos;

    while (pos < content.size()) {
        // Skip whitespace and commas
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == ','
               || content[pos] == '\n' || content[pos] == '\r' || content[pos] == '\t'))
            ++pos;

        if (pos >= content.size() || content[pos] == '}') break;

        // Parse key
        if (content[pos] != '"') break;
        ++pos;
        size_t key_start = pos;
        while (pos < content.size() && content[pos] != '"') ++pos;
        std::string key = content.substr(key_start, pos - key_start);
        ++pos; // closing quote

        // Skip colon
        while (pos < content.size() && content[pos] != ':') ++pos;
        ++pos;

        // Skip whitespace
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t'))
            ++pos;

        // Parse number
        size_t num_start = pos;
        while (pos < content.size() && content[pos] >= '0' && content[pos] <= '9')
            ++pos;
        if (pos > num_start) {
            result[key] = std::stoull(content.substr(num_start, pos - num_start));
        }
    }

    return result;
}

void FileSync::write_sequences(const fs::path& path,
                               const std::unordered_map<std::string, uint64_t>& seqs) {
    // Write to .tmp then rename for atomic update
    auto tmp_path = path;
    tmp_path += ".tmp";

    {
        std::ofstream f(tmp_path);
        f << '{';
        bool first = true;
        // Sort keys for deterministic output
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

std::pair<std::vector<Operation>, std::streamsize>
FileSync::read_bucket_file(const fs::path& path, std::streamsize after_byte) {
    std::vector<Operation> ops;

    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {ops, after_byte};

    f.seekg(after_byte);

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        // Remove trailing \r if present (Windows line endings)
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty()) continue;

        auto op = decode_operation(line);
        if (op) {
            ops.push_back(std::move(*op));
        }
    }

    // Return the position after all complete lines read
    std::streamsize new_offset;
    if (f.eof()) {
        // Read to end of file
        f.clear();
        f.seekg(0, std::ios::end);
        new_offset = f.tellg();
    } else {
        new_offset = f.tellg();
    }

    return {std::move(ops), new_offset};
}

} // namespace CollabText::Crdt
