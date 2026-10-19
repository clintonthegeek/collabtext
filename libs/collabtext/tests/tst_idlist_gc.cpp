#include <QTest>
#include "crdt/IdList.h"
#include "TestActor.h"

using CollabText::Test::actor;

using namespace CollabText::Crdt;

template <typename T>
concept HasRawWatermarkCompact = requires(T &value, const Global &watermark) {
    value.compact(watermark);
};
static_assert(!HasRawWatermarkCompact<IdList>);

class TestIdListGc : public QObject {
    Q_OBJECT
private slots:
    void collect_garbage_retains_trimmed_tombstones() {
        IdList list(CollabText::Test::test_counter(1));
        list.insert_after(Anchor::min(), 0xAA);
        list.remove_at(Anchor(Identity(actor(1), 1), Bias::Left));
        QVERIFY((list.set_max_undo_depth(0)).ok());  // discard all undo entries
        QVERIFY((list.set_max_undo_depth(1000)).ok());
        QCOMPARE(list.tombstone_count(), size_t{1});
        size_t collected = list.collect_garbage();
        QCOMPARE(collected, size_t{0});
        QCOMPARE(list.tombstone_count(), size_t{1});
    }

    void collect_garbage_skips_remote_deletions() {
        IdList alice(CollabText::Test::test_counter(1)), bob(CollabText::Test::test_counter(2));
        auto op_a = alice.insert_after(Anchor::min(), 0xAA);
        bob.apply_ops({op_a});
        auto rm_b = bob.remove_at(Anchor(Identity(actor(1), 1), Bias::Left));
        alice.apply_ops({rm_b});
        QVERIFY((alice.set_max_undo_depth(0)).ok());
        QVERIFY((alice.set_max_undo_depth(1000)).ok());
        QCOMPARE(alice.collect_garbage(), size_t{0});  // remote deletion remains restorable
    }

    void uncertified_gc_preserves_remote_undo() {
        IdList alice(CollabText::Test::test_counter(1)), bob(CollabText::Test::test_counter(2));
        auto op_a = alice.insert_after(Anchor::min(), 0xAA);
        bob.apply_ops({op_a});
        auto rm_b = bob.remove_at(Anchor(Identity(actor(1), 1), Bias::Left));
        alice.apply_ops({rm_b});

        QVERIFY((alice.set_max_undo_depth(0)).ok());
        QVERIFY((alice.set_max_undo_depth(1000)).ok());
        size_t reclaimed = alice.collect_garbage();
        QCOMPARE(reclaimed, size_t{0});
        QCOMPARE(alice.tombstone_count(), size_t{1});
        auto undo = bob.undo();
        QVERIFY(undo.has_value());
        alice.apply_ops({*undo});
        QCOMPARE(alice.ids(), std::vector<uint64_t>{0xAA});
        QCOMPARE(alice.ids(), bob.ids());
    }

    void gc_protects_entries_in_undo_stack() {
        IdList list(CollabText::Test::test_counter(1));
        list.insert_after(Anchor::min(), 0xAA);
        list.remove_at(Anchor(Identity(actor(1), 1), Bias::Left));  // deletion id is in undo stack
        QCOMPARE(list.collect_garbage(), size_t{0});  // protected
        QVERIFY((list.set_max_undo_depth(0)).ok());  // drop undo entry
        QVERIFY((list.set_max_undo_depth(1000)).ok());
        QCOMPARE(list.collect_garbage(), size_t{0});  // local trimming cannot prove global retirement
        QCOMPARE(list.tombstone_count(), size_t{1});
    }

    void gc_protects_undone_insertion_with_no_deletions() {
        // Entry was inserted then undone (no deletion tombstone), only visible=false
        // because its origin is flagged in undo_map. GC must not collect it while
        // the insertion is still in the undo stack (redo would break otherwise).
        IdList list(CollabText::Test::test_counter(1));
        list.insert_after(Anchor::min(), 0xAA);  // origin (1,1), undo entry: inserted_keys={(1,1)}
        list.undo();                              // invisible now, no deletion, but insertion protected
        QCOMPARE(list.size(), size_t{0});
        QCOMPARE(list.tombstone_count(), size_t{1});
        QCOMPARE(list.collect_garbage(), size_t{0});  // must NOT collect — redo still possible

        // Dropping this stack does not prove remote restoration/anchor retirement.
        QVERIFY((list.set_max_undo_depth(0)).ok());
        QVERIFY((list.set_max_undo_depth(1000)).ok());
        QCOMPARE(list.collect_garbage(), size_t{0});
        QCOMPARE(list.tombstone_count(), size_t{1});
    }

