#include <collabtext/CrdtEngine.h>
#include <collabtext/IdListEngine.h>

#include "TestActor.h"

#include <QTest>

using namespace CollabText;

namespace {

PositionScope text_scope() { return {"reset-doc", "text", Primitive::text, 1}; }

PositionScope list_scope() {
  return {"reset-doc", "items", Primitive::list, 1};
}

} // namespace

class ResetSemanticsTest final : public QObject {
  Q_OBJECT
private slots:
  void discardLocalTextViewPreservesCoverageAndInvalidatesPositions() {
    auto allocator = Test::test_counter(21);
    auto created = CrdtEngine::create(allocator, text_scope());
    QVERIFY(created);
    auto engine = std::move(created.value());
    auto empty_position = engine->boundary_at(0, Affinity::right);
    QVERIFY(empty_position);

    std::vector<Crdt::Operation> retained_log;
    engine->setOnLocalOp([&](const Crdt::Operation &operation) {
      retained_log.push_back(operation);
    });
    auto inserted = engine->try_replace(0, 0, "saved");
    QVERIFY(inserted);
    QCOMPARE(retained_log.size(), size_t(1));
    const auto next_counter = allocator->peek();
    int changes = 0;
    engine->setOnChange([&] { ++changes; });

    engine->discard_local_view();
    QCOMPARE(engine->text(), std::string{});
    QCOMPARE(allocator->peek(), next_counter);
    QCOMPARE(changes, 0);
    auto stale_position = engine->resolve(empty_position.value());
    QVERIFY(!stale_position);
    QCOMPARE(stale_position.error().code(), ErrorCode::history_unavailable);

    const auto replayed = engine->apply_remote(retained_log.front());
    QCOMPARE(replayed.kind(), ApplyOutcomeKind::duplicate);
    QCOMPARE(engine->text(), std::string{});

    auto recovered = CrdtEngine::create(Test::test_counter(22), text_scope());
    QVERIFY(recovered);
    QCOMPARE(recovered.value()->apply_remote(retained_log.front()).kind(),
             ApplyOutcomeKind::applied);
    QCOMPARE(recovered.value()->text(), std::string("saved"));

    auto fresh_edit = engine->try_replace(0, 0, "new");
    QVERIFY(fresh_edit);
    QVERIFY(Crdt::op_identity(fresh_edit.value()) >
            Crdt::op_identity(retained_log.front()));
    QCOMPARE(changes, 1);
    QCOMPARE(retained_log.size(), size_t(2));
  }

  void discardLocalListViewPreservesCoverageCallbacksAndRetirementQueue() {
    auto allocator = Test::test_counter(23);
    auto created = IdListEngine::create(allocator, list_scope());
    QVERIFY(created);
    auto list = std::move(created.value());
    auto empty_position = list->boundary_at(0, Affinity::left);
    QVERIFY(empty_position);
    auto inserted = list->try_insert_after(Crdt::Anchor::min(), 42);
    QVERIFY(inserted);
    QVERIFY(list->set_max_undo_depth(0));
    QCOMPARE(list->local_history_snapshot().retired_entries.size(), size_t(1));
    const auto old_version = list->version();
    const auto old_actor = list->actor_id();

    int changes = 0;
    int local_operations = 0;
    list->set_on_change([&] { ++changes; });
    list->set_on_local_op(
        [&](const Crdt::IdListOperation &) { ++local_operations; });
    list->discard_local_view();

    QVERIFY(list->ids().empty());
    QCOMPARE(list->entry_count(), size_t(0));
    QCOMPARE(list->actor_id(), old_actor);
    QCOMPARE(list->version(), old_version);
    QCOMPARE(list->undo_depth(), size_t(0));
    QCOMPARE(list->local_history_snapshot().retired_entries.size(), size_t(1));
    QCOMPARE(changes, 0);
    QCOMPARE(local_operations, 0);
    auto stale_position = list->resolve(empty_position.value());
    QVERIFY(!stale_position);
    QCOMPARE(stale_position.error().code(), ErrorCode::history_unavailable);

    const auto duplicate = list->apply_ops({inserted.value()});
    QCOMPARE(duplicate.front().kind(), ApplyOutcomeKind::duplicate);
    QVERIFY(list->ids().empty());

    auto recovered = IdListEngine::create(Test::test_counter(24), list_scope());
    QVERIFY(recovered);
    QCOMPARE(recovered.value()->apply_ops({inserted.value()}).front().kind(),
             ApplyOutcomeKind::applied);
    QCOMPARE(recovered.value()->ids(), (std::vector<uint64_t>{42}));

    auto next = list->try_insert_after(Crdt::Anchor::min(), 99);
    QVERIFY(next);
    QVERIFY(Crdt::idlist_op_identity(next.value()) >
            Crdt::idlist_op_identity(inserted.value()));
    QCOMPARE(changes, 1);
    QCOMPARE(local_operations, 1);
  }

