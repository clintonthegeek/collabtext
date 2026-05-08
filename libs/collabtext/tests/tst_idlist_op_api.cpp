/// tst_idlist_op_api.cpp — Tests for IdList::set_on_local_op / apply_remote_op
///
/// Proves that:
///   - set_on_local_op fires on every local op (insert_after, remove_at)
///   - set_on_local_op fires on undo and redo (only when they produce an op)
///   - set_on_local_op does NOT fire when apply_remote_op is called
///   - set_on_change DOES fire when apply_remote_op is called
///   - encode + decode round-trip preserves the op
///   - apply_remote_op converges two IdLists
///   - undo round-trip: undo on list 1 propagated to list 2 keeps both in sync
///   - two-way convergence: both lists insert, both apply each other's ops
///   - apply_remote_op returns true for valid ops

#include <collabtext/IdListOperations.h>
#include <collabtext/Serialization.h>
#include "crdt/IdList.h"

#include <QTest>
#include <optional>
#include <string>
#include <vector>

using namespace CollabText::Crdt;

class TestIdListOpApi : public QObject {
    Q_OBJECT

private slots:

    // ── callback fires on insert_after ────────────────────────────────────────

    void set_on_local_op_fires_on_insert() {
        IdList list(1);
        std::optional<IdListOperation> captured;
        list.set_on_local_op([&](const IdListOperation& op) {
            captured = op;
        });

        list.insert_after(Anchor::min(), 0xAA);

        QVERIFY(captured.has_value());
        QVERIFY(idlist_op_lamport(*captured).counter() > 0);
    }

    // ── callback fires on remove_at ───────────────────────────────────────────

    void set_on_local_op_fires_on_remove() {
        IdList list(1);
        auto insert_op = list.insert_after(Anchor::min(), 0xAA);

        std::optional<IdListOperation> captured;
        list.set_on_local_op([&](const IdListOperation& op) {
            captured = op;
        });

        // Remove the element we just inserted
        Anchor at_aa = list.anchor_of(0xAA);
        list.remove_at(at_aa);

        QVERIFY(captured.has_value());
    }

    // ── callback fires on undo ────────────────────────────────────────────────

    void set_on_local_op_fires_on_undo() {
        IdList list(1);
        list.insert_after(Anchor::min(), 0xAA);

        std::optional<IdListOperation> captured;
        list.set_on_local_op([&](const IdListOperation& op) {
            captured = op;
        });

        auto result = list.undo();
        QVERIFY(result.has_value());
        QVERIFY(captured.has_value());
    }

    // ── callback fires on redo ────────────────────────────────────────────────

    void set_on_local_op_fires_on_redo() {
        IdList list(1);
        list.insert_after(Anchor::min(), 0xAA);
        list.undo();

        std::optional<IdListOperation> captured;
        list.set_on_local_op([&](const IdListOperation& op) {
            captured = op;
        });

        auto result = list.redo();
        QVERIFY(result.has_value());
        QVERIFY(captured.has_value());
    }

    // ── undo with nothing to undo does NOT fire ───────────────────────────────

    void set_on_local_op_does_not_fire_on_noop_undo() {
        IdList list(1);
        int fire_count = 0;
        list.set_on_local_op([&](const IdListOperation&) { ++fire_count; });

        // Undo on empty list — should not fire
        auto result = list.undo();
        QVERIFY(!result.has_value());
        QCOMPARE(fire_count, 0);
    }

    // ── apply_remote_op does NOT fire set_on_local_op ────────────────────────

    void apply_remote_op_does_not_fire_local_op_callback() {
        IdList list1(1), list2(2);

        // Capture ops from list1
        std::vector<IdListOperation> captured_from_1;
        list1.set_on_local_op([&](const IdListOperation& op) {
            captured_from_1.push_back(op);
        });

        auto op = list1.insert_after(Anchor::min(), 0xAA);
        QCOMPARE(static_cast<int>(captured_from_1.size()), 1);

        // list2 should NOT fire local_op callback when applying remote op
        int list2_local_fire_count = 0;
        list2.set_on_local_op([&](const IdListOperation&) { ++list2_local_fire_count; });

        list2.apply_remote_op(op);

        QCOMPARE(list2_local_fire_count, 0);
    }

