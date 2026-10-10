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

  void retirementPreviewMustBeCurrentAndUnanimouslyAcknowledged() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto path=std::filesystem::path(dir.path().toStdString())/"member.log";
    const auto producer=actor(10), retiring=actor(11), member_a=actor(12), member_b=actor(13);
    auto opened=MembershipStore::open(path,producer); QVERIFY(opened.ok());
    auto store=std::move(opened.value());
    MembershipDescriptor descriptor{7,"epoch-seven-hash",{retiring,member_b,member_a}};
    std::vector<RetirementImpact> impacts{{"doc-one","text",7,4,2,"branch-hash-1"},
                                          {"doc-one","comments",7,0,0,"branch-hash-2"}};
    auto preview=preview_retirement(descriptor,retiring,impacts); QVERIFY(preview.ok());
    QCOMPARE(preview.value().next_epoch,uint64_t(8));
    QCOMPARE(preview.value().required_acknowledgers.size(),size_t(2));

    bool verifier_called=false;
    DurableAcknowledgementVerifier verifier=[&](const ActorId &who,std::string_view digest,
                                                 std::string_view signature) -> Result<void> {
      verifier_called=true;
      if(digest!=preview.value().digest || signature!="durable-ack")
        return Error(ErrorCode::corrupt_record,"ack does not bind this transition");
      if(who!=member_a && who!=member_b)
        return Error(ErrorCode::actor_conflict,"unexpected acknowledgement actor");
      return {};
    };
    auto missing=store.commit_retirement(preview.value(),descriptor,impacts,
        {{member_a,"durable-ack"}},"user-confirmation",verifier);
    QVERIFY(!missing.ok()); QVERIFY(!verifier_called); QCOMPARE(store.last_sequence(),uint64_t(0));

    auto changed_impacts=impacts;
    changed_impacts[0].recoverable_branch_hash="new-branch-hash";
    auto stale=store.commit_retirement(preview.value(),descriptor,changed_impacts,
        {{member_a,"durable-ack"},{member_b,"durable-ack"}},"user-confirmation",verifier);
    QVERIFY(!stale.ok()); QCOMPARE(stale.error().code(),ErrorCode::wrong_epoch);
    QCOMPARE(store.last_sequence(),uint64_t(0));

    auto forked_descriptor=descriptor;
    forked_descriptor.hash="concurrent-epoch-seven-hash";
    auto forked=store.commit_retirement(preview.value(),forked_descriptor,impacts,
        {{member_a,"durable-ack"},{member_b,"durable-ack"}},"user-confirmation",verifier);
    QVERIFY(!forked.ok()); QCOMPARE(forked.error().code(),ErrorCode::wrong_epoch);
    QCOMPARE(store.last_sequence(),uint64_t(0));

    DurableAcknowledgementVerifier rejected_verifier=[](
        const ActorId &,std::string_view,std::string_view)->Result<void> {
      return Error(ErrorCode::corrupt_record,"acknowledgement signature is invalid");
    };
    auto rejected=store.commit_retirement(preview.value(),descriptor,impacts,
        {{member_a,"bad-ack"},{member_b,"bad-ack"}},"user-confirmation",rejected_verifier);
    QVERIFY(!rejected.ok()); QCOMPARE(rejected.error().code(),ErrorCode::corrupt_record);
    QCOMPARE(store.last_sequence(),uint64_t(0));

    auto wrong_ack=store.commit_retirement(preview.value(),descriptor,impacts,
        {{member_a,"durable-ack"},{retiring,"durable-ack"}},"user-confirmation",verifier);
    QVERIFY(!wrong_ack.ok()); QCOMPARE(store.last_sequence(),uint64_t(0));

    auto committed=store.commit_retirement(preview.value(),descriptor,impacts,
        {{member_a,"durable-ack"},{member_b,"durable-ack"}},"user-confirmation",verifier);
    QVERIFY(committed.ok()); QVERIFY(verifier_called);
    QCOMPARE(committed.value().kind,MembershipEvent::Kind::Retired);
    QCOMPARE(committed.value().epoch,uint64_t(8));
    QVERIFY(committed.value().evidence.find("retirement-v1 ")==0);
    QVERIFY(committed.value().evidence.find(preview.value().digest)==std::string::npos);
    auto replay=store.commit_retirement(preview.value(),descriptor,impacts,
        {{member_a,"durable-ack"},{member_b,"durable-ack"}},"user-confirmation",verifier);
    QVERIFY(!replay.ok()); QCOMPARE(replay.error().code(),ErrorCode::wrong_epoch);
    QCOMPARE(store.last_sequence(),uint64_t(1));
    auto recovered=store.recover(); QVERIFY(recovered.ok());
    QCOMPARE(recovered.value().size(),size_t(1));
    QCOMPARE(recovered.value()[0].subject,retiring);
  }

  void enrollmentRequiresEveryCurrentMemberAndTheJoiningDevice() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    auto opened=MembershipStore::open(
        std::filesystem::path(dir.path().toStdString())/"enrollment.log",actor(30));
    QVERIFY(opened.ok()); auto store=std::move(opened.value());
    const auto member_a=actor(31), member_b=actor(32), joining=actor(33);
    MembershipDescriptor descriptor{4,"epoch-four-hash",{member_b,member_a}};
    auto preview=preview_enrollment(descriptor,joining); QVERIFY(preview.ok());
    QCOMPARE(preview.value().next_epoch,uint64_t(5));
    QCOMPARE(preview.value().required_acknowledgers.size(),size_t(3));
    int verified=0;
    DurableAcknowledgementVerifier verifier=[&](const ActorId &who,
        std::string_view digest,std::string_view signature)->Result<void> {
      ++verified;
      if(digest!=preview.value().digest || signature!="durable-ack")
        return Error(ErrorCode::corrupt_record,"enrollment acknowledgement is invalid");
      if(who!=member_a && who!=member_b && who!=joining)
        return Error(ErrorCode::actor_conflict,"unexpected enrollment actor");
      return {};
    };
    auto missing=store.commit_enrollment(preview.value(),descriptor,
        {{member_a,"durable-ack"},{member_b,"durable-ack"}},verifier);
    QVERIFY(!missing.ok()); QCOMPARE(missing.error().code(),ErrorCode::awaiting_state);
    QCOMPARE(store.last_sequence(),uint64_t(0)); QCOMPARE(verified,0);

    auto forked=descriptor; forked.hash="forked-epoch-four";
    auto stale=store.commit_enrollment(preview.value(),forked,
        {{member_a,"durable-ack"},{member_b,"durable-ack"},{joining,"durable-ack"}},verifier);
    QVERIFY(!stale.ok()); QCOMPARE(stale.error().code(),ErrorCode::wrong_epoch);
    QCOMPARE(store.last_sequence(),uint64_t(0));

    auto committed=store.commit_enrollment(preview.value(),descriptor,
        {{member_a,"durable-ack"},{member_b,"durable-ack"},{joining,"durable-ack"}},verifier);
    QVERIFY(committed.ok()); QCOMPARE(verified,3);
    QCOMPARE(committed.value().kind,MembershipEvent::Kind::Enrolled);
    QCOMPARE(committed.value().epoch,uint64_t(5));
    QCOMPARE(committed.value().subject,joining);
    auto replay=store.commit_enrollment(preview.value(),descriptor,
        {{member_a,"durable-ack"},{member_b,"durable-ack"},{joining,"durable-ack"}},verifier);
    QVERIFY(!replay.ok()); QCOMPARE(replay.error().code(),ErrorCode::wrong_epoch);
    QCOMPARE(store.last_sequence(),uint64_t(1));
    auto recovered=store.recover(); QVERIFY(recovered.ok());
    QCOMPARE(recovered.value().size(),size_t(1));
  }

  void cannotRetireTheLastMemberOrCommitWithoutConfirmation() {
    const auto lone=actor(20);
    MembershipDescriptor one_member{1,"epoch-one",{lone}};
    QVERIFY(!preview_retirement(one_member,lone,{}).ok());
    QTemporaryDir dir; QVERIFY(dir.isValid());
    auto opened=MembershipStore::open(std::filesystem::path(dir.path().toStdString())/"m.log",actor(21));
    QVERIFY(opened.ok());
    auto store=std::move(opened.value());
    MembershipDescriptor two_members{1,"epoch-one",{actor(22),actor(23)}};
    std::vector<RetirementImpact> impacts{{"doc","text",1,0,0,"branch"}};
    auto preview=preview_retirement(two_members,actor(22),impacts); QVERIFY(preview.ok());
    DurableAcknowledgementVerifier verifier=[](const ActorId&,std::string_view,std::string_view)->Result<void>{return {};};
    auto result=store.commit_retirement(preview.value(),two_members,impacts,
        {{actor(23),"ack"}},"",verifier);
    QVERIFY(!result.ok()); QCOMPARE(store.last_sequence(),uint64_t(0));
  }
};

QTEST_GUILESS_MAIN(TestMembership)
#include "tst_membership.moc"
