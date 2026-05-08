#pragma once

#include <crdt/SegmentReader.h>
#include <crdt/SegmentWriter.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace CollabText::Crdt {

struct StreamEntry {
    std::string id;
    uint16_t replica_id = 0;
    uint64_t seq = 0;
    std::string timestamp;
    std::string payload;
    bool tombstone = false;
};

class StreamSync {
public:
    enum class StreamType { AppendOnly, AnchorKeyed };

    StreamSync(const std::filesystem::path& shared_folder,
               const std::string& replica_name,
               WriterConfig writer_cfg = WriterConfig{});

    void start();
    void register_stream(const std::string& name, StreamType type);
    void push(const std::string& stream, const StreamEntry& entry);
    size_t poll();

    /// Force-fsync all open tails. Used by save/shutdown paths.
    void flush();

    std::vector<StreamEntry> entries(const std::string& stream) const;

    using NewEntriesCallback =
        std::function<void(const std::string& stream, size_t count)>;
    void set_on_new_entries(NewEntriesCallback cb);

private:
    struct StreamState {
        StreamType type = StreamType::AppendOnly;
        std::unique_ptr<SegmentWriter> writer;
        std::unordered_map<std::string, std::unique_ptr<SegmentReader>> readers;
        std::unordered_map<std::string, StreamEntry> merged;
    };

    StreamState& ensure_started_(const std::string& name);
    SegmentReader& reader_for_(StreamState& s, const std::string& stream,
                               const std::string& peer);
    size_t read_remote_stream_(const std::string& name, StreamState& s);

    std::filesystem::path m_shared_folder;
    std::string m_replica_name;
    WriterConfig m_writer_cfg;
    std::unordered_map<std::string, StreamState> m_streams;
    NewEntriesCallback m_on_new_entries;
    bool m_started = false;
};

} // namespace CollabText::Crdt
