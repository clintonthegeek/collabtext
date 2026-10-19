#include <QTest>
#include <QTemporaryDir>
#include <QProcess>

#include "crdt/SegmentWriter.h"
#include "crdt/SegmentFormat.h"
#include "crdt/Buffer.h"
#include "crdt/ZstdUtil.h"
#include <collabtext/Serialization.h>
#include "TestActor.h"

#include <fstream>

using namespace CollabText::Crdt;
namespace fs = std::filesystem;

namespace {
WriterConfig fast_test_config() {
    WriterConfig c;
    c.flush_bytes = 1;
    c.flush_idle  = std::chrono::milliseconds(0);
    c.seal_bytes  = 64;
    c.seal_idle   = std::chrono::seconds(30);
    c.zstd_level  = 1;
    return c;
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

size_t count_files_with_suffix(const fs::path& dir, std::string_view suffix) {
    size_t n = 0;
    for (auto& e : fs::directory_iterator(dir)) {
        std::string name = e.path().filename().string();
        if (name.size() >= suffix.size()
            && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            ++n;
        }
    }
    return n;
}

std::pair<std::string, uint64_t> make_operation_wire(Buffer& buffer, const std::string& text) {
    auto op = buffer.apply_local_edit({{0, 0}}, {text});
    auto wire = CollabText::Crdt::encode_operation(op);
    Q_ASSERT(wire.ok());
    return {wire.value(), op_identity(op).counter};
}
}

class TestSegmentWriter : public QObject {
    Q_OBJECT
private slots:
    void start_creates_directory_and_no_open_segment_yet() {
        QTemporaryDir tmp;
        fs::path dir = fs::path(tmp.path().toStdString()) / "log" / "ops";
        SegmentWriter w(dir, SegmentKind::Ops, fast_test_config());
        QVERIFY(w.start().ok());
        QVERIFY(fs::exists(dir));
        QCOMPARE(count_files_with_suffix(dir, ".open"), size_t(0));
    }

    void append_then_tick_writes_open_tail() {
        QTemporaryDir tmp;
        fs::path dir = fs::path(tmp.path().toStdString()) / "log" / "ops";
        SegmentWriter w(dir, SegmentKind::Ops, fast_test_config());
        QVERIFY(w.start().ok());
        auto t0 = std::chrono::steady_clock::time_point{};
        QVERIFY(w.append("hello", 1).ok());
        QVERIFY(w.tick(t0).ok());
        QCOMPARE(count_files_with_suffix(dir, ".open"), size_t(1));
        QCOMPARE(count_files_with_suffix(dir, ".seg.zst"), size_t(0));
        size_t bytes = 0;
        for (auto& e : fs::directory_iterator(dir))
            if (e.path().extension() == ".open") bytes = fs::file_size(e.path());
        QVERIFY(bytes > 0);
    }

    void seal_at_size_threshold_emits_seg_zst_and_unlinks_open() {
        QTemporaryDir tmp;
        fs::path dir = fs::path(tmp.path().toStdString()) / "log" / "ops";
        SegmentWriter w(dir, SegmentKind::Ops, fast_test_config());
        QVERIFY(w.start().ok());
        auto t0 = std::chrono::steady_clock::time_point{};
        for (int i = 0; i < 8; ++i) QVERIFY(w.append("0123456789", uint64_t(i + 1)).ok());
        QVERIFY(w.tick(t0).ok());
        QCOMPARE(count_files_with_suffix(dir, ".seg.zst"), size_t(1));
        // After seal, no open exists until next append.
        QCOMPARE(count_files_with_suffix(dir, ".open"), size_t(0));
    }

    void seal_records_first_and_last_lamport() {
        QTemporaryDir tmp;
        fs::path dir = fs::path(tmp.path().toStdString()) / "log" / "ops";
        SegmentWriter w(dir, SegmentKind::Ops, fast_test_config());
        QVERIFY(w.start().ok());
        auto t0 = std::chrono::steady_clock::time_point{};
        for (int i = 0; i < 8; ++i) QVERIFY(w.append("0123456789", uint64_t(i + 7)).ok());
        QVERIFY(w.tick(t0).ok());
        fs::path sealed_path;
        for (auto& e : fs::directory_iterator(dir))
            if (e.path().filename().string().ends_with(".seg.zst")) sealed_path = e.path();
        QVERIFY(!sealed_path.empty());
        auto seg = decode_sealed_segment(read_file(sealed_path));
        QVERIFY(seg.has_value());
        QCOMPARE(seg->header.first_lamport, uint64_t(7));
        QCOMPARE(seg->header.last_lamport, uint64_t(14));
        QCOMPARE(seg->records.size(), size_t(8));
    }

    void close_seals_current_segment() {
        QTemporaryDir tmp;
        fs::path dir = fs::path(tmp.path().toStdString()) / "log" / "ops";
        SegmentWriter w(dir, SegmentKind::Ops, fast_test_config());
        QVERIFY(w.start().ok());
        auto t0 = std::chrono::steady_clock::time_point{};
        QVERIFY(w.append("tiny", 1).ok());
        QVERIFY(w.tick(t0).ok());
        QVERIFY(w.close().ok());
        QCOMPARE(count_files_with_suffix(dir, ".seg.zst"), size_t(1));
        QCOMPARE(count_files_with_suffix(dir, ".open"), size_t(0));
    }

