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
    void lowest_peer_acked_lamport_returns_min_across_peers();
    void lowest_peer_acked_lamport_callback_fires_on_advance();
    void lowest_peer_acked_lamport_bounded_by_lagging_peer();
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

void TestOpStreamAcks::lowest_peer_acked_lamport_returns_min_across_peers() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    std::filesystem::path shared = tmp.path().toStdString();

    StreamSync R1(shared, "replica-1", 1);
    StreamSync R2(shared, "replica-2", 2);
    StreamSync R3(shared, "replica-3", 3);

    R1.register_stream("ops", StreamSync::StreamType::AppendOnly);
    R2.register_stream("ops", StreamSync::StreamType::AppendOnly);
    R3.register_stream("ops", StreamSync::StreamType::AppendOnly);
    R1.start();
    R2.start();
    R3.start();

    // Collect encoded op payloads from engine1 and track max Lamport
    CrdtEngine engine1(1);
    std::vector<std::string> payloads;
    uint64_t max_lamport = 0;
    engine1.setOnLocalOp([&](const Operation& op) {
        payloads.push_back(encode_operation(op));
        uint64_t lc = op_lamport(op).counter();
        if (lc > max_lamport) max_lamport = lc;
    });

    for (int i = 0; i < 4; ++i) {
        engine1.insert(i, "a");
    }
    QVERIFY(!payloads.empty());
    QVERIFY(max_lamport > 0);

    for (const auto& payload : payloads) {
        R1.push("ops", payload);
    }
    R1.flush();

    // R2 and R3 observe R1's ops and write acks.json with entry for "1"
    R2.poll();
    R3.poll();

    // R1 scans R2 and R3's acks.json and computes fence
    R1.poll();

    uint64_t fence = R1.lowest_peer_acked_lamport();
    QVERIFY2(fence > 0,
             qPrintable(QString("Expected fence > 0, got %1").arg(fence)));
    // Both R2 and R3 observed all ops from R1, so the fence equals max_lamport
    QCOMPARE(fence, max_lamport);
}

void TestOpStreamAcks::lowest_peer_acked_lamport_callback_fires_on_advance() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    std::filesystem::path shared = tmp.path().toStdString();

    StreamSync R1(shared, "replica-1", 1);
    StreamSync R2(shared, "replica-2", 2);

    R1.register_stream("ops", StreamSync::StreamType::AppendOnly);
    R2.register_stream("ops", StreamSync::StreamType::AppendOnly);
    R1.start();
    R2.start();

    std::vector<uint64_t> cb_values;
    R1.set_on_ack_update([&](uint64_t v) { cb_values.push_back(v); });

    CrdtEngine engine1(1);
    std::vector<std::string> payloads;
    uint64_t max_lamport_1 = 0;
    engine1.setOnLocalOp([&](const Operation& op) {
        payloads.push_back(encode_operation(op));
        uint64_t lc = op_lamport(op).counter();
        if (lc > max_lamport_1) max_lamport_1 = lc;
    });

    // First batch of inserts
    for (int i = 0; i < 3; ++i) {
        engine1.insert(i, "b");
    }
    QVERIFY(!payloads.empty());

    for (const auto& payload : payloads) {
        R1.push("ops", payload);
    }
    R1.flush();
    R2.poll();
    R1.poll();  // R1 reads R2's acks, fence advances → callback fires

    QVERIFY2(!cb_values.empty(),
             "Expected ack_update callback to fire after first batch");
    QVERIFY(cb_values.back() > 0);
    uint64_t first_fence = cb_values.back();

    // Second batch of inserts
    payloads.clear();
    uint64_t max_lamport_2 = 0;
    engine1.setOnLocalOp([&](const Operation& op) {
        payloads.push_back(encode_operation(op));
        uint64_t lc = op_lamport(op).counter();
        if (lc > max_lamport_2) max_lamport_2 = lc;
    });

    for (int i = 3; i < 6; ++i) {
        engine1.insert(i, "c");
    }
    QVERIFY(!payloads.empty());

    for (const auto& payload : payloads) {
        R1.push("ops", payload);
    }
    R1.flush();
    R2.poll();   // R2 observes second batch, updates acks.json
    R1.poll();   // R1 reads R2's updated acks, fence advances again → callback fires again

    size_t cb_count_before = cb_values.size();
    QVERIFY2(cb_count_before >= 2,
             qPrintable(QString("Expected at least 2 callback invocations, got %1").arg((int)cb_count_before)));
    QVERIFY2(cb_values.back() > first_fence,
             qPrintable(QString("Expected second fence %1 > first fence %2")
                 .arg(cb_values.back()).arg(first_fence)));
}

