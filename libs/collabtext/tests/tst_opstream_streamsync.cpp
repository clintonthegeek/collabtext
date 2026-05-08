#include <QTest>
#include <QTemporaryDir>
#include <collabtext/StreamSync.h>
#include <collabtext/OpStream.h>

using namespace CollabText::Crdt;

class TestOpStreamStreamSync : public QObject {
    Q_OBJECT
private slots:
    void opstream_push_received_via_inbound() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        std::filesystem::path shared = tmp.path().toStdString();

        StreamSync rawA(shared, "replica-A", /*replica_id=*/1);
        StreamSync rawB(shared, "replica-B", /*replica_id=*/2);

        CollabText::OpStream* a = &rawA;
        CollabText::OpStream* b = &rawB;

        rawA.register_stream("buf:doc", StreamSync::StreamType::AppendOnly);
        rawB.register_stream("buf:doc", StreamSync::StreamType::AppendOnly);
        rawA.start();
        rawB.start();

        std::string fired_stream;
        uint16_t fired_replica = 0;
        std::string fired_payload;
        bool fired = false;

        b->set_on_inbound([&](const std::string& sn, uint16_t rid, const std::string& pl) {
            fired_stream = sn;
            fired_replica = rid;
            fired_payload = pl;
            fired = true;
        });

        const std::string pushed_payload = R"({"op":"insert","pos":0,"text":"hello"})";
        a->push("buf:doc", pushed_payload);
        rawA.flush();
        rawB.poll();

        QVERIFY(fired);
        QCOMPARE(fired_stream, std::string("buf:doc"));
        QCOMPARE(fired_payload, pushed_payload);
        QCOMPARE(fired_replica, uint16_t(1));

        auto entries = rawB.entries("buf:doc");
        QCOMPARE(static_cast<int>(entries.size()), 1);
        QCOMPARE(entries[0].payload, pushed_payload);
    }

    void lowest_peer_acked_lamport_stub_returns_zero() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        std::filesystem::path shared = tmp.path().toStdString();

        StreamSync raw(shared, "replica-A", /*replica_id=*/1);
        CollabText::OpStream* stream = &raw;

        QCOMPARE(stream->lowest_peer_acked_lamport(), uint64_t(0));
    }

    void set_on_ack_update_stores_callback() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        std::filesystem::path shared = tmp.path().toStdString();
        StreamSync sync(shared, "replica-X", /*replica_id=*/1);
        bool called = false;
        CollabText::OpStream* op = &sync;
        op->set_on_ack_update([&](uint64_t) { called = true; });
        QVERIFY(!called);  // Phase 4 stub: never fires
    }
};

QTEST_MAIN(TestOpStreamStreamSync)
#include "tst_opstream_streamsync.moc"
