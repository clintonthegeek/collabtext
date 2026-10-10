#include <QTemporaryDir>
#include <QTest>

#include "storage/RetentionState.h"

#include <fstream>

using namespace CollabText;
using namespace CollabText::Storage;

namespace {
ActorId actor(uint8_t seed) {
  std::array<uint8_t, ActorId::kBytes> bytes{};
  bytes[0]=seed; bytes[15]=static_cast<uint8_t>(seed+1);
  return ActorId::from_bytes(bytes);
}
}

class TestUndoRetirement final : public QObject {
  Q_OBJECT
private slots:
  void trimMustCommitBeforeMonotonicRetirementPublication() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    auto path=std::filesystem::path(dir.path().toStdString())/"retention.log";
    const Crdt::Identity group{actor(2),17};
    {
      auto opened=RetentionState::open(path,actor(1)); QVERIFY(opened.ok());
      auto state=std::move(opened.value());
      auto competing=RetentionState::open(path,actor(1));
      QVERIFY(!competing.ok()); QCOMPARE(competing.error().code(),ErrorCode::writer_conflict);
      bool committed=false;
      auto refused=state.publish_after_durable_trim("doc-a",{group},[&]() -> Result<void> {
        return Error(ErrorCode::io_error,"history commit failed");
      });
      QVERIFY(!refused.ok()); QVERIFY(!state.is_retired("doc-a",group));
      auto published=state.publish_after_durable_trim("doc-a",{group},[&]() -> Result<void> {
        committed=true; return {};
      });
      QVERIFY(published.ok()); QVERIFY(committed); QVERIFY(state.is_retired("doc-a",group));
      // Repeated acknowledgement is idempotent and cannot lower the floor.
      QVERIFY(state.publish_after_durable_trim("doc-a",{group},[]() -> Result<void> { return {}; }).ok());
      QVERIFY(!state.is_retired("doc-b",group));
    }
    auto reopened=RetentionState::open(path,actor(1)); QVERIFY(reopened.ok());
    QVERIFY(reopened.value().is_retired("doc-a",group));
    QVERIFY(!reopened.value().is_retired("doc-b",group));
  }

  void refusesIncompleteJournalTail() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    auto path=std::filesystem::path(dir.path().toStdString())/"retention.log";
    auto opened=RetentionState::open(path,actor(4)); QVERIFY(opened.ok());
    auto state=std::move(opened.value());
    QVERIFY(state.publish_after_durable_trim("doc",{{actor(5),3}},[]() -> Result<void> { return {}; }).ok());
    { std::ofstream out(path,std::ios::binary|std::ios::app); out<<"CRS1 torn"; }
    QVERIFY(!RetentionState::open(path,actor(4)).ok());
  }

  void reconcilesRetirementWrittenBeforeSyncFailure() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto path=std::filesystem::path(dir.path().toStdString())/"uncertain.log";
    bool fail_sync=true;
    StepHook hook;
    hook.fail=[&](Step step,const fs::path&) -> std::optional<Error> {
      if(step==Step::FileSync && fail_sync) {
        fail_sync=false;
        return Error(ErrorCode::io_error,"injected retention sync failure");
      }
      return std::nullopt;
    };
    const Crdt::Identity first{actor(6),21}, second{actor(7),22};
    {
      auto opened=RetentionState::open(path,actor(5),&hook); QVERIFY(opened.ok());
      auto state=std::move(opened.value());
      bool trim_committed=false;
      auto uncertain=state.publish_after_durable_trim("doc",{first},[&]() -> Result<void> {
        trim_committed=true; return {};
      });
      QVERIFY(trim_committed); QVERIFY(!uncertain.ok());
      // The complete line reached disk before sync failed, so reopening the
      // journal must recover it and the next record must use sequence two.
      QVERIFY(state.is_retired("doc",first));
      auto next=state.publish_after_durable_trim("doc",{second},[]() -> Result<void> { return {}; });
      QVERIFY(next.ok());
      QVERIFY(state.is_retired("doc",first));
      QVERIFY(state.is_retired("doc",second));
    }
    auto reopened=RetentionState::open(path,actor(5)); QVERIFY(reopened.ok());
    QVERIFY(reopened.value().is_retired("doc",first));
    QVERIFY(reopened.value().is_retired("doc",second));
  }
};

QTEST_GUILESS_MAIN(TestUndoRetirement)
#include "tst_undo_retirement.moc"
