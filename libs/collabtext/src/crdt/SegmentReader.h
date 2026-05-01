#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace CollabText::Crdt {

class SegmentReader {
public:
    SegmentReader(std::filesystem::path peer_stream_dir,
                  std::filesystem::path cursor_path);

    void start();

    /// Read all records not yet consumed. Advances in-memory cursor.
    /// Caller must call commit() to persist progress to disk.
    std::vector<std::string> read_new();

    /// Atomic-rename the cursor file to reflect in-memory progress.
    void commit();

private:
    std::filesystem::path m_dir;
    std::filesystem::path m_cursor_path;

    uint64_t m_last_sealed = 0;
    uint64_t m_open_id = 0;
    uint64_t m_open_bytes = 0;

    void load_cursor_();
    void save_cursor_() const;
};

} // namespace CollabText::Crdt
