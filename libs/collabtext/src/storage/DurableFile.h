/// DurableFile.h — checked durable filesystem primitives.
///
/// Roadmap task L01, addressing finding F9 and recommendation R13.
///
/// ─── What was wrong ──────────────────────────────────────────────────────────
///
/// The engine claimed durability it did not have. `SegmentWriter::flush()`
/// flushed a `std::ofstream` — which moves bytes from user space into the
/// kernel — and then incremented a counter named `fsync_count`. No `fsync` was
/// ever called. `SegmentWriter::seal_open_()` renamed a temporary file over the
/// destination while discarding the `std::error_code`, having neither fsynced
/// the temporary nor synced the containing directory afterwards.
///
/// So the counter named after a durability guarantee counted something else
/// entirely, and a seal that failed looked exactly like one that succeeded. That
/// is F9: a name asserting a property the code did not have.
///
/// ─── What is guaranteed here, and what is not ───────────────────────────────
///
/// This file provides the *steps* and checks each one. It makes four promises,
///
///   1. `write_all` either writes every byte or reports how far it got. A short
///      write is a failure, never a silent truncation.
///   2. `replace_atomically` never removes the destination before the
///      replacement is in place. A reader sees the complete old file or the
///      complete new one.
///   3. The order write -> file sync -> replace -> directory sync is fixed, and
///      observable, so a test can assert it rather than trust it.
///   4. A failed step returns a typed error. Before publication the destination
///      is unchanged; after a rename or append, an error may mean publication
///      happened but its durability barrier was not confirmed. That outcome
///      must be reconciled by the store, not treated as an automatic rollback.
///
/// Sync calls request stable storage from the filesystem and device stack. Their
/// power-loss guarantees depend on that stack and platform; subprocess crash
/// tests alone do not establish power-loss behavior.
///
/// A platform that cannot supply a step returns `unsupported_operation`. It is
/// never quietly skipped, because a skipped sync is the bug this file exists to
/// remove.

#pragma once

#include <collabtext/ActorId.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace CollabText::Storage {

namespace fs = std::filesystem;

/// The steps of the durable write sequence, in the order they must occur.
enum class Step {
    Write,       ///< bytes handed to the OS
    FileSync,    ///< the file's own data forced out of the kernel
    Close,       ///< close the synced temporary before publication
    Replace,     ///< rename the temporary over the destination
    DirSync,     ///< the *name* forced out of the kernel
};

[[nodiscard]] std::string_view to_string(Step step);

/// Injected observation and failure, used by tests to assert ordering and to
/// make any individual step fail. Absent in production, where every step is the
/// real one.
///
/// The hook is called once per step, not once per byte, so it costs nothing in
/// the normal path.
struct StepHook {
    /// Fail the step by returning an error, or nullopt to let it proceed.
    std::function<std::optional<Error>(Step step, const fs::path& path)> fail;

    /// Called after every step, in order. This is how a test observes the
    /// sequence without reaching into the implementation.
    std::function<void(Step step, const fs::path& path)> observe;
};

/// The filesystem operations this file actually performs.
///
/// This exists because a hook that fires *around* a step can only prove intent.
/// A test that watches `Step::FileSync` and sees it announced is satisfied even if
/// the `fsync` call was deleted — which is exactly the F9 defect, and exactly
/// what a first attempt at these tests failed to catch. Injecting at the
/// syscall boundary makes the real calls observable and individually failable,
/// so "the file was synced before the rename" becomes a claim a test can check
/// rather than one it has to believe.
///
/// Unset entries fall back to the real implementation, so production constructs
/// one of these once and never touches it again.
struct FileOps {
    std::function<Result<size_t>(int fd, std::string_view bytes, size_t limit)> write;
    std::function<Result<void>(int fd, const fs::path& path)> sync_file;
    std::function<Result<void>(const fs::path& from, const fs::path& to)> rename;
    std::function<Result<void>(const fs::path& path)> remove;
    std::function<Result<void>(const fs::path& dir)> sync_dir;
    std::function<Result<void>(int fd, const fs::path& path)> close;

    /// The real operations.
    [[nodiscard]] static const FileOps& real();

    /// A copy with every unset entry filled in from real().
    [[nodiscard]] FileOps resolved() const;
};

/// Counters for the real operations performed. A test can compare these against
/// what the sequence claims, which is the only way to catch a step that was
/// announced and then skipped.
struct FileOpCounts {
    uint64_t writes = 0;
    uint64_t file_syncs = 0;
    uint64_t renames = 0;
    uint64_t removes = 0;
    uint64_t dir_syncs = 0;
};

/// Move-only ownership of an OS lock over a persistent local lock file.
/// The kernel lock, not PID text or elapsed time, establishes ownership. Never
/// unlink a lock file: replacement would let contenders lock different inodes.
/// Death/release closes the descriptor; PID text is diagnostic only. This local
/// process lease does not retire an offline replica or change membership.
struct ProcessLease {
    fs::path path;
    uint64_t holder_pid = 0;

    ProcessLease(const ProcessLease&) = delete;
    ProcessLease& operator=(const ProcessLease&) = delete;
    ProcessLease(ProcessLease&&) noexcept;
    ProcessLease& operator=(ProcessLease&&) noexcept;
    ~ProcessLease();

