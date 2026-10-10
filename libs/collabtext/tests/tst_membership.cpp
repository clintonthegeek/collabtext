#include <QTemporaryDir>
#include <QTest>

#include "storage/MembershipStore.h"

#include <fstream>

using namespace CollabText;
using namespace CollabText::Storage;

namespace {
ActorId actor(uint8_t seed) {
  std::array<uint8_t, ActorId::kBytes> bytes{};
  bytes[0] = seed;
  bytes[15] = static_cast<uint8_t>(seed + 1);
  return ActorId::from_bytes(bytes);
}
}

class TestMembership final : public QObject {
  Q_OBJECT
private slots:
  void persistsExplicitProducerEvents() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = std::filesystem::path(dir.path().toStdString()) / "member.log";
    const auto producer = actor(1), peer = actor(2);
    {
      auto opened = MembershipStore::open(path, producer);
      QVERIFY(opened.ok());
      auto store = std::move(opened.value());
      QVERIFY(store.append({1, 1, peer, MembershipEvent::Kind::Enrolled, "signed\nproof"}).ok());
      QVERIFY(store.append({2, 2, peer, MembershipEvent::Kind::Retired, "confirmed"}).ok());
      QVERIFY(!store.append({4, 3, peer, MembershipEvent::Kind::Enrolled, {}}).ok());
    }
    auto reopened = MembershipStore::open(path, producer);
    QVERIFY(reopened.ok());
    QCOMPARE(reopened.value().last_sequence(), uint64_t(2));
    auto events = reopened.value().recover();
    QVERIFY(events.ok());
    QCOMPARE(events.value().size(), size_t(2));
    QCOMPARE(events.value()[0].evidence, std::string("signed\nproof"));
    QCOMPARE(events.value()[1].kind, MembershipEvent::Kind::Retired);
    auto wrong_owner = MembershipStore::open(path, actor(9));
    QVERIFY(!wrong_owner.ok());
  }

  void refusesTornAndCorruptTailInsteadOfDroppingMembership() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = std::filesystem::path(dir.path().toStdString()) / "member.log";
    const auto producer = actor(3), peer = actor(4);
    {
      auto opened = MembershipStore::open(path, producer);
      QVERIFY(opened.ok());
      auto store = std::move(opened.value());
      QVERIFY(store.append({1, 1, peer, MembershipEvent::Kind::Enrolled, "ack"}).ok());
    }
    {
      std::ofstream out(path, std::ios::binary | std::ios::app);
      out << "CME1 torn";
    }
    auto torn = MembershipStore::open(path, producer);
    QVERIFY(!torn.ok());
    QVERIFY(torn.error().code() == ErrorCode::corrupt_record);
  }

  void reconcilesAppendThatWasWrittenBeforeSyncFailure() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto path=std::filesystem::path(dir.path().toStdString())/"member.log";
    auto producer=actor(6), peer=actor(7);
    bool fail_sync=true;
    StepHook hook;
    hook.fail=[&](Step step,const fs::path&) -> std::optional<Error> {
      if(step==Step::FileSync && fail_sync) {
        fail_sync=false;
        return Error(ErrorCode::io_error,"injected sync failure");
      }
      return std::nullopt;
    };
    auto opened=MembershipStore::open(path,producer,&hook); QVERIFY(opened.ok());
    auto store=std::move(opened.value());
    auto result=store.append({1,1,peer,MembershipEvent::Kind::Enrolled,"ack"});
    QVERIFY(!result.ok());
    // The append reached the file before fsync failed. The store has reconciled
    // its sequence from that complete record, so retry cannot reuse sequence 1.
    QCOMPARE(store.last_sequence(),uint64_t(1));
    QVERIFY(store.append({2,2,peer,MembershipEvent::Kind::Retired,"confirmed"}).ok());
    auto recovered=store.recover(); QVERIFY(recovered.ok());
    QCOMPARE(recovered.value().size(),size_t(2));
  }
};

QTEST_GUILESS_MAIN(TestMembership)
#include "tst_membership.moc"