    void restored_reclamation_frontier_rejects_late_remote_undo() {
        IdList alice(CollabText::Test::test_counter(1));
        IdList bob(CollabText::Test::test_counter(2));
        auto insert = alice.insert_after(Anchor::min(), 0xAA);
        QCOMPARE(bob.apply_remote_op_checked(insert).kind(),
                 CollabText::ApplyOutcomeKind::applied);

        auto state = bob.checkpoint_state();
        state.reclamation = {
            "id-list-stream", std::string(64, 'a'), std::string(64, 'b'),
            {{actor(1), 1}}};
        QVERIFY(bob.restore_checkpoint_state(state).ok());
        QCOMPARE(bob.checkpoint_state().reclamation, state.reclamation);
        auto beyond_causal = state;
        beyond_causal.reclamation.operation_frontier[actor(1)] = 2;
        auto invalid_restore = bob.restore_checkpoint_state(beyond_causal);
        QVERIFY(!invalid_restore.ok());
        QCOMPARE(invalid_restore.error().code(),
                 CollabText::ErrorCode::counter_regression);

        auto undo = alice.undo();
        QVERIFY(undo.has_value());
        const auto rejected = bob.apply_remote_op_checked(*undo);
        QCOMPARE(rejected.kind(), CollabText::ApplyOutcomeKind::rejected);
        QVERIFY(rejected.error().has_value());
        QCOMPARE(rejected.error()->code(),
                 CollabText::ErrorCode::history_unavailable);
        QCOMPARE(bob.ids(), std::vector<uint64_t>{0xAA});
    }

