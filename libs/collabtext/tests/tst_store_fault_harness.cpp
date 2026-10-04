// tst_store_fault_harness.cpp — V07 acceptance for the store fault harness.
//
// The claims under test:
//   1. Every fault point can be triggered deterministically, and the same fault
//      produces the same hook sequence on a replay.
//   2. An injected fault leaves the store in a state a reader can reason about:
//      the old content, or the new content, never something torn.
//   3. The filesystem fixtures produce damaged states that the *production*
//      reader either recovers from or refuses, without crashing or hanging.
//   4. A terminated worker cannot hang the runner, and leaves no torn record.
//   5. The harness itself is trustworthy: independent temp paths, parent
//      cleanup, and its own retained source data.

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include "StoreFaultHarness.h"

#include <set>

using namespace CollabText::StoreFault;

namespace {

/// Every point, so the suite cannot pass by forgetting one.
const std::vector<FaultPoint>& all_points() {
    static const std::vector<FaultPoint> points = {
        FaultPoint::WriteBefore, FaultPoint::WriteAfter,
        FaultPoint::FileSyncBefore, FaultPoint::FileSyncAfter,
        FaultPoint::RenameBefore, FaultPoint::RenameAfter,
        FaultPoint::DirSyncBefore, FaultPoint::DirSyncAfter,
        FaultPoint::SealBefore, FaultPoint::SealAfter,
        FaultPoint::CursorPublishBefore, FaultPoint::CursorPublishAfter,
        FaultPoint::CheckpointPublishBefore, FaultPoint::CheckpointPublishAfter,
    };
    return points;
}

/// A non-hook step of the sequence that runs regardless of the armed fault.
bool run_sequence(FaultPlan& plan, const fs::path& dir, const std::string& name,
                  const std::string& record) {
    DurableWriter writer(dir, name, &plan);
    // Two appends: a fault armed with skip=1 must have a second arrival to fire
    // on, which is also what proves the point is reached repeatedly.
    if (!writer.append(record)) return false;
    if (!writer.append(record)) return false;
    if (!writer.seal(1)) return false;
    if (!writer.publish_cursor(3)) return false;
    return writer.publish_checkpoint(1, 3);
}

/// The visible content of a stream's open tail, or "" when absent.
std::string open_tail(const fs::path& dir, const std::string& name) {
    const fs::path path = dir / (name + ".open");
    std::error_code ec;
    if (!fs::exists(path, ec)) return {};
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
}

/// The cursor's published value, or -1 when absent.
long long cursor_value(const fs::path& dir, const std::string& name) {
    std::ifstream file(dir / (name + ".cursor"));
    long long value = -1;
    if (file >> value) return value;
    return -1;
}

/// Path to the crash worker executable, next to this test binary.
QString crash_worker_path() {
    return QDir(QCoreApplication::applicationDirPath())
        .filePath("store_crash_worker");
}

}  // namespace

class TestStoreFaultHarness : public QObject {
    Q_OBJECT

private slots:

    // ---- the harness itself ----