  void collaborativeTextClearConvergesAndFreshReplayReconstructsIt() {
    auto writer = CrdtEngine::create(Test::test_counter(25), text_scope());
    auto peer = CrdtEngine::create(Test::test_counter(26), text_scope());
    auto restarted = CrdtEngine::create(Test::test_counter(27), text_scope());
    QVERIFY(writer && peer && restarted);

    auto insert = writer.value()->try_replace(0, 0, "shared");
    QVERIFY(insert);
    QCOMPARE(peer.value()->apply_remote(insert.value()).kind(),
             ApplyOutcomeKind::applied);
    auto clear = writer.value()->try_replace(0, 6, "");
    QVERIFY(clear);
    QCOMPARE(peer.value()->apply_remote(clear.value()).kind(),
             ApplyOutcomeKind::applied);
    QCOMPARE(writer.value()->text(), std::string{});
    QCOMPARE(peer.value()->text(), std::string{});

    QCOMPARE(restarted.value()->apply_remote(insert.value()).kind(),
             ApplyOutcomeKind::applied);
    QCOMPARE(restarted.value()->apply_remote(clear.value()).kind(),
             ApplyOutcomeKind::applied);
    QCOMPARE(restarted.value()->text(), std::string{});
  }

  void collaborativeListClearUsesRemovalsAndFreshReplayReconstructsIt() {
    auto writer = IdListEngine::create(Test::test_counter(28), list_scope());
    auto peer = IdListEngine::create(Test::test_counter(29), list_scope());
    auto restarted = IdListEngine::create(Test::test_counter(30), list_scope());
    QVERIFY(writer && peer && restarted);

    std::vector<Crdt::IdListOperation> log;
    auto first = writer.value()->try_insert_after(Crdt::Anchor::min(), 10);
    QVERIFY(first);
    log.push_back(first.value());
    auto first_anchor = writer.value()->try_anchor_of(10);
    QVERIFY(first_anchor);
    auto second = writer.value()->try_insert_after(first_anchor.value(), 20);
    QVERIFY(second);
    log.push_back(second.value());
    for (const auto &operation : log)
      QCOMPARE(peer.value()->apply_ops({operation}).front().kind(),
               ApplyOutcomeKind::applied);

    const auto visible_values = writer.value()->ids();
    for (const auto id : visible_values) {
      auto anchor = writer.value()->try_anchor_of(id);
      QVERIFY(anchor);
      auto removal = writer.value()->try_remove_at(anchor.value());
      QVERIFY(removal);
      log.push_back(removal.value());
      QCOMPARE(peer.value()->apply_ops({removal.value()}).front().kind(),
               ApplyOutcomeKind::applied);
    }
    QVERIFY(writer.value()->ids().empty());
    QVERIFY(peer.value()->ids().empty());

    for (const auto &operation : log)
      QCOMPARE(restarted.value()->apply_ops({operation}).front().kind(),
               ApplyOutcomeKind::applied);
    QVERIFY(restarted.value()->ids().empty());
  }
};

QTEST_GUILESS_MAIN(ResetSemanticsTest)
#include "tst_reset_semantics.moc"
