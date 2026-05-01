#pragma once

#include "crdt/Buffer.h"
#include "crdt/Operations.h"
#include "crdt/SegmentReader.h"
#include "crdt/SegmentWriter.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace CollabText::Crdt {

/// File-based sync transport for the CRDT engine using append-only segment
/// logs. One open tail per replica + immutable sealed bodies. Per-peer read
/// cursors live in `local/` and are not synced.
class FileSync {
public:
    using RemoteOpsCallback = std::function<void(size_t count)>;

    FileSync(Buffer& buffer,
             const std::filesystem::path& shared_folder,
             const std::string& replica_name,
             WriterConfig writer_cfg = WriterConfig{});

    void start();
    void push_local_op(const Operation& op);
    size_t poll();

    /// Force-fsync the open tail. Used by save/shutdown paths.
    void flush();

    void set_on_remote_ops(RemoteOpsCallback cb);

    const std::filesystem::path& shared_folder() const { return m_shared_folder; }
    const std::string& replica_name() const { return m_replica_name; }

private:
    void ensure_directory_structure_();
    SegmentReader& reader_for_(const std::string& peer_name);

    Buffer& m_buffer;
    std::filesystem::path m_shared_folder;
    std::string m_replica_name;
    WriterConfig m_writer_cfg;

    std::unique_ptr<SegmentWriter> m_writer;
    std::unordered_map<std::string, std::unique_ptr<SegmentReader>> m_readers;

    RemoteOpsCallback m_on_remote_ops;
    bool m_started = false;
};

} // namespace CollabText::Crdt
