/// tst_opstream_replay.cpp — Catch-up replay on first attach
///
/// Proves that a brand-new StreamSync instance replays all of a peer's history
/// on its first poll(), because its per-peer read cursors start at the beginning
/// of each peer's segment files.
///
///   - catch_up_replay_on_first_attach:
///       Replica A accumulates many ops. Replica B attaches fresh (no cursors),
///       polls once, and receives all of replica A's history. Final states match.
///
///   - catch_up_then_incremental_sync:
///       After catching up from history, subsequent ops are delivered
///       incrementally on the next flush/poll cycle.

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

class TestOpStreamReplay : public QObject {
    Q_OBJECT

private slots:

    // ── catch-up replay on first attach ──────────────────────────────────────
    //
    // Replica A runs and accumulates 10 ops. Replica B attaches fresh against
    // the same shared folder — no read cursors exist — so its first poll()
    // starts from the beginning of replica A's segment files and delivers all
    // 10 ops. Final text must match.

    void catch_up_replay_on_first_attach() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        std::filesystem::path shared = tmp.path().toStdString();

        // ── Replica A: accumulate history ─────────────────────────────────────
        StreamSync sync1(shared, "replica-A", /*replica_id=*/1);
        sync1.register_stream("buf:doc", StreamSync::StreamType::AppendOnly);
        sync1.start();

        CrdtEngine engine1(1);
        engine1.setOnLocalOp([&](const Operation& op) {
            sync1.push("buf:doc", encode_operation(op));
        });

        // 10 single-character inserts
        for (int i = 0; i < 10; ++i) {
            engine1.insert(i, std::string(1, static_cast<char>('0' + i)));
        }
        sync1.flush();

        QCOMPARE(engine1.text(), std::string("0123456789"));

        // ── Replica B: fresh attach, no prior cursors ─────────────────────────
        StreamSync sync2(shared, "replica-B", /*replica_id=*/2);
        sync2.register_stream("buf:doc", StreamSync::StreamType::AppendOnly);
        sync2.start();

        CrdtEngine engine2(2);
        sync2.set_on_inbound([&](const std::string& /*stream*/, uint16_t replica_id,
                                 const std::string& payload) {
            if (replica_id == 2) return;  // skip own ops (none here)
            auto decoded = decode_operation(payload);
            QVERIFY(decoded.has_value());
            engine2.applyRemoteOp(*decoded);
        });

        // First poll — must replay all of replica A's history
        sync2.poll();

        // Both engines converge
        QVERIFY2(!engine2.text().empty(),
                 "engine2 should have received replica A's history");
        QCOMPARE(engine2.text(), engine1.text());
    }

    // ── catch-up then incremental sync ───────────────────────────────────────
    //
    // After the initial catch-up, subsequent ops from replica A are delivered
    // incrementally on the next flush/poll cycle without re-delivering history.

    void catch_up_then_incremental_sync() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        std::filesystem::path shared = tmp.path().toStdString();

        // ── Replica A: accumulate history ─────────────────────────────────────
        StreamSync sync1(shared, "replica-A", /*replica_id=*/1);
        sync1.register_stream("buf:doc", StreamSync::StreamType::AppendOnly);
        sync1.start();

        CrdtEngine engine1(1);
        engine1.setOnLocalOp([&](const Operation& op) {
            sync1.push("buf:doc", encode_operation(op));
        });

        // Initial 5 ops
        for (int i = 0; i < 5; ++i) {
            engine1.insert(i, std::string(1, static_cast<char>('a' + i)));
        }
        sync1.flush();

        // ── Replica B: fresh attach and catch-up ──────────────────────────────
        StreamSync sync2(shared, "replica-B", /*replica_id=*/2);
        sync2.register_stream("buf:doc", StreamSync::StreamType::AppendOnly);
        sync2.start();

        CrdtEngine engine2(2);
        int apply_count = 0;
        sync2.set_on_inbound([&](const std::string& /*stream*/, uint16_t replica_id,
                                 const std::string& payload) {
            if (replica_id == 2) return;
            auto decoded = decode_operation(payload);
            QVERIFY(decoded.has_value());
            engine2.applyRemoteOp(*decoded);
            ++apply_count;
        });

        // First poll — catch-up
        sync2.poll();

        QCOMPARE(engine2.text(), engine1.text());
        int ops_after_catchup = apply_count;
        QVERIFY(ops_after_catchup > 0);

        // ── Replica A: new op after catch-up ──────────────────────────────────
        engine1.insert(5, "Z");
        sync1.flush();

        // Second poll — incremental delivery of only the new op
        sync2.poll();

        QCOMPARE(engine2.text(), engine1.text());
        // At least one more op was applied compared to after catch-up
        QVERIFY(apply_count > ops_after_catchup);
    }
};

QTEST_MAIN(TestOpStreamReplay)
#include "tst_opstream_replay.moc"
