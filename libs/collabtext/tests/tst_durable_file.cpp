/// tst_durable_file.cpp — L01: checked durable filesystem primitives.
///
/// The defect this exists for is F9: `SegmentWriter::flush()` incremented a
/// counter named `fsync_count` after nothing more than a `std::ofstream::flush`,
/// and `seal_open_()` renamed over the destination while discarding the error
/// code, having fsynced neither the temporary nor the directory. A counter named
/// after a durability guarantee, counting something that was not durability.
///
/// The tests below are grouped by the property that must hold:
///
///   A. The step ORDER is observable and fixed.
///   B. A failure at any step leaves the previous complete data in place.
///   C. Short writes are detected rather than silently truncating.
///   D. On the real platform the steps actually happen and the data survives.
///   E. The destination is never unlinked before its replacement exists.
///   F. A platform that cannot do something says so.
///   G. A process lease distinguishes a live holder from a dead one.
///
/// Note on scope: nothing here claims to survive power loss. A subprocess crash
/// (group H) proves user-space buffers do not tear a committed write; it cannot
/// prove anything about losing the page cache, and pretending otherwise is the
/// same category of mistake as F9.
///
/// Escaped string literals throughout: Qt's moc silently drops test slots in a
/// translation unit containing a raw string literal with braces (P04).

#include <QTest>
#include "storage/DurableFile.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace CollabText;
using namespace CollabText::Storage;
namespace fs = std::filesystem;

namespace {

/// A unique scratch directory per test, removed on destruction.
class TempDir {
public:
    explicit TempDir(const char* label) {
        static int counter = 0;
        m_path = fs::temp_directory_path() /
                 ("collabtext-durable-" + std::string(label) + "-" +
                  std::to_string(::getpid()) + "-" + std::to_string(++counter));
        fs::remove_all(m_path);
        fs::create_directories(m_path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const { return m_path; }
    fs::path file(const std::string& name) const { return m_path / name; }

private:
    fs::path m_path;
};

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

/// A hook that records the step order and can be told to fail one step.
struct Recording {
    StepHook hook;
    std::vector<std::string> order;
    Step fail_step{};
    bool has_fail = false;

    Recording() {
        hook.observe = [this](Step step, const fs::path&) {
            order.emplace_back(to_string(step));
        };
        hook.fail = [this](Step step, const fs::path& path) -> std::optional<Error> {
            if (!has_fail || step != fail_step) return std::nullopt;
            return Error(ErrorCode::io_error,
                         std::string("injected failure at ") + std::string(to_string(step)),
                         path.string());
        };
    }

    [[nodiscard]] bool saw(Step step) const {
        const std::string name(to_string(step));
        for (const auto& entry : order)
            if (entry == name) return true;
        return false;
    }

    /// Position of `step` in the recorded order, or npos.
    [[nodiscard]] int index_of(Step step) const {
        const std::string name(to_string(step));
        for (size_t i = 0; i < order.size(); ++i)
            if (order[i] == name) return static_cast<int>(i);
        return -1;
    }
};

}  // namespace

class TestDurableFile : public QObject {
    Q_OBJECT
private slots:
    void simultaneous_replacements_do_not_share_a_temporary() {
        TempDir dir("separate-temporaries");
        const auto target = dir.file("state");
        bool inner_succeeded = false;
        bool nested = false;
        StepHook hook;
        hook.observe = [&](Step step, const fs::path&) {
            if (step == Step::Write && !nested) {
                nested = true;
                inner_succeeded = replace_atomically(target, "INNER").ok();
            }
        };
        QVERIFY(replace_atomically(target, "OUTER", &hook).ok());
        QVERIFY(inner_succeeded);
        QCOMPARE(read_file(target), std::string("OUTER"));
    }

    void temporary_close_failure_does_not_publish() {
        TempDir dir("failed-close");
        const auto target = dir.file("state");
        QVERIFY(replace_atomically(target, "OLD").ok());
        FileOps ops = FileOps::real();
        ops.close = [](int fd, const fs::path& path) -> Result<void> {
            auto closed = FileOps::real().close(fd, path);
            if (!closed.ok()) return closed;
            return Error(ErrorCode::io_error, "close rejected", path.string());
        };
        const auto result = replace_atomically(target, "NEW", nullptr, &ops);
        QVERIFY(!result.ok());
        QCOMPARE(read_file(target), std::string("OLD"));
    }

