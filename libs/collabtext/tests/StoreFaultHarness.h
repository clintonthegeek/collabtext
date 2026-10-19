// tests/StoreFaultHarness.h — deterministic store fault injection and
// filesystem fixtures.
//
// Roadmap task V07 ("Add store fault-injection and subprocess crash
// harnesses").
//
// What this is
// ------------
// A store is durable only if every step of its write sequence can fail without
// leaving something worse than "the write did not happen": a truncated record
// that a reader believes, a cursor that advanced past data that is not there, a
// rename that half-succeeded. None of that can be tested by writing normally.
//
// This harness provides:
//
//  * FaultPoint — every step of the durable write sequence: write, file sync,
//    rename, directory sync, segment seal, cursor publication, checkpoint
//    publication, each with a before and after hook.
//  * FaultPlan — which point fails, how, and after how many arrivals. The
//    default plan injects nothing, so ordinary use is a no-op.
//  * A hook log — every hook arrival, in order, with the operation name and
//    path. Two identical runs produce identical logs, which is what makes a
//    fault "deterministically triggered and replayed" rather than merely
//    reproducible once.
//  * DurableWriter — the canonical sequence (temp file, real fsync, rename,
//    real directory fsync, seal, publish), with the hooks between the steps.
//  * Filesystem fixtures that build a stream directory in a known damaged
//    state: missing segment, out-of-order segment, partial tail, corrupt
//    segment, failed rename, simulated disk-full, and a contended writer lock.
//
// What this is not
// ----------------
// A durability claim. The production writer today flushes a std::ofstream and
// increments a counter named fsync_count without calling fsync (finding F9);
// fixing that is L01/L02. This harness does what production cannot yet do, so
// that the later fix has something to be tested against.
//
// It is also not power-loss evidence. Terminating a process proves what happens
// when user-space buffers vanish. It does not prove what survives losing the
// page cache, because the kernel keeps writing the file. Anything that claims
// otherwise is claiming something it did not test.

#pragma once

#include "crdt/SegmentFormat.h"
#include "crdt/SegmentReader.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CollabText::StoreFault {

namespace fs = std::filesystem;

/// Steps in the durable write sequence, each with a before and after hook.
enum class FaultPoint {
    WriteBefore,
    WriteAfter,
    FileSyncBefore,
    FileSyncAfter,
    RenameBefore,
    RenameAfter,
    DirSyncBefore,
    DirSyncAfter,
    SealBefore,
    SealAfter,
    CursorPublishBefore,
    CursorPublishAfter,
    CheckpointPublishBefore,
    CheckpointPublishAfter,
};

inline constexpr size_t kFaultPointCount = 14;

inline std::string_view point_name(FaultPoint point) {
    switch (point) {
    case FaultPoint::WriteBefore: return "write.before";
    case FaultPoint::WriteAfter: return "write.after";
    case FaultPoint::FileSyncBefore: return "file_sync.before";
    case FaultPoint::FileSyncAfter: return "file_sync.after";
    case FaultPoint::RenameBefore: return "rename.before";
    case FaultPoint::RenameAfter: return "rename.after";
    case FaultPoint::DirSyncBefore: return "dir_sync.before";
    case FaultPoint::DirSyncAfter: return "dir_sync.after";
    case FaultPoint::SealBefore: return "seal.before";
    case FaultPoint::SealAfter: return "seal.after";
    case FaultPoint::CursorPublishBefore: return "cursor_publish.before";
    case FaultPoint::CursorPublishAfter: return "cursor_publish.after";
    case FaultPoint::CheckpointPublishBefore: return "checkpoint_publish.before";
    case FaultPoint::CheckpointPublishAfter: return "checkpoint_publish.after";
    }
    return "unknown";
}

/// What an injected fault does to the operation it interrupts.
enum class FaultAction {
    Pass,        ///< do nothing; the default everywhere
    Fail,        ///< the operation reports failure and changes nothing
    Abort,       ///< the operation stops immediately (simulates _exit)
    Truncate,    ///< the write silently produces only the first N bytes
};

inline std::string_view action_name(FaultAction action) {
    switch (action) {
    case FaultAction::Pass: return "pass";
    case FaultAction::Fail: return "fail";
    case FaultAction::Abort: return "abort";
    case FaultAction::Truncate: return "truncate";
    }
    return "unknown";
}

