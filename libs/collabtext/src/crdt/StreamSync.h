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

} // namespace CollabText::Crdt