    // ── apply_remote_op DOES fire set_on_change ───────────────────────────────

    void apply_remote_op_fires_change_callback() {
        IdList list1(1), list2(2);

        auto op = list1.insert_after(Anchor::min(), 0xAA);

        int change_count = 0;
        list2.set_on_change([&]() { ++change_count; });

        list2.apply_remote_op(op);

        QVERIFY(change_count > 0);
    }

    // ── one-way convergence via apply_remote_op ───────────────────────────────

    void apply_remote_op_converges_insert() {
        IdList list1(1), list2(2);

        std::vector<std::string> wire_ops;
        list1.set_on_local_op([&](const IdListOperation& op) {
            wire_ops.push_back(encode_idlist_operation(op));
        });

        list1.insert_after(Anchor::min(), 0xAA);

        QVERIFY(!wire_ops.empty());
        for (const auto& wire : wire_ops) {
            auto decoded = decode_idlist_operation(wire);
            QVERIFY(decoded.has_value());
            bool ok = list2.apply_remote_op(*decoded);
            QVERIFY(ok);
        }

        QCOMPARE(list1.ids(), list2.ids());
        QCOMPARE(list2.ids(), (std::vector<uint64_t>{0xAA}));
    }

    // ── encode/decode round-trip preserves Lamport identity ──────────────────

    void encode_decode_preserves_lamport() {
        IdList list(1);
        std::optional<IdListOperation> captured;
        list.set_on_local_op([&](const IdListOperation& op) {
            if (!captured) captured = op;
        });

        list.insert_after(Anchor::min(), 0xAA);
        QVERIFY(captured.has_value());

        std::string wire = encode_idlist_operation(*captured);
        auto decoded = decode_idlist_operation(wire);
        QVERIFY(decoded.has_value());

        QCOMPARE(idlist_op_lamport(*decoded).counter(), idlist_op_lamport(*captured).counter());
        QCOMPARE(idlist_op_lamport(*decoded).replica_id, idlist_op_lamport(*captured).replica_id);
    }

    // ── undo round-trip: undo on list 1 propagated to list 2 ─────────────────

    void apply_remote_op_undo_roundtrip() {
        IdList list1(1), list2(2);

        // Collect all ops list1 emits (insert + undo)
        std::vector<std::string> wire_ops;
        list1.set_on_local_op([&](const IdListOperation& op) {
            wire_ops.push_back(encode_idlist_operation(op));
        });

        list1.insert_after(Anchor::min(), 0xAA);
        list1.undo();

        QCOMPARE(static_cast<int>(wire_ops.size()), 2); // insert + undo

        // Apply all ops to list2 in order
        for (const auto& wire : wire_ops) {
            auto decoded = decode_idlist_operation(wire);
            QVERIFY(decoded.has_value());
            bool ok = list2.apply_remote_op(*decoded);
            QVERIFY(ok);
        }

        // Both should have empty ids after undo propagated
        QCOMPARE(list1.ids(), list2.ids());
        QCOMPARE(list1.ids(), (std::vector<uint64_t>{}));
    }

    // ── two-way convergence: sequential (non-conflicting) ────────────────────
    //
    // list1 inserts 0xAA; list2 receives it, then inserts 0xBB after it.
    // Both should converge to [0xAA, 0xBB].