/// Exit code used by FaultAction::Abort. Distinct from any signal, so a parent
/// can tell an injected abort from an unexpected crash.
inline constexpr int kAbortExitCode = 70;

struct HookVisit {
    FaultPoint point;
    std::string operation;
    fs::path path;
    size_t arrival = 0;   ///< nth arrival at this point, across all operations

    std::string describe() const {
        return std::string(point_name(point)) + " op=" + operation +
               " path=" + path.filename().string() +
               " arrival=" + std::to_string(arrival);
    }
};

/// Which faults are armed, and the record of what happened.
class FaultPlan {
public:
    struct Armed {
        FaultAction action = FaultAction::Pass;
        size_t skip = 0;        ///< let this many arrivals pass first
        size_t truncate_to = 0; ///< bytes written when action == Truncate
    };

    void arm(FaultPoint point, FaultAction action, size_t skip = 0,
             size_t truncate_to = 0) {
        m_armed[static_cast<size_t>(point)] = Armed{action, skip, truncate_to};
    }

    void disarm_all() {
        for (auto& entry : m_armed) entry = Armed{};
    }

    /// Record a hook arrival and return what should happen. `skip` arrivals are
    /// counted before the fault fires, so a test can say "fail the third rename"
    /// without any state of its own.
    FaultAction visit(FaultPoint point, std::string_view operation, const fs::path& path) {
        const size_t index = static_cast<size_t>(point);
        ++m_arrivals[index];
        m_log.push_back(HookVisit{point, std::string(operation), path, m_arrivals[index]});
        const Armed& fault = m_armed[index];
        if (fault.action == FaultAction::Pass) return FaultAction::Pass;
        if (m_arrivals[index] <= fault.skip) return FaultAction::Pass;
        return fault.action;
    }

    size_t truncate_to(FaultPoint point) const {
        return m_armed[static_cast<size_t>(point)].truncate_to;
    }

    /// Number of arrivals at a point.
    size_t arrivals(FaultPoint point) const {
        return m_arrivals[static_cast<size_t>(point)];
    }

    const std::vector<HookVisit>& log() const { return m_log; }

    /// A stable text form, for comparing two runs.
    std::string log_text() const {
        std::string text;
        for (const auto& visit : m_log) {
            text += visit.describe();
            text += '\n';
        }
        return text;
    }

private:
    std::array<Armed, kFaultPointCount> m_armed{};
    std::array<size_t, kFaultPointCount> m_arrivals{};
    std::vector<HookVisit> m_log;
};

/// fsync a directory so a rename is durable, not merely visible.
inline bool sync_directory(const fs::path& dir) {
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    const int rc = ::fsync(fd);
    ::close(fd);
    return rc == 0;
}

/// The canonical durable write sequence, with a hook between every step.
///
/// Records are base64-free newline-delimited plain text in a `.log` file plus a
/// sealed `.seg` companion, which keeps the fixtures readable by a test without
/// a decoder in the way. Real fsync is used at both the file and the directory
/// level: the point of the harness is that the steps exist and can fail.
class DurableWriter {
public:
    DurableWriter(fs::path dir, std::string name, FaultPlan* plan = nullptr)
        : m_dir(std::move(dir)), m_name(std::move(name)), m_plan(plan) {
        fs::create_directories(m_dir);
        m_open_path = m_dir / (m_name + ".open");
        m_cursor_path = m_dir / (m_name + ".cursor");
        m_checkpoint_path = m_dir / (m_name + ".checkpoint");
    }

    const fs::path& directory() const { return m_dir; }
    const fs::path& open_path() const { return m_open_path; }
    const fs::path& cursor_path() const { return m_cursor_path; }
    const fs::path& checkpoint_path() const { return m_checkpoint_path; }

    size_t fsync_count() const { return m_fsync_count; }
    size_t rename_count() const { return m_rename_count; }
    const std::string& last_error() const { return m_last_error; }
    uint64_t published_cursor() const { return m_published_cursor; }

    /// Simulate a full filesystem: writes stop after this many bytes.
    void set_disk_full_after(size_t bytes) { m_disk_full_after = bytes; }

