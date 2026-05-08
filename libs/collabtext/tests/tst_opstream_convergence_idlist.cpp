/// tst_opstream_convergence_idlist.cpp — Two-replica IdList convergence over OpStream
///
/// Proves that two IdLists wired through StreamSync (via the OpStream interface)
/// converge correctly:
///   - Sequential inserts: list1 inserts two elements, list2 receives and converges.
///   - Divergent concurrent inserts: both lists insert at head before any sync, then
///     exchange ops and converge to the same order with both ids present.
///   - Undo propagation: list1 undoes an insert; the undo op travels to list2 and
///     both end up with an empty list.

#include <QTest>
#include <QTemporaryDir>
#include <collabtext/StreamSync.h>
#include <collabtext/OpStream.h>
#include <collabtext/IdListOperations.h>
#include <collabtext/Serialization.h>
#include "crdt/IdList.h"

#include <filesystem>
#include <string>

using namespace CollabText;
using namespace CollabText::Crdt;

class TestOpStreamConvergenceIdList : public QObject {
    Q_OBJECT

private slots:

    // ── sequential inserts: list1 inserts two elements, list2 converges ───────
    //
    // list1 inserts id=100 at head, then id=200 after 100.  sync2 polls and
    // list2 applies both ops.  Both must hold the same ids() in the same order.

    void two_replica_idlist_convergence_over_opstream() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        std::filesystem::path shared = tmp.path().toStdString();

        StreamSync sync1(shared, "replica-A", /*replica_id=*/1);
        StreamSync sync2(shared, "replica-B", /*replica_id=*/2);

        sync1.register_stream("idlist:structure", StreamSync::StreamType::AppendOnly);
        sync2.register_stream("idlist:structure", StreamSync::StreamType::AppendOnly);
        sync1.start();
        sync2.start();

        OpStream* op1 = &sync1;
        OpStream* op2 = &sync2;

        IdList list1(1);
        IdList list2(2);

        // Wire list1 → op1 (outbound)
        list1.set_on_local_op([&](const IdListOperation& op) {
            op1->push("idlist:structure", encode_idlist_operation(op));
        });

        // Wire list2 → op2 (outbound) — not used in this test but wired for symmetry
        list2.set_on_local_op([&](const IdListOperation& op) {
            op2->push("idlist:structure", encode_idlist_operation(op));
        });

        // Wire op1 inbound → list1 (skip own-replica ops)
        op1->set_on_inbound([&](const std::string& /*stream*/, uint16_t replica_id,
                                const std::string& payload) {
            if (replica_id == 1) return;
            auto decoded = decode_idlist_operation(payload);
            QVERIFY(decoded.has_value());
            list1.apply_remote_op(*decoded);
        });

        // Wire op2 inbound → list2 (skip own-replica ops)
        op2->set_on_inbound([&](const std::string& /*stream*/, uint16_t replica_id,
                                const std::string& payload) {
            if (replica_id == 2) return;
            auto decoded = decode_idlist_operation(payload);
            QVERIFY(decoded.has_value());
            list2.apply_remote_op(*decoded);
        });

        // Step 1: list1 inserts id=100 at head, then id=200 after 100
        list1.insert_after(Anchor::min(), 100);
        Anchor after_100 = list1.anchor_of(100, Bias::Right);
        list1.insert_after(after_100, 200);
        QCOMPARE(list1.ids(), (std::vector<uint64_t>{100, 200}));

        // Step 2: sync1 flushes; sync2 polls — list2 receives both ops
        sync1.flush();
        sync2.poll();

