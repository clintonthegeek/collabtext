/// DurableFile.cpp — see DurableFile.h for the contract and for why it exists.

#include "storage/DurableFile.h"

#include <cerrno>
#include <algorithm>
#include <climits>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  include <io.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  define COLLABTEXT_OPEN  ::_wopen
#  define COLLABTEXT_CLOSE ::_close
#  define COLLABTEXT_FSYNC ::_commit
#  define COLLABTEXT_WRITE ::_write
#  define COLLABTEXT_LSEEK ::_lseeki64
#  define COLLABTEXT_O_APPEND _O_APPEND
#  define COLLABTEXT_O_TRUNC  _O_TRUNC
#  define COLLABTEXT_O_CREAT  _O_CREAT
#  define COLLABTEXT_O_WRONLY  _O_WRONLY
#  define COLLABTEXT_O_RDWR _O_RDWR
#  define COLLABTEXT_O_EXCL _O_EXCL
#  define COLLABTEXT_O_EXTRA (_O_BINARY | _O_NOINHERIT)
#else
#  include <fcntl.h>
#  include <unistd.h>
#  include <sys/file.h>
#  define COLLABTEXT_OPEN  ::open
#  define COLLABTEXT_CLOSE ::close
#  define COLLABTEXT_FSYNC ::fsync
#  define COLLABTEXT_WRITE ::write
#  define COLLABTEXT_LSEEK ::lseek
#  define COLLABTEXT_O_APPEND O_APPEND
#  define COLLABTEXT_O_TRUNC  O_TRUNC
#  define COLLABTEXT_O_CREAT  O_CREAT
#  define COLLABTEXT_O_WRONLY  O_WRONLY
#  define COLLABTEXT_O_RDWR O_RDWR
#  define COLLABTEXT_O_EXCL O_EXCL
#  define COLLABTEXT_O_EXTRA O_CLOEXEC
#endif

