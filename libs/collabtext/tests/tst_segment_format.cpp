#include <QTest>
#include "crdt/SegmentFormat.h"

using namespace CollabText::Crdt;

class TestSegmentFormat : public QObject {
    Q_OBJECT
private slots:
    void header_byte_layout_is_stable() {
        SegmentHeader h;
        h.format_version = 1;
        h.kind = SegmentKind::Ops;
        h.flags = 0;
        h.first_lamport = 7;
        h.last_lamport = 99;
        h.record_count = 12;
        h.sha256.fill(0xAB);
        std::string bytes = encode_segment_header(h);
        QCOMPARE(bytes.size(), size_t(60));
        QCOMPARE(bytes[0], 'C');
        QCOMPARE(bytes[1], 'T');
        QCOMPARE(bytes[2], 'S');
        QCOMPARE(bytes[3], 'G');
        QCOMPARE(uint8_t(bytes[4]), uint8_t(1));
        QCOMPARE(uint8_t(bytes[5]), uint8_t(SegmentKind::Ops));
    }

    void header_round_trip() {
        SegmentHeader h;
        h.format_version = 1;
        h.kind = SegmentKind::Stream;
        h.flags = 0;
        h.first_lamport = 1234567890ull;
        h.last_lamport = 9876543210ull;
        h.record_count = 42;
        for (size_t i = 0; i < h.sha256.size(); ++i) h.sha256[i] = uint8_t(i * 7);
        std::string bytes = encode_segment_header(h);
        auto parsed = decode_segment_header(bytes);
        QVERIFY(parsed.has_value());
        QCOMPARE(parsed->format_version, h.format_version);
        QCOMPARE(int(parsed->kind), int(h.kind));
        QCOMPARE(parsed->first_lamport, h.first_lamport);
        QCOMPARE(parsed->last_lamport, h.last_lamport);
        QCOMPARE(parsed->record_count, h.record_count);
        QVERIFY(parsed->sha256 == h.sha256);
    }

    void decode_rejects_bad_magic() {
        SegmentHeader h{};
        h.format_version = 1;
        std::string bytes = encode_segment_header(h);
        bytes[0] = 'X';
        QVERIFY(!decode_segment_header(bytes).has_value());
    }

    void decode_rejects_bad_version() {
        SegmentHeader h{};
        h.format_version = 99;
        std::string bytes = encode_segment_header(h);
        QVERIFY(!decode_segment_header(bytes).has_value());
    }

    void seal_round_trip_recovers_payload() {
        std::vector<std::string> records = {
            "alpha",
            "beta gamma",
            "{\"json\":\"like\"}",
        };
        SegmentHeader header_in;
        header_in.format_version = 1;
        header_in.kind = SegmentKind::Ops;
        header_in.flags = 0;
        header_in.first_lamport = 1;
        header_in.last_lamport = 3;
        std::string sealed = encode_sealed_segment(header_in, records);

        auto decoded = decode_sealed_segment(sealed);
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->records.size(), records.size());
        QCOMPARE(decoded->records, records);
        QCOMPARE(decoded->header.record_count, uint32_t(3));
        QCOMPARE(decoded->header.first_lamport, uint64_t(1));
        QCOMPARE(decoded->header.last_lamport, uint64_t(3));
    }

    void seal_round_trip_detects_payload_corruption() {
        // Use a long, repetitive payload so the zstd frame is comfortably
        // large and flipping a middle byte reliably breaks something.
        std::vector<std::string> records;
        for (int i = 0; i < 200; ++i)
            records.push_back("the quick brown fox jumps over the lazy dog");
        SegmentHeader header_in{};
        header_in.format_version = 1;
        header_in.kind = SegmentKind::Ops;
        std::string sealed = encode_sealed_segment(header_in, records);
        QVERIFY(sealed.size() > 64);
        // Flip a byte well inside the frame.
        sealed[sealed.size() / 2] ^= 0xFF;
        auto decoded = decode_sealed_segment(sealed);
        QVERIFY(!decoded.has_value());
    }

    void empty_records_seals_and_unseals() {
        SegmentHeader h{};
        h.format_version = 1;
        h.kind = SegmentKind::Stream;
        std::string sealed = encode_sealed_segment(h, {});
        auto decoded = decode_sealed_segment(sealed);
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->records.size(), size_t(0));
        QCOMPARE(decoded->header.record_count, uint32_t(0));
    }
};

QTEST_APPLESS_MAIN(TestSegmentFormat)
#include "tst_segment_format.moc"
