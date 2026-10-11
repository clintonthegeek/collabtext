/// src/transport/StreamSync.h — the file-backed transport's header.
///
/// Roadmap task A02a moved this out of include/collabtext/. It is here rather
/// than in the public tree because it names SegmentWriter and SegmentReader in
/// its members, and those reach zstd — so a Core-only consumer could not have
/// included it. A02a's rule is that every *advertised* public header compiles
/// on its own; the file transport is implemented but is not part of this alpha's
/// advertised surface, and A06a owns its exported form. W02 and W03 verify it
/// through the installed package.
///
/// The alternative was a pimpl here. That is a real answer, but it is A06a's
/// call rather than A02a's, and doing it now would have meant inventing an
/// ownership shape nobody has reviewed.

#pragma once

#include <collabtext/ActorId.h>
#include <collabtext/OpStream.h>
#include <crdt/SegmentReader.h>
#include <crdt/SegmentWriter.h>
#include <storage/DurableFile.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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

/// Decode the legacy ack JSON observation for one peer. Input is bounded and
/// malformed or overflowing decimal values are rejected.
std::optional<uint64_t> decode_legacy_peer_ack(std::string_view json,
                                               uint16_t replica_id);

// Legacy file-backed synchronization implementation. P07's OpStream contract
// requires a durable receiver recovery-point transaction; this implementation
// does not provide that guarantee and therefore is not an OpStream adapter.
class StreamSync {
public:
    enum class StreamType { AppendOnly, AnchorKeyed };

    StreamSync(const std::filesystem::path& shared_folder,
               const std::string& replica_name,
               uint16_t replica_id = 0,
               WriterConfig writer_cfg = WriterConfig{});

    void start();
    void register_stream(const std::string& name, StreamType type);

    // Legacy push — constructs StreamEntry internally.
    void push(const std::string& stream_name, const std::string& payload);

    // Existing low-level push — takes a fully populated StreamEntry
    void push(const std::string& stream, const StreamEntry& entry);

    size_t poll();

    const std::optional<Error>& last_error() const { return m_last_error; }
    /// Codec failures retain the entire uncommitted batch and freeze its reader.
    /// P07 defines the new contract; L03/L08 must implement it for this adapter.
    const std::vector<std::string>& blocked_records(const std::string& stream,
                                                   const std::string& peer) const;

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
    void set_on_inbound(InboundCallback cb);

    uint64_t lowest_peer_acked_lamport() const;
    void set_on_ack_update(std::function<void(uint64_t)> cb);

private:
    struct StreamState {
        StreamType type = StreamType::AppendOnly;
        std::unique_ptr<SegmentWriter> writer;
        std::unordered_map<std::string, std::unique_ptr<SegmentReader>> readers;
        std::unordered_map<std::string, StreamEntry> merged;
        std::unordered_set<std::string> callback_completed;
        std::unordered_map<std::string, std::vector<std::string>> blocked;
        uint64_t next_seq = 1;
        bool sequence_recovered = false;
    };

    struct AckState {
        uint64_t max_lamport = 0;
        std::string last_observed_at;  // ISO 8601 UTC, refreshed only when max_lamport advances
    };

    StreamState& ensure_started_(const std::string& name);
    SegmentReader& reader_for_(StreamState& s, const std::string& stream,
                               const std::string& peer);
    bool recover_next_sequence_(const std::string& stream, StreamState& s);
    size_t read_remote_stream_(const std::string& name, StreamState& s);
    void write_acks_();
    void recompute_fence_();
    uint64_t read_peer_ack_(const std::filesystem::path& acks_path) const;
    std::string current_iso_utc_() const;

    std::filesystem::path m_shared_folder;
    std::string m_replica_name;
    uint16_t m_replica_id = 0;
    WriterConfig m_writer_cfg;
    std::optional<Storage::ProcessLease> m_writer_lease;
    std::unordered_map<std::string, StreamState> m_streams;
    NewEntriesCallback m_on_new_entries;
    InboundCallback m_on_inbound;
    std::function<void(uint64_t)> m_ack_update_cb;
    std::unordered_map<uint16_t, AckState> m_ack_state;  // peer replica_id → state
    uint64_t m_cached_fence = 0;
    bool m_started = false;
    std::optional<Error> m_last_error;
};

/// Durable file-backed implementation of the P07 contract. Kept separate
/// from the legacy StreamSync API so callers must opt into ActorId/epoch
/// scoped delivery explicitly.
class FileOpStream final : public CollabText::OpStream {
public:
    FileOpStream(std::filesystem::path shared_folder, ActorId local_actor,
                 WriterConfig writer_cfg = WriterConfig{});
    ~FileOpStream() override;
    FileOpStream(const FileOpStream&) = delete;
    FileOpStream& operator=(const FileOpStream&) = delete;

    [[nodiscard]] Result<void> start();
    /// Read every retained envelope in a scope without consulting receiver
    /// receipts or advancing cursors. Used by full-replay session recovery.
    [[nodiscard]] Result<void> replay_all(
        const StreamScope&, const std::function<Result<void>(const InboundDelivery&)>& visitor) const;
    /// Scan and validate the complete retained delivery log, returning each
    /// producer's highest contiguous sequence. This describes log coverage
    /// only; callers must separately prove that their checkpoint represents
    /// every covered delivery.
    [[nodiscard]] Result<std::map<ActorId, uint64_t>>
    delivery_frontiers(const StreamScope &) const;
    /// Validate the complete retained log but visit only records beyond the
    /// checkpoint's per-producer delivery frontiers. Covered records are still
    /// scanned for gaps/corruption; this does not persist reader cursors.
    [[nodiscard]] Result<void> replay_uncovered(
        const StreamScope &, const std::map<ActorId, uint64_t> &delivery_frontiers,
        const std::function<Result<void>(const InboundDelivery &)> &visitor) const;
    /// Read one producer's retained envelopes without consulting receiver
    /// receipts or persisting a read cursor. Used for explicit branch export.
    [[nodiscard]] Result<void> replay_producer(
        const StreamScope&, const ActorId& producer,
        const std::function<Result<void>(const InboundDelivery&)>& visitor) const;
    [[nodiscard]] Result<void> flush();
    [[nodiscard]] Result<void> close();
    [[nodiscard]] Result<void> register_stream(const StreamScope&) override;
    [[nodiscard]] Result<void> push(const OutboundDelivery&, PushDurability) override;
    [[nodiscard]] Result<SubscriptionId> subscribe(const StreamScope&, InboundHandler) override;
    [[nodiscard]] Result<SubscriptionId> subscribe_receipts(ReceiptHandler) override;
    [[nodiscard]] Result<void> unsubscribe(SubscriptionId) override;
    [[nodiscard]] Result<PollReport> poll(const StreamScope&, PollBudget = {}) override;
    [[nodiscard]] Result<std::vector<StreamReceipt>> receipts(const StreamScope&) const override;

private:
    struct ScopeState;
    struct WriterState;
    std::filesystem::path m_shared_folder;
    ActorId m_local_actor;
    WriterConfig m_writer_cfg;
    std::optional<Storage::ProcessLease> m_lease;
    std::unordered_map<std::string, std::unique_ptr<ScopeState>> m_scopes;
    std::unordered_map<std::string, std::unique_ptr<WriterState>> m_writers;
    std::unordered_map<SubscriptionId, StreamScope> m_subscriptions;
    std::unordered_map<SubscriptionId, ReceiptHandler> m_receipt_subscriptions;
    void refresh_receipts_(ScopeState& state);
    SubscriptionId m_next_subscription = 1;
    bool m_started = false;
};

} // namespace CollabText::Crdt
