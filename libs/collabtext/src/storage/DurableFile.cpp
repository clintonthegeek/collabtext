/// DurableFile.cpp — see DurableFile.h for the contract and for why it exists.

#include "storage/DurableFile.h"

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  include <io.h>
#  include <windows.h>
#  define COLLABTEXT_OPEN  ::_open
#  define COLLABTEXT_CLOSE ::_close
#  define COLLABTEXT_FSYNC ::_commit
#  define COLLABTEXT_WRITE ::_write
#  define COLLABTEXT_LSEEK ::_lseeki64
#  define COLLABTEXT_O_APPEND _O_APPEND
#  define COLLABTEXT_O_TRUNC  _O_TRUNC
#  define COLLABTEXT_O_CREAT  _O_CREAT
#  define COLLABTEXT_O_WRONLY  _O_WRONLY
#else
#  include <fcntl.h>
#  include <unistd.h>
#  define COLLABTEXT_OPEN  ::open
#  define COLLABTEXT_CLOSE ::close
#  define COLLABTEXT_FSYNC ::fsync
#  define COLLABTEXT_WRITE ::write
#  define COLLABTEXT_LSEEK ::lseek
#  define COLLABTEXT_O_APPEND O_APPEND
#  define COLLABTEXT_O_TRUNC  O_TRUNC
#  define COLLABTEXT_O_CREAT  O_CREAT
#  define COLLABTEXT_O_WRONLY  O_WRONLY
#endif

