#include <QTemporaryDir>
#include <QTest>

#include "crdt/Buffer.h"
#include "storage/LocalHistoryLog.h"
#include "TestActor.h"

using namespace CollabText;
using namespace CollabText::Crdt;

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
};

QTEST_GUILESS_MAIN(TestPersistentUndo)
#include "tst_persistent_undo.moc"
