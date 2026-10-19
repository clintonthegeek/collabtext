#pragma once

#include <collabtext/ActorId.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CollabText { class RecoveryPoint; }

namespace CollabText::Crdt {

struct SegmentCursor {
    /// Highest contiguous sealed segment whose records were committed.
    uint64_t last_sealed = 0;
    /// Provisional segment currently being consumed, or zero.
    uint64_t open_id = 0;
    /// Byte offset through complete open-tail lines only.
    uint64_t open_bytes = 0;
    /// Identity of the consumed open-tail prefix, allowing rewrites to replay.
    std::array<uint8_t, 32> open_prefix_hash{};
    /// Durable consumer checkpoint associated with this persisted frontier.
    std::string recovery_point_id;
};

/// Decode a versioned persisted cursor. Trailing and truncated bytes fail.
std::optional<SegmentCursor> decode_segment_cursor(std::string_view bytes);

class SegmentDeliveryToken {
public:
    SegmentDeliveryToken() = default;
    [[nodiscard]] bool operator==(const SegmentDeliveryToken&) const = default;

private:
    uint64_t owner_id = 0;
    uint64_t generation = 0;
    friend class SegmentReader;
};

struct SegmentReadBatch {
    std::vector<std::string> records;
    SegmentDeliveryToken token;
    /// Missing or invalid next segment. Any returned records still form a
    /// contiguous prefix and may be committed after the consumer accepts them.
    std::optional<CollabText::Error> gap;

    [[nodiscard]] size_t size() const noexcept { return records.size(); }
    [[nodiscard]] bool empty() const noexcept { return records.empty(); }
    const std::string& operator[](size_t index) const { return records[index]; }
    auto begin() noexcept { return records.begin(); }
    auto end() noexcept { return records.end(); }
    auto begin() const noexcept { return records.begin(); }
    auto end() const noexcept { return records.end(); }
};

class SegmentReader {
public:
    SegmentReader(std::filesystem::path peer_stream_dir,
                  std::filesystem::path cursor_path);

    /// Without a recovered checkpoint, persisted coverage is ignored and logs
    /// replay from the beginning. A matching checkpoint opts into that cursor.
    void start(const CollabText::RecoveryPoint* recovered_point = nullptr);

    /// Re-read from the durable cursor and return one bounded contiguous batch.
    /// A subsequent read invalidates the prior token; reading never commits.
    [[nodiscard]] SegmentReadBatch read_new();

    /// Accept this batch in memory only. Legacy consumers without a durable
    /// application recovery point must use this so a restart replays logs.
    [[nodiscard]] CollabText::Result<void> commit(const SegmentDeliveryToken& token);

    /// Persist coverage only after the consumer has durably stored the named
    /// recovery point that can restore all accepted records.
    [[nodiscard]] CollabText::Result<void> commit_durable(
        const SegmentDeliveryToken& token, const CollabText::RecoveryPoint& point);

private:
    std::filesystem::path m_dir;
    std::filesystem::path m_cursor_path;
    uint64_t m_reader_id = 0;
    SegmentCursor m_committed;
    uint64_t m_generation = 0;
    std::optional<SegmentCursor> m_issued_cursor;

    void load_cursor_(const CollabText::RecoveryPoint* recovered_point);
};

} // namespace CollabText::Crdt
