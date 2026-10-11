#include <QTest>
#include "crdt/IdList.h"
#include "TestActor.h"

using CollabText::Test::actor;

using namespace CollabText::Crdt;

class TestIdList : public QObject {
    Q_OBJECT
private slots:
    void empty_list_has_no_ids() {
        IdList list(CollabText::Test::test_counter(1));
        QCOMPARE(list.size(), 0u);
        QCOMPARE(list.ids().size(), size_t{0});
    }

    void actor_id_round_trips() {
        IdList list(CollabText::Test::test_counter(42));
        QCOMPARE(list.actor_id(), actor(42));
    }

    void checkpoint_state_rebuilds_and_rejects_atomically() {
        IdList source(CollabText::Test::test_counter(1));
        source.insert_after(Anchor::min(), 10);
        source.insert_after(source.anchor_of(10), 20);
        QVERIFY(source.try_remove_at(source.anchor_of(10)));
        QCOMPARE(source.ids(), (std::vector<uint64_t>{20}));
        QVERIFY(source.tombstone_count() > 0);

        const auto checkpoint = source.checkpoint_state();
        IdList owner_restore(CollabText::Test::test_counter(1, 1000));
        QVERIFY(owner_restore.restore_checkpoint_state(checkpoint));
        QVERIFY(owner_restore.restore_local_history(source.local_history_snapshot()));
        QCOMPARE(owner_restore.undo_depth(), source.undo_depth());
        QVERIFY(owner_restore.undo().has_value());
        QCOMPARE(owner_restore.ids(), (std::vector<uint64_t>{10, 20}));

        IdList restored(CollabText::Test::test_counter(2));
        QVERIFY(restored.restore_checkpoint_state(checkpoint));
        QCOMPARE(restored.ids(), source.ids());
        QCOMPARE(restored.entries().size(), source.entries().size());
        QCOMPARE(restored.version(), source.version());
        QCOMPARE(restored.actor_id(), actor(2));
        QVERIFY(!restored.undo().has_value()); // shareable state excludes actor 1 history
        const auto anchor = source.anchor_of(20);
        QCOMPARE(restored.resolve_anchor(anchor), source.resolve_anchor(anchor));

        auto malformed = checkpoint;
        malformed.entries[1].origin = malformed.entries[0].origin;
        const auto failed = restored.restore_checkpoint_state(malformed);
        QVERIFY(!failed);
        QCOMPARE(restored.ids(), (std::vector<uint64_t>{20}));
        QCOMPARE(restored.version(), checkpoint.version);

        IdList owner_with_history(CollabText::Test::test_counter(4));
        owner_with_history.insert_after(Anchor::min(), 40);
        const auto owner_ids = owner_with_history.ids();
        const auto owner_depth = owner_with_history.undo_depth();
        QVERIFY(!owner_with_history.restore_checkpoint_state(checkpoint));
        QCOMPARE(owner_with_history.ids(), owner_ids);
        QCOMPARE(owner_with_history.undo_depth(), owner_depth);

        IdList stale_owner(CollabText::Test::test_counter(1));
        const auto stale_restore = stale_owner.restore_checkpoint_state(checkpoint);
        QVERIFY(!stale_restore);
        QCOMPARE(stale_restore.error().code(), CollabText::ErrorCode::counter_regression);
        QVERIFY(stale_owner.ids().empty());

        auto new_local = restored.try_insert_after(restored.anchor_of(20), 30);
        QVERIFY(new_local);
        QCOMPARE(get_idlist_op_identity(new_local.value()).actor, actor(2));
        source.apply_ops({new_local.value()});
        QCOMPARE(source.ids(), restored.ids());
    }

    void checkpoint_restores_deferred_operation_for_retry() {
        IdList producer(CollabText::Test::test_counter(1));
        auto first = producer.try_insert_after(Anchor::min(), 10);
        QVERIFY(first);
        auto second = producer.try_insert_after(producer.anchor_of(10), 20);
        QVERIFY(second);

        IdList receiver(CollabText::Test::test_counter(2));
        const auto delayed = receiver.apply_ops({second.value()});
        QCOMPARE(delayed.front().kind(), CollabText::ApplyOutcomeKind::deferred);
        QCOMPARE(receiver.deferred_count(), size_t{1});

        for (const DeferredBudget budget : {DeferredBudget{0, 8u << 20},
                                            DeferredBudget{1, 0}}) {
            IdList constrained(CollabText::Test::test_counter(4));
            constrained.set_deferred_budget(budget);
            const auto refused = constrained.restore_checkpoint_state(
                receiver.checkpoint_state());
            QVERIFY(!refused);
            QCOMPARE(refused.error().code(), CollabText::ErrorCode::limit_exceeded);
            QVERIFY(constrained.ids().empty());
        }

        IdList restored(CollabText::Test::test_counter(3));
        QVERIFY(restored.restore_checkpoint_state(receiver.checkpoint_state()));
        QCOMPARE(restored.deferred_count(), size_t{1});
        restored.apply_ops({first.value()});
        QCOMPARE(restored.ids(), (std::vector<uint64_t>{10, 20}));
        QCOMPARE(restored.deferred_count(), size_t{0});
    }

