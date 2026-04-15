#include <QTest>
#include <QTemporaryDir>
#include "crdt/StreamSync.h"
#include "crdt/StreamSerialization.h"

using namespace CollabText::Crdt;

class TestStreamSync : public QObject {
    Q_OBJECT
private slots:
    void stream_entry_serialization_round_trip() {
        StreamEntry e;
        e.id = "1-42";
        e.replica_id = 1;
        e.seq = 42;
        e.timestamp = "2026-04-13T10:00:00Z";
        e.payload = R"({"body":"hello world","author":"alice"})";
        e.tombstone = false;

        std::string json = encode_stream_entry(e);
        auto decoded = decode_stream_entry(json);
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->id, e.id);
        QCOMPARE(decoded->replica_id, e.replica_id);
        QCOMPARE(decoded->seq, e.seq);
        QCOMPARE(decoded->timestamp, e.timestamp);
        QCOMPARE(decoded->payload, e.payload);
        QCOMPARE(decoded->tombstone, e.tombstone);
    }

    void stream_entry_serialization_with_tombstone() {
        StreamEntry e;
        e.id = "comment-uuid-1";
        e.replica_id = 2;
        e.seq = 100;
        e.timestamp = "2026-04-13T11:00:00Z";
        e.payload = "";
        e.tombstone = true;

        std::string json = encode_stream_entry(e);
        auto decoded = decode_stream_entry(json);
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->id, e.id);
        QCOMPARE(decoded->tombstone, true);
    }

    void stream_entry_payload_with_special_chars() {
        StreamEntry e;
        e.id = "1-1";
        e.replica_id = 1;
        e.seq = 1;
        e.timestamp = "2026-04-13T10:00:00Z";
        e.payload = std::string("{\"body\":\"line1\\nline2\\ttab\\\"quoted\\\"\"}");

        std::string json = encode_stream_entry(e);
        auto decoded = decode_stream_entry(json);
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->payload, e.payload);
    }
};

QTEST_MAIN(TestStreamSync)
#include "tst_stream_sync.moc"
