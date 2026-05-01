#include <QTest>
#include <QTemporaryDir>

#include "crdt/SegmentWriter.h"
#include "crdt/SegmentFormat.h"

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
}

class TestSegmentWriter : public QObject {
    Q_OBJECT
private slots:
    void start_creates_directory_and_no_open_segment_yet() {
        QTemporaryDir tmp;
        fs::path dir = fs::path(tmp.path().toStdString()) / "log" / "ops";
        SegmentWriter w(dir, SegmentKind::Ops, fast_test_config());
        w.start();
        QVERIFY(fs::exists(dir));
        QCOMPARE(count_files_with_suffix(dir, ".open"), size_t(0));
    }

    void append_then_tick_writes_open_tail() {
        QTemporaryDir tmp;
        fs::path dir = fs::path(tmp.path().toStdString()) / "log" / "ops";
        SegmentWriter w(dir, SegmentKind::Ops, fast_test_config());
        w.start();
        auto t0 = std::chrono::steady_clock::time_point{};
        w.append("hello", 1);
        w.tick(t0);
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
        w.start();
        auto t0 = std::chrono::steady_clock::time_point{};
        for (int i = 0; i < 8; ++i) w.append("0123456789", uint64_t(i + 1));
        w.tick(t0);
        QCOMPARE(count_files_with_suffix(dir, ".seg.zst"), size_t(1));
        // After seal, no open exists until next append.
        QCOMPARE(count_files_with_suffix(dir, ".open"), size_t(0));
    }

    void seal_records_first_and_last_lamport() {
        QTemporaryDir tmp;
        fs::path dir = fs::path(tmp.path().toStdString()) / "log" / "ops";
        SegmentWriter w(dir, SegmentKind::Ops, fast_test_config());
        w.start();
        auto t0 = std::chrono::steady_clock::time_point{};
        for (int i = 0; i < 8; ++i) w.append("0123456789", uint64_t(i + 7));
        w.tick(t0);
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
        w.start();
        auto t0 = std::chrono::steady_clock::time_point{};
        w.append("tiny", 1);
        w.tick(t0);
        w.close();
        QCOMPARE(count_files_with_suffix(dir, ".seg.zst"), size_t(1));
        QCOMPARE(count_files_with_suffix(dir, ".open"), size_t(0));
    }

    void next_append_after_seal_opens_next_id() {
        QTemporaryDir tmp;
        fs::path dir = fs::path(tmp.path().toStdString()) / "log" / "ops";
        SegmentWriter w(dir, SegmentKind::Ops, fast_test_config());
        w.start();
        auto t0 = std::chrono::steady_clock::time_point{};
        for (int i = 0; i < 8; ++i) w.append("0123456789", uint64_t(i + 1));
        w.tick(t0);
        w.append("after-seal", 100);
        w.tick(t0);
        QCOMPARE(count_files_with_suffix(dir, ".seg.zst"), size_t(1));
        QCOMPARE(count_files_with_suffix(dir, ".open"), size_t(1));
    }

    void recovery_truncates_partial_trailing_line_in_open() {
        QTemporaryDir tmp;
        fs::path dir = fs::path(tmp.path().toStdString()) / "log" / "ops";
        fs::create_directories(dir);
        fs::path torn = dir / "0000000001.open";
        {
            std::ofstream f(torn, std::ios::binary);
            f << "aGVsbG8=\n"
              << "dGhpc2lzcGFydGlhbA";
        }
        SegmentWriter w(dir, SegmentKind::Ops, fast_test_config());
        w.start();
        std::string content = read_file(torn);
        QCOMPARE(content, std::string("aGVsbG8=\n"));
    }

    void recovery_deletes_stale_tmp_files() {
        QTemporaryDir tmp;
        fs::path dir = fs::path(tmp.path().toStdString()) / "log" / "ops";
        fs::create_directories(dir);
        fs::path stale = dir / "0000000001.seg.zst.tmp";
        std::ofstream(stale, std::ios::binary) << "garbage";
        SegmentWriter w(dir, SegmentKind::Ops, fast_test_config());
        w.start();
        QVERIFY(!fs::exists(stale));
    }

    void recovery_unlinks_open_with_id_le_highest_sealed() {
        QTemporaryDir tmp;
        fs::path dir = fs::path(tmp.path().toStdString()) / "log" / "ops";
        fs::create_directories(dir);
        fs::path sealed = dir / "0000000005.seg.zst";
        fs::path open5  = dir / "0000000005.open";
        std::ofstream(sealed, std::ios::binary) << "anything";
        std::ofstream(open5, std::ios::binary)  << "stale\n";
        SegmentWriter w(dir, SegmentKind::Ops, fast_test_config());
        w.start();
        QVERIFY(fs::exists(sealed));
        QVERIFY(!fs::exists(open5));
    }
};

QTEST_APPLESS_MAIN(TestSegmentWriter)
#include "tst_segment_writer.moc"