    [[nodiscard]] std::string describe() const;
    [[nodiscard]] static bool process_alive(uint64_t pid);
    /// Probe the OS lock without trusting a stale or incomplete PID record.
    [[nodiscard]] static bool held_by_live_process(const fs::path& lease_path);
    [[nodiscard]] static Result<ProcessLease> acquire(const fs::path& lease_path,
                                                      const FileOps* ops = nullptr);
    [[nodiscard]] Result<void> release();

private:
    ProcessLease() = default;
    int m_fd = -1;
};

/// Write every byte or fail.
///
/// Returns `io_error` with the byte counts in the context if the platform wrote
/// fewer bytes than requested. Retries interrupted writes and continues while
/// short writes make progress. Zero progress or an I/O error is a failure.
[[nodiscard]] Result<void> write_all(int fd, std::string_view bytes,
                                     const StepHook* hook = nullptr,
                                     const fs::path& path = {},
                                     const FileOps* ops = nullptr);

/// Force a file's data out of the kernel.
[[nodiscard]] Result<void> sync_file(int fd, const fs::path& path,
                                     const StepHook* hook = nullptr,
                                     const FileOps* ops = nullptr,
                                     FileOpCounts* counts = nullptr);

/// Force a directory entry (a rename) out of the kernel.
///
/// Returns `unsupported_operation` where the platform has no equivalent.
[[nodiscard]] Result<void> sync_directory(const fs::path& dir,
                                          const StepHook* hook = nullptr,
                                          const FileOps* ops = nullptr,
                                          FileOpCounts* counts = nullptr);

/// Replace `target` with `contents`, atomically.
///
/// The sequence is: write a sibling temporary, fsync it, rename it over the
/// target, fsync the parent directory. The target is never unlinked first, so a
/// failure before rename leaves the previous complete contents in place. A
/// directory-sync failure after rename reports commit_uncertain; the new file
/// is already published. Each attempt owns a unique sibling temporary, which
/// is removed on pre-publication failure; a cleanup failure is reported
/// rather than ignored.
[[nodiscard]] Result<void> replace_atomically(const fs::path& target,
                                              std::string_view contents,
                                              const StepHook* hook = nullptr,
                                              const FileOps* ops = nullptr,
                                              FileOpCounts* counts = nullptr);

/// An append-only durable log: the shape an operation log or a segment tail
/// needs, where atomic replacement would be wrong because it would discard the
/// records already written.
class DurableAppendFile {
public:
    /// Open (creating if needed) for appending. Does not create the parent
    /// directory; a missing directory is a caller error, not something to paper
    /// over by making one.
    [[nodiscard]] static Result<DurableAppendFile> open(const fs::path& path,
                                                        const StepHook* hook = nullptr,
                                                        const FileOps* ops = nullptr);

    DurableAppendFile(DurableAppendFile&&) noexcept;
    DurableAppendFile& operator=(DurableAppendFile&&) noexcept;
    ~DurableAppendFile();

    DurableAppendFile(const DurableAppendFile&) = delete;
    DurableAppendFile& operator=(const DurableAppendFile&) = delete;

    /// Append bytes and fsync. Returns only after the data is out of the kernel,
    /// including the directory barrier on the first sync for this handle.
    /// An error may leave bytes in the file; callers need framed recovery.
    [[nodiscard]] Result<void> append(std::string_view bytes);

    /// Append without syncing. Returns how many bytes reached the OS. Exposed
    /// for bulk loading, where the caller syncs once at the end; a caller using
    /// this must not treat it as durable.
    [[nodiscard]] Result<size_t> append_unsynced(std::string_view bytes);

    /// Force everything written so far out of the kernel.
    [[nodiscard]] Result<void> sync();

    /// Sync and close. Safe to call twice; the second call succeeds.
    [[nodiscard]] Result<void> close();

    [[nodiscard]] const fs::path& path() const { return m_path; }
    [[nodiscard]] uint64_t bytes_written() const { return m_bytes_written; }

    /// Number of successful `fsync` calls on this handle. Counts real calls, so
    /// unlike the counter F9 was about, it cannot report durability that did not
    /// happen.
    [[nodiscard]] uint64_t sync_count() const { return m_sync_count; }

    /// Cap the bytes a single write may deliver, to exercise short-write
    /// handling. Zero means unlimited. Production leaves it alone.
    void set_write_limit_for_testing(size_t bytes) { m_write_limit = bytes; }

    /// Counts of the real filesystem operations performed through this handle.
    [[nodiscard]] const FileOpCounts& counts() const { return m_counts; }

private:
    DurableAppendFile() = default;
    [[nodiscard]] Result<void> write_and_sync(std::string_view bytes, bool sync);

    fs::path m_path;
    int m_fd = -1;
    uint64_t m_bytes_written = 0;
    uint64_t m_sync_count = 0;
    size_t m_write_limit = 0;
    const StepHook* m_hook = nullptr;
    const FileOps* m_ops = nullptr;
    FileOpCounts m_counts;
    bool m_needs_directory_sync = true;
};

}  // namespace CollabText::Storage
