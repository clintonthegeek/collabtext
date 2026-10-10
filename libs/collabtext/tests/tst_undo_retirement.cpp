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
};

QTEST_GUILESS_MAIN(TestUndoRetirement)
#include "tst_undo_retirement.moc"