    /// Contended writer lock. Only one writer may hold the stream at a time.
    bool acquire_lock() {
        std::error_code ec;
        if (fs::exists(lock_path(), ec)) {
            m_last_error = "writer lock already held";
            return false;
        }
        std::ofstream file(lock_path(), std::ios::binary | std::ios::trunc);
        file << getpid() << '\n';
        file.flush();
        if (!file) {
            m_last_error = "cannot create writer lock";
            return false;
        }
        m_locked = true;
        return true;
    }

    void release_lock() {
        if (!m_locked) return;
        std::error_code ec;
        fs::remove(lock_path(), ec);
        m_locked = false;
    }

    fs::path lock_path() const { return m_dir / (m_name + ".lock"); }

    /// Append one record to the open tail.
    ///
    /// The tail is appended in place and fsynced: an append-only log is not
    /// replaced through a temp file, because that would discard the records
    /// already written. The atomic temp-and-rename discipline applies to the
    /// steps that *replace* a file — sealing a segment, publishing a cursor or
    /// a checkpoint — and those are where the rename and directory-sync hooks
    /// live.
    bool append(std::string_view record) {
        m_last_error.clear();
        const std::string line = std::string(record) + "\n";

        // One hook arrival per step, so the log stays a faithful sequence.
        const FaultAction at_write =
            stop(FaultPoint::WriteBefore, "append.write", m_open_path);
        if (at_write == FaultAction::Fail) {
            m_last_error = "write failed (injected)";
            return false;
        }
        if (m_disk_full_after != kNoLimit &&
            m_bytes_written + line.size() > m_disk_full_after) {
            m_last_error = "no space left on device (simulated)";
            return false;
        }

        std::string payload = line;
        if (at_write == FaultAction::Truncate) {
            const size_t keep =
                m_plan ? m_plan->truncate_to(FaultPoint::WriteBefore) : 0;
            payload = line.substr(0, std::min(keep, line.size()));
        }
        {
            std::ofstream file(m_open_path, std::ios::binary | std::ios::app);
            file << payload;
            file.flush();
            if (!file) {
                m_last_error = "short write (simulated)";
                return false;
            }
        }
        m_bytes_written += payload.size();
        note(FaultPoint::WriteAfter, "append.write", m_open_path);

        if (stop(FaultPoint::FileSyncBefore, "append.file_sync", m_open_path)
            == FaultAction::Fail) {
            m_last_error = "file sync failed (injected)";
            return false;
        }
        if (!fsync_file(m_open_path)) {
            m_last_error = "fsync failed";
            return false;
        }
        ++m_fsync_count;
        note(FaultPoint::FileSyncAfter, "append.file_sync", m_open_path);
        return true;
    }

    /// Seal the open tail.
    ///
    /// The seal is the step that replaces a file: the tail is written to a temp
    /// file, fsynced, renamed into place, and the directory is fsynced so the
    /// new name is durable. A seal that fails at any step leaves the open tail
    /// exactly as it was, so nothing is lost by an interrupted seal.
    bool seal(uint64_t segment_id) {
        m_last_error.clear();
        const fs::path sealed = m_dir / sealed_file_name(segment_id);
        const fs::path tmp = fs::path(sealed.string() + ".tmp");

        if (stop(FaultPoint::SealBefore, "seal.segment", sealed) == FaultAction::Fail) {
            m_last_error = "segment seal failed (injected)";
            return false;
        }
        std::string contents;
        {
            std::ifstream in(m_open_path, std::ios::binary);
            contents.assign(std::istreambuf_iterator<char>(in),
                            std::istreambuf_iterator<char>());
        }
        if (stop(FaultPoint::WriteBefore, "seal.write", tmp) == FaultAction::Fail) {
            m_last_error = "sealed write failed (injected)";
            return false;
        }
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out << contents;
            out.flush();
            if (!out) {
                m_last_error = "sealed segment write failed";
                return false;
            }
        }
        note(FaultPoint::WriteAfter, "seal.write", tmp);
        if (stop(FaultPoint::FileSyncBefore, "seal.file_sync", tmp) == FaultAction::Fail) {
            m_last_error = "sealed file sync failed (injected)";
            return false;
        }
        if (!fsync_file(tmp)) {
            m_last_error = "sealed fsync failed";
            return false;
        }
        ++m_fsync_count;
        note(FaultPoint::FileSyncAfter, "seal.file_sync", tmp);