        // Both must converge to the same ids
        QCOMPARE(list1.ids(), list2.ids());
        QCOMPARE(list2.ids(), (std::vector<uint64_t>{100, 200}));
    }

    // ── divergent concurrent inserts converge ────────────────────────────────
    //
    // list1 inserts id=10 at head; list2 concurrently inserts id=20 at head.
    // After a two-pass exchange both replicas converge to the same order.
    // The CRDT tiebreak determines which id ends up first — we assert only that
    // both ids are present and both replicas hold the same sequence.

    void divergent_idlist_inserts_converge() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        std::filesystem::path shared = tmp.path().toStdString();

        StreamSync sync1(shared, "replica-A", /*replica_id=*/1);
        StreamSync sync2(shared, "replica-B", /*replica_id=*/2);

        sync1.register_stream("idlist:structure", StreamSync::StreamType::AppendOnly);
        sync2.register_stream("idlist:structure", StreamSync::StreamType::AppendOnly);
        sync1.start();
        sync2.start();

        OpStream* op1 = &sync1;
        OpStream* op2 = &sync2;

        IdList list1(1);
        IdList list2(2);

        list1.set_on_local_op([&](const IdListOperation& op) {
            op1->push("idlist:structure", encode_idlist_operation(op));
        });

        list2.set_on_local_op([&](const IdListOperation& op) {
            op2->push("idlist:structure", encode_idlist_operation(op));
        });

        op1->set_on_inbound([&](const std::string& /*stream*/, uint16_t replica_id,
                                const std::string& payload) {
            if (replica_id == 1) return;
            auto decoded = decode_idlist_operation(payload);
            QVERIFY(decoded.has_value());
            list1.apply_remote_op(*decoded);
        });

        op2->set_on_inbound([&](const std::string& /*stream*/, uint16_t replica_id,
                                const std::string& payload) {
            if (replica_id == 2) return;
            auto decoded = decode_idlist_operation(payload);
            QVERIFY(decoded.has_value());
            list2.apply_remote_op(*decoded);
        });

        // Concurrent inserts — neither list has seen the other's op yet
        list1.insert_after(Anchor::min(), 10);
        list2.insert_after(Anchor::min(), 20);

        // First pass: sync1 → sync2 (list2 receives list1's op)
        sync1.flush();
        sync2.poll();

        // Second pass: sync2 → sync1 (list1 receives list2's op)
        sync2.flush();
        sync1.poll();

        // Both replicas must converge to the same sequence
        QCOMPARE(list1.ids(), list2.ids());

        // Both ids must be present
        auto ids = list1.ids();
        QCOMPARE(static_cast<int>(ids.size()), 2);
        QVERIFY(std::find(ids.begin(), ids.end(), uint64_t(10)) != ids.end());
        QVERIFY(std::find(ids.begin(), ids.end(), uint64_t(20)) != ids.end());
    }

    // ── undo propagates over OpStream ────────────────────────────────────────
    //
    // list1 inserts id=42, then undoes it.  Both the insert op and the undo op
    // travel over the OpStream to list2.  Both lists end up empty.

    void idlist_undo_propagates_over_opstream() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        std::filesystem::path shared = tmp.path().toStdString();

        StreamSync sync1(shared, "replica-A", /*replica_id=*/1);
        StreamSync sync2(shared, "replica-B", /*replica_id=*/2);

        sync1.register_stream("idlist:structure", StreamSync::StreamType::AppendOnly);
        sync2.register_stream("idlist:structure", StreamSync::StreamType::AppendOnly);
        sync1.start();
        sync2.start();

        OpStream* op1 = &sync1;

        IdList list1(1);
        IdList list2(2);

        // One-directional wiring: list1 → op1 → list2
        list1.set_on_local_op([&](const IdListOperation& op) {
            op1->push("idlist:structure", encode_idlist_operation(op));
        });

        sync2.set_on_inbound([&](const std::string& /*stream*/, uint16_t /*replica_id*/,
                                 const std::string& payload) {
            auto decoded = decode_idlist_operation(payload);
            QVERIFY(decoded.has_value());
            list2.apply_remote_op(*decoded);
        });

        // Step 1: list1 inserts id=42 at head
        list1.insert_after(Anchor::min(), 42);

        // Step 2: deliver the insert to list2
        sync1.flush();
        sync2.poll();
        QCOMPARE(list2.ids(), (std::vector<uint64_t>{42}));

        // Step 3: list1 undoes the insert
        auto undo_op = list1.undo();
        QVERIFY(undo_op.has_value());
        QVERIFY(list1.ids().empty());

        // Step 4: deliver the undo op to list2
        sync1.flush();
        sync2.poll();

        // Both must be empty
        QVERIFY(list1.ids().empty());
        QVERIFY(list2.ids().empty());
    }
};

QTEST_MAIN(TestOpStreamConvergenceIdList)
#include "tst_opstream_convergence_idlist.moc"
