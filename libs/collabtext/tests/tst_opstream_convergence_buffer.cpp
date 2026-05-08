/// tst_opstream_convergence_buffer.cpp — Two-replica Buffer convergence over OpStream
///
/// Proves that two CrdtEngines wired through StreamSync (via the OpStream interface)
/// converge correctly:
///   - Sequential edits: engine1 inserts, engine2 receives and appends, both converge.
///   - Divergent concurrent edits: both engines edit before syncing, then exchange ops
///     and converge to the same text with all characters preserved.
///   - Multi-op sequential: several ops from engine1 all arrive at engine2 correctly.

#include <QTest>
#include <QTemporaryDir>
#include <collabtext/CrdtEngine.h>
#include <collabtext/Operations.h>
#include <collabtext/Serialization.h>
#include <collabtext/StreamSync.h>
#include <collabtext/OpStream.h>

#include <filesystem>
#include <string>

using namespace CollabText;
using namespace CollabText::Crdt;

class TestOpStreamConvergenceBuffer : public QObject {
    Q_OBJECT

private slots:

    // ── two replicas: sequential edit-then-append converges ──────────────────
    //
    // engine1 inserts "hello " first.  sync2 polls and engine2 applies those ops.
    // engine2 then appends "world" at position 6.  sync1 polls and engine1 applies
    // engine2's op.  Both replicas must converge to the same text containing both
    // "hello" and "world".

    void two_replica_buffer_convergence_over_opstream() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        std::filesystem::path shared = tmp.path().toStdString();

        StreamSync sync1(shared, "replica-A", /*replica_id=*/1);
        StreamSync sync2(shared, "replica-B", /*replica_id=*/2);

        sync1.register_stream("buf:doc", StreamSync::StreamType::AppendOnly);
        sync2.register_stream("buf:doc", StreamSync::StreamType::AppendOnly);
        sync1.start();
        sync2.start();

        OpStream* op1 = &sync1;
        OpStream* op2 = &sync2;

        CrdtEngine engine1(1);
        CrdtEngine engine2(2);

        // Wire engine1 → op1 (outbound)
        engine1.setOnLocalOp([&](const Operation& op) {
            op1->push("buf:doc", encode_operation(op));
        });

        // Wire engine2 → op2 (outbound)
        engine2.setOnLocalOp([&](const Operation& op) {
            op2->push("buf:doc", encode_operation(op));
        });

        // Wire op1 inbound → engine1 (skip own-replica ops)
        op1->set_on_inbound([&](const std::string& /*stream*/, uint16_t replica_id,
                                const std::string& payload) {
            if (replica_id == 1) return;  // don't apply own ops
            auto decoded = decode_operation(payload);
            QVERIFY(decoded.has_value());
            engine1.applyRemoteOp(*decoded);
        });

        // Wire op2 inbound → engine2 (skip own-replica ops)
        op2->set_on_inbound([&](const std::string& /*stream*/, uint16_t replica_id,
                                const std::string& payload) {
            if (replica_id == 2) return;  // don't apply own ops
            auto decoded = decode_operation(payload);
            QVERIFY(decoded.has_value());
            engine2.applyRemoteOp(*decoded);
        });

        // Step 1: engine1 inserts "hello " at position 0
        engine1.insert(0, "hello ");
        QCOMPARE(engine1.text(), std::string("hello "));

        // Step 2: sync1 flushes; sync2 polls — engine2 receives engine1's ops
        sync1.flush();
        sync2.poll();
        QCOMPARE(engine2.text(), std::string("hello "));

        // Step 3: engine2 appends "world" at position 6 (after "hello ")
        engine2.insert(6, "world");
        QCOMPARE(engine2.text(), std::string("hello world"));

        // Step 4: sync2 flushes; sync1 polls — engine1 receives engine2's op
        sync2.flush();
        sync1.poll();

