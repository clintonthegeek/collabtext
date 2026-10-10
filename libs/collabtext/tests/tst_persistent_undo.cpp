#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <optional>
#include <vector>

#include "crdt/Buffer.h"
#include "crdt/IdList.h"
#include "storage/LocalHistoryLog.h"
#include "TestActor.h"

using namespace CollabText;
using namespace CollabText::Crdt;

namespace {
class ToggleAllocator final : public IdentityAllocator {
public:
  explicit ToggleAllocator(ActorId owner) : m_owner(std::move(owner)) {}
  Result<uint64_t> reserve_run(uint64_t count) override {
    if (fail_reservations)
      return Error(ErrorCode::counter_exhausted, "injected identity exhaustion");
    if (count > UINT64_MAX - m_next)
      return Error(ErrorCode::counter_exhausted, "identity counter exhausted");
    const auto base = m_next;
    m_next += count;
    return base;
  }
  void observe(uint64_t counter) override {
    if (counter >= m_next && counter != UINT64_MAX) m_next = counter + 1;
  }
  uint64_t peek() const noexcept override { return m_next; }
  const ActorId &actor() const noexcept override { return m_owner; }
  bool fail_reservations = false;

private:
  ActorId m_owner;
  uint64_t m_next = 1;
};
} // namespace

class TestPersistentUndo : public QObject {
  Q_OBJECT
private slots:
  void snapshot_restores_undo_and_redo_cursor_after_replay() {
    Buffer original(Test::test_counter(9));
    auto edit = original.try_apply_local_edit({{0, 0}}, {"abc"});
    QVERIFY(edit.ok());
    const auto snapshot = original.local_history_snapshot();

    Buffer replayed(Test::test_counter(9, 100));
    const auto applied = replayed.apply_ops({edit.value()});
    QCOMPARE(applied.size(), size_t(1));
    QCOMPARE(applied[0].kind(), ApplyOutcomeKind::applied);
    QVERIFY(replayed.restore_local_history(snapshot).ok());
    QCOMPARE(replayed.undo_depth(), size_t(1));

    auto undo = replayed.undo();
    QVERIFY(undo.has_value());
    QCOMPARE(replayed.text(), std::string{});
    auto redo = replayed.redo();
    QVERIFY(redo.has_value());
    QCOMPARE(replayed.text(), std::string("abc"));
  }

  void idlist_snapshot_restores_undo_and_redo_after_replay() {
    IdList original(Test::test_counter(9));
    auto insert = original.try_insert_after(Anchor::min(), 42);
    QVERIFY(insert.ok());
    const auto snapshot = original.local_history_snapshot();

    IdList replayed(Test::test_counter(9, 100));
    const auto outcomes = replayed.apply_ops({insert.value()});
    QCOMPARE(outcomes.size(), size_t(1));
    QCOMPARE(outcomes[0].kind(), ApplyOutcomeKind::applied);
    QVERIFY(replayed.restore_local_history(snapshot).ok());
    auto undo = replayed.undo();
    QVERIFY(undo.has_value());
    QCOMPARE(replayed.size(), uint32_t(0));
    auto redo = replayed.redo();
    QVERIFY(redo.has_value());
    QCOMPARE(replayed.ids(), std::vector<uint64_t>{42});
  }

  void idlist_history_obeys_retained_byte_budget() {
    IdList list(Test::test_counter(9));
    list.set_max_undo_bytes(2 * (sizeof(uint64_t) + sizeof(UndoMapKey)));
    QVERIFY(list.try_insert_after(Anchor::min(), 1).ok());
    QVERIFY(list.try_insert_after(Anchor::min(), 2).ok());
    QCOMPARE(list.undo_depth(), size_t(2));
    list.set_max_undo_bytes(sizeof(uint64_t) + sizeof(UndoMapKey));
    QCOMPARE(list.undo_depth(), size_t(1));
    QVERIFY(list.undo_history_bytes() <= list.max_undo_bytes());
  }