    void certified_collection_reclaims_tombstone_and_rejects_late_dependencies() {
        IdList author(CollabText::Test::test_counter(1));
        IdList deleter(CollabText::Test::test_counter(2));
        IdList collected(CollabText::Test::test_counter(3));
        IdList unreclaimed(CollabText::Test::test_counter(4));
        const auto inserted = author.insert_after(Anchor::min(), 0xAA);
        QCOMPARE(collected.apply_remote_op_checked(inserted).kind(),
                 CollabText::ApplyOutcomeKind::applied);
        QCOMPARE(unreclaimed.apply_remote_op_checked(inserted).kind(),
                 CollabText::ApplyOutcomeKind::applied);
        QCOMPARE(deleter.apply_remote_op_checked(inserted).kind(),
                 CollabText::ApplyOutcomeKind::applied);
        const Identity origin(actor(1), 1);
        const auto removed = deleter.remove_at(Anchor(origin, Bias::Left));
        QCOMPARE(collected.apply_remote_op_checked(removed).kind(),
                 CollabText::ApplyOutcomeKind::applied);
        QCOMPARE(unreclaimed.apply_remote_op_checked(removed).kind(),
                 CollabText::ApplyOutcomeKind::applied);
        const auto undo = deleter.undo();
        QVERIFY(undo.has_value());
        QCOMPARE(collected.apply_remote_op_checked(*undo).kind(),
                 CollabText::ApplyOutcomeKind::applied);
        QCOMPARE(unreclaimed.apply_remote_op_checked(*undo).kind(),
                 CollabText::ApplyOutcomeKind::applied);
        const auto redo = deleter.redo();
        QVERIFY(redo.has_value());
        QCOMPARE(collected.apply_remote_op_checked(*redo).kind(),
                 CollabText::ApplyOutcomeKind::applied);
        QCOMPARE(unreclaimed.apply_remote_op_checked(*redo).kind(),
                 CollabText::ApplyOutcomeKind::applied);
        QCOMPARE(collected.ids(), std::vector<uint64_t>{});
        QCOMPARE(collected.tombstone_count(), size_t{1});

        auto checkpoint = collected.checkpoint_state();
        checkpoint.reclamation = {
            "id-list-stream", std::string(64, 'a'), std::string(64, 'b'),
            {{actor(1), checkpoint.version.get(actor(1))},
             {actor(2), checkpoint.version.get(actor(2))}}};
        QVERIFY(collected.restore_checkpoint_state(checkpoint).ok());
        QCOMPARE(collected.collect_garbage(std::set<Identity>{origin}).value(),
                 size_t{0});
        QCOMPARE(collected.tombstone_count(), size_t{1});
        auto reclaimed = collected.collect_garbage({});
        QVERIFY(reclaimed.ok());
        QCOMPARE(reclaimed.value(), size_t{1});
        QCOMPARE(collected.entry_count(), size_t{0});
        QCOMPARE(collected.tombstone_count(), size_t{0});
        QVERIFY(collected.checkpoint_state().undo_entries.empty());

        const auto late_undo = deleter.undo();
        QVERIFY(late_undo.has_value());
        const auto refused_undo = collected.apply_remote_op_checked(*late_undo);
        QCOMPARE(refused_undo.kind(), CollabText::ApplyOutcomeKind::rejected);
        QVERIFY(refused_undo.error().has_value());
        QCOMPARE(refused_undo.error()->code(),
                 CollabText::ErrorCode::history_unavailable);
        QCOMPARE(collected.ids(), std::vector<uint64_t>{});

        IdList placement_peer(CollabText::Test::test_counter(5));
        const auto placement_history = placement_peer.apply_ops(
            {inserted, removed, *undo, *redo});
        QCOMPARE(placement_history.size(), size_t(4));
        for (const auto &outcome : placement_history)
            QCOMPARE(outcome.kind(), CollabText::ApplyOutcomeKind::applied);
        const auto late_insert = placement_peer.insert_after(
            Anchor(origin, Bias::Left), 0xBB);
        const auto refused_insert = collected.apply_remote_op_checked(late_insert);
        QCOMPARE(refused_insert.kind(), CollabText::ApplyOutcomeKind::rejected);
        QVERIFY(refused_insert.error().has_value());
        QCOMPARE(refused_insert.error()->code(),
                 CollabText::ErrorCode::history_unavailable);
        QCOMPARE(unreclaimed.apply_remote_op_checked(*late_undo).kind(),
                 CollabText::ApplyOutcomeKind::applied);
        QCOMPARE(unreclaimed.apply_remote_op_checked(late_insert).kind(),
                 CollabText::ApplyOutcomeKind::applied);
        const std::vector<uint64_t> expected_ids{0xAA, 0xBB};
        QCOMPARE(unreclaimed.ids(), expected_ids);

        IdList removal_peer(CollabText::Test::test_counter(6));
        const auto removal_history = removal_peer.apply_ops(
            {inserted, removed, *undo, *redo});
        QCOMPARE(removal_history.size(), size_t(4));
        for (const auto &outcome : removal_history)
            QCOMPARE(outcome.kind(), CollabText::ApplyOutcomeKind::applied);
        const auto late_remove =
            removal_peer.remove_at(Anchor(origin, Bias::Left));
        const auto refused_remove =
            collected.apply_remote_op_checked(late_remove);
        QCOMPARE(refused_remove.kind(), CollabText::ApplyOutcomeKind::rejected);
        QVERIFY(refused_remove.error().has_value());
        QCOMPARE(refused_remove.error()->code(),
                 CollabText::ErrorCode::history_unavailable);
        QCOMPARE(unreclaimed.apply_remote_op_checked(late_remove).kind(),
                 CollabText::ApplyOutcomeKind::applied);
    }

    void certified_collection_preserves_independent_root_convergence() {
        IdList author(CollabText::Test::test_counter(1));
        IdList deleter(CollabText::Test::test_counter(2));
        IdList collected(CollabText::Test::test_counter(3));
        IdList reference(CollabText::Test::test_counter(4));

        const auto inserted = author.insert_after(Anchor::min(), 0xAA);
        for (IdList *replica : {&deleter, &collected, &reference})
            QCOMPARE(replica->apply_remote_op_checked(inserted).kind(),
                     CollabText::ApplyOutcomeKind::applied);
        const auto removed = deleter.remove_at(
            Anchor(Identity(actor(1), 1), Bias::Left));
        for (IdList *replica : {&collected, &reference})
            QCOMPARE(replica->apply_remote_op_checked(removed).kind(),
                     CollabText::ApplyOutcomeKind::applied);

        auto checkpoint = collected.checkpoint_state();
        checkpoint.reclamation = {
            "id-list-stream", std::string(64, '1'), std::string(64, '2'),
            {{actor(1), 1}, {actor(2), checkpoint.version.get(actor(2))}}};
        QVERIFY(collected.restore_checkpoint_state(checkpoint).ok());
        auto reclaimed = collected.collect_garbage({});
        QVERIFY(reclaimed.ok());
        QCOMPARE(reclaimed.value(), size_t{1});
        QCOMPARE(collected.entry_count(), size_t{0});
        QCOMPARE(reference.entry_count(), size_t{1});

        IdList left_writer(CollabText::Test::test_counter(5));
        IdList right_writer(CollabText::Test::test_counter(6));
        const auto left_root =
            left_writer.insert_after(Anchor::min(), 0xB0);
        const auto right_root =
            right_writer.insert_after(Anchor::min(), 0xC0);
        QCOMPARE(collected.apply_remote_op_checked(left_root).kind(),
                 CollabText::ApplyOutcomeKind::applied);
        QCOMPARE(collected.apply_remote_op_checked(right_root).kind(),
                 CollabText::ApplyOutcomeKind::applied);
        QCOMPARE(reference.apply_remote_op_checked(right_root).kind(),
                 CollabText::ApplyOutcomeKind::applied);
        QCOMPARE(reference.apply_remote_op_checked(left_root).kind(),
                 CollabText::ApplyOutcomeKind::applied);

        QCOMPARE(collected.ids(), reference.ids());
        QCOMPARE(collected.ids().size(), size_t{2});
    }

