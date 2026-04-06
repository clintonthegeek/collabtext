#pragma once

#include "crdt/Buffer.h"
#include "crdt/Operations.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace CollabText::Crdt {

/// File-based sync transport for the CRDT engine.
///
/// Manages a shared folder per CRDT_SYNC_SPEC.md: writes local operations
/// to hash-bucketed files, reads remote operations, maintains sequence
/// counters, and feeds operations to a Buffer.
///
/// Qt-free. Platform-specific file watching is the caller's responsibility;
/// call poll() on a timer or after file change notifications.
///
/// Usage:
///   Buffer buf(replica_id);
///   FileSync sync(buf, shared_folder, "laptop-3a");
///   sync.start();
///
///   // After each local edit:
///   auto op = buf.apply_local_edit(ranges, text);
///   sync.push_local_op(op);
///
///   // Periodically (e.g., every 500ms):
///   sync.poll();
///
class FileSync {
public:
    using RemoteOpsCallback = std::function<void(size_t count)>;

    /// Construct a FileSync for the given buffer and shared folder.
    /// The replica_name is the human-readable folder name (e.g., "laptop-3a").
    FileSync(Buffer& buffer,
             const std::filesystem::path& shared_folder,
             const std::string& replica_name);

    /// Ensure directory structure exists. Call once before poll().
    void start();

    /// Queue a local operation for writing to the shared folder.
    /// Call this after every apply_local_edit(), undo(), or redo().
    void push_local_op(const Operation& op);

    /// Run one sync cycle:
    /// 1. Flush pending local ops to disk.
    /// 2. Scan remote replicas for new ops.
    /// 3. Apply new remote ops to the buffer.
    /// Returns the number of remote operations applied.
    size_t poll();

    /// Set a callback invoked after remote ops are applied.
    void set_on_remote_ops(RemoteOpsCallback cb);

    /// Get the path to the shared folder.
    const std::filesystem::path& shared_folder() const { return m_shared_folder; }

    /// Get the replica name.
    const std::string& replica_name() const { return m_replica_name; }

private:
    void ensure_directory_structure();
    void flush_local_ops();
    size_t read_remote_ops();

    /// Compute hash bucket (0-255) for a Lamport timestamp.
    static uint8_t hash_bucket(const Lamport& ts);
    /// Format a bucket number as 2-char hex.
    static std::string bucket_hex(uint8_t bucket);

    /// Read sequences.json for a replica. Returns {bucket_hex -> sequence}.
    static std::unordered_map<std::string, uint64_t>
    read_sequences(const std::filesystem::path& path);

    /// Write sequences.json atomically.
    static void write_sequences(const std::filesystem::path& path,
                                const std::unordered_map<std::string, uint64_t>& seqs);

    /// Read operations from a bucket file starting after byte offset.
    /// Returns {operations, new_byte_offset}.
    static std::pair<std::vector<Operation>, std::streamsize>
    read_bucket_file(const std::filesystem::path& path, std::streamsize after_byte);

    Buffer& m_buffer;
    std::filesystem::path m_shared_folder;
    std::string m_replica_name;

    // Pending local ops not yet flushed to disk
    std::vector<Operation> m_pending_ops;

    // Our write counters per bucket (written to our sequences.json)
    std::unordered_map<std::string, uint64_t> m_local_sequences;

    // Per-peer read state: {peer_name -> {bucket_hex -> bytes_read}}
    // Tracks how far we've read into each peer's bucket files.
    struct PeerReadState {
        std::unordered_map<std::string, std::streamsize> bucket_bytes;
    };
    std::unordered_map<std::string, PeerReadState> m_peer_read_state;

    RemoteOpsCallback m_on_remote_ops;
    bool m_started = false;
};

} // namespace CollabText::Crdt
