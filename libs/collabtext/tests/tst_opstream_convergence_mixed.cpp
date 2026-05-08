/// tst_opstream_convergence_mixed.cpp — Mixed Buffer + IdList convergence over OpStream
///
/// Models the "document with structure" scenario: one IdList for block ordering
/// and one CrdtEngine per block for text content.  Both stream types travel over
/// the same OpStream pair (StreamSync), and all four convergence properties are
/// verified:
///
///   - mixed_idlist_and_buffer_convergence_over_opstream:
///       Sequential structural + text ops — both streams arrive at replica 2,
///       which appends text, and the edit propagates back to replica 1.
///
///   - mixed_concurrent_structural_and_text_edits:
///       Both replicas emit structural (IdList) and text (Buffer) ops concurrently.
///       After a full two-way sync both replicas converge on ids and text.

#include <QTest>
#include <QTemporaryDir>
#include <collabtext/CrdtEngine.h>
#include <collabtext/Operations.h>
#include <collabtext/StreamSync.h>
#include <collabtext/OpStream.h>
#include <collabtext/IdListOperations.h>
#include <collabtext/Serialization.h>
#include "crdt/IdList.h"

#include <filesystem>
#include <string>

using namespace CollabText;
using namespace CollabText::Crdt;

class TestOpStreamConvergenceMixed : public QObject {
    Q_OBJECT

private slots:

    // ── sequential structural + text ops converge ─────────────────────────────
    //
    // Replica 1: inserts block id=1 into IdList, then inserts "hello" into engine.
    // Sync delivers both streams to replica 2.
    // Replica 2: appends " world" to engine2 (after receiving "hello").
    // Sync delivers replica 2's text edit back to replica 1.
    // Both list1/list2 and engine1/engine2 must converge.

    void mixed_idlist_and_buffer_convergence_over_opstream() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        std::filesystem::path shared = tmp.path().toStdString();

        StreamSync sync1(shared, "replica-A", /*replica_id=*/1);
        StreamSync sync2(shared, "replica-B", /*replica_id=*/2);

        sync1.register_stream("idlist:structure", StreamSync::StreamType::AppendOnly);
        sync2.register_stream("idlist:structure", StreamSync::StreamType::AppendOnly);
        sync1.register_stream("buffer:block-1", StreamSync::StreamType::AppendOnly);
        sync2.register_stream("buffer:block-1", StreamSync::StreamType::AppendOnly);
        sync1.start();
        sync2.start();

        OpStream* op1 = &sync1;
        OpStream* op2 = &sync2;

        IdList list1(1);
        IdList list2(2);
        CrdtEngine engine1(1);
        CrdtEngine engine2(2);

        // Wire IdList ops outbound
        list1.set_on_local_op([&](const IdListOperation& op) {
            op1->push("idlist:structure", encode_idlist_operation(op));
        });
        list2.set_on_local_op([&](const IdListOperation& op) {
            op2->push("idlist:structure", encode_idlist_operation(op));
        });

        // Wire CrdtEngine ops outbound
        engine1.setOnLocalOp([&](const Operation& op) {
            op1->push("buffer:block-1", encode_operation(op));
        });
        engine2.setOnLocalOp([&](const Operation& op) {
            op2->push("buffer:block-1", encode_operation(op));
        });

        // Wire op1 inbound → list1 + engine1 (skip own-replica)
        op1->set_on_inbound([&](const std::string& stream, uint16_t replica_id,
                                const std::string& payload) {
            if (replica_id == 1) return;
            if (stream == "idlist:structure") {
                auto op = decode_idlist_operation(payload);
                QVERIFY(op.has_value());
                list1.apply_remote_op(*op);
            } else if (stream == "buffer:block-1") {
                auto op = decode_operation(payload);
                QVERIFY(op.has_value());
                engine1.applyRemoteOp(*op);
            }
        });

        // Wire op2 inbound → list2 + engine2 (skip own-replica)
        op2->set_on_inbound([&](const std::string& stream, uint16_t replica_id,
                                const std::string& payload) {
            if (replica_id == 2) return;
            if (stream == "idlist:structure") {
                auto op = decode_idlist_operation(payload);
                QVERIFY(op.has_value());
                list2.apply_remote_op(*op);
            } else if (stream == "buffer:block-1") {
                auto op = decode_operation(payload);
                QVERIFY(op.has_value());
                engine2.applyRemoteOp(*op);
            }
        });

        // Replica 1: insert block id=1 into IdList, then "hello" into engine
        list1.insert_after(Anchor::min(), 1);
        engine1.insert(0, "hello");