    void next_append_after_seal_opens_next_id() {
        QTemporaryDir tmp;
        fs::path dir = fs::path(tmp.path().toStdString()) / "log" / "ops";
        SegmentWriter w(dir, SegmentKind::Ops, fast_test_config());
        QVERIFY(w.start().ok());
        auto t0 = std::chrono::steady_clock::time_point{};
        for (int i = 0; i < 8; ++i) QVERIFY(w.append("0123456789", uint64_t(i + 1)).ok());
        QVERIFY(w.tick(t0).ok());
        QVERIFY(w.append("after-seal", 100).ok());
        QVERIFY(w.tick(t0).ok());
        QCOMPARE(count_files_with_suffix(dir, ".seg.zst"), size_t(1));
        QCOMPARE(count_files_with_suffix(dir, ".open"), size_t(1));
    }

    void recovery_truncates_partial_trailing_line_in_open() {
        QTemporaryDir tmp;
        fs::path dir = fs::path(tmp.path().toStdString()) / "log" / "ops";
        fs::create_directories(dir);
        fs::path torn = dir / "0000000001.open";
        Buffer buffer(CollabText::Test::test_counter(1));
        const auto [wire, counter] = make_operation_wire(buffer, "hello");
        (void)counter;
        {
            std::ofstream f(torn, std::ios::binary);
            f << base64_encode(wire) << '\n'
              << "dGhpc2lzcGFydGlhbA";
        }
        SegmentWriter w(dir, SegmentKind::Ops, fast_test_config());
        QVERIFY(w.start().ok());
        std::string content = read_file(torn);
        QCOMPARE(content, base64_encode(wire) + "\n");
    }

    void invalid_interrupted_seal_is_retained_and_reported() {
        QTemporaryDir tmp;
        fs::path dir = fs::path(tmp.path().toStdString()) / "log" / "ops";
        fs::create_directories(dir);
        fs::path stale = dir / "0000000001.seg.zst.tmp";
        std::ofstream(stale, std::ios::binary) << "garbage";
        SegmentWriter w(dir, SegmentKind::Ops, fast_test_config());
        const auto started = w.start();
        QVERIFY(!started.ok());
        QVERIFY(fs::exists(stale));
    }

    void invalid_sealed_file_does_not_discard_recoverable_open_tail() {
        QTemporaryDir tmp;
        fs::path dir = fs::path(tmp.path().toStdString()) / "log" / "ops";
        fs::create_directories(dir);
        fs::path sealed = dir / "0000000005.seg.zst";
        fs::path open5  = dir / "0000000005.open";
        std::ofstream(sealed, std::ios::binary) << "anything";
        Buffer buffer(CollabText::Test::test_counter(1));
        const auto [wire, counter] = make_operation_wire(buffer, "stale");
        (void)counter;
        std::ofstream(open5, std::ios::binary) << base64_encode(wire) << '\n';
        SegmentWriter w(dir, SegmentKind::Ops, fast_test_config());
        QVERIFY(w.start().ok());
        QVERIFY(fs::exists(sealed));
        QVERIFY(fs::exists(open5));
    }

    void fsync_count_tracks_successful_storage_syncs() {
        QTemporaryDir tmp;
        const fs::path dir = fs::path(tmp.path().toStdString()) / "log" / "ops";
        SegmentWriter w(dir, SegmentKind::Ops, fast_test_config());
        QVERIFY(w.start().ok());
        QVERIFY(w.append("record", 1).ok());
        QVERIFY(w.tick(std::chrono::steady_clock::now()).ok());
        QVERIFY(w.stats().fsync_count >= 1);
        QVERIFY(w.close().ok());
    }

    void failed_seal_keeps_the_open_tail_for_recovery() {
        QTemporaryDir tmp;
        const fs::path dir = fs::path(tmp.path().toStdString()) / "log" / "ops";
        CollabText::Storage::StepHook hook;
        hook.fail = [](CollabText::Storage::Step step, const fs::path& path)
                -> std::optional<CollabText::Error> {
            if (step == CollabText::Storage::Step::FileSync
                && path.filename().string().find(".seg.zst.tmp.") != std::string::npos)
                return CollabText::Error(CollabText::ErrorCode::io_error,
                                         "injected seal sync failure");
            return std::nullopt;
        };
        auto cfg = fast_test_config();
        cfg.step_hook = &hook;
        cfg.seal_bytes = 1024 * 1024;
        Buffer buffer(CollabText::Test::test_counter(8));
        const auto [wire, counter] = make_operation_wire(buffer, "payload");
        {
            SegmentWriter w(dir, SegmentKind::Ops, cfg);
            QVERIFY(w.start().ok());
            QVERIFY(w.append(wire, counter).ok());
            QVERIFY(w.tick(std::chrono::steady_clock::now()).ok());
            const auto syncs_before_seal = w.stats().fsync_count;
            QVERIFY(!w.close().ok());
            QCOMPARE(w.stats().fsync_count, syncs_before_seal + 1);
            QCOMPARE(count_files_with_suffix(dir, ".open"), size_t(1));
            QCOMPARE(count_files_with_suffix(dir, ".seg.zst"), size_t(0));
        }
        SegmentWriter recovered(dir, SegmentKind::Ops, fast_test_config());
        QVERIFY(recovered.start().ok());
        QVERIFY(recovered.close().ok());
        QCOMPARE(count_files_with_suffix(dir, ".seg.zst"), size_t(1));
        QCOMPARE(count_files_with_suffix(dir, ".open"), size_t(0));
    }