namespace CollabText::Storage {

namespace fs = std::filesystem;

namespace {

/// Upper bound on retries for a short write.
///
/// A short write is usually a signal interrupting the transfer, and retrying is
/// correct. The bound exists so a permanently full disk or a broken descriptor
/// produces an error instead of a spin.
constexpr int kMaxWriteRetries = 64;

/// Returns the Error rather than a Result<void> so it converts to any
/// Result<T>, which keeps the failure paths from needing a local.
[[nodiscard]] Error io_error(std::string message, const fs::path& path) {
    return Error(ErrorCode::io_error, std::move(message), path.string());
}

/// Run the injected failure hook, if there is one.
[[nodiscard]] std::optional<Error> injected_failure(const StepHook* hook, Step step,
                                                    const fs::path& path) {
    if (!hook || !hook->fail) return std::nullopt;
    return hook->fail(step, path);
}

void observed(const StepHook* hook, Step step, const fs::path& path) {
    if (hook && hook->observe) hook->observe(step, path);
}

/// Write the whole buffer, tolerating short writes.
///
/// Returns the number of bytes the OS accepted in `accepted`. A short write that
/// runs out of retries is reported as `io_error` naming both counts, because
/// "wrote some of it" is not a state a caller can recover from by assuming
/// success.
Result<size_t> write_bytes(int fd, std::string_view bytes, size_t limit) {
    size_t total = 0;
    int attempts = 0;

    while (total < bytes.size()) {
        if (++attempts > kMaxWriteRetries) {
            return Error(ErrorCode::io_error,
                         "short write persisted after " +
                             std::to_string(attempts) + " attempts: " +
                             std::to_string(total) + " of " +
                             std::to_string(bytes.size()) + " bytes accepted");
        }

        size_t want = bytes.size() - total;
        if (limit != 0 && want > limit) want = limit;

#if defined(_WIN32)
        const int n = ::_write(fd, bytes.data() + total, static_cast<unsigned>(want));
#else
        // Retry short writes caused by EINTR rather than reporting them.
        ssize_t n;
        do {
            n = ::write(fd, bytes.data() + total, want);
        } while (n < 0 && errno == EINTR);
#endif
        if (n < 0) {
            return Error(ErrorCode::io_error,
                         std::string("write failed: ") + std::strerror(errno));
        }
        if (n == 0) {
            // No progress and no error: the descriptor is not writable. Treat
            // it as a failure rather than looping forever.
            return Error(ErrorCode::io_error,
                         "write made no progress at offset " + std::to_string(total));
        }
        total += static_cast<size_t>(n);
    }
    return total;
}

}  // namespace

const FileOps& FileOps::real() {
    static const FileOps ops = [] {
        FileOps o;
        o.write = [](int fd, std::string_view bytes, size_t limit) {
            return write_bytes(fd, bytes, limit);
        };
        o.sync_file = [](int fd, const fs::path&) {
#if defined(_WIN32)
            if (::_commit(fd) != 0)
                return Result<void>(io_error(std::string("commit failed: ") +
                                              std::strerror(errno), {}));
            return Result<void>::success();
#else
            if (::fsync(fd) != 0)
                return Result<void>(io_error(std::string("fsync failed: ") +
                                              std::strerror(errno), {}));
            return Result<void>::success();
#endif
        };
        o.rename = [](const fs::path& from, const fs::path& to) {
#if defined(_WIN32)
            if (!::MoveFileExA(from.c_str(), to.c_str(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                return Result<void>(io_error("replace failed", to));
            }
            return Result<void>::success();
#else
            std::error_code ec;
            fs::rename(from, to, ec);
            if (ec)
                return Result<void>(io_error("atomic replace failed: " + ec.message(), to));
            return Result<void>::success();
#endif
        };
        o.remove = [](const fs::path& p) {
            std::error_code ec;
            fs::remove(p, ec);
            if (ec) return Result<void>(io_error("remove failed: " + ec.message(), p));
            return Result<void>::success();
        };
        o.sync_dir = [](const fs::path& dir) {
            return sync_directory(dir, nullptr);
        };
        return o;
    }();
    return ops;
}

FileOps FileOps::resolved() const {
    FileOps out = *this;
    const FileOps& r = real();
    if (!out.write) out.write = r.write;
    if (!out.sync_file) out.sync_file = r.sync_file;
    if (!out.rename) out.rename = r.rename;
    if (!out.remove) out.remove = r.remove;
    if (!out.sync_dir) out.sync_dir = r.sync_dir;
    return out;
}

std::string_view to_string(Step step) {
    switch (step) {
    case Step::Write: return "write";
    case Step::FileSync: return "file_sync";
    case Step::Replace: return "replace";
    case Step::DirSync: return "dir_sync";
    case Step::Close: return "close";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Steps
// ---------------------------------------------------------------------------

Result<void> write_all(int fd, std::string_view bytes, const StepHook* hook,
                       const fs::path& path, const FileOps* ops) {
    if (fd < 0) return io_error("write to an invalid descriptor", path);
    if (bytes.empty()) return Result<void>::success();
    if (auto failure = injected_failure(hook, Step::Write, path)) return *failure;

    auto written = (ops ? *ops : FileOps::real()).write(fd, bytes, /*limit=*/0);
    if (!written.ok()) return written.error().with_context(path.string());
    observed(hook, Step::Write, path);
    return Result<void>::success();
}

Result<void> sync_file(int fd, const fs::path& path, const StepHook* hook,
                       const FileOps* ops, FileOpCounts* counts) {
    if (fd < 0) return io_error("sync of an invalid descriptor", path);
    if (auto failure = injected_failure(hook, Step::FileSync, path)) return *failure;

    // Routed through FileOps so a test can observe the real call. Counting here
    // rather than at the caller is deliberate: this is the only place a sync
    // genuinely happened, so a counter incremented by a caller could lie.
    auto synced = (ops ? *ops : FileOps::real()).sync_file(fd, path);
    if (!synced.ok()) return synced.error().with_context(path.string());
    if (counts) ++counts->file_syncs;
    observed(hook, Step::FileSync, path);
    return Result<void>::success();
}

Result<void> sync_directory(const fs::path& dir, const StepHook* hook,
                            const FileOps* ops, FileOpCounts* counts) {
    if (auto failure = injected_failure(hook, Step::DirSync, dir)) return *failure;

    if (ops && ops->sync_dir) {
        auto synced = ops->sync_dir(dir);
        if (!synced.ok()) return synced;
        if (counts) ++counts->dir_syncs;
        observed(hook, Step::DirSync, dir);
        return Result<void>::success();
    }

#if defined(_WIN32)
    // Opening a directory for sync is not supported on Windows. Reported rather
    // than skipped: a caller that asked for durability it cannot have needs to
    // know, because on this platform a rename is durable only once the *file*
    // handle has been committed.
    return Error(ErrorCode::unsupported_operation,
                 "directory sync is not available on this platform; a rename here "
                 "is durable only after the file handle is committed",
                 dir.string());
#else
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        // A filesystem that cannot open a directory for reading cannot sync it
        // either. That is an unsupported guarantee, not an I/O failure.
        return Error(ErrorCode::unsupported_operation,
                     std::string("cannot open directory for sync: ") +
                         std::strerror(errno),
                     dir.string());
    }
    const int rc = ::fsync(fd);
    const int saved = errno;
    ::close(fd);
    if (rc != 0) {
        // Some filesystems (and some container overlays) reject fsync on a
        // directory with EINVAL. That is precisely the "cannot promise this"
        // case, so it is named rather than reported as a generic failure.
        if (saved == EINVAL || saved == ENOTSUP) {
            return Error(ErrorCode::unsupported_operation,
                         std::string("directory fsync unsupported here: ") +
                             std::strerror(saved),
                         dir.string());
        }
        return io_error(std::string("directory fsync failed: ") + std::strerror(saved),
                        dir);
    }
    observed(hook, Step::DirSync, dir);
    return Result<void>::success();
#endif
}

Result<void> replace_atomically(const fs::path& target, std::string_view contents,
                                const StepHook* hook, const FileOps* ops,
                                FileOpCounts* counts) {
    const FileOps real_ops = (ops ? ops->resolved() : FileOps::real());
    // The temporary is a sibling so the rename stays within one filesystem, which
    // is what makes it atomic. A temporary elsewhere would silently degrade to a
    // copy.
    const fs::path tmp = fs::path(target.string() + ".tmp");

    // Start from a clean slate: a temporary left by a previous crash must not be
    // mistaken for this attempt's work. Note this removes the *temporary*, never
    // the destination -- see the rename below.
    real_ops.remove(tmp);

    // Fail early and cheaply if the destination's directory does not exist,
    // rather than after creating a file we cannot place.
    const fs::path dir = target.parent_path().empty() ? fs::path(".")
                                                      : target.parent_path();

    const int fd = COLLABTEXT_OPEN(tmp.c_str(),
                                   COLLABTEXT_O_WRONLY | COLLABTEXT_O_CREAT |
                                       COLLABTEXT_O_TRUNC,
                                   0644);
    if (fd < 0) {
        return io_error(std::string("cannot create temporary: ") + std::strerror(errno),
                        tmp);
    }

    // Every failure past this point must remove the temporary, and a failure to
    // remove it is reported rather than ignored -- a leftover temporary is
    // indistinguishable from state to a later reader.
    const auto fail = [&](Error error) -> Error {
        COLLABTEXT_CLOSE(fd);
        if (auto cleaned = real_ops.remove(tmp); !cleaned.ok()) {
            return error.with_context(
                "also could not remove the temporary: " +
                std::string(cleaned.error().message()));
        }
        return error;
    };

    if (auto failure = injected_failure(hook, Step::Write, tmp)) {
        return fail(std::move(*failure));
    }
    {
        auto written = real_ops.write(fd, contents, /*limit=*/0);
        if (!written.ok()) return fail(written.error().with_context(tmp.string()));
        if (counts) ++counts->writes;
    }
    observed(hook, Step::Write, tmp);

    if (auto failure = injected_failure(hook, Step::FileSync, tmp)) {
        return fail(std::move(*failure));
    }
    {
        // Sync the temporary's *data* before the rename. Renaming first would
        // publish a name pointing at bytes that are still only in the page
        // cache. This is the step F9 skipped.
        if (auto synced = sync_file(fd, tmp, nullptr, &real_ops, counts);
            !synced.ok()) {
            return fail(std::move(synced.error()));
        }
    }
    observed(hook, Step::FileSync, tmp);

    if (COLLABTEXT_CLOSE(fd) != 0) {
        const int saved = errno;
        std::error_code ec;
        fs::remove(tmp, ec);
        return io_error(std::string("close of temporary failed: ") +
                            std::strerror(saved),
                        tmp);
    }

    if (auto failure = injected_failure(hook, Step::Replace, tmp)) {
        real_ops.remove(tmp);
        return std::move(*failure);
    }
    {
        // The destination is never unlinked first. rename replaces the name
        // atomically, so a reader sees the whole old file or the whole new one.
        // Unlinking first would open a window in which the file does not exist,
        // which is indistinguishable from data loss to a reader.
        if (auto renamed = real_ops.rename(tmp, target); !renamed.ok()) {
            real_ops.remove(tmp);
            return renamed.error();
        }
        if (counts) ++counts->renames;
    }
    observed(hook, Step::Replace, target);

    // The rename is durable only once the *name* is out of the kernel. Skipping
    // this is how a committed write disappears across a crash that loses the
    // directory entry but not the data.
    if (auto synced = sync_directory(dir, hook, &real_ops, counts); !synced.ok()) {
        return synced.error();
    }
    return Result<void>::success();
}

// ---------------------------------------------------------------------------
// DurableAppendFile
// ---------------------------------------------------------------------------

Result<DurableAppendFile> DurableAppendFile::open(const fs::path& path,
                                                  const StepHook* hook,
                                                  const FileOps* ops) {
    std::error_code ec;
    if (!fs::exists(path.parent_path(), ec)) {
        return Error(ErrorCode::io_error,
                     "parent directory does not exist; the store decides its own "
                     "layout and this primitive will not create one",
                     path.parent_path().string());
    }

    const int fd = COLLABTEXT_OPEN(path.c_str(),
                                   COLLABTEXT_O_WRONLY | COLLABTEXT_O_CREAT |
                                       COLLABTEXT_O_APPEND,
                                   0644);
    if (fd < 0) {
        return io_error(std::string("cannot open for append: ") + std::strerror(errno),
                        path);
    }

    DurableAppendFile file;
    file.m_path = path;
    file.m_fd = fd;
    file.m_hook = hook;
    file.m_ops = ops;
    return file;
}

DurableAppendFile::DurableAppendFile(DurableAppendFile&& other) noexcept
    : m_path(std::move(other.m_path)),
      m_fd(std::exchange(other.m_fd, -1)),
      m_bytes_written(other.m_bytes_written),
      m_sync_count(other.m_sync_count),
      m_write_limit(other.m_write_limit),
      m_hook(other.m_hook),
      m_ops(other.m_ops),
      m_counts(other.m_counts) {}

DurableAppendFile& DurableAppendFile::operator=(DurableAppendFile&& other) noexcept {
    if (this != &other) {
        if (m_fd >= 0) COLLABTEXT_CLOSE(m_fd);
        m_path = std::move(other.m_path);
        m_fd = std::exchange(other.m_fd, -1);
        m_bytes_written = other.m_bytes_written;
        m_sync_count = other.m_sync_count;
        m_write_limit = other.m_write_limit;
        m_hook = other.m_hook;
        m_ops = other.m_ops;
        m_counts = other.m_counts;
    }
    return *this;
}

DurableAppendFile::~DurableAppendFile() {
    if (m_fd >= 0) COLLABTEXT_CLOSE(m_fd);
}

Result<void> DurableAppendFile::write_and_sync(std::string_view bytes, bool sync) {
    if (m_fd < 0) return io_error("append to a closed file", m_path);
    if (auto failure = injected_failure(m_hook, Step::Write, m_path)) {
        return *failure;
    }

    auto written = (m_ops ? m_ops->resolved() : FileOps::real())
                       .write(m_fd, bytes, m_write_limit);
    if (!written.ok()) return written.error().with_context(m_path.string());
    m_bytes_written += written.value();
    ++m_counts.writes;
    observed(m_hook, Step::Write, m_path);

    if (!sync) return Result<void>::success();

    if (auto failure = injected_failure(m_hook, Step::FileSync, m_path)) {
        return *failure;
    }
    if (auto synced = sync_file(m_fd, m_path, nullptr, m_ops, &m_counts);
        !synced.ok()) {
        return synced.error();
    }
    // Incremented only because the real call above returned success.
    ++m_sync_count;
    observed(m_hook, Step::FileSync, m_path);
    return Result<void>::success();
}

Result<void> DurableAppendFile::append(std::string_view bytes) {
    return write_and_sync(bytes, /*sync=*/true);
}

Result<size_t> DurableAppendFile::append_unsynced(std::string_view bytes) {
    if (m_fd < 0) return io_error("append to a closed file", m_path);
    if (auto failure = injected_failure(m_hook, Step::Write, m_path)) {
        return *failure;
    }
    auto written = (m_ops ? m_ops->resolved() : FileOps::real())
                       .write(m_fd, bytes, m_write_limit);
    if (!written.ok()) return written.error().with_context(m_path.string());
    m_bytes_written += written.value();
    ++m_counts.writes;
    observed(m_hook, Step::Write, m_path);
    return written.value();
}

Result<void> DurableAppendFile::sync() {
    if (m_fd < 0) return io_error("sync of a closed file", m_path);
    if (auto failure = injected_failure(m_hook, Step::FileSync, m_path)) {
        return *failure;
    }
    if (auto synced = sync_file(m_fd, m_path, nullptr, m_ops, &m_counts);
        !synced.ok()) {
        return synced.error();
    }
    ++m_sync_count;
    observed(m_hook, Step::FileSync, m_path);
    return Result<void>::success();
}

Result<void> DurableAppendFile::close() {
    if (m_fd < 0) return Result<void>::success();  // idempotent
    if (auto failure = injected_failure(m_hook, Step::Close, m_path)) {
        return *failure;
    }
    // Sync before close: closing drops the descriptor, and anything still in its
    // buffers would be lost with no way left to force it out.
    if (auto synced = sync_file(m_fd, m_path, nullptr, m_ops, &m_counts);
        !synced.ok()) {
        return synced.error();
    }
    ++m_sync_count;
    observed(m_hook, Step::Close, m_path);

    if (COLLABTEXT_CLOSE(m_fd) != 0) {
        const int saved = errno;
        m_fd = -1;
        return io_error(std::string("close failed: ") + std::strerror(saved), m_path);
    }
    m_fd = -1;
    return Result<void>::success();
}

// ---------------------------------------------------------------------------
// Process lease
// ---------------------------------------------------------------------------

std::string ProcessLease::describe() const {
    return "pid " + std::to_string(holder_pid) + " at " + path.string();
}

bool ProcessLease::process_alive(uint64_t pid) {
    // pid 0 means "the caller's process group" to kill(2), so it is not a
    // process identity here; treating it as alive would make a malformed lease
    // file wedge the store forever.
    if (pid == 0 || pid > static_cast<uint64_t>(INT32_MAX)) return false;
#if defined(_WIN32)
    HANDLE handle = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!handle) return false;
    const bool waiting = ::WaitForSingleObject(handle, 0) == WAIT_TIMEOUT;
    ::CloseHandle(handle);
    return waiting;
#else
    // Signal 0 performs error checking without delivering a signal. EPERM means
    // the process exists but belongs to another user, which still counts as
    // alive: someone else's writer is a conflict, not a free slot.
    if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;
    return errno == EPERM;
#endif
}

bool ProcessLease::held_by_live_process(const fs::path& lease_path) {
    std::error_code ec;
    if (!fs::exists(lease_path, ec)) return false;

    std::ifstream in(lease_path, std::ios::binary);
    if (!in) {
        // The file exists but cannot be read. Treat it as held: an unreadable
        // lease is more likely a permissions or sharing problem than a free
        // slot, and quietly stealing it would be the worse error.
        return true;
    }
    std::string text;
    text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    uint64_t holder = 0;
    try {
        holder = text.empty() ? 0 : std::stoull(text);
    } catch (...) {
        // Unparseable contents are not a usable identity, so they cannot be
        // checked for liveness. Reported as not-held so a corrupt lease cannot
        // wedge the store permanently; the acquire path re-reads it and reports
        // a conflict if the file turns out to be unreadable.
        return false;
    }
    return process_alive(holder);
}

Result<ProcessLease> ProcessLease::acquire(const fs::path& lease_path) {
    // Exclusive creation is the primitive: it is atomic with respect to other
    // creators, which an advisory lock is not.
    const int fd = COLLABTEXT_OPEN(lease_path.c_str(),
                                   COLLABTEXT_O_WRONLY | COLLABTEXT_O_CREAT |
#if defined(_WIN32)
                                       _O_EXCL | COLLABTEXT_O_TRUNC,
#else
                                       O_EXCL | COLLABTEXT_O_TRUNC,
#endif
                                   0644);
    if (fd >= 0) {
        const uint64_t self =
#if defined(_WIN32)
            static_cast<uint64_t>(::GetCurrentProcessId());
#else
            static_cast<uint64_t>(::getpid());
#endif
        write_bytes(fd, std::to_string(self) + "\n", /*limit=*/0);
        // The lease is itself a durability claim: if the pid is not on disk
        // before we act on it, a crash leaves a lease nobody can attribute.
        COLLABTEXT_FSYNC(fd);
        COLLABTEXT_CLOSE(fd);
        return ProcessLease{lease_path, self};
    }

    // Creation failed. If a live process already holds it, that is a conflict.
    std::ifstream in(lease_path, std::ios::binary);
    std::string text;
    if (in) text.assign((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    uint64_t holder = 0;
    try {
        holder = text.empty() ? 0 : std::stoull(text);
    } catch (...) {
        holder = 0;
    }

    if (ProcessLease::process_alive(holder)) {
        return Error(ErrorCode::writer_conflict,
                     "lease held by a live process (pid " + std::to_string(holder) + ")",
                     lease_path.string());
    }

    // The holder is gone (or the file is unreadable). Breaking the lease is
    // still an overwrite of someone else's state, so it is reported distinctly
    // and the caller decides -- only the caller knows whether losing this race
    // would lose work.
    std::error_code ec;
    fs::remove(lease_path, ec);
    if (ec) {
        return io_error("cannot clear a stale lease: " + ec.message(), lease_path);
    }
    return acquire(lease_path);
}

}  // namespace CollabText::Storage
