#include <QTest>
#include <QTemporaryDir>

#include <collabtext/OpStream.h>

#include "crdt/SegmentReader.h"
#include "transport/StreamSync.h"
#include "crdt/SegmentWriter.h"
#include "crdt/SegmentFormat.h"
#include "crdt/ZstdUtil.h"

#include <fstream>
#include <algorithm>
#include <array>

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

void write_sealed(const fs::path& path, std::string record) {
    SegmentHeader header;
    const auto bytes = encode_sealed_segment(header, {record}, 1);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
}

class TestSegmentReader : public QObject {
    Q_OBJECT
private slots:
    void cursor_decoder_requires_exact_wire_size() {
        std::string bytes("CTCR\x01\0\0\0", 8);
        bytes.resize(66, '\0');
        QVERIFY(decode_segment_cursor(bytes));
        QVERIFY(!decode_segment_cursor(std::string_view(bytes).substr(0, 65)));
        bytes.push_back('\0');
        QVERIFY(!decode_segment_cursor(bytes));
    }

    void ack_decoder_rejects_overflow_and_bounds_input() {
        const auto valid = decode_legacy_peer_ack(
            R"({"acks":{"1":{"max_lamport_observed":42}}})", 1);
        QVERIFY(valid.has_value());
        QCOMPARE(*valid, uint64_t(42));
        QVERIFY(!decode_legacy_peer_ack(
            R"({"acks":{"1":{"max_lamport_observed":18446744073709551616}}})", 1));
        QVERIFY(!decode_legacy_peer_ack(std::string(1024 * 1024 + 1, 'x'), 1));
    }

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
        QVERIFY(w.start().ok());
        auto t0 = std::chrono::steady_clock::time_point{};
        QVERIFY(w.append("alpha", 1).ok());
        QVERIFY(w.append("beta",  2).ok());
        QVERIFY(w.tick(t0).ok());

