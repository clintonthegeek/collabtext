#include <QTest>
#include "crdt/ZstdUtil.h"

using namespace CollabText::Crdt;

class TestZstdUtil : public QObject {
    Q_OBJECT
private slots:
    void zstd_round_trip() {
        std::string original = "the quick brown fox jumps over the lazy dog\n";
        for (int i = 0; i < 8; ++i) original += original;
        auto compressed = zstd_compress(original, 3);
        QVERIFY(!compressed.empty());
        QVERIFY(compressed.size() < original.size());
        auto decompressed = zstd_decompress(compressed);
        QVERIFY(decompressed.has_value());
        QCOMPARE(*decompressed, original);
    }

    void zstd_decompress_rejects_garbage() {
        std::string garbage = "not a zstd frame";
        auto out = zstd_decompress(garbage);
        QVERIFY(!out.has_value());
    }

    void base64_round_trip_no_newlines_in_output() {
        std::string original = "{\"x\":1,\"y\":\"hi\\nthere\"}";
        auto encoded = base64_encode(original);
        QVERIFY(encoded.find('\n') == std::string::npos);
        auto decoded = base64_decode(encoded);
        QVERIFY(decoded.has_value());
        QCOMPARE(*decoded, original);
    }

    void base64_round_trip_various_lengths() {
        for (size_t n = 0; n < 32; ++n) {
            std::string s(n, 'A' + char(n % 26));
            auto enc = base64_encode(s);
            auto dec = base64_decode(enc);
            QVERIFY(dec.has_value());
            QCOMPARE(*dec, s);
        }
    }

    void base64_decode_rejects_invalid() {
        QVERIFY(!base64_decode("!!!").has_value());          // length not multiple of 4
        QVERIFY(!base64_decode("AAAA!AAA").has_value());      // bad char
    }
};

QTEST_APPLESS_MAIN(TestZstdUtil)
#include "tst_zstd_util.moc"