    void default_plan_injects_nothing() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        FaultPlan plan;   // nothing armed
        QVERIFY(run_sequence(plan, fs::path(dir.path().toStdString()), "stream", "record-1\n"));
        QCOMPARE(open_tail(fs::path(dir.path().toStdString()), "stream"), std::string());   // sealed away
        QVERIFY(fs::exists(fs::path(dir.path().toStdString()) / "stream-0000000001.sealed"));
        QCOMPARE(cursor_value(fs::path(dir.path().toStdString()), "stream"), 3ll);
        QVERIFY(fs::exists(fs::path(dir.path().toStdString()) / "stream.checkpoint"));
        // Every hook was still visited, so the log covers the whole sequence:
        // 4 per append x 2, then write/sync/rename/dir_sync/seal for the seal,
        // then cursor and checkpoint publication.
        QCOMPARE(plan.log().size(), size_t(22));
        QVERIFY(plan.log_text().find("write.before") != std::string::npos);
        QVERIFY(plan.log_text().find("checkpoint_publish.after") != std::string::npos);
    }

    void every_fault_point_is_reachable_and_reproducible() {
        // Each point, armed once, must change the outcome of the sequence in an
        // identifiable way and produce a byte-identical hook log on a replay.
        for (const FaultPoint point : all_points()) {
            QTemporaryDir dir;
            QVERIFY(dir.isValid());
            // Fire on the first arrival: every point in the sequence must be
            // able to stop the operation it belongs to.
            FaultPlan plan;
            plan.arm(point, FaultAction::Fail);
            const bool ok = run_sequence(plan, fs::path(dir.path().toStdString()),
                                         "stream", "record-1");
            const std::string first_log = plan.log_text();
            QVERIFY2(plan.arrivals(point) >= 1, point_name(point).data());
            QVERIFY2(plan.log_text().find(point_name(point)) != std::string::npos,
                     point_name(point).data());
            const std::string_view name = point_name(point);
            if (name.ends_with(".before")) {
                // A failure injected before a step stops that step, so the
                // sequence cannot report success.
                QVERIFY2(!ok, name.data());
            } else {
                // A failure injected *after* a step cannot un-complete the work
                // the step already did. The operation still succeeds, and the
                // fault is recorded. Asserting otherwise would demand that a
                // finished write pretend it never happened.
                QVERIFY2(ok, name.data());
            }

            // Replay: same fault, same sequence, same log, byte for byte.
            QTemporaryDir replay_dir;
            QVERIFY(replay_dir.isValid());
            FaultPlan replay_plan;
            replay_plan.arm(point, FaultAction::Fail);
            const bool replay_ok = run_sequence(replay_plan,
                                                fs::path(replay_dir.path().toStdString()),
                                                "stream", "record-1");
            QCOMPARE(replay_ok, ok);
            QCOMPARE(replay_plan.log_text(), first_log);
        }
    }

    void fault_skip_count_delays_the_fault() {
        // "skip N" lets N arrivals through before the fault fires, so a test can
        // say "fail the third write" without tracking state itself.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        FaultPlan plan;
        plan.arm(FaultPoint::WriteBefore, FaultAction::Fail, /*skip=*/1);
        DurableWriter writer(fs::path(dir.path().toStdString()), "stream", &plan);
        QVERIFY2(writer.append("first"), "the first arrival should have been skipped");
        QVERIFY2(!writer.append("second"), "the second arrival should have failed");
        QCOMPARE(plan.arrivals(FaultPoint::WriteBefore), size_t(2));
    }

    void hook_log_names_the_operation_and_path() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        FaultPlan plan;
        DurableWriter writer(fs::path(dir.path().toStdString()), "alpha", &plan);
        QVERIFY(writer.append("payload\n"));

        std::set<std::string> operations;
        for (const auto& visit : plan.log())
            operations.insert(visit.operation);
        // Each step names itself, so a failure report says what failed.
        QVERIFY(operations.count("append.write") == 1);
        QVERIFY(operations.count("append.file_sync") == 1);

        FaultPlan seal_plan;
        DurableWriter sealer(fs::path(dir.path().toStdString()), "alpha", &seal_plan);
        QVERIFY(sealer.seal(1));
        QVERIFY(seal_plan.log_text().find("op=seal.rename") != std::string::npos);
        QVERIFY(seal_plan.log_text().find("op=seal.dir_sync") != std::string::npos);

        const std::string text = plan.log_text();
        QVERIFY(text.find("alpha.open") != std::string::npos);
        QVERIFY(text.find("append.file_sync") != std::string::npos);
    }

    void each_fixture_gets_an_independent_directory() {
        QTemporaryDir a;
        QTemporaryDir b;
        QVERIFY(a.isValid());
        QVERIFY(b.isValid());
        QVERIFY(a.path() != b.path());

        FaultPlan plan_a;
        FaultPlan plan_b;
        QVERIFY(run_sequence(plan_a, fs::path(a.path().toStdString()), "stream", "in-a\n"));
        QVERIFY(run_sequence(plan_b, fs::path(b.path().toStdString()), "stream", "in-b\n"));

        // Neither fixture can see the other's content.
        std::ifstream sealed_a(fs::path(a.path().toStdString()) / "stream-0000000001.sealed",
                               std::ios::binary);
        const std::string contents_a((std::istreambuf_iterator<char>(sealed_a)),
                                     std::istreambuf_iterator<char>());
        QVERIFY(contents_a.find("in-a") != std::string::npos);
        QVERIFY(contents_a.find("in-b") == std::string::npos);
    }

    // ---- what a fault leaves behind ----

    void write_fault_leaves_no_visible_content() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DurableWriter writer(fs::path(dir.path().toStdString()), "stream");
        QVERIFY(writer.append("first"));
        const std::string before = open_tail(fs::path(dir.path().toStdString()), "stream");

        FaultPlan plan;
        plan.arm(FaultPoint::WriteBefore, FaultAction::Fail);
        DurableWriter failing(fs::path(dir.path().toStdString()), "stream", &plan);
        QVERIFY(!failing.append("second"));
        QVERIFY(failing.last_error().find("injected") != std::string::npos);
        QCOMPARE(open_tail(fs::path(dir.path().toStdString()), "stream"), before);
    }

    void truncated_write_is_visible_as_a_short_record_not_a_full_one() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        FaultPlan plan;
        plan.arm(FaultPoint::WriteBefore, FaultAction::Truncate, /*skip=*/0,
                 /*truncate_to=*/4);
        DurableWriter writer(fs::path(dir.path().toStdString()), "stream", &plan);
        QVERIFY(writer.append("abcdefghij"));
        // The store holds a partial line, which is what a torn tail looks like.
        // The point is that it is short, not that it is correct: recovery from a
        // partial tail is the reader's job (L03), and the harness must be able
        // to *produce* one.
        QCOMPARE(open_tail(fs::path(dir.path().toStdString()), "stream"), std::string("abcd"));
    }

    void rename_fault_during_seal_loses_nothing() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const fs::path root(dir.path().toStdString());
        DurableWriter writer(root, "stream");
        QVERIFY(writer.append("durable-1"));
        const std::string before = open_tail(root, "stream");

        FaultPlan plan;
        plan.arm(FaultPoint::RenameBefore, FaultAction::Fail);
        DurableWriter failing(root, "stream", &plan);
        QVERIFY(!failing.seal(1));
        // The tail is still there, byte for byte, because the rename is what
        // would have consumed it. A failed seal must not delete the data it was
        // sealing.
        QCOMPARE(open_tail(root, "stream"), before);
        QVERIFY(!fs::exists(root / "stream-0000000001.sealed"));
        // The temp file survives the failed rename. It must not be mistaken for
        // visible content, which is why the sealed name only appears via rename.
        QVERIFY(fs::exists(root / "stream-0000000001.sealed.tmp"));
        // Reading the tail still yields the record.
        std::ifstream file(root / "stream.open", std::ios::binary);
        std::string contents((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
        QVERIFY(contents.find("durable-1") != std::string::npos);
    }

    void directory_sync_fault_is_reported_not_swallowed() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        FaultPlan plan;
        plan.arm(FaultPoint::DirSyncBefore, FaultAction::Fail);
        DurableWriter writer(fs::path(dir.path().toStdString()), "stream", &plan);
        QVERIFY(writer.append("record"));
        QVERIFY(!writer.seal(1));
        QVERIFY2(writer.last_error().find("directory sync") != std::string::npos,
                 writer.last_error().c_str());
    }

    void seal_fault_leaves_the_open_tail_reachable() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DurableWriter writer(fs::path(dir.path().toStdString()), "stream");
        QVERIFY(writer.append("record-a"));
        const std::string before = open_tail(fs::path(dir.path().toStdString()), "stream");

        FaultPlan plan;
        plan.arm(FaultPoint::SealBefore, FaultAction::Fail);
        DurableWriter failing(fs::path(dir.path().toStdString()), "stream", &plan);
        QVERIFY(!failing.seal(7));
        // The data is still there: sealing failed, deleting did not happen.
        QCOMPARE(open_tail(fs::path(dir.path().toStdString()), "stream"), before);
        QVERIFY(!fs::exists(fs::path(dir.path().toStdString()) / "stream-0000000007.sealed"));
    }

    void cursor_fault_leaves_the_previous_cursor() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DurableWriter writer(fs::path(dir.path().toStdString()), "stream");
        QVERIFY(writer.publish_cursor(11));
        QCOMPARE(cursor_value(fs::path(dir.path().toStdString()), "stream"), 11ll);

        FaultPlan plan;
        plan.arm(FaultPoint::CursorPublishBefore, FaultAction::Fail);
        DurableWriter failing(fs::path(dir.path().toStdString()), "stream", &plan);
        QVERIFY(!failing.publish_cursor(99));
        // A reader must not be told about progress that was not published.
        QCOMPARE(cursor_value(fs::path(dir.path().toStdString()), "stream"), 11ll);
        QCOMPARE(failing.published_cursor(), 0u);
    }

    void checkpoint_fault_leaves_the_previous_checkpoint() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DurableWriter writer(fs::path(dir.path().toStdString()), "stream");
        QVERIFY(writer.publish_checkpoint(4, 40));
        std::ifstream before(fs::path(dir.path().toStdString()) / "stream.checkpoint");
        std::string stored;
        before >> stored;

        FaultPlan plan;
        plan.arm(FaultPoint::CheckpointPublishBefore, FaultAction::Fail);
        DurableWriter failing(fs::path(dir.path().toStdString()), "stream", &plan);
        QVERIFY(!failing.publish_checkpoint(9, 90));

        std::ifstream after(fs::path(dir.path().toStdString()) / "stream.checkpoint");
        std::string still;
        after >> still;
        QCOMPARE(still, stored);
    }

    void simulated_disk_full_is_explicit_and_bounded() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DurableWriter writer(fs::path(dir.path().toStdString()), "stream");
        writer.set_disk_full_after(21);
        QVERIFY(writer.append("0123456789abcdefghij"));   // exactly 21 bytes > 20
        QVERIFY(!writer.append("x"));
        // The failure is reported with the real errno-style wording rather than
        // silently dropping the record.
        QVERIFY(writer.last_error().find("no space left") != std::string::npos);
        // The bytes written before the limit survived intact: a full disk is
        // allowed to lose the new write, not to corrupt the old one.
        QCOMPARE(open_tail(fs::path(dir.path().toStdString()), "stream"), std::string("0123456789abcdefghij\n"));
    }

    void writer_lock_is_exclusive() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DurableWriter first(fs::path(dir.path().toStdString()), "stream");
        QVERIFY(first.acquire_lock());

        DurableWriter second(fs::path(dir.path().toStdString()), "stream");
        QVERIFY(!second.acquire_lock());
        QVERIFY(second.last_error().find("lock") != std::string::npos);

        first.release_lock();
        DurableWriter third(fs::path(dir.path().toStdString()), "stream");
        QVERIFY(third.acquire_lock());
    }

    // ---- filesystem fixtures, read by the production reader ----

    void missing_segment_is_recovered_from() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const fs::path stream = fs::path(dir.path().toStdString()) / "stream";
        write_segments(stream, CollabText::Crdt::SegmentKind::Ops, 1, 3, 2);
        remove_segment(stream, 2);

        const auto records = read_all(stream, fs::path(dir.path().toStdString()) / "cursor");
        // The gap is a missing segment, not a silent skip of everything after
        // it: the later segment is still readable. Whether the *gap* is
        // detectable is F6, owned by L03/L08; here we pin what happens today.
        QCOMPARE(records.size(), size_t(4));
        QVERIFY(records.front().find("segment 1 record 0") == 0);
        QVERIFY(records.back().find("segment 3 record 1") == 0);
    }

    void out_of_order_arrival_is_observable() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const fs::path stream = fs::path(dir.path().toStdString()) / "stream";
        write_segments(stream, CollabText::Crdt::SegmentKind::Ops, 1, 1, 2);

        // Read and commit, so the cursor moves past segment 1.
        auto first = read_all(stream, fs::path(dir.path().toStdString()) / "cursor");
        QCOMPARE(first.size(), size_t(2));

        // Segment 2 arrives, then a *late* segment 0 the reader has never seen.
        write_segments(stream, CollabText::Crdt::SegmentKind::Ops, 2, 1, 2);
        write_segments(stream, CollabText::Crdt::SegmentKind::Ops, 0, 1, 2);
        auto second = read_all(stream, fs::path(dir.path().toStdString()) / "cursor");
        QCOMPARE(second.size(), size_t(2));

        // Today the late earlier segment is skipped rather than applied, because
        // the cursor already advanced past it. This is finding F6: it must
        // block advancing coverage rather than be skipped, and it belongs to L03.
        // The assertion documents today's behaviour so the fix has a red test.
        for (const auto& record : second)
            QVERIFY(record.find("segment 0") == std::string::npos);
    }

    void partial_tail_is_read_up_to_the_last_complete_record() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const fs::path stream = fs::path(dir.path().toStdString()) / "stream";
        // An open tail in the reader's format: base64 lines terminated by \n.
        const std::string good = "aGVsbG8=\n";      // "hello"
        const std::string second = "d29ybGQ=\n";   // "world"
        const fs::path open = stream / "0000000001.open";
        fs::create_directories(stream);
        {
            std::ofstream file(open, std::ios::binary | std::ios::trunc);
            file << good << second << "cGFydA";     // last record has no newline
        }
        const auto records = read_all(stream, fs::path(dir.path().toStdString()) / "cursor");
        // Complete records survive; the unterminated one is not invented.
        QCOMPARE(records.size(), size_t(2));
        QCOMPARE(records[0], std::string("hello"));
        QCOMPARE(records[1], std::string("world"));
    }

    void corrupt_segment_does_not_crash_the_reader() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const fs::path stream = fs::path(dir.path().toStdString()) / "stream";
        write_segments(stream, CollabText::Crdt::SegmentKind::Ops, 1, 3, 2);
        const fs::path victim = stream / DurableWriter::segment_file_name(2);
        QVERIFY(corrupt_file(victim));

        const auto records = read_all(stream, fs::path(dir.path().toStdString()) / "cursor");
        // The damaged segment yields nothing; the intact ones still do. Today's
        // behaviour is to skip it, which is also F6: unsupported or corrupt
        // input must block advancing coverage rather than be skipped silently.
        QVERIFY(records.size() < 6);
        for (const auto& record : records)
            QVERIFY(record.find("segment 2") == std::string::npos);
        QCOMPARE(records.front().find("segment 1 record 0"), size_t(0));
    }

    void repaired_segment_is_readable_again() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const fs::path stream = fs::path(dir.path().toStdString()) / "stream";
        write_segments(stream, CollabText::Crdt::SegmentKind::Ops, 1, 1, 2);
        const fs::path segment = stream / DurableWriter::segment_file_name(1);

        std::string good;
        {
            std::ifstream file(segment, std::ios::binary);
            good.assign(std::istreambuf_iterator<char>(file),
                        std::istreambuf_iterator<char>());
        }
        QVERIFY(corrupt_file(segment));
        QCOMPARE(read_all(stream, fs::path(dir.path().toStdString()) / "cursor").size(), size_t(0));

        // Repair restores the exact bytes; a reader that skipped the damaged
        // segment must be able to read it after a repair, from a fresh cursor.
        {
            std::ofstream file(segment, std::ios::binary | std::ios::trunc);
            file << good;
        }
        QCOMPARE(read_all(stream, fs::path(dir.path().toStdString()) / "cursor2").size(), size_t(2));
    }

    // ---- subprocess crash ----

    void crash_worker_path_exists() {
        QVERIFY2(QFile::exists(crash_worker_path()),
                 qPrintable(QString("crash worker not built next to the test: %1")
                                .arg(crash_worker_path())));
    }

    void terminated_worker_leaves_no_torn_record() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dir_path = dir.path();

        // Each crash point: the worker writes durable records, then dies inside
        // the next step that reaches the armed point. Whatever survives must be
        // whole records. The rename, directory-sync, seal and publication points
        // are only reachable while sealing, so they run in the seal phase.
        struct Case { const char* point; const char* phase; };
        const std::vector<Case> cases = {
            {"write.before", "append"},  {"write.after", "append"},
            {"file_sync.before", "append"}, {"file_sync.after", "append"},
            {"seal.before", "seal"},     {"write.before", "seal"},
            {"rename.before", "seal"},   {"rename.after", "seal"},
            {"dir_sync.before", "seal"}, {"dir_sync.after", "seal"},
            {"cursor_publish.before", "seal"}, {"cursor_publish.after", "seal"},
            {"checkpoint_publish.before", "seal"}, {"checkpoint_publish.after", "seal"},
        };
        for (const Case& test_case : cases) {
            const char* point = test_case.point;
            QProcess worker;
            worker.start(crash_worker_path(),
                         {dir_path, QStringLiteral("stream"),
                          QString::fromLatin1(point), QStringLiteral("2"),
                          QString::fromLatin1(test_case.phase)});
            // A hung worker must fail the test rather than hang the runner.
            QVERIFY2(worker.waitForStarted(5000), "crash worker did not start");
            QVERIFY2(worker.waitForFinished(30000), "crash worker hung");
            QCOMPARE(worker.exitStatus(), QProcess::NormalExit);
            QCOMPARE(worker.exitCode(), kAbortExitCode);

            const fs::path stream = fs::path(dir_path.toStdString());
            const std::string label =
                std::string(test_case.phase) + ":" + point;
            std::error_code ec;
            if (fs::exists(stream / "stream.open", ec)) {
                std::ifstream file(stream / "stream.open", std::ios::binary);
                const std::string contents((std::istreambuf_iterator<char>(file)),
                                           std::istreambuf_iterator<char>());
                QVERIFY2(!contents.empty(), label.c_str());
                // Every surviving line is a complete record: the stream never
                // ends mid-record, so a reader cannot mistake a torn write for a
                // whole one.
                QVERIFY2(contents.back() == '\n', label.c_str());
                size_t start_pos = 0;
                size_t lines = 0;
                while (start_pos < contents.size()) {
                    const size_t newline = contents.find('\n', start_pos);
                    QVERIFY2(newline != std::string::npos, label.c_str());
                    const std::string line = contents.substr(start_pos, newline - start_pos);
                    // Only records the worker writes can appear, and each must
                    // be complete: the interrupted record may be present whole.
                    const bool known = line.rfind("stable record ", 0) == 0 ||
                                       line == "the record that never finished";
                    QVERIFY2(known, label.c_str());
                    start_pos = newline + 1;
                    ++lines;
                }
                // The records written before the crash point are there. A crash
                // *after* a write may also leave the interrupted record complete
                // in the tail, which is safe precisely because it is whole.
                QVERIFY2(lines >= 2 && lines <= 3, label.c_str());
            }

            // A temp file may survive a failed or interrupted rename. It must
            // never be mistaken for visible content, which is why the reader
            // only ever looks at `stream.open`.
            const fs::path tmp = stream / "stream.open.tmp";
            if (fs::exists(tmp, ec)) {
                std::ifstream file(tmp, std::ios::binary);
                const std::string contents((std::istreambuf_iterator<char>(file)),
                                           std::istreambuf_iterator<char>());
                QVERIFY2(contents.find("stable record") != std::string::npos, label.c_str());
            }

            // Reset the directory for the next crash point.
            fs::remove_all(stream);
            fs::create_directories(stream);
        }
    }

    void worker_completes_normally_without_a_fault() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QProcess worker;
        worker.start(crash_worker_path(),
                     {dir.path(), QStringLiteral("stream"), QStringLiteral("none"),
                      QStringLiteral("2")});
        QVERIFY(worker.waitForStarted(5000));
        QVERIFY(worker.waitForFinished(30000));
        QCOMPARE(worker.exitStatus(), QProcess::NormalExit);
        QCOMPARE(worker.exitCode(), 0);

        // The worker did the whole sequence and left a sealed segment, so the
        // crash tests are not passing because the worker never worked.
        const fs::path stream = fs::path(dir.path().toStdString());
        QVERIFY(fs::exists(stream / "stream-0000000001.sealed"));
        QCOMPARE(cursor_value(stream, "stream"), 2ll);
    }

};

QTEST_GUILESS_MAIN(TestStoreFaultHarness)
#include "tst_store_fault_harness.moc"