  void restore_rejects_history_not_covered_by_replay() {
    Buffer buffer(Test::test_counter(9));
    LocalHistorySnapshot forged;
    forged.cursor = 1;
    LocalUndoEntrySnapshot entry;
    entry.inserted_items.push_back(Test::id(9, 1));
    forged.entries.push_back(entry);
    const auto result = buffer.restore_local_history(forged);
    QVERIFY(!result.ok());
    QCOMPARE(result.error().code(), ErrorCode::history_unavailable);
    QCOMPARE(buffer.undo_depth(), size_t(0));
  }

  void history_budget_trims_old_steps_and_refuses_one_oversized_edit() {
    auto allocator = Test::test_counter(9);
    Buffer buffer(allocator);
    buffer.set_max_undo_bytes(80);
    QVERIFY(buffer.try_apply_local_edit({{0, 0}}, {"a"}).ok());
    QVERIFY(buffer.try_apply_local_edit({{1, 1}}, {"b"}).ok());
    QCOMPARE(buffer.undo_depth(), size_t(2));
    buffer.set_max_undo_bytes(40);
    QCOMPARE(buffer.undo_depth(), size_t(1));
    QVERIFY(buffer.undo_history_bytes() <= buffer.max_undo_bytes());
    auto undo = buffer.undo();
    QVERIFY(undo.has_value());
    QCOMPARE(buffer.text(), std::string("a"));

    Buffer limited(Test::test_counter(10));
    limited.set_max_undo_bytes(32);
    auto refused = limited.try_apply_local_edit({{0, 0}}, {"too large"});
    QVERIFY(!refused.ok());
    QCOMPARE(refused.error().code(), ErrorCode::limit_exceeded);
    QCOMPARE(limited.text(), std::string{});
  }

  void checked_undo_redo_preserve_reservation_errors_and_cursor() {
    auto buffer_allocator = std::make_shared<ToggleAllocator>(Test::actor(9));
    Buffer buffer(buffer_allocator);
    QVERIFY(buffer.try_apply_local_edit({{0, 0}}, {"x"}).ok());
    buffer_allocator->fail_reservations = true;
    auto buffer_undo = buffer.try_undo();
    QVERIFY(!buffer_undo.ok());
    QCOMPARE(buffer_undo.error().code(), ErrorCode::counter_exhausted);
    QCOMPARE(buffer.undo_depth(), size_t(1));
    QCOMPARE(buffer.text(), std::string("x"));
    buffer_allocator->fail_reservations = false;
    QVERIFY(buffer.try_undo().value().has_value());
    buffer_allocator->fail_reservations = true;
    auto buffer_redo = buffer.try_redo();
    QVERIFY(!buffer_redo.ok());
    QCOMPARE(buffer_redo.error().code(), ErrorCode::counter_exhausted);
    QCOMPARE(buffer.undo_depth(), size_t(0));
    QCOMPARE(buffer.text(), std::string{});

    auto list_allocator = std::make_shared<ToggleAllocator>(Test::actor(10));
    IdList list(list_allocator);
    QVERIFY(list.try_insert_after(Anchor::min(), 99).ok());
    list_allocator->fail_reservations = true;
    auto list_undo = list.try_undo();
    QVERIFY(!list_undo.ok());
    QCOMPARE(list_undo.error().code(), ErrorCode::counter_exhausted);
    QCOMPARE(list.undo_depth(), size_t(1));
    QCOMPARE(list.ids(), std::vector<uint64_t>{99});
    list_allocator->fail_reservations = false;
    QVERIFY(list.try_undo().value().has_value());
    list_allocator->fail_reservations = true;
    auto list_redo = list.try_redo();
    QVERIFY(!list_redo.ok());
    QCOMPARE(list_redo.error().code(), ErrorCode::counter_exhausted);
    QCOMPARE(list.undo_depth(), size_t(0));
    QCOMPARE(list.size(), uint32_t(0));
  }