void TestOpStreamAcks::lowest_peer_acked_lamport_bounded_by_lagging_peer() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    std::filesystem::path shared = tmp.path().toStdString();

    StreamSync R1(shared, "replica-1", 1);
    StreamSync R2(shared, "replica-2", 2);
    StreamSync R3(shared, "replica-3", 3);

    R1.register_stream("ops", StreamSync::StreamType::AppendOnly);
    R2.register_stream("ops", StreamSync::StreamType::AppendOnly);
    R3.register_stream("ops", StreamSync::StreamType::AppendOnly);
    R1.start();
    R2.start();
    R3.start();

    CrdtEngine engine1(1);
    std::vector<std::string> batch1_payloads;
    uint64_t batch1_max = 0;

    engine1.setOnLocalOp([&](const Operation& op) {
        batch1_payloads.push_back(encode_operation(op));
        uint64_t lc = op_lamport(op).counter();
        if (lc > batch1_max) batch1_max = lc;
    });

    // Batch 1: 3 inserts
    for (int i = 0; i < 3; ++i) {
        engine1.insert(i, "x");
    }
    QVERIFY(!batch1_payloads.empty());
    QVERIFY(batch1_max > 0);

    for (const auto& payload : batch1_payloads) {
        R1.push("ops", payload);
    }
    R1.flush();

    // R3 polls — sees only batch 1; writes acks.json with batch1_max
    R3.poll();

    // Batch 2: 3 more inserts
    std::vector<std::string> batch2_payloads;
    uint64_t batch2_max = 0;

    engine1.setOnLocalOp([&](const Operation& op) {
        batch2_payloads.push_back(encode_operation(op));
        uint64_t lc = op_lamport(op).counter();
        if (lc > batch2_max) batch2_max = lc;
    });

    for (int i = 3; i < 6; ++i) {
        engine1.insert(i, "y");
    }
    QVERIFY(!batch2_payloads.empty());
    QVERIFY(batch2_max > batch1_max);

    for (const auto& payload : batch2_payloads) {
        R1.push("ops", payload);
    }
    R1.flush();

    // R2 polls — sees both batches; writes acks.json with batch2_max
    R2.poll();

    // R1 polls — reads R2 and R3 acks, computes min across peers
    R1.poll();

    uint64_t fence = R1.lowest_peer_acked_lamport();

    // fence must be positive: R3 did observe batch 1
    QVERIFY2(fence > 0,
             qPrintable(QString("Expected fence > 0, got %1").arg(fence)));

    // fence must equal batch1_max: R3 is the lagging peer and caps the min
    QVERIFY2(fence == batch1_max,
             qPrintable(QString("Expected fence == batch1_max (%1), got %2")
                 .arg(batch1_max).arg(fence)));

    // fence must be strictly less than batch2_max: R3 hasn't caught up
    QVERIFY2(fence < batch2_max,
             qPrintable(QString("Expected fence < batch2_max (%1), got %2")
                 .arg(batch2_max).arg(fence)));
}

QTEST_MAIN(TestOpStreamAcks)
#include "tst_opstream_acks.moc"