    void two_way_convergence_sequential() {
        IdList list1(1), list2(2);

        std::vector<std::string> ops_from_1;
        std::vector<std::string> ops_from_2;

        list1.set_on_local_op([&](const IdListOperation& op) {
            ops_from_1.push_back(encode_idlist_operation(op));
        });
        list2.set_on_local_op([&](const IdListOperation& op) {
            ops_from_2.push_back(encode_idlist_operation(op));
        });

        // list1 inserts 0xAA
        list1.insert_after(Anchor::min(), 0xAA);

        // list2 receives list1's op
        for (const auto& wire : ops_from_1) {
            auto decoded = decode_idlist_operation(wire);
            QVERIFY(decoded.has_value());
            QVERIFY(list2.apply_remote_op(*decoded));
        }
        ops_from_1.clear();

        // list2 inserts 0xBB after 0xAA (non-conflicting)
        Anchor after_aa = list2.anchor_of(0xAA, Bias::Right);
        list2.insert_after(after_aa, 0xBB);

        // list1 receives list2's op
        for (const auto& wire : ops_from_2) {
            auto decoded = decode_idlist_operation(wire);
            QVERIFY(decoded.has_value());
            QVERIFY(list1.apply_remote_op(*decoded));
        }

        // Both should converge to [0xAA, 0xBB]
        QCOMPARE(list1.ids(), list2.ids());
        QCOMPARE(list1.ids(), (std::vector<uint64_t>{0xAA, 0xBB}));
    }

    // ── two-way convergence: concurrent inserts ───────────────────────────────
    //
    // Both lists insert concurrently. Don't assert specific order — just assert
    // both see the same ids() after exchanging ops.

    void two_way_convergence_concurrent() {
        IdList list1(1), list2(2);

        std::vector<std::string> ops_from_1;
        std::vector<std::string> ops_from_2;

        list1.set_on_local_op([&](const IdListOperation& op) {
            ops_from_1.push_back(encode_idlist_operation(op));
        });
        list2.set_on_local_op([&](const IdListOperation& op) {
            ops_from_2.push_back(encode_idlist_operation(op));
        });

        // Both insert concurrently at the beginning
        list1.insert_after(Anchor::min(), 0xAA);
        list2.insert_after(Anchor::min(), 0xBB);

        // Exchange ops
        for (const auto& wire : ops_from_1) {
            auto decoded = decode_idlist_operation(wire);
            QVERIFY(decoded.has_value());
            QVERIFY(list2.apply_remote_op(*decoded));
        }
        for (const auto& wire : ops_from_2) {
            auto decoded = decode_idlist_operation(wire);
            QVERIFY(decoded.has_value());
            QVERIFY(list1.apply_remote_op(*decoded));
        }

        // Both should converge to the same ids (order determined by CRDT tiebreaking)
        QCOMPARE(list1.ids(), list2.ids());
        QCOMPARE(list1.size(), 2u);
    }

    // ── apply_remote_op returns true for valid ops ────────────────────────────

    void apply_remote_op_returns_true_for_valid_op() {
        IdList list1(1), list2(2);

        std::optional<IdListOperation> captured;
        list1.set_on_local_op([&](const IdListOperation& op) {
            if (!captured) captured = op;
        });

        list1.insert_after(Anchor::min(), 0xAA);
        QVERIFY(captured.has_value());

        bool result = list2.apply_remote_op(*captured);
        QVERIFY(result);
    }

    // ── multi-op sequence convergence ────────────────────────────────────────

    void sequential_multi_op_convergence() {
        IdList list1(1), list2(2);

        std::vector<std::string> wire_ops;
        list1.set_on_local_op([&](const IdListOperation& op) {
            wire_ops.push_back(encode_idlist_operation(op));
        });

        // Insert 0xAA, 0xBB after it, then remove 0xAA
        list1.insert_after(Anchor::min(), 0xAA);
        Anchor after_aa = list1.anchor_of(0xAA, Bias::Right);
        list1.insert_after(after_aa, 0xBB);
        Anchor at_aa = list1.anchor_of(0xAA);
        list1.remove_at(at_aa);

        QCOMPARE(list1.ids(), (std::vector<uint64_t>{0xBB}));

        for (const auto& wire : wire_ops) {
            auto decoded = decode_idlist_operation(wire);
            QVERIFY(decoded.has_value());
            QVERIFY(list2.apply_remote_op(*decoded));
        }

        QCOMPARE(list1.ids(), list2.ids());
        QCOMPARE(list2.ids(), (std::vector<uint64_t>{0xBB}));
    }
};

QTEST_GUILESS_MAIN(TestIdListOpApi)
#include "tst_idlist_op_api.moc"