    void recovery_publishes_a_valid_interrupted_seal() {
        QTemporaryDir tmp;
        const fs::path dir = fs::path(tmp.path().toStdString()) / "log" / "ops";
        fs::create_directories(dir);
        SegmentHeader h{};
        h.kind = SegmentKind::Ops;
        const auto bytes = encode_sealed_segment(h, {"record"});
        const fs::path temporary = dir / "0000000001.seg.zst.tmp";
        std::ofstream(temporary, std::ios::binary).write(bytes.data(), bytes.size());
        SegmentWriter w(dir, SegmentKind::Ops, fast_test_config());
        QVERIFY(w.start().ok());
        QVERIFY(!fs::exists(temporary));
        const fs::path published = dir / "0000000001.seg.zst";
        QVERIFY(fs::exists(published));
        auto decoded = decode_sealed_segment(read_file(published));
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->records, std::vector<std::string>{"record"});
    }

    void recovery_rebuilds_header_counter_range() {
        QTemporaryDir tmp;
        const fs::path dir = fs::path(tmp.path().toStdString()) / "log" / "ops";
        fs::create_directories(dir);
        Buffer buffer(CollabText::Test::test_counter(7));
        const auto [first_wire, first_counter] = make_operation_wire(buffer, "first");
        const auto [last_wire, last_counter] = make_operation_wire(buffer, "second");
        const fs::path open = dir / "0000000003.open";
        {
            std::ofstream f(open, std::ios::binary);
            f << base64_encode(first_wire) << '\n' << base64_encode(last_wire) << '\n';
        }
        SegmentWriter w(dir, SegmentKind::Ops, fast_test_config());
        QVERIFY(w.start().ok());
        QVERIFY(w.close().ok());
        const fs::path sealed = dir / "0000000003.seg.zst";
        auto decoded = decode_sealed_segment(read_file(sealed));
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->header.first_lamport, first_counter);
        QCOMPARE(decoded->header.last_lamport, last_counter);
    }

    void append_exposes_bounded_backpressure() {
        QTemporaryDir tmp;
        auto cfg = fast_test_config();
        cfg.max_pending_bytes = 4;
        SegmentWriter w(fs::path(tmp.path().toStdString()) / "log",
                        SegmentKind::Ops, cfg);
        QVERIFY(w.start().ok());
        QVERIFY(w.append("four", 1).ok());
        const auto rejected = w.append("x", 2);
        QVERIFY(!rejected.ok());
        QCOMPARE(rejected.error().code(), CollabText::ErrorCode::deferred_queue_full);
    }

    void subprocess_crashes_during_seal_recover_the_record_once() {
        for (const auto& [point, code] :
             {std::pair<const char*, int>{"temp_sync", 81}, {"publish", 82}}) {
            QTemporaryDir tmp;
            QVERIFY(tmp.isValid());
            const fs::path dir = fs::path(tmp.path().toStdString()) / "log" / "ops";
            QProcess worker;
            worker.start(QString::fromUtf8(COLLABTEXT_SEGMENT_WRITER_CRASH_WORKER),
                         {QString::fromStdString(dir.string()), QString::fromLatin1(point)});
            QVERIFY2(worker.waitForStarted(5000), qPrintable(worker.errorString()));
            QVERIFY2(worker.waitForFinished(15000), "segment writer crash worker hung");
            QCOMPARE(worker.exitStatus(), QProcess::NormalExit);
            QCOMPARE(worker.exitCode(), code);

            SegmentWriter recovered(dir, SegmentKind::Ops, fast_test_config());
            QVERIFY(recovered.start().ok());
            QVERIFY(recovered.close().ok());
            const fs::path sealed = dir / "0000000001.seg.zst";
            QVERIFY(fs::exists(sealed));
            auto segment = decode_sealed_segment(read_file(sealed));
            QVERIFY(segment.has_value());
            QCOMPARE(segment->records.size(), size_t(1));
            auto operation = CollabText::Crdt::decode_operation(segment->records.front());
            QVERIFY(operation.ok());
            QCOMPARE(CollabText::Crdt::op_identity(operation.value()).counter, uint64_t(11));
            QCOMPARE(count_files_with_suffix(dir, ".open"), size_t(0));
        }
    }
};

QTEST_APPLESS_MAIN(TestSegmentWriter)
#include "tst_segment_writer.moc"
