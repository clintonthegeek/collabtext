#include <QTest>
#include <QTemporaryDir>
#include <collabtext/StreamSync.h>
#include <collabtext/CrdtEngine.h>
#include <collabtext/Serialization.h>
#include <collabtext/Operations.h>
#include <fstream>
#include <filesystem>

using namespace CollabText::Crdt;
using namespace CollabText;

class TestOpStreamAcks : public QObject {
    Q_OBJECT
private slots:
    void acks_json_written_after_poll();
    void acks_json_monotone_across_restarts();
};

void TestOpStreamAcks::acks_json_written_after_poll() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    std::filesystem::path shared = tmp.path().toStdString();

    StreamSync sync1(shared, "replica-A", 1);
    StreamSync sync2(shared, "replica-B", 2);

    sync1.register_stream("ops", StreamSync::StreamType::AppendOnly);
    sync2.register_stream("ops", StreamSync::StreamType::AppendOnly);
    sync1.start();
    sync2.start();

    // Collect encoded op payloads from engine1 and track max Lamport
    CrdtEngine engine1(1);
    std::vector<std::string> payloads;
    uint64_t max_lamport = 0;
    engine1.setOnLocalOp([&](const Operation& op) {
        payloads.push_back(encode_operation(op));
        uint64_t lc = op_lamport(op).counter();
        if (lc > max_lamport) max_lamport = lc;
    });

    // Do 5 inserts
    for (int i = 0; i < 5; ++i) {
        engine1.insert(i, "x");
    }
    QVERIFY(payloads.size() >= 5);
    QVERIFY(max_lamport > 0);

    // Push all payloads through sync1
    for (const auto& payload : payloads) {
        sync1.push("ops", payload);
    }
    sync1.flush();
    sync2.poll();

    auto acks_path = shared / "replicas" / "replica-B" / "acks.json";
    QVERIFY(std::filesystem::exists(acks_path));

    std::ifstream f(acks_path);
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    // The observed max_lamport must match what the ops emitted
    std::string expected_tag = "\"max_lamport_observed\": " + std::to_string(max_lamport);
    QVERIFY2(content.find(expected_tag) != std::string::npos,
             qPrintable(QString::fromStdString("acks.json content: " + content
                                               + " expected: " + expected_tag)));
    QVERIFY2(content.find("\"last_observed_at\"") != std::string::npos,
             qPrintable(QString::fromStdString("acks.json content: " + content)));
    // Key "1" for replica_id 1
    QVERIFY2(content.find("\"1\"") != std::string::npos,
             qPrintable(QString::fromStdString("acks.json content: " + content)));
}

void TestOpStreamAcks::acks_json_monotone_across_restarts() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    std::filesystem::path shared = tmp.path().toStdString();

    uint64_t max_lamport_session1 = 0;

    // First session: push 3 inserts, sync2 polls
    {
        StreamSync sync1(shared, "replica-A", 1);
        StreamSync sync2(shared, "replica-B", 2);

        sync1.register_stream("ops", StreamSync::StreamType::AppendOnly);
        sync2.register_stream("ops", StreamSync::StreamType::AppendOnly);
        sync1.start();
        sync2.start();

        CrdtEngine engine1(1);
        std::vector<std::string> payloads;
        engine1.setOnLocalOp([&](const Operation& op) {
            payloads.push_back(encode_operation(op));
            uint64_t lc = op_lamport(op).counter();
            if (lc > max_lamport_session1) max_lamport_session1 = lc;
        });

        for (int i = 0; i < 3; ++i) {
            engine1.insert(i, "x");
        }
        QVERIFY(payloads.size() >= 3);
        QVERIFY(max_lamport_session1 > 0);

        for (const auto& payload : payloads) {
            sync1.push("ops", payload);
        }
        sync1.flush();
        sync2.poll();

        auto acks_path = shared / "replicas" / "replica-B" / "acks.json";
        QVERIFY(std::filesystem::exists(acks_path));

        std::ifstream f(acks_path);
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        std::string expected = "\"max_lamport_observed\": " + std::to_string(max_lamport_session1);
        QVERIFY2(content.find(expected) != std::string::npos,
                 qPrintable(QString::fromStdString("first session acks.json: " + content)));
    }

    // Second session: fresh sync1 + sync2 (simulates restart), push 2 inserts from a
    // fresh engine. The fresh engine starts at Lamport 1, so its max_lamport will be
    // less than max_lamport_session1 (which came from 3 inserts with internal ops).
    // The file must preserve max_lamport_session1 via the monotonicity read.
    {
        StreamSync sync1(shared, "replica-A", 1);
        StreamSync sync2(shared, "replica-B", 2);

        sync1.register_stream("ops", StreamSync::StreamType::AppendOnly);
        sync2.register_stream("ops", StreamSync::StreamType::AppendOnly);
        sync1.start();
        sync2.start();

        // Fresh engine — Lamport resets to 1; 2 inserts will produce max < max_lamport_session1
        CrdtEngine engine_new(1);
        uint64_t max_lamport_session2 = 0;
        std::vector<std::string> payloads;
        engine_new.setOnLocalOp([&](const Operation& op) {
            payloads.push_back(encode_operation(op));
            uint64_t lc = op_lamport(op).counter();
            if (lc > max_lamport_session2) max_lamport_session2 = lc;
        });

        for (int i = 0; i < 1; ++i) {
            engine_new.insert(i, "y");
        }
        QVERIFY(payloads.size() >= 1);
        // Verify the new session's max is less than the first session's max
        // (1 insert always produces fewer/lower Lamport ops than 3 inserts)
        QVERIFY(max_lamport_session2 < max_lamport_session1);

        for (const auto& payload : payloads) {
            sync1.push("ops", payload);
        }
        sync1.flush();
        sync2.poll();

        auto acks_path = shared / "replicas" / "replica-B" / "acks.json";
        std::ifstream f(acks_path);
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());

        // Monotonicity: file must preserve max_lamport_session1 (the larger value)
        std::string expected = "\"max_lamport_observed\": " + std::to_string(max_lamport_session1);
        QVERIFY2(content.find(expected) != std::string::npos,
                 qPrintable(QString::fromStdString(
                     "second session acks.json (expect max=" +
                     std::to_string(max_lamport_session1) + "): " + content)));
    }
}

QTEST_MAIN(TestOpStreamAcks)
#include "tst_opstream_acks.moc"