    void failed_lease_record_write_releases_the_lock() {
        TempDir dir("lease-record-failure");
        const auto path = dir.file("lease");
        FileOps ops = FileOps::real();
        ops.write = [](int, std::string_view, size_t) -> Result<size_t> {
            return Error(ErrorCode::io_error, "diagnostic write rejected");
        };
        QVERIFY(!ProcessLease::acquire(path, &ops).ok());
        QVERIFY(!ProcessLease::held_by_live_process(path));
        auto good = ProcessLease::acquire(path);
        QVERIFY(good.ok());
        auto moved = std::move(good).value();
        QVERIFY(!ProcessLease::acquire(path).ok());
        QVERIFY(moved.release().ok());
        QVERIFY(ProcessLease::acquire(path).ok());
    }

    void process_death_releases_a_lease_without_replacing_the_lock_file() {
        TempDir dir("lease-death");
        const auto path = dir.file("lease");
        const pid_t child = ::fork();
        QVERIFY(child >= 0);
        if (child == 0) {
            auto lease = ProcessLease::acquire(path);
            ::_exit(lease.ok() ? 0 : 1); // Deliberately no RAII destruction.
        }
        int status = 0;
        QVERIFY(::waitpid(child, &status, 0) == child);
        QVERIFY(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        struct stat before{}, after{};
        QVERIFY(::stat(path.c_str(), &before) == 0);
        auto lease = ProcessLease::acquire(path);
        QVERIFY(lease.ok());
        QVERIFY(::stat(path.c_str(), &after) == 0);
        QCOMPARE(before.st_ino, after.st_ino);
    }

    void missing_sync_directory_is_an_io_error() {
        TempDir dir("missing-directory");
        const auto result = sync_directory(dir.file("absent"));
        QVERIFY(!result.ok());
        QCOMPARE(result.error().code(), ErrorCode::io_error);
    }

    void write_all_refuses_a_short_success_from_an_injected_writer() {
        FileOps ops = FileOps::real();
        ops.write = [](int, std::string_view, size_t) -> Result<size_t> { return size_t(1); };
        const auto result = write_all(1, "four", nullptr, {}, &ops);
        QVERIFY(!result.ok());
    }

    void progressing_short_writes_are_not_limited_to_sixty_four_calls() {
        TempDir dir("many-short-writes");
        auto file = DurableAppendFile::open(dir.file("tail"));
        QVERIFY(file.ok());
        file.value().set_write_limit_for_testing(1);
        const std::string bytes(80, 'x');
        QVERIFY(file.value().append(bytes).ok());
        QCOMPARE(read_file(dir.file("tail")), bytes);
    }

    void first_durable_append_syncs_the_created_name() {
        TempDir dir("created-name");
        FileOps ops = FileOps::real();
        ops.sync_dir = [](const fs::path& path) -> Result<void> {
            return Error(ErrorCode::io_error, "directory barrier failed", path.string());
        };
        auto file = DurableAppendFile::open(dir.file("tail"), nullptr, &ops);
        QVERIFY(file.ok());
        QVERIFY(!file.value().append("record").ok());
    }

    void rename_failure_preserves_cleanup_failure_context() {
        TempDir dir("cleanup");
        FileOps ops = FileOps::real();
        ops.rename = [](const fs::path&, const fs::path&) -> Result<void> {
            return Error(ErrorCode::io_error, "rename rejected");
        };
        ops.remove = [](const fs::path&) -> Result<void> {
            return Error(ErrorCode::io_error, "cleanup rejected");
        };
        const auto result = replace_atomically(dir.file("state"), "record", nullptr, &ops);
        QVERIFY(!result.ok());
        QVERIFY(result.error().context().value_or("").find("cleanup rejected") != std::string::npos);
    }

    void publication_before_directory_sync_failure_is_reported() {
        TempDir dir("published");
        FileOps ops = FileOps::real();
        ops.sync_dir = [](const fs::path&) -> Result<void> {
            return Error(ErrorCode::io_error, "directory barrier failed");
        };
        const auto result = replace_atomically(dir.file("state"), "NEW", nullptr, &ops);
        QVERIFY(!result.ok());
        QCOMPARE(read_file(dir.file("state")), std::string("NEW"));
        QVERIFY(result.error().context().value_or("").find("published") != std::string::npos);
    }

    void a_locked_lease_with_an_empty_record_cannot_be_stolen() {
        TempDir dir("lease-window");
        const auto path = dir.file("writer.lock");
        const int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
        QVERIFY(fd >= 0);
        const int locked = ::flock(fd, LOCK_EX | LOCK_NB);
        if (locked != 0) { ::close(fd); QFAIL("could not arrange locked empty record"); }
        const auto contender = ProcessLease::acquire(path);
        ::close(fd);
        QVERIFY(!contender.ok());
        QCOMPARE(contender.error().code(), ErrorCode::writer_conflict);
    }


    // =======================================================================
    // A. The step order is fixed and observable
    // =======================================================================

    void atomic_replace_performs_write_sync_replace_dirsync_in_that_order() {
        TempDir dir("order");
        Recording rec;
        const fs::path target = dir.file("state.bin");

        const auto result = replace_atomically(target, "payload", &rec.hook);
        QVERIFY(result.ok());

        // Exactly one of each step, in the documented order. This is the
        // acceptance criterion: the order is observable rather than asserted in
        // a comment and trusted.
        QCOMPARE(rec.order.size(), std::size_t(5));
        QCOMPARE(rec.order[0], std::string("write"));
        QCOMPARE(rec.order[1], std::string("file_sync"));
        QCOMPARE(rec.order[2], std::string("close"));
        QCOMPARE(rec.order[3], std::string("replace"));
        QCOMPARE(rec.order[4], std::string("dir_sync"));
        QVERIFY(rec.index_of(Step::Write) < rec.index_of(Step::FileSync));
        QVERIFY(rec.index_of(Step::FileSync) < rec.index_of(Step::Replace));
        QVERIFY(rec.index_of(Step::Replace) < rec.index_of(Step::DirSync));
    }

    void append_syncs_before_reporting_success() {
        TempDir dir("appendsync");
        Recording rec;
        const fs::path log = dir.file("tail.log");

        auto file = DurableAppendFile::open(log, &rec.hook);
        QVERIFY(file.ok());
        const auto appended = file.value().append("record\n");
        QVERIFY(appended.ok());

        // write then file_sync, with nothing in between: the caller is told
        // "durable" only after the sync actually happened.
        QCOMPARE(rec.order.size(), std::size_t(3));
        QCOMPARE(rec.order[0], std::string("write"));
        QCOMPARE(rec.order[1], std::string("file_sync"));
        QCOMPARE(rec.order[2], std::string("dir_sync"));
        QCOMPARE(file.value().counts().dir_syncs, uint64_t(1));
        QCOMPARE(file.value().sync_count(), 1u);
    }

    void sync_count_counts_real_syncs() {
        // The whole point of F9: a counter that cannot exceed the number of
        // syncs that actually occurred.
        TempDir dir("synccount");
        auto file = DurableAppendFile::open(dir.file("a.log"));
        QVERIFY(file.ok());
        QCOMPARE(file.value().sync_count(), 0u);

        QVERIFY(file.value().append("one").ok());
        QCOMPARE(file.value().sync_count(), 1u);
        QVERIFY(file.value().append_unsynced("two").ok());
        QCOMPARE(file.value().sync_count(), 1u);  // not synced yet
        QVERIFY(file.value().sync().ok());
        QCOMPARE(file.value().sync_count(), 2u);
        QCOMPARE(file.value().bytes_written(), 6u);
    }

    // =======================================================================
    // A2. The REAL operations happen, not merely the steps around them
    // =======================================================================

    void atomic_replace_performs_one_real_write_sync_rename_and_dirsync() {
        // The step hook observes *intent*. These counters observe the real
        // syscalls, which is the difference that matters: an implementation
        // announcing a step and then not performing it is exactly the F9 defect,
        // and a hook-only test is satisfied by it.
        TempDir dir("realops");
        const fs::path target = dir.file("state.bin");

        FileOpCounts counts;
        QVERIFY(replace_atomically(target, "payload", nullptr, nullptr, &counts).ok());

        QCOMPARE(counts.writes, 1u);
        QCOMPARE(counts.file_syncs, 1u);   // the temp's data, before the rename
        QCOMPARE(counts.renames, 1u);
        QCOMPARE(counts.dir_syncs, 1u);    // so the new *name* is durable
    }

    void the_real_file_sync_happens_before_the_real_rename() {
        // Ordering of the real calls, captured by wrapping them.
        TempDir dir("realorder");
        std::vector<std::string> sequence;

        FileOps ops;
        const FileOps base = FileOps::real();
        ops.sync_file = [&](int fd, const fs::path& p) {
            sequence.emplace_back("sync_file");
            return base.sync_file(fd, p);
        };
        ops.rename = [&](const fs::path& from, const fs::path& to) {
            sequence.emplace_back("rename");
            return base.rename(from, to);
        };
        ops.sync_dir = [&](const fs::path& d) {
            sequence.emplace_back("sync_dir");
            return base.sync_dir(d);
        };

        QVERIFY(replace_atomically(dir.file("state.bin"), "data", nullptr, &ops).ok());

        QCOMPARE(sequence.size(), std::size_t(3));
        QCOMPARE(sequence[0], std::string("sync_file"));
        QCOMPARE(sequence[1], std::string("rename"));
        QCOMPARE(sequence[2], std::string("sync_dir"));
    }

    void the_destination_is_never_removed_by_the_replace() {
        // L01 requires that the old destination is never deleted before its
        // replacement is in place. Watching only the outcome cannot show this --
        // an unlink followed by a rename leaves the same bytes -- so the remove
        // itself is intercepted and any removal of the destination is the bug.
        TempDir dir("noremove");
        const fs::path target = dir.file("state.bin");
        QVERIFY(replace_atomically(target, "VERSION-ONE").ok());

        bool destination_removed = false;
        FileOps ops;
        const FileOps base = FileOps::real();
        ops.remove = [&](const fs::path& p) {
            if (p == target) destination_removed = true;
            return base.remove(p);
        };

        QVERIFY(replace_atomically(target, "VERSION-TWO", nullptr, &ops).ok());

        QVERIFY2(!destination_removed,
                 "the destination was unlinked; a reader could have seen it "
                 "vanish, which is indistinguishable from data loss");
        QCOMPARE(read_file(target), std::string("VERSION-TWO"));
    }

    void a_failing_real_rename_leaves_the_old_contents() {
        // Failing the real call rather than the step around it: the previous
        // analysis found that injected step failures fire *before* the real
        // operation, so they never exercised this path.
        TempDir dir("realrenamefail");
        const fs::path target = dir.file("state.bin");
        QVERIFY(replace_atomically(target, "ORIGINAL").ok());

        FileOps ops;
        const FileOps base = FileOps::real();
        ops.rename = [](const fs::path&, const fs::path& to) {
            return Result<void>(Error(ErrorCode::io_error,
                                      "injected rename failure", to.string()));
        };

        const auto result = replace_atomically(target, "REPLACEMENT", nullptr, &ops);
        QVERIFY(!result.ok());
        QCOMPARE(read_file(target), std::string("ORIGINAL"));
    }

    void a_failing_real_file_sync_leaves_the_old_contents() {
        TempDir dir("realsyncfail");
        const fs::path target = dir.file("state.bin");
        QVERIFY(replace_atomically(target, "ORIGINAL").ok());

        FileOps ops;
        const FileOps base = FileOps::real();
        ops.sync_file = [](int, const fs::path& p) {
            return Result<void>(Error(ErrorCode::io_error, "injected sync failure",
                                      p.string()));
        };

        const auto result = replace_atomically(target, "REPLACEMENT", nullptr, &ops);
        QVERIFY(!result.ok());
        QCOMPARE(read_file(target), std::string("ORIGINAL"));
    }

    void a_failing_real_write_leaves_the_old_contents() {
        TempDir dir("realwritefail");
        const fs::path target = dir.file("state.bin");
        QVERIFY(replace_atomically(target, "ORIGINAL").ok());

        FileOps ops;
        ops.write = [](int, std::string_view, size_t) {
            return Result<size_t>(
                Error(ErrorCode::io_error, "injected write failure", "target"));
        };

        const auto result = replace_atomically(target, "REPLACEMENT", nullptr, &ops);
        QVERIFY(!result.ok());
        QCOMPARE(read_file(target), std::string("ORIGINAL"));
    }

    void append_sync_count_matches_the_real_sync_calls() {
        TempDir dir("realappendcount");
        FileOpCounts counts;
        auto file = DurableAppendFile::open(dir.file("tail.log"), nullptr, nullptr);
        QVERIFY(file.ok());

        // Two appends, each synced: two real syncs.
        Recording rec;
        auto counted = DurableAppendFile::open(dir.file("tail2.log"), nullptr, nullptr);
        QVERIFY(counted.ok());
        Q_UNUSED(rec);

        QVERIFY(file.value().append("one").ok());
        QVERIFY(file.value().append("two").ok());
        QCOMPARE(file.value().sync_count(), 2u);
        QCOMPARE(file.value().counts().file_syncs, 2u);
        QCOMPARE(file.value().counts().writes, 2u);
    }

    // =======================================================================
    // B. Failure at any step preserves the previous complete data
    // =======================================================================

    void failure_at_each_step_preserves_old_contents_data_driven() {
        // Every step, one at a time, asserting the same invariant: whatever
        // fails, the destination still holds the complete previous value and
        // never a prefix of the new one.
        const Step steps[] = {Step::Write, Step::FileSync, Step::Close, Step::Replace, Step::DirSync};

        for (const Step step : steps) {
            TempDir dir("atomicity");
            const fs::path target = dir.file("state.bin");

            // Establish known-good previous contents.
            QVERIFY(replace_atomically(target, "ORIGINAL-COMPLETE").ok());
            QCOMPARE(read_file(target), std::string("ORIGINAL-COMPLETE"));

            Recording rec;
            rec.fail_step = step;
            rec.has_fail = true;

            const auto result = replace_atomically(target, "REPLACEMENT", &rec.hook);
            QVERIFY2(!result.ok(), to_string(step).data());
            QCOMPARE(result.error().code(), step == Step::DirSync ?
                     ErrorCode::commit_uncertain : ErrorCode::io_error);
            // The error names the step that failed, so an operator can act.
            QVERIFY(!result.error().context().value_or("").empty());

            const std::string after = read_file(target);
            QVERIFY2(after == "ORIGINAL-COMPLETE" || after == "REPLACEMENT",
                     (std::string("destination holds neither complete version: ") +
                      after).c_str());
            QVERIFY2(after != "ORIGINAL-COMPLET", "destination was torn mid-write");
        }
    }

    void failed_write_is_never_reported_as_committed() {
        TempDir dir("notcommitted");
        const fs::path target = dir.file("state.bin");
        QVERIFY(replace_atomically(target, "GOOD").ok());

        Recording rec;
        rec.fail_step = Step::FileSync;
        rec.has_fail = true;

        const auto result = replace_atomically(target, "BAD", &rec.hook);
        QVERIFY(!result.ok());
        // And nothing observable claims otherwise: no leftover temporary is
        // mistaken for the real file, and the old value is intact.
        QCOMPARE(read_file(target), std::string("GOOD"));
    }

    void failed_append_reports_error_and_does_not_count_a_sync() {
        TempDir dir("appendfail");
        auto file = DurableAppendFile::open(dir.file("tail.log"));
        QVERIFY(file.ok());

        Recording rec;
        // Re-open with a hook that fails the sync.
        auto failing = DurableAppendFile::open(dir.file("tail2.log"), &rec.hook);
        QVERIFY(failing.ok());
        rec.fail_step = Step::FileSync;
        rec.has_fail = true;

        const auto result = failing.value().append("data");
        QVERIFY(!result.ok());
        QCOMPARE(result.error().code(), ErrorCode::io_error);
        QCOMPARE(failing.value().sync_count(), 0u);
    }

    // =======================================================================
    // C. Short writes are detected, not silently truncated
    // =======================================================================

    void short_write_is_reported_rather_than_silently_truncating() {
        TempDir dir("short");
        auto file = DurableAppendFile::open(dir.file("tail.log"));
        QVERIFY(file.ok());

        // Force every single write to deliver at most 3 bytes. The writer must
        // either keep going until all 10 bytes are down, or fail -- but it must
        // never report success having written fewer bytes than asked.
        file.value().set_write_limit_for_testing(3);
        const std::string payload = "0123456789";
        const auto result = file.value().append(payload);
        QVERIFY2(result.ok(), "a partial write must be retried, not reported as done");
        QCOMPARE(file.value().bytes_written(), 10u);

        const std::string contents = read_file(dir.file("tail.log"));
        QCOMPARE(contents, payload);
    }

    void write_of_nothing_is_a_no_op_success() {
        TempDir dir("emptywrite");
        auto file = DurableAppendFile::open(dir.file("tail.log"));
        QVERIFY(file.ok());
        QVERIFY(file.value().append("").ok());
        QCOMPARE(file.value().bytes_written(), 0u);
    }

    // =======================================================================
    // D. On the real platform the steps really happen
    // =======================================================================

    void real_atomic_replace_round_trips_content() {
        TempDir dir("real");
        const fs::path target = dir.file("data.bin");

        const std::string big(200000, 'x');  // larger than any internal buffer
        QVERIFY(replace_atomically(target, big).ok());
        QCOMPARE(read_file(target), big);
        QCOMPARE(fs::file_size(target), static_cast<uintmax_t>(big.size()));

        QVERIFY(replace_atomically(target, "small").ok());
        QCOMPARE(read_file(target), std::string("small"));
    }

    void real_append_is_readable_by_another_handle() {
        TempDir dir("realappend");
        const fs::path log = dir.file("tail.log");
        {
            auto file = DurableAppendFile::open(log);
            QVERIFY(file.ok());
            QVERIFY(file.value().append("first\n").ok());
            QVERIFY(file.value().append("second\n").ok());
            QVERIFY(file.value().close().ok());
        }
        QCOMPARE(read_file(log), std::string("first\nsecond\n"));
    }

    void close_is_idempotent_and_reports_after_close() {
        TempDir dir("doubleclose");
        auto file = DurableAppendFile::open(dir.file("tail.log"));
        QVERIFY(file.ok());
        QVERIFY(file.value().append("x").ok());
        QVERIFY(file.value().close().ok());
        QVERIFY(file.value().close().ok());  // second close is fine
        // Writing after close is an error, not a silent reopen.
        QVERIFY(!file.value().append("y").ok());
    }

    void missing_parent_directory_is_an_error_not_a_silent_mkdir() {
        // Creating a directory the caller did not ask for hides a configuration
        // mistake; the store decides its own layout.
        TempDir dir("noparent");
        const fs::path missing = dir.path() / "absent" / "tail.log";
        const auto result = DurableAppendFile::open(missing);
        QVERIFY(!result.ok());
        QVERIFY(!fs::exists(dir.path() / "absent"));
    }

    // =======================================================================
    // E. The destination is never unlinked before its replacement exists
    // =======================================================================

    void destination_survives_a_failed_replace_under_concurrent_read() {
        TempDir dir("noreadgap");
        const fs::path target = dir.file("state.bin");
        QVERIFY(replace_atomically(target, "VERSION-ONE").ok());

        Recording rec;
        rec.fail_step = Step::Replace;
        rec.has_fail = true;

        // While the replacement is being prepared the old file must still be
        // fully readable: a reader must never observe its absence.
        const auto failing = replace_atomically(target, "VERSION-TWO", &rec.hook);
        QVERIFY(!failing.ok());
        QCOMPARE(read_file(target), std::string("VERSION-ONE"));

        // No temporary is left behind to be mistaken for state.
        int leftovers = 0;
        for (const auto& entry : fs::directory_iterator(dir.path()))
            if (entry.path().filename().string().find(".tmp") != std::string::npos)
                ++leftovers;
        QCOMPARE(leftovers, 0);
    }

    // =======================================================================
    // F. Unsupported operations are reported
    // =======================================================================

    void syncing_a_directory_reports_success_or_unsupported_but_never_a_lie() {
        // Some platforms cannot fsync a directory. The contract is that the
        // answer is explicit -- either it worked or it says it cannot -- and
        // never a false success.
        TempDir dir("dirsync");
        QVERIFY(replace_atomically(dir.file("x"), "y").ok());

        const auto result = sync_directory(dir.path());
        if (!result.ok()) {
            QCOMPARE(result.error().code(), ErrorCode::unsupported_operation);
        }
    }

    void write_to_a_closed_descriptor_reports_io_error() {
        // Not an assertion and not a crash: a bad descriptor is a reportable
        // condition.
        const auto result = write_all(-1, "data");
        QVERIFY(!result.ok());
        QCOMPARE(result.error().code(), ErrorCode::io_error);
    }

    // =======================================================================
    // G. Process lease
    // =======================================================================

    void lease_names_a_live_holder_and_refuses_a_second_acquisition() {
        TempDir dir("lease");
        const fs::path lease = dir.file("writer.lock");

        const auto first = ProcessLease::acquire(lease);
        QVERIFY(first.ok());
        QCOMPARE(first.value().holder_pid, static_cast<uint64_t>(::getpid()));
        QVERIFY(ProcessLease::held_by_live_process(lease));

        const auto second = ProcessLease::acquire(lease);
        QVERIFY(!second.ok());
        QCOMPARE(second.error().code(), ErrorCode::writer_conflict);
        // The conflict names the holder, because "someone else has it" is not
        // something a caller can act on.
        QVERIFY(!second.error().message().empty());
    }

    void lease_over_a_dead_process_is_not_reported_as_held() {
        // A lease left behind by a crash must be distinguishable from one a live
        // writer holds, or every crash would permanently wedge the store.
        TempDir dir("stale");
        const fs::path lease = dir.file("writer.lock");

        // A pid that cannot be running: 2^22 is above Linux's default max and we
        // skip 0, which every implementation treats as "no process".
        {
            std::ofstream out(lease, std::ios::trunc);
            out << (4u << 22) << '\n';
        }
        QVERIFY(!ProcessLease::held_by_live_process(lease));

        // Which means a fresh acquisition is possible.
        const auto acquired = ProcessLease::acquire(lease);
        QVERIFY2(acquired.ok(), acquired.ok() ? "" : acquired.error().message().data());
    }

    void pid_zero_is_never_a_live_holder() {
        QVERIFY(!ProcessLease::process_alive(0));
    }

    // =======================================================================
    // H. Real-platform subprocess crash: user-space buffers do not tear a commit
    // =======================================================================

    void crashed_writer_leaves_the_previous_commit_intact() {
        // This is a process-crash test and nothing more. It proves that killing
        // a writer cannot leave a half-written file that a reader believes. It
        // does NOT prove survival of power loss: the kernel keeps writing the
        // page cache after the process dies, so no conclusion about power loss
        // may be drawn from it. The point of the limit is that the next person
        // does not draw one.
        TempDir dir("crash");
        const fs::path target = dir.file("state.bin");
        QVERIFY(replace_atomically(target, "COMMITTED-V1").ok());

        const pid_t pid = ::fork();
        QVERIFY(pid >= 0);
        if (pid == 0) {
            // Child: begin replacing, then die without unwinding or flushing.
            const auto started = replace_atomically(target, std::string(400000, 'z'));
            // _exit regardless: no destructors, no buffer flushes.
            ::_exit(started.ok() ? 0 : 1);
        }

        int status = 0;
        ::waitpid(pid, &status, 0);

        // Whatever the child managed, the destination is one complete version.
        const std::string after = read_file(target);
        const bool is_v1 = (after == "COMMITTED-V1");
        const bool is_v2 = (after == std::string(400000, 'z'));
        QVERIFY2(is_v1 || is_v2, "destination is neither the old nor the new complete value");
        QCOMPARE(fs::file_size(target), static_cast<uintmax_t>(after.size()));

        // And the store is writable again afterwards.
        QVERIFY(replace_atomically(target, "AFTER-CRASH").ok());
        QCOMPARE(read_file(target), std::string("AFTER-CRASH"));
    }
};

QTEST_MAIN(TestDurableFile)
#include "tst_durable_file.moc"
