#include <QTest>
#include <QTemporaryDir>

#include "crdt/SegmentReader.h"
#include "crdt/SegmentWriter.h"
#include "crdt/SegmentFormat.h"
#include "crdt/ZstdUtil.h"

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
}

class TestSegmentReader : public QObject {
    Q_OBJECT
private slots:
    void no_data_yields_nothing() {
        QTemporaryDir tmp;
        fs::path stream = fs::path(tmp.path().toStdString()) / "stream";
        fs::create_directories(stream);
        fs::path cursor = fs::path(tmp.path().toStdString()) / "cur.bin";
        SegmentReader r(stream, cursor);
        r.start();
        auto out = r.read_new();
        QVERIFY(out.empty());
    }

    void reads_open_tail_records() {
        QTemporaryDir tmp;
        fs::path stream = fs::path(tmp.path().toStdString()) / "stream";
        fs::path cursor = fs::path(tmp.path().toStdString()) / "cur.bin";
        SegmentWriter w(stream, SegmentKind::Ops, fast_test_config());
        w.start();
        auto t0 = std::chrono::steady_clock::time_point{};
        w.append("alpha", 1);
        w.append("beta",  2);
        w.tick(t0);

        SegmentReader r(stream, cursor);
        r.start();
        auto out = r.read_new();
        QCOMPARE(out.size(), size_t(2));
        QCOMPARE(out[0], std::string("alpha"));
        QCOMPARE(out[1], std::string("beta"));
        r.commit();
    }

    void reads_sealed_then_open_in_order() {
        QTemporaryDir tmp;
        fs::path stream = fs::path(tmp.path().toStdString()) / "stream";
        fs::path cursor = fs::path(tmp.path().toStdString()) / "cur.bin";
        SegmentWriter w(stream, SegmentKind::Ops, fast_test_config());
        w.start();
        auto t0 = std::chrono::steady_clock::time_point{};
        for (int i = 0; i < 8; ++i) w.append("0123456789", uint64_t(i + 1));
        w.tick(t0);
        w.append("after-seal", 100);
        w.tick(t0);

        SegmentReader r(stream, cursor);
        r.start();
        auto out = r.read_new();
        QCOMPARE(out.size(), size_t(9));
        QCOMPARE(out[8], std::string("after-seal"));
    }

    void commit_persists_cursor_across_reader_restarts() {
        QTemporaryDir tmp;
        fs::path stream = fs::path(tmp.path().toStdString()) / "stream";
        fs::path cursor = fs::path(tmp.path().toStdString()) / "cur.bin";
        SegmentWriter w(stream, SegmentKind::Ops, fast_test_config());
        w.start();
        auto t0 = std::chrono::steady_clock::time_point{};
        w.append("first", 1);
        w.tick(t0);

        {
            SegmentReader r(stream, cursor);
            r.start();
            auto out = r.read_new();
            QCOMPARE(out.size(), size_t(1));
            r.commit();
        }
        {
            SegmentReader r2(stream, cursor);
            r2.start();
            auto out2 = r2.read_new();
            QVERIFY(out2.empty());
        }
    }

    void picks_up_after_seal_boundary() {
        QTemporaryDir tmp;
        fs::path stream = fs::path(tmp.path().toStdString()) / "stream";
        fs::path cursor = fs::path(tmp.path().toStdString()) / "cur.bin";
        SegmentWriter w(stream, SegmentKind::Ops, fast_test_config());
        w.start();
        auto t0 = std::chrono::steady_clock::time_point{};
        w.append("one", 1);
        w.tick(t0);

        SegmentReader r(stream, cursor);
        r.start();
        auto a = r.read_new();
        QCOMPARE(a.size(), size_t(1));
        r.commit();

        // Fill the open segment past seal threshold so it gets sealed.
        for (int i = 0; i < 8; ++i) w.append("0123456789", uint64_t(i + 2));
        w.tick(t0);
        // The previously-open segment id is now sealed; reader's
        // open_segment_id is stale. The reader should pick up the
        // remaining records via the new sealed segment.
        auto b = r.read_new();
        QVERIFY(b.size() >= 8);
    }

    void partial_trailing_line_in_open_is_left() {
        QTemporaryDir tmp;
        fs::path stream = fs::path(tmp.path().toStdString()) / "stream";
        fs::create_directories(stream);
        fs::path open1 = stream / "0000000001.open";
        std::ofstream(open1, std::ios::binary)
            << base64_encode("complete") << "\n"
            << base64_encode("partial");

        fs::path cursor = fs::path(tmp.path().toStdString()) / "cur.bin";
        SegmentReader r(stream, cursor);
        r.start();
        auto out = r.read_new();
        QCOMPARE(out.size(), size_t(1));
        QCOMPARE(out[0], std::string("complete"));
    }

    void corrupt_sealed_segment_does_not_advance_cursor() {
        QTemporaryDir tmp;
        fs::path stream = fs::path(tmp.path().toStdString()) / "stream";
        fs::create_directories(stream);
        fs::path bad = stream / "0000000001.seg.zst";
        std::ofstream(bad, std::ios::binary) << "not a zstd frame";
        fs::path cursor = fs::path(tmp.path().toStdString()) / "cur.bin";
        SegmentReader r(stream, cursor);
        r.start();
        auto out = r.read_new();
        QVERIFY(out.empty());
        r.commit();
    }
};

QTEST_APPLESS_MAIN(TestSegmentReader)
#include "tst_segment_reader.moc"
