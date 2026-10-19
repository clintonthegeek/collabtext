#pragma once

#include "crdt/SegmentFormat.h"
#include "storage/DurableFile.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CollabText::Crdt {

struct WriterConfig {
    size_t flush_bytes = 1024;
    std::chrono::milliseconds flush_idle{250};
    size_t seal_bytes = 64 * 1024;
    std::chrono::seconds seal_idle{30};
    int    zstd_level = 3;
    size_t max_pending_bytes = 8u * 1024u * 1024u;
    size_t max_tail_bytes = 64u * 1024u * 1024u;
    const Storage::StepHook* step_hook = nullptr;
    const Storage::FileOps* file_ops = nullptr;
};

struct SegmentStats {
    uint64_t segments_sealed     = 0;
    uint64_t segments_opened     = 0;
    uint64_t bytes_written_open  = 0;
    uint64_t bytes_written_sealed = 0;
    uint64_t fsync_count         = 0;
};

class SegmentWriter {
public:
    SegmentWriter(std::filesystem::path stream_dir,
                  SegmentKind kind,
                  WriterConfig config = WriterConfig{});

    ~SegmentWriter();
    [[nodiscard]] Result<void> start();

    /// Append one record. `lamport` is captured for header bookkeeping.
    /// `payload` is the raw caller blob; the writer wraps base64 + newline.
    [[nodiscard]] Result<void> append(std::string_view payload, uint64_t lamport);

    /// Drive flush + seal decisions using injected time.
    [[nodiscard]] Result<void> tick(std::chrono::steady_clock::time_point now);

    /// Force-fsync the current open tail (no seal).
    [[nodiscard]] Result<void> flush();

    /// Force-seal the current open tail. Used by shutdown.
    [[nodiscard]] Result<void> close();

    SegmentStats stats() const { return m_stats; }

private:
    [[nodiscard]] Result<void> recover_();
    [[nodiscard]] Result<void> open_new_segment_(uint64_t id);
    [[nodiscard]] Result<void> flush_pending_to_open_(std::chrono::steady_clock::time_point now);
    [[nodiscard]] Result<void> seal_open_();
    static std::string segment_filename_(uint64_t id, std::string_view suffix);

    std::filesystem::path m_dir;
    SegmentKind m_kind;
    WriterConfig m_cfg;

    struct Pending {
        std::string payload;
        uint64_t lamport;
    };
    std::vector<Pending> m_pending;
    size_t m_pending_bytes = 0;
    std::optional<std::chrono::steady_clock::time_point> m_first_pending;

    uint64_t m_open_id = 0;
    std::optional<Storage::DurableAppendFile> m_open_file;
    size_t m_open_size = 0;
    uint64_t m_open_first_lamport = 0;
    uint64_t m_open_last_lamport  = 0;
    bool m_open_has_records = false;
    std::optional<std::chrono::steady_clock::time_point> m_open_first_record_time;

    std::vector<std::string> m_open_records_b64;

    SegmentStats m_stats;
    bool m_started = false;
    bool m_closed = false;
    std::optional<Error> m_failure;
};

} // namespace CollabText::Crdt
