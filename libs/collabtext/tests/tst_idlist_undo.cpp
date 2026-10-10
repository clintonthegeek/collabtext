#include <QTest>
#include <collabtext/IdListEngine.h>
#include "crdt/IdList.h"
#include "TestActor.h"

using CollabText::Test::actor;

using namespace CollabText::Crdt;

class TestIdListUndo : public QObject {
    Q_OBJECT
private slots:
    void undo_local_insert() {
        IdList list(CollabText::Test::test_counter(1));
        list.insert_after(Anchor::min(), 0xAA);
        QCOMPARE(list.size(), 1u);
        auto undo_op = list.undo();
        QVERIFY(undo_op.has_value());
        QCOMPARE(list.size(), 0u);
    }

    void undo_local_remove_restores_element() {
        IdList list(CollabText::Test::test_counter(1));
        list.insert_after(Anchor::min(), 0xAA);
        list.remove_at(Anchor(Identity(actor(1), 1), Bias::Left));
        QCOMPARE(list.size(), 0u);
        list.undo();
        QCOMPARE(list.size(), 1u);
        QCOMPARE(list.ids(), std::vector<uint64_t>{0xAA});
    }

    void redo_after_undo_restores_state() {
        IdList list(CollabText::Test::test_counter(1));
        list.insert_after(Anchor::min(), 0xAA);
        list.undo();
        list.redo();
        QCOMPARE(list.ids(), std::vector<uint64_t>{0xAA});
    }

    void undo_only_unwinds_local_edits() {
        IdList alice(CollabText::Test::test_counter(1)), bob(CollabText::Test::test_counter(2));
        auto op_a = alice.insert_after(Anchor::min(), 0xAA);
        bob.apply_ops({op_a});
        auto op_b = bob.insert_after(Anchor::min(), 0xBB);
        alice.apply_ops({op_b});
        QCOMPARE(alice.size(), 2u);

        auto undo_op = alice.undo();
        QVERIFY(undo_op.has_value());
        bob.apply_ops({*undo_op});
        QCOMPARE(alice.ids(), bob.ids());
        QCOMPARE(alice.ids(), std::vector<uint64_t>{0xBB});  // bob's insert remains
    }

    void coalesce_last_undo_groups_two_inserts() {
        IdList list(CollabText::Test::test_counter(1));
        list.insert_after(Anchor::min(), 1);
        list.insert_after(Anchor::min(), 2);
        QVERIFY(list.coalesce_last_undo());
        list.undo();  // single undo unwinds both
        QCOMPARE(list.size(), 0u);
    }

    void max_undo_depth_trims_oldest() {
        IdList list(CollabText::Test::test_counter(1));
        QVERIFY((list.set_max_undo_depth(3)).ok());
        for (int i = 0; i < 5; ++i) list.insert_after(Anchor::min(), static_cast<uint64_t>(i));
        QCOMPARE(list.undo_depth(), size_t{3});
    }

    void public_engine_restores_snapshot_after_owner_operation_replay() {
        CollabText::IdListEngine original(CollabText::Test::test_counter(7));
        auto insert = original.try_insert_after(Anchor::min(), 42);
        QVERIFY(insert.ok());
        const auto saved = original.local_history_snapshot();
        const auto group = original.next_undo_group();
        QVERIFY(group.has_value());
        QCOMPARE(group->operation_ids.size(), size_t(1));
        QCOMPARE(group->group_id, idlist_op_identity(insert.value()));

        CollabText::IdListEngine replayed(
            CollabText::Test::test_counter(7, 100));
        const auto outcomes = replayed.apply_ops({insert.value()});
        QCOMPARE(outcomes.size(), size_t(1));
        QVERIFY(replayed.restore_local_history(saved).ok());
        QCOMPARE(replayed.local_history_snapshot().cursor, size_t(1));
        QVERIFY((replayed.set_max_undo_bytes(512)).ok());
        QCOMPARE(replayed.max_undo_bytes(), size_t(512));
        QVERIFY(replayed.undo_history_bytes() <= replayed.max_undo_bytes());
        QVERIFY(replayed.undo().has_value());
        QCOMPARE(replayed.size(), uint32_t(0));
    }

    void public_engine_persists_trim_retirement_intents() {
        CollabText::IdListEngine original(CollabText::Test::test_counter(8));
        QVERIFY((original.set_max_undo_depth(1)).ok());
        auto first = original.try_insert_after(Anchor::min(), 1);
        auto second = original.try_insert_after(Anchor::min(), 2);
        QVERIFY(first.ok());
        QVERIFY(second.ok());
        const auto saved = original.local_history_snapshot();
        QCOMPARE(saved.entries.size(), size_t(1));
        QCOMPARE(saved.retired_entries.size(), size_t(1));
        QCOMPARE(saved.retired_entries[0].group_id,
                 idlist_op_identity(first.value()));

        CollabText::IdListEngine replayed(
            CollabText::Test::test_counter(8, 100));
        QVERIFY((replayed.set_max_undo_depth(1)).ok());
        QCOMPARE(replayed.apply_ops({first.value(), second.value()}).size(),
                 size_t(2));
        QVERIFY(replayed.restore_local_history(saved).ok());
        QVERIFY(replayed.acknowledge_undo_retirements(
            {saved.retired_entries[0].group_id}).ok());
        QCOMPARE(replayed.local_history_snapshot().retired_entries.size(),
                 size_t(0));
        QVERIFY(replayed.undo().has_value());
        QCOMPARE(replayed.ids(), std::vector<uint64_t>{1});
        QVERIFY(!replayed.undo().has_value());
    }
};

QTEST_GUILESS_MAIN(TestIdListUndo)
#include "tst_idlist_undo.moc"