    void initial_version_is_empty() {
        IdList list(CollabText::Test::test_counter(1));
        // Version starts unobserved — all entries zero or vector empty
        QVERIFY(list.version().empty());
    }

    void insert_into_empty_list() {
        IdList list(CollabText::Test::test_counter(1));
        list.insert_after(Anchor::min(), 0xAA);
        QCOMPARE(list.size(), 1u);
        QCOMPARE(list.ids(), std::vector<uint64_t>{0xAA});
    }

    void insert_at_start_pushes_existing_back() {
        IdList list(CollabText::Test::test_counter(1));
        list.insert_after(Anchor::min(), 0xAA);  // list: [0xAA]
        list.insert_after(Anchor::min(), 0xBB);  // list: [0xBB, 0xAA]
        QCOMPARE(list.size(), 2u);
        QCOMPARE(list.ids(), (std::vector<uint64_t>{0xBB, 0xAA}));
    }

    void multiple_inserts_at_start_reverse_order() {
        IdList list(CollabText::Test::test_counter(1));
        list.insert_after(Anchor::min(), 1);
        list.insert_after(Anchor::min(), 2);
        list.insert_after(Anchor::min(), 3);
        QCOMPARE(list.ids(), (std::vector<uint64_t>{3, 2, 1}));
    }

    void same_id_value_can_appear_twice() {
        IdList list(CollabText::Test::test_counter(1));
        list.insert_after(Anchor::min(), 0xAA);
        list.insert_after(Anchor::min(), 0xAA);
        QCOMPARE(list.size(), 2u);
        QCOMPARE(list.ids(), (std::vector<uint64_t>{0xAA, 0xAA}));
    }

    void remove_only_element() {
        IdList list(CollabText::Test::test_counter(1));
        list.insert_after(Anchor::min(), 0xAA);  // origin=(1,1)
        Anchor at_aa(Identity(actor(1), 1), Bias::Left);
        list.remove_at(at_aa);
        QCOMPARE(list.size(), 0u);
        QCOMPARE(list.ids().size(), size_t{0});
        QCOMPARE(list.tombstone_count(), size_t{1});
    }

    void remove_middle_keeps_neighbours() {
        IdList list(CollabText::Test::test_counter(1));
        list.insert_after(Anchor::min(), 1);  // origin (1,1) → [1]
        list.insert_after(Anchor::min(), 2);  // origin (1,2) → [2, 1]
        list.insert_after(Anchor::min(), 3);  // origin (1,3) → [3, 2, 1]
        list.remove_at(Anchor(Identity(actor(1), 2), Bias::Left));  // remove "2"
        QCOMPARE(list.ids(), (std::vector<uint64_t>{3, 1}));
        QCOMPARE(list.tombstone_count(), size_t{1});
    }

    void discard_local_view_empties_the_list() {
        IdList list(CollabText::Test::test_counter(1));
        list.insert_after(Anchor::min(), 100);
        list.insert_after(list.anchor_of(100, Bias::Right), 200);
        QCOMPARE(list.size(), 2u);

        list.discard_local_view();

        QCOMPARE(list.size(), 0u);
        QCOMPARE(list.tombstone_count(), size_t{0});
        QCOMPARE(list.entry_count(), size_t{0});
        QCOMPARE(list.undo_depth(), size_t{0});
        QCOMPARE(list.actor_id(), actor(1));
    }

    void discard_local_view_preserves_clock_monotonicity() {
        IdList list(CollabText::Test::test_counter(1));
        list.insert_after(Anchor::min(), 100);
        auto op1 = list.insert_after(list.anchor_of(100, Bias::Right), 200);
        const Identity stampBefore = get_idlist_op_identity(op1);

        list.discard_local_view();

        auto op2 = list.insert_after(Anchor::min(), 300);
        const Identity stampAfter = get_idlist_op_identity(op2);

        // Clock must not regress (Identity has operator<=>).
        QVERIFY(stampBefore < stampAfter);
    }

    void discard_local_view_does_not_fire_callbacks() {
        IdList list(CollabText::Test::test_counter(1));
        list.insert_after(Anchor::min(), 100);

        int changeCalls = 0;
        int localOpCalls = 0;
        list.set_on_change([&]{ ++changeCalls; });
        list.set_on_local_op([&](const IdListOperation &){ ++localOpCalls; });

        list.discard_local_view();

        QCOMPARE(changeCalls, 0);
        QCOMPARE(localOpCalls, 0);

        // Callbacks survive the clear: a subsequent local op fires them.
        list.insert_after(Anchor::min(), 200);
        QCOMPARE(changeCalls, 1);
        QCOMPARE(localOpCalls, 1);
    }

    void discard_local_view_then_insert_chain_works() {
        IdList list(CollabText::Test::test_counter(1));
        list.insert_after(Anchor::min(), 100);
        list.insert_after(list.anchor_of(100, Bias::Right), 200);

        list.discard_local_view();

        list.insert_after(Anchor::min(), 300);
        list.insert_after(list.anchor_of(300, Bias::Right), 400);

        QCOMPARE(list.size(), 2u);
        QCOMPARE(list.ids(), (std::vector<uint64_t>{300, 400}));
    }
};

QTEST_GUILESS_MAIN(TestIdList)
#include "tst_idlist.moc"
