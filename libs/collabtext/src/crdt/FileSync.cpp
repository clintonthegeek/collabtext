#include "crdt/FileSync.h"
#include "crdt/Serialization.h"

#include <fstream>

namespace fs = std::filesystem;

namespace CollabText::Crdt {

FileSync::FileSync(Buffer& buffer, const fs::path& shared_folder,
                   const std::string& replica_name, WriterConfig cfg)
    : m_buffer(buffer)
    , m_shared_folder(shared_folder)
    , m_replica_name(replica_name)
    , m_writer_cfg(cfg) {}

void FileSync::ensure_directory_structure_() {
    auto stream_dir = m_shared_folder / "replicas" / m_replica_name / "log" / "ops";
    fs::create_directories(stream_dir);
    fs::create_directories(m_shared_folder / "local" / m_replica_name
                           / "read-cursors" / "ops");
    fs::create_directories(m_shared_folder / "meta");
    fs::create_directories(m_shared_folder / "snapshots");

    auto stignore = m_shared_folder / ".stignore";
    if (!fs::exists(stignore)) {
        std::ofstream f(stignore);
        f << "local/\n*.tmp\n*.part\n";
    }
}

void FileSync::start() {
    ensure_directory_structure_();
    auto stream_dir = m_shared_folder / "replicas" / m_replica_name / "log" / "ops";
    m_writer = std::make_unique<SegmentWriter>(stream_dir, SegmentKind::Ops, m_writer_cfg);
    m_writer->start();
    m_started = true;
}

void FileSync::push_local_op(const Operation& op) {
    if (!m_writer) return;
    Lamport ts;
    if (auto* e = std::get_if<EditOperation>(&op))      ts = e->timestamp;
    else if (auto* u = std::get_if<UndoOperation>(&op)) ts = u->timestamp;
    m_writer->append(encode_operation(op), ts.value);
}

void FileSync::flush() {
    if (m_writer) m_writer->flush();
}

SegmentReader& FileSync::reader_for_(const std::string& peer_name) {
    auto it = m_readers.find(peer_name);
    if (it != m_readers.end()) return *it->second;

    auto peer_stream_dir = m_shared_folder / "replicas" / peer_name / "log" / "ops";
    auto cursor_path = m_shared_folder / "local" / m_replica_name
                       / "read-cursors" / "ops" / (peer_name + ".bin");
    auto r = std::make_unique<SegmentReader>(peer_stream_dir, cursor_path);
    r->start();
    auto [ins, _] = m_readers.emplace(peer_name, std::move(r));
    return *ins->second;
}

size_t FileSync::poll() {
    if (!m_started) return 0;

    if (m_writer) m_writer->tick(std::chrono::steady_clock::now());

    auto replicas_dir = m_shared_folder / "replicas";
    if (!fs::exists(replicas_dir)) return 0;

    size_t total_applied = 0;

    for (auto& entry : fs::directory_iterator(replicas_dir)) {
        if (!entry.is_directory()) continue;
        std::string peer = entry.path().filename().string();
        if (peer == m_replica_name) continue;

        auto& reader = reader_for_(peer);
        auto records = reader.read_new();
        if (records.empty()) continue;

        std::vector<Operation> ops;
        ops.reserve(records.size());
        for (auto& r : records) {
            auto op = decode_operation(r);
            if (op) ops.push_back(std::move(*op));
        }
        if (!ops.empty()) {
            m_buffer.apply_ops(ops);
            total_applied += ops.size();
        }
        reader.commit();
    }

    if (total_applied > 0 && m_on_remote_ops)
        m_on_remote_ops(total_applied);
    return total_applied;
}

void FileSync::set_on_remote_ops(RemoteOpsCallback cb) {
    m_on_remote_ops = std::move(cb);
}

} // namespace CollabText::Crdt