        // Both engines must converge to the same text
        QCOMPARE(engine1.text(), engine2.text());
        QVERIFY(engine1.text().find("hello") != std::string::npos);
        QVERIFY(engine1.text().find("world") != std::string::npos);
    }

    // ── divergent concurrent edits converge ───────────────────────────────────
    //
    // Both engines insert concurrently at position 0 before any sync.  After a
    // two-pass exchange (1→2, then 2→1), both replicas must hold identical text
    // with all 6 characters preserved.  Exact character order is CRDT-determined
    // and not asserted here.

    void divergent_edits_converge_over_opstream() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        std::filesystem::path shared = tmp.path().toStdString();

        StreamSync sync1(shared, "replica-A", /*replica_id=*/1);
        StreamSync sync2(shared, "replica-B", /*replica_id=*/2);

        sync1.register_stream("buf:doc", StreamSync::StreamType::AppendOnly);
        sync2.register_stream("buf:doc", StreamSync::StreamType::AppendOnly);
        sync1.start();
        sync2.start();

        OpStream* op1 = &sync1;
        OpStream* op2 = &sync2;

        CrdtEngine engine1(1);
        CrdtEngine engine2(2);

        engine1.setOnLocalOp([&](const Operation& op) {
            op1->push("buf:doc", encode_operation(op));
        });

        engine2.setOnLocalOp([&](const Operation& op) {
            op2->push("buf:doc", encode_operation(op));
        });

        op1->set_on_inbound([&](const std::string& /*stream*/, uint16_t replica_id,
                                const std::string& payload) {
            if (replica_id == 1) return;
            auto decoded = decode_operation(payload);
            QVERIFY(decoded.has_value());
            engine1.applyRemoteOp(*decoded);
        });

        op2->set_on_inbound([&](const std::string& /*stream*/, uint16_t replica_id,
                                const std::string& payload) {
            if (replica_id == 2) return;
            auto decoded = decode_operation(payload);
            QVERIFY(decoded.has_value());
            engine2.applyRemoteOp(*decoded);
        });

        // Concurrent local inserts — neither engine has seen the other's ops yet
        engine1.insert(0, "ABC");
        engine2.insert(0, "XYZ");

        // First pass: sync1 → sync2 (engine2 receives engine1's ops)
        sync1.flush();
        sync2.poll();

        // Second pass: sync2 → sync1 (engine1 receives engine2's ops)
        sync2.flush();
        sync1.poll();

        // Both replicas must converge
        QCOMPARE(engine1.text(), engine2.text());
        // All 6 characters must be preserved (CRDT property)
        QCOMPARE(static_cast<int>(engine1.text().size()), 6);
    }

    // ── multiple sequential ops from engine1 all arrive at engine2 ───────────
    //
    // engine1 performs three edits in sequence.  A single flush/poll cycle
    // delivers all ops to engine2.  Both must converge to "helloworld".

    void multi_op_sequential_convergence() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        std::filesystem::path shared = tmp.path().toStdString();

        StreamSync sync1(shared, "replica-A", /*replica_id=*/1);
        StreamSync sync2(shared, "replica-B", /*replica_id=*/2);

        sync1.register_stream("buf:doc", StreamSync::StreamType::AppendOnly);
        sync2.register_stream("buf:doc", StreamSync::StreamType::AppendOnly);
        sync1.start();
        sync2.start();

        OpStream* op1 = &sync1;

        CrdtEngine engine1(1);
        CrdtEngine engine2(2);

        // One-directional wiring: engine1 → op1 → engine2 only
        engine1.setOnLocalOp([&](const Operation& op) {
            op1->push("buf:doc", encode_operation(op));
        });

        sync2.set_on_inbound([&](const std::string& /*stream*/, uint16_t /*replica_id*/,
                                 const std::string& payload) {
            auto decoded = decode_operation(payload);
            QVERIFY(decoded.has_value());
            engine2.applyRemoteOp(*decoded);
        });

        // Three sequential edits on engine1
        engine1.insert(0, "hello");
        engine1.insert(5, " world");
        engine1.remove(5, 1);  // remove the space

        QCOMPARE(engine1.text(), std::string("helloworld"));

        // Deliver all ops in one flush/poll
        sync1.flush();
        sync2.poll();

        QCOMPARE(engine2.text(), std::string("helloworld"));
        QCOMPARE(engine1.text(), engine2.text());
    }
};

QTEST_MAIN(TestOpStreamConvergenceBuffer)
#include "tst_opstream_convergence_buffer.moc"
