#include <QTemporaryDir>
#include <QTest>

#include "storage/RetentionState.h"

#include <fstream>
#include <stdexcept>

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

  void multiStreamTrimPublishesRetryableDurablePrefix() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto path = std::filesystem::path(dir.path().toStdString()) / "multi.log";
    const Crdt::Identity first{actor(31), 4}, second{actor(32), 9};
    unsigned writes = 0;
    StepHook hook;
    hook.fail = [&](Step step, const fs::path &) -> std::optional<Error> {
      if (step == Step::Write && ++writes == 2)
        return Error(ErrorCode::io_error, "injected second-stream append failure");
      return std::nullopt;
    };
    const std::vector<StreamUndoRetirement> batch{
        {"stream-z", {second}}, {"stream-a", {first}}};
    unsigned trim_commits = 0;
    {
      auto opened = RetentionState::open(path, actor(30), &hook);
      QVERIFY(opened.ok());
      auto state = std::move(opened).value();
      auto thrown = state.publish_after_durable_trim(batch, []() -> Result<void> {
        throw std::runtime_error("trim provider interrupted");
      });
      QVERIFY(!thrown.ok());
      QCOMPARE(thrown.error().code(), ErrorCode::io_error);
      QVERIFY(!state.is_retired("stream-a", first));
      auto partial = state.publish_after_durable_trim(batch, [&]() -> Result<void> {
        ++trim_commits;
        return {};
      });
      QVERIFY(!partial.ok());
      QCOMPARE(trim_commits, 1u);
      QVERIFY(state.is_retired("stream-a", first));
      QVERIFY(!state.is_retired("stream-z", second));
    }

    auto reopened = RetentionState::open(path, actor(30), &hook);
    QVERIFY(reopened.ok());
    auto state = std::move(reopened).value();
    QVERIFY(state.is_retired("stream-a", first));
    QVERIFY(!state.is_retired("stream-z", second));
    auto retry = state.publish_after_durable_trim(batch, [&]() -> Result<void> {
      ++trim_commits; // consumer trim persistence is idempotently retried
      return {};
    });
    QVERIFY(retry.ok());
    QCOMPARE(trim_commits, 2u);
    QVERIFY(state.is_retired("stream-a", first));
    QVERIFY(state.is_retired("stream-z", second));

    auto invalid = state.publish_after_durable_trim(
        std::vector<StreamUndoRetirement>{{"duplicate", {first}},
                                          {"duplicate", {second}}},
        [&]() -> Result<void> {
          ++trim_commits;
          return {};
        });
    QVERIFY(!invalid.ok());
    QCOMPARE(invalid.error().code(), ErrorCode::malformed);
    QCOMPARE(trim_commits, 2u);
  }
};

QTEST_GUILESS_MAIN(TestUndoRetirement)
#include "tst_undo_retirement.moc"