        if (stop(FaultPoint::RenameBefore, "seal.rename", tmp) == FaultAction::Fail) {
            m_last_error = "seal rename failed (injected)";
            return false;
        }
        std::error_code ec;
        fs::rename(tmp, sealed, ec);
        if (ec) {
            m_last_error = "seal rename failed: " + ec.message();
            return false;
        }
        ++m_rename_count;
        note(FaultPoint::RenameAfter, "seal.rename", sealed);

        if (stop(FaultPoint::DirSyncBefore, "seal.dir_sync", m_dir / m_name)
            == FaultAction::Fail) {
            m_last_error = "directory sync failed (injected)";
            return false;
        }
        if (!sync_directory(m_dir)) {
            m_last_error = "directory fsync failed";
            return false;
        }
        ++m_fsync_count;
        note(FaultPoint::DirSyncAfter, "seal.dir_sync", m_dir / m_name);

        fs::remove(m_open_path, ec);
        note(FaultPoint::SealAfter, "seal.segment", sealed);
        return true;
    }

    /// Publish read progress. The cursor is written to a temp file and renamed,
    /// so a reader sees the old or the new value and never a partial one.
    bool publish_cursor(uint64_t position) {
        m_last_error.clear();
        if (stop(FaultPoint::CursorPublishBefore, "cursor.publish", m_cursor_path)
            == FaultAction::Fail) {
            m_last_error = "cursor publication failed (injected)";
            return false;
        }
        const fs::path tmp = m_cursor_path;
        std::string tmp_path = tmp.string() + ".tmp";
        {
            std::ofstream file(tmp_path, std::ios::binary | std::ios::trunc);
            file << position << '\n';
            file.flush();
        }
        std::error_code ec;
        fs::rename(tmp_path, tmp, ec);
        if (ec) {
            m_last_error = "cursor rename failed: " + ec.message();
            return false;
        }
        sync_directory(m_dir);
        m_published_cursor = position;
        note(FaultPoint::CursorPublishAfter, "cursor.publish", m_cursor_path);
        return true;
    }

    /// Publish a checkpoint (last sealed id + cursor), same discipline.
    bool publish_checkpoint(uint64_t last_sealed, uint64_t cursor) {
        m_last_error.clear();
        if (stop(FaultPoint::CheckpointPublishBefore, "checkpoint.publish",
                 m_checkpoint_path) == FaultAction::Fail) {
            m_last_error = "checkpoint publication failed (injected)";
            return false;
        }
        const std::string tmp_path = m_checkpoint_path.string() + ".tmp";
        {
            std::ofstream file(tmp_path, std::ios::binary | std::ios::trunc);
            file << last_sealed << ' ' << cursor << '\n';
            file.flush();
        }
        std::error_code ec;
        fs::rename(tmp_path, m_checkpoint_path, ec);
        if (ec) {
            m_last_error = "checkpoint rename failed: " + ec.message();
            return false;
        }
        sync_directory(m_dir);
        note(FaultPoint::CheckpointPublishAfter, "checkpoint.publish",
             m_checkpoint_path);
        return true;
    }

    std::string sealed_file_name(uint64_t id) const {
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "%s-%010llu.sealed",
                      m_name.c_str(), static_cast<unsigned long long>(id));
        return std::string(buffer);
    }

    /// Production segment naming, used by the fixtures that the real
    /// SegmentReader reads back.
    static std::string segment_file_name(uint64_t id) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%010llu.seg.zst",
                      static_cast<unsigned long long>(id));
        return std::string(buffer);
    }

