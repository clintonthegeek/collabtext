#pragma once

#include <collabtext/OpStream.h>
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

class StreamSync : public CollabText::OpStream {
public:
    enum class StreamType { AppendOnly, AnchorKeyed };

    StreamSync(const std::filesystem::path& shared_folder,
               const std::string& replica_name,
               uint16_t replica_id = 0,
               WriterConfig writer_cfg = WriterConfig{});

    void start();
    void register_stream(const std::string& name, StreamType type);

    // OpStream::push override — constructs StreamEntry internally
    void push(const std::string& stream_name, const std::string& payload) override;

    // Existing low-level push — takes a fully populated StreamEntry
    void push(const std::string& stream, const StreamEntry& entry);

    size_t poll();

    /// Force-fsync all open tails. Used by save/shutdown paths.
    void flush();

    std::vector<StreamEntry> entries(const std::string& stream) const;

    using NewEntriesCallback =
        std::function<void(const std::string& stream, size_t count)>;
    void set_on_new_entries(NewEntriesCallback cb);

    using InboundCallback =
        std::function<void(const std::string& stream_name,
                           uint16_t           producer_replica_id,
                           const std::string& payload)>;
    void set_on_inbound(InboundCallback cb) override;

    uint64_t lowest_peer_acked_lamport() const override;
    void set_on_ack_update(std::function<void(uint64_t)> cb) override;

private:
    struct StreamState {
        StreamType type = StreamType::AppendOnly;
        std::unique_ptr<SegmentWriter> writer;
        std::unordered_map<std::string, std::unique_ptr<SegmentReader>> readers;
        std::unordered_map<std::string, StreamEntry> merged;
        uint64_t next_seq = 1;
    };

    struct AckState {
        uint64_t max_lamport = 0;
        std::string last_observed_at;  // ISO 8601 UTC, refreshed only when max_lamport advances
    };

    StreamState& ensure_started_(const std::string& name);
    SegmentReader& reader_for_(StreamState& s, const std::string& stream,
                               const std::string& peer);
    size_t read_remote_stream_(const std::string& name, StreamState& s);
    void write_acks_();
    void recompute_fence_();
    uint64_t read_peer_ack_(const std::filesystem::path& acks_path) const;
    std::string current_iso_utc_() const;

    std::filesystem::path m_shared_folder;
    std::string m_replica_name;
    uint16_t m_replica_id = 0;
    WriterConfig m_writer_cfg;
    std::unordered_map<std::string, StreamState> m_streams;
    NewEntriesCallback m_on_new_entries;
    InboundCallback m_on_inbound;
    std::function<void(uint64_t)> m_ack_update_cb;
    std::unordered_map<uint16_t, AckState> m_ack_state;  // peer replica_id → state
    uint64_t m_cached_fence = 0;
    bool m_started = false;
};

} // namespace CollabText::Crdt