        // Deliver to replica 2
        sync1.flush();
        sync2.poll();

        QCOMPARE(list2.ids(), (std::vector<uint64_t>{1}));
        QCOMPARE(engine2.text(), std::string("hello"));

        // Replica 2: append " world" at position 5
        engine2.insert(5, " world");
        QCOMPARE(engine2.text(), std::string("hello world"));

        // Deliver replica 2's text edit back to replica 1
        sync2.flush();
        sync1.poll();

        // Structural convergence
        QCOMPARE(list1.ids(), list2.ids());

        // Text convergence
        QCOMPARE(engine1.text(), engine2.text());

        // Both sub-strings are present
        QVERIFY(engine1.text().find("hello") != std::string::npos);
        QVERIFY(engine1.text().find("world") != std::string::npos);
    }

    // ── concurrent structural + text edits converge ───────────────────────────
    //
    // Replica 1 and replica 2 both insert a block into their IdLists and text
    // into their engines without first syncing.  After a full two-way sync both
    // replicas converge on list ids() and engine text().

    void mixed_concurrent_structural_and_text_edits() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        std::filesystem::path shared = tmp.path().toStdString();

        StreamSync sync1(shared, "replica-A", /*replica_id=*/1);
        StreamSync sync2(shared, "replica-B", /*replica_id=*/2);

        sync1.register_stream("idlist:structure", StreamSync::StreamType::AppendOnly);
        sync2.register_stream("idlist:structure", StreamSync::StreamType::AppendOnly);
        sync1.register_stream("buffer:block-1", StreamSync::StreamType::AppendOnly);
        sync2.register_stream("buffer:block-1", StreamSync::StreamType::AppendOnly);
        sync1.start();
        sync2.start();

        OpStream* op1 = &sync1;
        OpStream* op2 = &sync2;

        IdList list1(1);
        IdList list2(2);
        CrdtEngine engine1(1);
        CrdtEngine engine2(2);

        list1.set_on_local_op([&](const IdListOperation& op) {
            op1->push("idlist:structure", encode_idlist_operation(op));
        });
        list2.set_on_local_op([&](const IdListOperation& op) {
            op2->push("idlist:structure", encode_idlist_operation(op));
        });

        engine1.setOnLocalOp([&](const Operation& op) {
            op1->push("buffer:block-1", encode_operation(op));
        });
        engine2.setOnLocalOp([&](const Operation& op) {
            op2->push("buffer:block-1", encode_operation(op));
        });

        op1->set_on_inbound([&](const std::string& stream, uint16_t replica_id,
                                const std::string& payload) {
            if (replica_id == 1) return;
            if (stream == "idlist:structure") {
                auto op = decode_idlist_operation(payload);
                QVERIFY(op.has_value());
                list1.apply_remote_op(*op);
            } else if (stream == "buffer:block-1") {
                auto op = decode_operation(payload);
                QVERIFY(op.has_value());
                engine1.applyRemoteOp(*op);
            }
        });

        op2->set_on_inbound([&](const std::string& stream, uint16_t replica_id,
                                const std::string& payload) {
            if (replica_id == 2) return;
            if (stream == "idlist:structure") {
                auto op = decode_idlist_operation(payload);
                QVERIFY(op.has_value());
                list2.apply_remote_op(*op);
            } else if (stream == "buffer:block-1") {
                auto op = decode_operation(payload);
                QVERIFY(op.has_value());
                engine2.applyRemoteOp(*op);
            }
        });

        // Concurrent local edits — neither replica has seen the other's ops
        list1.insert_after(Anchor::min(), 10);
        engine1.insert(0, "foo");

        list2.insert_after(Anchor::min(), 20);
        engine2.insert(0, "bar");

        // First pass: sync1 → sync2
        sync1.flush();
        sync2.poll();

        // Second pass: sync2 → sync1
        sync2.flush();
        sync1.poll();

        // Structural convergence — same blocks in same CRDT-determined order
        QCOMPARE(list1.ids(), list2.ids());
        QCOMPARE(static_cast<int>(list1.ids().size()), 2);

        // Text convergence
        QCOMPARE(engine1.text(), engine2.text());

        // All characters preserved (CRDT property) — "foo" + "bar" = 6 chars
        QCOMPARE(static_cast<int>(engine1.text().size()), 6);
    }
};

QTEST_MAIN(TestOpStreamConvergenceMixed)
#include "tst_opstream_convergence_mixed.moc"