  void durable_history_log_recovers_latest_complete_snapshot() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const auto path = std::filesystem::path(temp.path().toStdString()) / "undo.log";
    const auto owner = Test::actor(9);
    Crdt::LocalHistorySnapshot saved;
    saved.cursor = 1;
    Crdt::LocalUndoEntrySnapshot entry;
    entry.inserted_items.push_back(Test::id(9, 1));
    saved.entries.push_back(entry);
    {
      auto opened = Storage::LocalHistoryLog::open(path, owner);
      QVERIFY(opened.ok());
      auto log = std::move(opened).value();
      QVERIFY(log.store_snapshot(saved).ok());
      auto newer = saved;
      newer.cursor = 0;
      QVERIFY(log.store_snapshot(newer).ok());
      QVERIFY(log.close().ok());
    }
    auto reopened = Storage::LocalHistoryLog::open(path, owner);
    QVERIFY(reopened.ok());
    auto log = std::move(reopened).value();
    auto recovered = log.recover();
    QVERIFY(recovered.ok());
    QVERIFY(recovered.value().has_value());
    QCOMPARE(recovered.value()->cursor, size_t(0));
    QCOMPARE(recovered.value()->entries.size(), size_t(1));
    QCOMPARE(recovered.value()->entries[0].inserted_items[0], Test::id(9, 1));
    QVERIFY(log.close().ok());
  }

  void pending_commit_survives_until_explicit_reconciliation() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const auto path = std::filesystem::path(temp.path().toStdString()) / "undo.log";
    auto opened = Storage::LocalHistoryLog::open(path, Test::actor(9));
    QVERIFY(opened.ok());
    auto log = std::move(opened).value();
    Crdt::LocalHistorySnapshot snapshot;
    snapshot.cursor = 0;
    Storage::PendingLocalHistoryCommit pending{7, "encoded-operation", snapshot};
    QVERIFY(log.store_pending(pending).ok());
    auto recovered = log.recover_pending();
    QVERIFY(recovered.ok());
    QVERIFY(recovered.value().has_value());
    QCOMPARE(recovered.value()->sequence, uint64_t(7));
    QCOMPARE(recovered.value()->operation, std::string("encoded-operation"));
    QCOMPARE(recovered.value()->snapshot.cursor, size_t(0));
    QVERIFY(log.clear_pending().ok());
    auto cleared = log.recover_pending();
    QVERIFY(cleared.ok());
    QVERIFY(!cleared.value().has_value());
  }

  void history_commit_fault_matrix_keeps_old_or_recoverable_state() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const auto root = std::filesystem::path(temp.path().toStdString());
    const auto owner = Test::actor(9);
    Crdt::LocalHistorySnapshot empty;
    Storage::PendingLocalHistoryCommit pending{7, "operation-bytes", empty};

    const std::vector<Storage::Step> pre_publish_steps{
        Storage::Step::Write, Storage::Step::FileSync,
        Storage::Step::Close, Storage::Step::Replace};
    for (size_t i = 0; i < pre_publish_steps.size(); ++i) {
      const auto path = root / ("pre-" + std::to_string(i)) / "history.snapshot";
      std::filesystem::create_directories(path.parent_path());
      auto fail_once = std::make_shared<bool>(false);
      Storage::StepHook hook;
      hook.fail = [step = pre_publish_steps[i], fail_once](Storage::Step actual,
                                                          const auto &) {
        if (actual == step && !*fail_once) {
          *fail_once = true;
          return std::optional<Error>(
              Error(ErrorCode::io_error, "injected checkpoint failure"));
        }
        return std::optional<Error>{};
      };
      auto opened = Storage::LocalHistoryLog::open(path, owner, &hook);
      QVERIFY(opened.ok());
      auto log = std::move(opened).value();
      QVERIFY(!log.store_pending(pending).ok());
      auto recovered = log.recover_pending();
      QVERIFY(recovered.ok());
      QVERIFY(!recovered.value().has_value());
    }

    // A directory-sync failure is after rename: the call reports uncertainty,
    // while recovery sees the complete intent and can retry it.
    const auto uncertain_path = root / "uncertain" / "history.snapshot";
    std::filesystem::create_directories(uncertain_path.parent_path());
    Storage::StepHook dir_sync_failure;
    dir_sync_failure.fail = [](Storage::Step step, const auto &) {
      if (step == Storage::Step::DirSync)
        return std::optional<Error>(Error(ErrorCode::io_error,
                                           "injected directory-sync failure"));
      return std::optional<Error>{};
    };
    auto uncertain_open =
        Storage::LocalHistoryLog::open(uncertain_path, owner, &dir_sync_failure);
    QVERIFY(uncertain_open.ok());
    auto uncertain = std::move(uncertain_open).value();
    auto publish_result = uncertain.store_pending(pending);
    QVERIFY(!publish_result.ok());
    QCOMPARE(publish_result.error().code(), ErrorCode::commit_uncertain);
    auto intent = uncertain.recover_pending();
    QVERIFY(intent.ok());
    QVERIFY(intent.value().has_value());
    QCOMPARE(intent.value()->sequence, uint64_t(7));

    // Once intent publication succeeds, a checkpoint failure leaves both the
    // previous checkpoint and the pending transaction available for replay.
    auto clean_open = Storage::LocalHistoryLog::open(uncertain_path, owner);
    QVERIFY(clean_open.ok());
    auto clean = std::move(clean_open).value();
    Crdt::LocalHistorySnapshot next;
    next.cursor = 0;
    Storage::StepHook checkpoint_failure;
    checkpoint_failure.fail = [](Storage::Step step, const auto &file_path) {
      if (step == Storage::Step::FileSync &&
          file_path.filename().string().starts_with("history.snapshot.tmp."))
        return std::optional<Error>(Error(ErrorCode::io_error,
                                           "injected history checkpoint failure"));
      return std::optional<Error>{};
    };
    auto checkpoint_open = Storage::LocalHistoryLog::open(
        uncertain_path, owner, &checkpoint_failure);
    QVERIFY(checkpoint_open.ok());
    auto checkpoint_log = std::move(checkpoint_open).value();
    QVERIFY(!checkpoint_log.store_snapshot(next).ok());
    auto still_pending = checkpoint_log.recover_pending();
    QVERIFY(still_pending.ok() && still_pending.value().has_value());
    auto old_checkpoint = checkpoint_log.recover();
    QVERIFY(old_checkpoint.ok() && !old_checkpoint.value().has_value());

    // A failed clear leaves a harmless replayable intent after checkpointing;
    // recovery can repeat publication and complete the clear.
    Storage::StepHook clear_failure;
    clear_failure.fail = [](Storage::Step step, const auto &file_path) {
      if (step == Storage::Step::FileSync &&
          file_path.filename().string().starts_with("history.snapshot.pending.tmp."))
        return std::optional<Error>(Error(ErrorCode::io_error,
                                           "injected pending-marker clear failure"));
      return std::optional<Error>{};
    };
    auto clear_open = Storage::LocalHistoryLog::open(uncertain_path, owner,
                                                     &clear_failure);
    QVERIFY(clear_open.ok());
    auto clear_log = std::move(clear_open).value();
    QVERIFY(clear_log.store_snapshot(next).ok());
    QVERIFY(!clear_log.clear_pending().ok());
    QVERIFY(clear_log.recover_pending().value().has_value());
    auto committed_checkpoint = clear_log.recover();
    QVERIFY(committed_checkpoint.ok());
    QVERIFY(committed_checkpoint.value().has_value());
    QVERIFY(clean.clear_pending().ok());
    QVERIFY(!clean.recover_pending().value().has_value());
  }
};

QTEST_GUILESS_MAIN(TestPersistentUndo)
#include "tst_persistent_undo.moc"