private:
    static constexpr size_t kNoLimit = static_cast<size_t>(-1);

    static bool fsync_file(const fs::path& path) {
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) return false;
        const int rc = ::fsync(fd);
        ::close(fd);
        return rc == 0;
    }

    /// Handle a "before" hook. An Abort terminates the *process* with _exit:
    /// no unwinding, no destructors, no stdio buffers flushed. That is what a
    /// crash looks like from the filesystem's side, and it is the whole point of
    /// using _exit instead of an exception.
    FaultAction stop(FaultPoint point, std::string_view operation, const fs::path& path) {
        if (!m_plan) return FaultAction::Pass;
        const FaultAction action = m_plan->visit(point, operation, path);
        if (action == FaultAction::Abort) {
            std::fflush(nullptr);
            ::_exit(kAbortExitCode);
        }
        return action;
    }

    /// An "after" hook. Abort is honored here too: a crash immediately after a
    /// step is the more interesting case, because the step's effect is already
    /// on disk and nothing afterwards got to clean up after it.
    void note(FaultPoint point, std::string_view operation, const fs::path& path) {
        if (!m_plan) return;
        if (m_plan->visit(point, operation, path) == FaultAction::Abort) {
            std::fflush(nullptr);
            ::_exit(kAbortExitCode);
        }
    }

    fs::path m_dir;
    std::string m_name;
    FaultPlan* m_plan = nullptr;
    fs::path m_open_path;
    fs::path m_cursor_path;
    fs::path m_checkpoint_path;
    size_t m_fsync_count = 0;
    size_t m_rename_count = 0;
    size_t m_bytes_written = 0;
    size_t m_disk_full_after = kNoLimit;
    uint64_t m_published_cursor = 0;
    bool m_locked = false;
    std::string m_last_error;
};

// ============================================================================
// Filesystem fixtures
// ============================================================================
//
// Build a stream directory in a known damaged state. The segment files use the
// production sealed-segment encoding, so the production SegmentReader is what
// reads them back: the fixtures are checked against the real reader rather than
// a convenient one.

/// Write `count` sealed segments with `records_per_segment` records each.
inline void write_segments(const fs::path& dir, Crdt::SegmentKind kind,
                           uint64_t first_id, size_t count, size_t records_per_segment) {
    fs::create_directories(dir);
    for (size_t s = 0; s < count; ++s) {
        const uint64_t id = first_id + s;
        std::vector<std::string> records;
        for (size_t r = 0; r < records_per_segment; ++r)
            records.push_back("segment " + std::to_string(id) + " record " +
                              std::to_string(r));
        Crdt::SegmentHeader header;
        header.format_version = 1;
        header.kind = kind;
        header.flags = 0;
        header.first_lamport = id * 1000;
        header.last_lamport = id * 1000 + records_per_segment;
        const std::string bytes =
            Crdt::encode_sealed_segment(header, records, /*zstd_level=*/1);
        const fs::path path = dir / DurableWriter::segment_file_name(id);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << bytes;
    }
}

/// Remove a segment, leaving a gap in the sequence.
inline void remove_segment(const fs::path& dir, uint64_t id) {
    std::error_code ec;
    fs::remove(dir / DurableWriter::segment_file_name(id), ec);
}

/// Truncate a file to `bytes`, producing a partial tail.
inline bool truncate_file(const fs::path& path, size_t bytes) {
    std::error_code ec;
    const uint64_t size = fs::file_size(path, ec);
    if (ec || bytes >= size) return false;
    fs::resize_file(path, bytes, ec);
    return !ec;
}

/// Corrupt a sealed segment by flipping bytes in its payload, so decompression
/// or the checksum fails. Returns false if the file is too small to damage.
inline bool corrupt_file(const fs::path& path) {
    std::error_code ec;
    const uint64_t size = fs::file_size(path, ec);
    if (ec || size < 8) return false;
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file) return false;
    // Leave the first bytes alone so the header still parses and the failure is
    // in the body, which is the interesting case.
    for (uint64_t offset = size / 2; offset < size / 2 + 4 && offset < size; ++offset) {
        file.seekg(static_cast<std::streamoff>(offset));
        char byte = 0;
        file.read(&byte, 1);
        byte = static_cast<char>(byte ^ 0xFF);
        file.seekp(static_cast<std::streamoff>(offset));
        file.write(&byte, 1);
    }
    return true;
}

/// Read every record the production reader can recover from a directory,
/// advancing a cursor file as it goes.
inline std::vector<std::string> read_all(const fs::path& dir, const fs::path& cursor) {
    Crdt::SegmentReader reader(dir, cursor);
    reader.start();
    std::vector<std::string> records;
    while (true) {
        auto batch = reader.read_new();
        if (batch.empty()) break;
        records.insert(records.end(), batch.begin(), batch.end());
        auto committed = reader.commit(batch.token);
        if (!committed.ok()) break;
    }
    return records;
}

}  // namespace CollabText::StoreFault