    void certified_collection_preserves_local_history_and_live_placements() {
        IdList local(CollabText::Test::test_counter(8));
        local.insert_after(Anchor::min(), 0x10);
        local.remove_at(Anchor(Identity(actor(8), 1), Bias::Left));
        auto local_checkpoint = local.checkpoint_state();
        local_checkpoint.reclamation = {
            "id-list-stream", std::string(64, 'c'), std::string(64, 'd'),
            {{actor(8), local_checkpoint.version.get(actor(8))}}};
        QVERIFY(local.restore_checkpoint_state(local_checkpoint).ok());
        auto local_result = local.collect_garbage({});
        QVERIFY(local_result.ok());
        QCOMPARE(local_result.value(), size_t{0});
        QCOMPARE(local.tombstone_count(), size_t{1});

        IdList author(CollabText::Test::test_counter(9));
        IdList deleter(CollabText::Test::test_counter(10));
        IdList collected(CollabText::Test::test_counter(11));
        const auto first = author.insert_after(Anchor::min(), 0x20);
        const auto child = author.insert_after(
            Anchor(Identity(actor(9), 1), Bias::Left), 0x21);
        for (const auto &operation : {first, child}) {
            QCOMPARE(deleter.apply_remote_op_checked(operation).kind(),
                     CollabText::ApplyOutcomeKind::applied);
            QCOMPARE(collected.apply_remote_op_checked(operation).kind(),
                     CollabText::ApplyOutcomeKind::applied);
        }
        const Identity first_origin(actor(9), 1);
        const auto remove_first =
            deleter.remove_at(Anchor(first_origin, Bias::Left));
        QCOMPARE(collected.apply_remote_op_checked(remove_first).kind(),
                 CollabText::ApplyOutcomeKind::applied);
        auto checkpoint = collected.checkpoint_state();
        checkpoint.reclamation = {
            "id-list-stream", std::string(64, 'e'), std::string(64, 'f'),
            {{actor(9), checkpoint.version.get(actor(9))},
             {actor(10), checkpoint.version.get(actor(10))}}};
        QVERIFY(collected.restore_checkpoint_state(checkpoint).ok());
        auto first_pass = collected.collect_garbage({});
        QVERIFY(first_pass.ok());
        QCOMPARE(first_pass.value(), size_t{0});

        const auto remove_child = deleter.remove_at(
            Anchor(Identity(actor(9), 2), Bias::Left));
        QCOMPARE(collected.apply_remote_op_checked(remove_child).kind(),
                 CollabText::ApplyOutcomeKind::applied);
        checkpoint = collected.checkpoint_state();
        checkpoint.reclamation.operation_frontier[actor(10)] =
            checkpoint.version.get(actor(10));
        QVERIFY(collected.restore_checkpoint_state(checkpoint).ok());
        auto child_pass = collected.collect_garbage({});
        QVERIFY(child_pass.ok());
        QCOMPARE(child_pass.value(), size_t{1});
        QCOMPARE(collected.entry_count(), size_t{1});
        auto parent_pass = collected.collect_garbage({});
        QVERIFY(parent_pass.ok());
        QCOMPARE(parent_pass.value(), size_t{1});
        QCOMPARE(collected.entry_count(), size_t{0});
    }
};

QTEST_GUILESS_MAIN(TestIdListGc)
#include "tst_idlist_gc.moc"
