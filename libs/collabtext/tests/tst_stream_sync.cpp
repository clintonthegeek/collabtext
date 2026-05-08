#include <QTest>
#include <QTemporaryDir>
#include <collabtext/StreamSync.h>
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

    void append_only_round_trip() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        std::filesystem::path shared = tmp.path().toStdString();

        StreamSync syncA(shared, "replica-A");
        StreamSync syncB(shared, "replica-B");
        syncA.register_stream("chat", StreamSync::StreamType::AppendOnly);
        syncB.register_stream("chat", StreamSync::StreamType::AppendOnly);
        syncA.start();
        syncB.start();

        for (uint64_t i = 1; i <= 5; ++i) {
            StreamEntry e;
            e.id = "1-" + std::to_string(i);
            e.replica_id = 1;
            e.seq = i;
            e.timestamp = "2026-04-13T10:00:0" + std::to_string(i) + "Z";
            e.payload = "{\"body\":\"msg" + std::to_string(i) + "\"}";
            syncA.push("chat", e);
        }
        syncA.poll();
        syncA.flush();

        size_t applied = syncB.poll();
        QCOMPARE(applied, size_t(5));

        auto entries = syncB.entries("chat");
        QCOMPARE(entries.size(), size_t(5));

        for (size_t i = 0; i < entries.size(); ++i) {
            QCOMPARE(entries[i].seq, uint64_t(i + 1));
        }

        QVERIFY(entries[0].payload.find("msg1") != std::string::npos);
        QVERIFY(entries[4].payload.find("msg5") != std::string::npos);
    }

    void append_only_dedup() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        std::filesystem::path shared = tmp.path().toStdString();

        StreamSync syncA(shared, "replica-A");
        StreamSync syncB(shared, "replica-B");
        syncA.register_stream("chat", StreamSync::StreamType::AppendOnly);
        syncB.register_stream("chat", StreamSync::StreamType::AppendOnly);
        syncA.start();
        syncB.start();

        StreamEntry e;
        e.id = "1-1";
        e.replica_id = 1;
        e.seq = 1;
        e.timestamp = "2026-04-13T10:00:00Z";
        e.payload = "{\"body\":\"hello\"}";

        syncA.push("chat", e);
        syncA.push("chat", e);
        syncA.poll();
        syncA.flush();

        syncB.poll();
        auto entries = syncB.entries("chat");
        QCOMPARE(entries.size(), size_t(1));
    }

    void anchor_keyed_lww_merge() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        std::filesystem::path shared = tmp.path().toStdString();

        StreamSync syncA(shared, "replica-A");
        StreamSync syncB(shared, "replica-B");
        syncA.register_stream("comments", StreamSync::StreamType::AnchorKeyed);
        syncB.register_stream("comments", StreamSync::StreamType::AnchorKeyed);
        syncA.start();
        syncB.start();

        StreamEntry e1;
        e1.id = "comment-1";
        e1.replica_id = 1;
        e1.seq = 1;
        e1.timestamp = "2026-04-13T10:00:00Z";
        e1.payload = "{\"body\":\"first version\"}";
        syncA.push("comments", e1);
        syncA.poll();
        syncA.flush();

        StreamEntry e2;
        e2.id = "comment-1";
        e2.replica_id = 2;
        e2.seq = 1;
        e2.timestamp = "2026-04-13T11:00:00Z";
        e2.payload = "{\"body\":\"updated version\"}";
        syncB.push("comments", e2);
        syncB.poll();
        syncB.flush();

        syncA.poll();
        auto entries = syncA.entries("comments");
        QCOMPARE(entries.size(), size_t(1));
        QVERIFY(entries[0].payload.find("updated version") != std::string::npos);
    }

    void anchor_keyed_tombstone() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        std::filesystem::path shared = tmp.path().toStdString();

        StreamSync syncA(shared, "replica-A");
        StreamSync syncB(shared, "replica-B");
        syncA.register_stream("comments", StreamSync::StreamType::AnchorKeyed);
        syncB.register_stream("comments", StreamSync::StreamType::AnchorKeyed);
        syncA.start();
        syncB.start();

        StreamEntry e;
        e.id = "comment-1";
        e.replica_id = 1;
        e.seq = 1;
        e.timestamp = "2026-04-13T10:00:00Z";
        e.payload = "{\"body\":\"to be deleted\"}";
        syncA.push("comments", e);
        syncA.poll();
        syncA.flush();

        syncB.poll();
        QCOMPARE(syncB.entries("comments").size(), size_t(1));

        StreamEntry tomb;
        tomb.id = "comment-1";
        tomb.replica_id = 1;
        tomb.seq = 2;
        tomb.timestamp = "2026-04-13T12:00:00Z";
        tomb.tombstone = true;
        syncA.push("comments", tomb);
        syncA.poll();
        syncA.flush();

        syncB.poll();
        QCOMPARE(syncB.entries("comments").size(), size_t(0));
    }

    void per_entry_inbound_callback() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        std::filesystem::path shared = tmp.path().toStdString();

        StreamSync syncA(shared, "replica-A");
        StreamSync syncB(shared, "replica-B");
        syncA.register_stream("chat", StreamSync::StreamType::AppendOnly);
        syncB.register_stream("chat", StreamSync::StreamType::AppendOnly);
        syncA.start();
        syncB.start();

        StreamEntry e;
        e.id = "1-42";
        e.replica_id = 7;
        e.seq = 42;
        e.timestamp = "2026-05-08T10:00:00Z";
        e.payload = "{\"body\":\"hello inbound\"}";
        syncA.push("chat", e);
        syncA.poll();
        syncA.flush();

        std::vector<std::string> fired_streams;
        std::vector<uint16_t> fired_replicas;
        std::vector<std::string> fired_payloads;
        syncB.set_on_inbound([&](const std::string& sn, uint16_t rid, const std::string& pl) {
            fired_streams.push_back(sn);
            fired_replicas.push_back(rid);
            fired_payloads.push_back(pl);
        });

        syncB.poll();

        QCOMPARE(fired_streams.size(), size_t(1));
        QCOMPARE(fired_streams[0], std::string("chat"));
        QCOMPARE(fired_replicas[0], uint16_t(7));
        QCOMPARE(fired_payloads[0], std::string("{\"body\":\"hello inbound\"}"));
    }

    void multi_stream_isolation() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        std::filesystem::path shared = tmp.path().toStdString();

        StreamSync syncA(shared, "replica-A");
        StreamSync syncB(shared, "replica-B");
        syncA.register_stream("chat", StreamSync::StreamType::AppendOnly);
        syncA.register_stream("comments", StreamSync::StreamType::AnchorKeyed);
        syncB.register_stream("chat", StreamSync::StreamType::AppendOnly);
        syncB.register_stream("comments", StreamSync::StreamType::AnchorKeyed);
        syncA.start();
        syncB.start();

        StreamEntry chat_entry;
        chat_entry.id = "1-1";
        chat_entry.replica_id = 1;
        chat_entry.seq = 1;
        chat_entry.timestamp = "2026-04-13T10:00:00Z";
        chat_entry.payload = "{\"body\":\"chat msg\"}";
        syncA.push("chat", chat_entry);

        StreamEntry comment_entry;
        comment_entry.id = "comment-1";
        comment_entry.replica_id = 1;
        comment_entry.seq = 2;
        comment_entry.timestamp = "2026-04-13T10:00:01Z";
        comment_entry.payload = "{\"body\":\"comment text\"}";
        syncA.push("comments", comment_entry);

        syncA.poll();
        syncA.flush();
        syncB.poll();

        auto chat_entries = syncB.entries("chat");
        auto comment_entries = syncB.entries("comments");
        QCOMPARE(chat_entries.size(), size_t(1));
        QCOMPARE(comment_entries.size(), size_t(1));
        QVERIFY(chat_entries[0].payload.find("chat msg") != std::string::npos);
        QVERIFY(comment_entries[0].payload.find("comment text") != std::string::npos);
    }
};

QTEST_MAIN(TestStreamSync)
#include "tst_stream_sync.moc"