namespace CollabText::Storage {

namespace fs = std::filesystem;

namespace {

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
/// Continue every progressing short write. An I/O failure or zero progress is
/// an error, never confirmation that the whole buffer was written.
Result<size_t> write_bytes(int fd, std::string_view bytes, size_t limit) {
    size_t total = 0;

    while (total < bytes.size()) {
        size_t want = bytes.size() - total;
        if (limit != 0 && want > limit) want = limit;
        want = std::min(want, static_cast<size_t>(std::numeric_limits<int>::max()));

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
            if (!::MoveFileExW(from.c_str(), to.c_str(),
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
        o.close = [](int fd, const fs::path& path) -> Result<void> {
            // Do not retry close: the descriptor may already have been released.
            if (COLLABTEXT_CLOSE(fd) != 0)
                return io_error(std::string("close failed: ") + std::strerror(errno), path);
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
    if (!out.close) out.close = r.close;
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

    auto written = (ops ? ops->resolved() : FileOps::real()).write(fd, bytes, /*limit=*/0);
    if (!written.ok()) return written.error().with_context(path.string());
    if (written.value() != bytes.size())
        return io_error("incomplete write: " + std::to_string(written.value()) +
                        " of " + std::to_string(bytes.size()) + " bytes", path);
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
    auto synced = (ops ? ops->resolved() : FileOps::real()).sync_file(fd, path);
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
    // This implementation has no verified Windows directory barrier. Report
    // that gap explicitly; a file commit is not evidence for the name barrier.
    return Error(ErrorCode::unsupported_operation,
                 "a verified directory durability barrier is not implemented "
                 "on this platform",
                 dir.string());
#else
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        // A missing or inaccessible directory is an I/O failure, not evidence
        // that the platform lacks the barrier.
        return Error(ErrorCode::io_error,
                     std::string("cannot open directory for sync: ") +
                         std::strerror(errno),
                     dir.string());
    }
    const int rc = ::fsync(fd);
    const int saved = errno;
    const int close_rc = ::close(fd);
    const int close_errno = errno;
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
    if (close_rc != 0)
        return io_error(std::string("directory close failed: ") +
                        std::strerror(close_errno), dir);
    if (counts) ++counts->dir_syncs;
    observed(hook, Step::DirSync, dir);
    return Result<void>::success();
#endif
}

Result<void> replace_atomically(const fs::path& target, std::string_view contents,
                                const StepHook* hook, const FileOps* ops,
                                FileOpCounts* counts) {
    const FileOps real_ops = ops ? ops->resolved() : FileOps::real();
    auto nonce = ActorId::generate();
    if (!nonce.ok()) return nonce.error();
    fs::path tmp = target;
    tmp += ".tmp." + nonce.value().to_string();
    const fs::path dir = target.parent_path().empty() ? fs::path(".") : target.parent_path();
    int fd = COLLABTEXT_OPEN(tmp.c_str(), COLLABTEXT_O_WRONLY | COLLABTEXT_O_CREAT |
                             COLLABTEXT_O_EXCL | COLLABTEXT_O_EXTRA, 0600);
    if (fd < 0) return io_error(std::string("cannot create temporary: ") +
                               std::strerror(errno), tmp);
    const auto fail = [&](Error error) -> Error {
        if (fd >= 0) {
            const int closing = std::exchange(fd, -1);
            if (auto closed = real_ops.close(closing, tmp); !closed.ok())
                error = error.with_context("also could not close temporary: " +
                                          std::string(closed.error().message()));
        }
        if (auto cleaned = real_ops.remove(tmp); !cleaned.ok())
            error = error.with_context("also could not remove temporary: " +
                                      std::string(cleaned.error().message()));
        else if (counts) ++counts->removes;
        return error;
    };
    if (auto failure = injected_failure(hook, Step::Write, tmp)) return fail(*failure);
    auto written = real_ops.write(fd, contents, 0);
    if (!written.ok()) return fail(written.error().with_context(tmp.string()));
    if (written.value() != contents.size())
        return fail(io_error("incomplete temporary write", tmp));
    if (counts) ++counts->writes;
    observed(hook, Step::Write, tmp);
    if (auto synced = sync_file(fd, tmp, hook, &real_ops, counts); !synced.ok())
        return fail(synced.error());
    if (auto failure = injected_failure(hook, Step::Close, tmp)) return fail(*failure);
    const int closing = std::exchange(fd, -1);
    if (auto closed = real_ops.close(closing, tmp); !closed.ok()) return fail(closed.error());
    observed(hook, Step::Close, tmp);
    if (auto failure = injected_failure(hook, Step::Replace, tmp)) return fail(*failure);
    if (auto renamed = real_ops.rename(tmp, target); !renamed.ok()) return fail(renamed.error());
    if (counts) ++counts->renames;
    observed(hook, Step::Replace, target);
    if (auto synced = sync_directory(dir, hook, &real_ops, counts); !synced.ok())
        return Error(ErrorCode::commit_uncertain,
                     "replacement published; directory durability was not confirmed: " +
                         std::string(synced.error().message()),
                     target.string() + " (published) <- " + dir.string());
    return Result<void>::success();
}

// ---------------------------------------------------------------------------
// DurableAppendFile
// ---------------------------------------------------------------------------

Result<DurableAppendFile> DurableAppendFile::open(const fs::path& path,
                                                  const StepHook* hook,
                                                  const FileOps* ops) {
    std::error_code ec;
    const fs::path parent = path.parent_path().empty() ? fs::path(".") : path.parent_path();
    if (!fs::exists(parent, ec)) {
        return Error(ErrorCode::io_error,
                     "parent directory does not exist; the store decides its own "
                     "layout and this primitive will not create one",
                     path.parent_path().string());
    }

    const int fd = COLLABTEXT_OPEN(path.c_str(),
                                   COLLABTEXT_O_WRONLY | COLLABTEXT_O_CREAT |
                                       COLLABTEXT_O_APPEND | COLLABTEXT_O_EXTRA,
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
      m_counts(other.m_counts),
      m_needs_directory_sync(other.m_needs_directory_sync) {}

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
        m_needs_directory_sync = other.m_needs_directory_sync;
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
    if (written.value() != bytes.size())
        return io_error("incomplete append: " + std::to_string(written.value()) +
                        " of " + std::to_string(bytes.size()) + " bytes", m_path);
    ++m_counts.writes;
    observed(m_hook, Step::Write, m_path);

    if (!sync) return Result<void>::success();

    return this->sync();
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
    if (written.value() != bytes.size())
        return io_error("incomplete append: " + std::to_string(written.value()) +
                        " of " + std::to_string(bytes.size()) + " bytes", m_path);
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
    if (m_needs_directory_sync) {
        const fs::path dir = m_path.parent_path().empty() ? fs::path(".") : m_path.parent_path();
        if (auto synced = sync_directory(dir, m_hook, m_ops, &m_counts); !synced.ok())
            return Error(ErrorCode::commit_uncertain,
                         "append published; directory durability was not confirmed: " +
                             std::string(synced.error().message()), m_path.string());
        m_needs_directory_sync = false;
    }
    return Result<void>::success();
}

Result<void> DurableAppendFile::close() {
    if (m_fd < 0) return Result<void>::success();
    if (auto failure = injected_failure(m_hook, Step::Close, m_path)) return *failure;
    if (auto synced = sync(); !synced.ok()) return synced;
    const int fd = std::exchange(m_fd, -1);
    auto closed = (m_ops ? m_ops->resolved() : FileOps::real()).close(fd, m_path);
    if (!closed.ok()) return closed;
    observed(m_hook, Step::Close, m_path);
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

namespace {
Result<void> lock_descriptor(int fd, const fs::path& path) {
#if defined(_WIN32)
    OVERLAPPED range{};
    if (!::LockFileEx(reinterpret_cast<HANDLE>(::_get_osfhandle(fd)),
                       LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                       0, MAXDWORD, MAXDWORD, &range)) {
        const auto error = ::GetLastError();
        if (error == ERROR_LOCK_VIOLATION)
            return Error(ErrorCode::writer_conflict, "writer lease holder is active", path.string());
        return io_error("cannot acquire writer lock: " + std::to_string(error), path);
    }
#else
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN)
            return Error(ErrorCode::writer_conflict, "writer lease holder is active", path.string());
        return io_error(std::string("cannot acquire writer lock: ") + std::strerror(errno), path);
    }
#endif
    return Result<void>::success();
}
} // namespace

ProcessLease::ProcessLease(ProcessLease&& other) noexcept
    : path(std::move(other.path)), holder_pid(other.holder_pid),
      m_fd(std::exchange(other.m_fd, -1)) {}

ProcessLease& ProcessLease::operator=(ProcessLease&& other) noexcept {
    if (this != &other) {
        if (m_fd >= 0) COLLABTEXT_CLOSE(m_fd);
        path = std::move(other.path);
        holder_pid = other.holder_pid;
        m_fd = std::exchange(other.m_fd, -1);
    }
    return *this;
}

ProcessLease::~ProcessLease() { if (m_fd >= 0) COLLABTEXT_CLOSE(m_fd); }

Result<void> ProcessLease::release() {
    if (m_fd < 0) return Result<void>::success();
    const int fd = std::exchange(m_fd, -1);
    return FileOps::real().close(fd, path);
}

bool ProcessLease::held_by_live_process(const fs::path& lease_path) {
    const int fd = COLLABTEXT_OPEN(lease_path.c_str(), COLLABTEXT_O_RDWR | COLLABTEXT_O_EXTRA);
    if (fd < 0) return errno != ENOENT; // Unreadable is not a free slot.
    const auto result = lock_descriptor(fd, lease_path);
    COLLABTEXT_CLOSE(fd);
    return !result.ok();
}

Result<ProcessLease> ProcessLease::acquire(const fs::path& lease_path, const FileOps* ops) {
    const int fd = COLLABTEXT_OPEN(lease_path.c_str(), COLLABTEXT_O_RDWR |
                                   COLLABTEXT_O_CREAT | COLLABTEXT_O_EXTRA, 0600);
    if (fd < 0) return io_error(std::string("cannot open writer lease: ") +
                               std::strerror(errno), lease_path);
    ProcessLease lease;
    lease.path = lease_path;
    lease.m_fd = fd;
    if (auto locked = lock_descriptor(fd, lease_path); !locked.ok()) return locked.error();
    lease.holder_pid =
#if defined(_WIN32)
        static_cast<uint64_t>(::GetCurrentProcessId());
    if (::_chsize_s(fd, 0) != 0) return io_error("cannot truncate lease diagnostic", lease_path);
#else
        static_cast<uint64_t>(::getpid());
    if (::ftruncate(fd, 0) != 0) return io_error("cannot truncate lease diagnostic", lease_path);
#endif
    // Checked diagnostic write. It never establishes ownership; the OS lock does.
    const auto written = write_all(fd, std::to_string(lease.holder_pid) + "\n", nullptr,
                                   lease_path, ops);
    if (!written.ok()) return written.error();
    return lease;
}

}  // namespace CollabText::Storage