        SegmentReader r(stream, cursor);
        r.start();
        auto out = r.read_new();
        QCOMPARE(out.size(), size_t(2));
        QCOMPARE(out[0], std::string("alpha"));
        QCOMPARE(out[1], std::string("beta"));
        QVERIFY(r.commit(out.token).ok());
    }

    void reads_sealed_then_open_in_order() {
        QTemporaryDir tmp;
        fs::path stream = fs::path(tmp.path().toStdString()) / "stream";
        fs::path cursor = fs::path(tmp.path().toStdString()) / "cur.bin";
        SegmentWriter w(stream, SegmentKind::Ops, fast_test_config());
        QVERIFY(w.start().ok());
        auto t0 = std::chrono::steady_clock::time_point{};
        for (int i = 0; i < 8; ++i) QVERIFY(w.append("0123456789", uint64_t(i + 1)).ok());
        QVERIFY(w.tick(t0).ok());
        QVERIFY(w.append("after-seal", 100).ok());
        QVERIFY(w.tick(t0).ok());

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
        QVERIFY(w.start().ok());
        auto t0 = std::chrono::steady_clock::time_point{};
        QVERIFY(w.append("first", 1).ok());
        QVERIFY(w.tick(t0).ok());

        {
            SegmentReader r(stream, cursor);
            r.start();
            auto out = r.read_new();
            QCOMPARE(out.size(), size_t(1));
            const auto checkpoint = CollabText::RecoveryPoint::checkpoint("application-state-1");
            QVERIFY(r.commit_durable(out.token,
                checkpoint).ok());
        }
        {
            SegmentReader r2(stream, cursor);
            const auto checkpoint = CollabText::RecoveryPoint::checkpoint("application-state-1");
            r2.start(&checkpoint);
            auto out2 = r2.read_new();
            QVERIFY(out2.empty());
        }
    }

    void picks_up_after_seal_boundary() {
        QTemporaryDir tmp;
        fs::path stream = fs::path(tmp.path().toStdString()) / "stream";
        fs::path cursor = fs::path(tmp.path().toStdString()) / "cur.bin";
        SegmentWriter w(stream, SegmentKind::Ops, fast_test_config());
        QVERIFY(w.start().ok());
        auto t0 = std::chrono::steady_clock::time_point{};
        QVERIFY(w.append("one", 1).ok());
        QVERIFY(w.tick(t0).ok());

        SegmentReader r(stream, cursor);
        r.start();
        auto a = r.read_new();
        QCOMPARE(a.size(), size_t(1));
        QVERIFY(r.commit(a.token).ok());

        // Fill the open segment past seal threshold so it gets sealed.
        for (int i = 0; i < 8; ++i) QVERIFY(w.append("0123456789", uint64_t(i + 2)).ok());
        QVERIFY(w.tick(t0).ok());
        // The previously-open segment id is now sealed; reader's
        // open_segment_id is stale. The reader should pick up the
        // remaining records via the new sealed segment.
        auto b = r.read_new();
        QCOMPARE(b.size(), size_t(8));
        QVERIFY(r.commit(b.token).ok());
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
        QVERIFY(out.gap.has_value());
        QVERIFY(r.commit(out.token).ok());
    }

    void three_segment_arrival_permutations_never_skip_the_frontier() {
        std::array<int, 3> order{1, 2, 3};
        do {
            QTemporaryDir tmp;
            const fs::path root(tmp.path().toStdString());
            const fs::path stream = root / "stream";
            fs::create_directories(stream);
            SegmentReader reader(stream, root / "cursor");
            reader.start();
            std::vector<std::string> observed;
            for (const int id : order) {
                const auto name = (id < 10 ? "000000000" : "00000000")
                    + std::to_string(id) + ".seg.zst";
                write_sealed(stream / name, "record-" + std::to_string(id));
                auto batch = reader.read_new();
                observed.insert(observed.end(), batch.records.begin(), batch.records.end());
                QVERIFY(reader.commit(batch.token).ok());
            }
            QCOMPARE(observed, (std::vector<std::string>{"record-1", "record-2", "record-3"}));
        } while (std::next_permutation(order.begin(), order.end()));
    }

    void repaired_invalid_predecessor_releases_later_segment() {
        QTemporaryDir tmp;
        const fs::path root(tmp.path().toStdString());
        const fs::path stream = root / "stream";
        fs::create_directories(stream);
        write_sealed(stream / "0000000001.seg.zst", "one");
        write_sealed(stream / "0000000002.seg.zst", "two");
        std::ofstream(stream / "0000000001.seg.zst", std::ios::binary | std::ios::trunc)
            << "corrupt";

        SegmentReader reader(stream, root / "cursor");
        reader.start();
        auto blocked = reader.read_new();
        QVERIFY(blocked.records.empty());
        QVERIFY(blocked.gap.has_value());
        QVERIFY(reader.commit(blocked.token).ok());

        write_sealed(stream / "0000000001.seg.zst", "one");
        auto repaired = reader.read_new();
        QCOMPARE(repaired.records, (std::vector<std::string>{"one", "two"}));
    }

    void uncommitted_batch_replays_and_stale_token_cannot_commit() {
        QTemporaryDir tmp;
        const fs::path root(tmp.path().toStdString());
        const fs::path stream = root / "stream";
        fs::create_directories(stream);
        write_sealed(stream / "0000000001.seg.zst", "one");
        SegmentReader reader(stream, root / "cursor");
        reader.start();
        auto first = reader.read_new();
        auto second = reader.read_new();
        QVERIFY(!reader.commit(first.token).ok());
        QCOMPARE(second.records, (std::vector<std::string>{"one"}));
        SegmentReader other(stream, root / "other-cursor");
        other.start();
        auto foreign = other.read_new();
        QVERIFY(!other.commit(second.token).ok());
        QVERIFY(other.commit(foreign.token).ok());
        const auto checkpoint = CollabText::RecoveryPoint::checkpoint("application-state-1");
        QVERIFY(reader.commit_durable(second.token, checkpoint).ok());

        SegmentReader restarted(stream, root / "cursor");
        restarted.start(&checkpoint);
        QVERIFY(restarted.read_new().records.empty());
        SegmentReader mismatched(stream, root / "cursor");
        const auto older_checkpoint = CollabText::RecoveryPoint::checkpoint("older-state");
        mismatched.start(&older_checkpoint);
        QCOMPARE(mismatched.read_new().records, (std::vector<std::string>{"one"}));
    }

    void in_memory_acceptance_does_not_persist_without_a_recovery_point() {
        QTemporaryDir tmp;
        const fs::path root(tmp.path().toStdString());
        const fs::path stream = root / "stream";
        fs::create_directories(stream);
        write_sealed(stream / "0000000001.seg.zst", "one");
        {
            SegmentReader reader(stream, root / "cursor");
            reader.start();
            auto batch = reader.read_new();
            QVERIFY(reader.commit(batch.token).ok());
        }
        SegmentReader restarted(stream, root / "cursor");
        restarted.start();
        QCOMPARE(restarted.read_new().records, (std::vector<std::string>{"one"}));
    }

    void replaced_open_prefix_replays_instead_of_skipping_repaired_bytes() {
        QTemporaryDir tmp;
        const fs::path root(tmp.path().toStdString());
        const fs::path stream = root / "stream";
        fs::create_directories(stream);
        const std::string first = base64_encode("same0001") + "\n";
        const std::string replacement = base64_encode("same0002") + "\n";
        QCOMPARE(first.size(), replacement.size());
        std::ofstream(stream / "0000000001.open", std::ios::binary) << first;
        SegmentReader reader(stream, root / "cursor");
        reader.start();
        auto initial = reader.read_new();
        QCOMPARE(initial.records, (std::vector<std::string>{"same0001"}));
        QVERIFY(reader.commit(initial.token).ok());

        std::ofstream(stream / "0000000001.open", std::ios::binary | std::ios::trunc)
            << replacement;
        auto repaired = reader.read_new();
        QCOMPARE(repaired.records, (std::vector<std::string>{"same0002"}));
    }

    void large_sealed_segment_resumes_across_bounded_batches() {
        QTemporaryDir tmp;
        const fs::path root(tmp.path().toStdString());
        const fs::path stream = root / "stream";
        fs::create_directories(stream);
        std::vector<std::string> source;
        source.reserve(4097);
        for (size_t i = 0; i < 4097; ++i) source.push_back("record-" + std::to_string(i));
        SegmentHeader header;
        const auto bytes = encode_sealed_segment(header, source, 1);
        std::ofstream file(stream / "0000000001.seg.zst", std::ios::binary);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        file.close();

        SegmentReader reader(stream, root / "cursor");
        reader.start();
        auto first = reader.read_new();
        QCOMPARE(first.size(), size_t(4096));
        QVERIFY(!first.gap.has_value());
        QVERIFY(reader.commit(first.token).ok());
        auto second = reader.read_new();
        QCOMPARE(second.records, (std::vector<std::string>{"record-4096"}));
    }
};

QTEST_APPLESS_MAIN(TestSegmentReader)
#include "tst_segment_reader.moc"
