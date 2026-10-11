#include <QCoreApplication>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include "storage/MembershipStore.h"
#include "core/Sha256.h"

#include <fstream>
#include <iostream>
#include <optional>
#include <algorithm>
#include <stdexcept>

using namespace CollabText;
using namespace CollabText::Storage;

namespace {
ActorId actor(uint8_t seed) {
  std::array<uint8_t, ActorId::kBytes> bytes{};
  bytes[0] = seed;
  bytes[15] = static_cast<uint8_t>(seed + 1);
  return ActorId::from_bytes(bytes);
}

MembershipDescriptor fixture_descriptor(uint64_t epoch,
                                        std::vector<ActorId> members) {
  std::sort(members.begin(), members.end());
  MembershipDescriptor descriptor;
  descriptor.epoch = epoch;
  if (epoch > 1) {
    descriptor.parent_hash = sha256_hex("fixture-parent-" + std::to_string(epoch));
    descriptor.transition_event_hash = sha256_hex("fixture-event-" + std::to_string(epoch));
  }
  descriptor.base_checkpoint_hash = sha256_hex("fixture-checkpoint-" + std::to_string(epoch));
  descriptor.members = std::move(members);
  MembershipDescriptor::StreamFrontier frontier;
  frontier.stream = "text";
  for (const auto &member : descriptor.members) {
    frontier.operation.emplace(member, 0);
    frontier.delivery.emplace(member, 0);
  }
  descriptor.base_frontiers.push_back(std::move(frontier));
  descriptor.supported_token_versions = {1};
  descriptor.translation_manifest_hash = sha256_hex("fixture-token-manifest");
  auto sealed = seal_membership_descriptor(std::move(descriptor));
  if (!sealed.ok()) throw std::runtime_error(std::string(sealed.error().message()));
  return std::move(sealed.value());
}

std::string simulated_ack(const ActorId &who, std::string_view digest) {
  return "simulated-durable-ack:" + who.to_string() + ":" + std::string(digest);
}

std::string membership_record(const ActorId &producer, uint64_t sequence,
                              uint64_t epoch, const ActorId &subject) {
  auto body = "CME2 " + producer.to_string() + " " + std::to_string(sequence) +
      " " + std::to_string(epoch) + " " + subject.to_string() + " E " +
      sha256_hex("fixture-parent") + " 6161";
  return body + " " + sha256_hex(body) + "\n";
}
}

class TestMembership final : public QObject {
  Q_OBJECT
private slots:
  void descriptorHashBindsAllEpochContents() {
    const auto current = fixture_descriptor(3, {actor(2), actor(1)});
    QVERIFY(validate_membership_descriptor(current).ok());
    QVERIFY(std::is_sorted(current.members.begin(), current.members.end()));
    auto reordered = current;
    std::reverse(reordered.members.begin(), reordered.members.end());
    auto resealed = seal_membership_descriptor(reordered);
    QVERIFY(resealed.ok()); QCOMPARE(resealed.value().hash, current.hash);
    auto historical = current;
    historical.base_frontiers.front().operation.emplace(actor(9), 7);
    historical.base_frontiers.front().delivery.emplace(actor(9), 5);
    auto historical_sealed = seal_membership_descriptor(historical);
    QVERIFY(historical_sealed.ok());
    QVERIFY(validate_membership_descriptor(historical_sealed.value()).ok());
    QVERIFY(historical_sealed.value().hash != current.hash);

    auto changed = current;
    changed.base_checkpoint_hash = sha256_hex("different-checkpoint");
    QVERIFY(!validate_membership_descriptor(changed).ok());
    changed = current;
    changed.transition_event_hash = sha256_hex("different-event");
    QVERIFY(!validate_membership_descriptor(changed).ok());
    changed = current;
    changed.parent_hash = sha256_hex("different-parent");
    QVERIFY(!validate_membership_descriptor(changed).ok());
    changed = current;
    const auto removed_member = changed.members.back();
    changed.members.back() = actor(3);
    changed.base_frontiers.front().operation.erase(removed_member);
    changed.base_frontiers.front().delivery.erase(removed_member);
    changed.base_frontiers.front().operation.emplace(actor(3), 0);
    changed.base_frontiers.front().delivery.emplace(actor(3), 0);
    QVERIFY(!validate_membership_descriptor(changed).ok());
    changed = current;
    ++changed.base_frontiers.front().operation.begin()->second;
    QVERIFY(!validate_membership_descriptor(changed).ok());
    changed = current;
    changed.base_frontiers.front().delivery.erase(changed.members.front());
    QVERIFY(!validate_membership_descriptor(changed).ok());
    changed = current;
    changed.supported_token_versions.push_back(2);
    QVERIFY(!validate_membership_descriptor(changed).ok());
    changed = current;
    changed.translation_manifest_hash = sha256_hex("different-manifest");
    QVERIFY(!validate_membership_descriptor(changed).ok());

    changed = current;
    changed.hash = sha256_hex("forged-descriptor-hash");
    auto rejected = preview_enrollment(changed, actor(3));
    QVERIFY(!rejected.ok()); QCOMPARE(rejected.error().code(), ErrorCode::corrupt_record);

    auto oversized = current;
    oversized.base_frontiers.resize(4097);
    auto too_large = seal_membership_descriptor(std::move(oversized));
    QVERIFY(!too_large.ok()); QCOMPARE(too_large.error().code(), ErrorCode::limit_exceeded);
  }

  void persistsExplicitProducerEvents() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = std::filesystem::path(dir.path().toStdString()) / "member.log";
    const auto producer = actor(1), peer = actor(2);
    {
      auto opened = MembershipStore::open(path, producer);
      QVERIFY(opened.ok());
      auto store = std::move(opened.value());
      auto competing = MembershipStore::open(path, producer);
      QVERIFY(!competing.ok());
      QCOMPARE(competing.error().code(), ErrorCode::writer_conflict);
      const auto invalid_kind = static_cast<MembershipEvent::Kind>(99);
      QVERIFY(!store.append({1, 1, peer, invalid_kind, sha256_hex("parent"), "proof"}).ok());
      QCOMPARE(store.last_sequence(), uint64_t(0));
      QVERIFY(store.append({1, 1, peer, MembershipEvent::Kind::Enrolled, sha256_hex("parent"), "signed\nproof"}).ok());
      QVERIFY(store.append({2, 2, peer, MembershipEvent::Kind::Retired, sha256_hex("parent"), "confirmed"}).ok());
      QVERIFY(!store.append({4, 3, peer, MembershipEvent::Kind::Enrolled, sha256_hex("parent"), {}}).ok());
    }
    {
      auto reopened = MembershipStore::open(path, producer);
      QVERIFY(reopened.ok());
      QCOMPARE(reopened.value().last_sequence(), uint64_t(2));
      auto events = reopened.value().recover();
      QVERIFY(events.ok());
      QCOMPARE(events.value().size(), size_t(2));
      QCOMPARE(events.value()[0].evidence, std::string("signed\nproof"));
      QCOMPARE(events.value()[1].kind, MembershipEvent::Kind::Retired);
    }
    auto wrong_owner = MembershipStore::open(path, actor(9));
    QVERIFY(!wrong_owner.ok());
  }

  void appendRequiresContiguousEpochsAcrossRestart() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto path = std::filesystem::path(dir.path().toStdString()) / "epochs.log";
    const auto producer = actor(4), peer = actor(5);
    {
      auto opened = MembershipStore::open(path, producer); QVERIFY(opened.ok());
      auto store = std::move(opened.value());
      QVERIFY(store.append({1, 9, peer, MembershipEvent::Kind::Enrolled, sha256_hex("parent"), "base"}).ok());
      QVERIFY(!store.append({2, 11, peer, MembershipEvent::Kind::Retired, sha256_hex("parent"), "skip"}).ok());
      QVERIFY(!store.append({2, 9, peer, MembershipEvent::Kind::Retired, sha256_hex("parent"), "repeat"}).ok());
      QCOMPARE(store.last_sequence(), uint64_t(1));
      QVERIFY(store.append({2, 10, peer, MembershipEvent::Kind::Retired, sha256_hex("parent"), "next"}).ok());
    }
    {
      auto opened = MembershipStore::open(path, producer); QVERIFY(opened.ok());
      auto store = std::move(opened.value());
      QVERIFY(!store.append({3, 12, peer, MembershipEvent::Kind::Enrolled, sha256_hex("parent"), "skip"}).ok());
      QVERIFY(store.append({3, 11, peer, MembershipEvent::Kind::Enrolled, sha256_hex("parent"), "next"}).ok());
      auto events = store.recover(); QVERIFY(events.ok());
      QCOMPARE(events.value().back().epoch, uint64_t(11));
    }
  }

  void recoveryRejectsChecksummedSkippedEpoch() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto path = std::filesystem::path(dir.path().toStdString()) / "skipped.log";
    const auto producer = actor(8), peer = actor(9);
    {
      std::ofstream out(path, std::ios::binary);
      out << membership_record(producer, 1, 9, peer)
          << membership_record(producer, 2, 11, peer);
      QVERIFY(out.good());
    }
    auto opened = MembershipStore::open(path, producer);
    QVERIFY(!opened.ok());
    QCOMPARE(opened.error().code(), ErrorCode::corrupt_record);
  }

  void recoveryReadsLegacyEventsButTheyCannotProveTransitions() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto producer = actor(35), subject = actor(36);
    const auto path = std::filesystem::path(dir.path().toStdString()) / "legacy.log";
    const auto legacy_body = "CME1 " + producer.to_string() + " 1 7 " +
        subject.to_string() + " E 6161";
    {
      std::ofstream out(path, std::ios::binary);
      out << legacy_body << " " << sha256_hex(legacy_body) << "\n";
    }
    auto opened = MembershipStore::open(path, producer); QVERIFY(opened.ok());
    auto events = opened.value().recover(); QVERIFY(events.ok());
    QCOMPARE(events.value().size(), size_t(1));
    QVERIFY(events.value().front().parent_descriptor_hash.empty());
    QVERIFY(events.value().front().evidence == "aa");

    auto parent = fixture_descriptor(6, {actor(37)});
    auto child = parent;
    child.epoch = 7;
    child.parent_hash = parent.hash;
    child.members.push_back(subject);
    std::sort(child.members.begin(), child.members.end());
    for (auto &frontier : child.base_frontiers) {
      frontier.operation.emplace(subject, 0);
      frontier.delivery.emplace(subject, 0);
    }
    child.transition_event_hash = sha256_hex(
        legacy_body + " " + sha256_hex(legacy_body));
    child.base_checkpoint_hash = sha256_hex("legacy-child-checkpoint");
    child.hash.clear();
    auto sealed = seal_membership_descriptor(child); QVERIFY(sealed.ok());
    QVERIFY(!opened.value().validate_transition_link(parent, sealed.value()).ok());
  }

  void descriptorTransitionMustMatchDurableJournalEvent() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto producer = actor(30), joining = actor(31), existing = actor(32);
    const auto parent = fixture_descriptor(3, {existing});
    const auto path = std::filesystem::path(dir.path().toStdString()) / "linked.log";
    auto opened = MembershipStore::open(path, producer); QVERIFY(opened.ok());
    auto store = std::move(opened.value());
    MembershipEvent event{1, 4, joining, MembershipEvent::Kind::Enrolled,
                          parent.hash, "durable enrollment evidence"};
    QVERIFY(store.append(event).ok());

    auto child = parent;
    child.epoch = 4;
    child.parent_hash = parent.hash;
    child.transition_event_hash = membership_event_hash(event, producer);
    child.members.push_back(joining);
    for (auto &frontier : child.base_frontiers) {
      frontier.operation.emplace(joining, 0);
      frontier.delivery.emplace(joining, 0);
    }
    child.base_checkpoint_hash = sha256_hex("child-checkpoint");
    child.hash.clear();
    auto sealed = seal_membership_descriptor(child); QVERIFY(sealed.ok());
    QVERIFY(store.validate_transition_link(parent, sealed.value()).ok());
    auto no_evidence_verifier = store.validate_transition(
        parent, sealed.value(), {}, [](const MembershipDescriptor &,
                                       const MembershipDescriptor &) {
          return Result<void>{};
        });
    QVERIFY(!no_evidence_verifier.ok());
    QCOMPARE(no_evidence_verifier.error().code(), ErrorCode::awaiting_state);
    bool evidence_seen = false;
    auto verify_evidence = [&](const MembershipDescriptor &verified_parent,
                               const MembershipDescriptor &verified_child,
                               const MembershipEvent &verified_event,
                               const ActorId &verified_producer) -> Result<void> {
      evidence_seen = verified_parent.hash == parent.hash &&
          verified_child.hash == sealed.value().hash &&
          verified_event.evidence == event.evidence && verified_producer == producer;
      if (!evidence_seen)
        return Error(ErrorCode::corrupt_record,
                     "evidence verifier received unexpected transition inputs");
      return {};
    };
    bool checkpoint_seen = false;
    auto verify_checkpoint = [&](const MembershipDescriptor &verified_parent,
                                 const MembershipDescriptor &verified_child) {
      checkpoint_seen = verified_parent.hash == parent.hash &&
          verified_child.base_checkpoint_hash == child.base_checkpoint_hash;
      return checkpoint_seen
          ? Result<void>{}
          : Result<void>{Error(ErrorCode::corrupt_record,
                               "checkpoint verifier received an unexpected descriptor")};
    };
    auto fully_validated = store.validate_transition(
        parent, sealed.value(), verify_evidence, verify_checkpoint);
    QVERIFY(fully_validated.ok());
    QVERIFY(evidence_seen);
    QVERIFY(checkpoint_seen);
    checkpoint_seen = false;
    auto corrupt_evidence = store.validate_transition(
        parent, sealed.value(), [](const MembershipDescriptor &,
                                   const MembershipDescriptor &,
                                   const MembershipEvent &,
                                   const ActorId &) -> Result<void> {
          return Error(ErrorCode::corrupt_record, "simulated invalid acknowledgements");
        }, verify_checkpoint);
    QVERIFY(!corrupt_evidence.ok());
    QCOMPARE(corrupt_evidence.error().code(), ErrorCode::corrupt_record);
    QVERIFY(!checkpoint_seen);
    auto throwing_evidence = store.validate_transition(
        parent, sealed.value(), [](const MembershipDescriptor &,
                                   const MembershipDescriptor &,
                                   const MembershipEvent &,
                                   const ActorId &) -> Result<void> {
          throw std::runtime_error("simulated evidence verifier exception");
        }, verify_checkpoint);
    QVERIFY(!throwing_evidence.ok());
    QCOMPARE(throwing_evidence.error().code(), ErrorCode::io_error);
    auto corrupt_checkpoint = store.validate_transition(
        parent, sealed.value(), verify_evidence,
        [](const MembershipDescriptor &, const MembershipDescriptor &) -> Result<void> {
          return Error(ErrorCode::corrupt_record, "simulated corrupt checkpoint");
        });
    QVERIFY(!corrupt_checkpoint.ok());
    QCOMPARE(corrupt_checkpoint.error().code(), ErrorCode::corrupt_record);
    auto throwing_checkpoint = store.validate_transition(
        parent, sealed.value(), verify_evidence,
        [](const MembershipDescriptor &, const MembershipDescriptor &) -> Result<void> {
          throw std::runtime_error("simulated checkpoint reader exception");
        });
    QVERIFY(!throwing_checkpoint.ok());
    QCOMPARE(throwing_checkpoint.error().code(), ErrorCode::io_error);

    auto wrong_event = event;
    wrong_event.subject = actor(33);
    auto wrong_child = child;
    wrong_child.transition_event_hash = membership_event_hash(wrong_event, producer);
    auto wrong_sealed = seal_membership_descriptor(wrong_child); QVERIFY(wrong_sealed.ok());
    QVERIFY(!store.validate_transition_link(parent, wrong_sealed.value()).ok());

    auto wrong_delta = child;
    wrong_delta.members = parent.members;
    wrong_delta.base_frontiers.front().operation.erase(joining);
    wrong_delta.base_frontiers.front().delivery.erase(joining);
    wrong_delta.hash.clear();
    auto delta_sealed = seal_membership_descriptor(wrong_delta); QVERIFY(delta_sealed.ok());
    QVERIFY(!store.validate_transition_link(parent, delta_sealed.value()).ok());

    auto missing_parent_link = child;
    missing_parent_link.parent_hash = sha256_hex("unrelated-parent");
    missing_parent_link.hash.clear();
    auto missing_parent_sealed = seal_membership_descriptor(missing_parent_link);
    QVERIFY(missing_parent_sealed.ok());
    QVERIFY(!store.validate_transition_link(parent, missing_parent_sealed.value()).ok());
  }

  void writerLeaseExcludesASecondProcess() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto path=std::filesystem::path(dir.path().toStdString())/"cross-process.log";
    QProcess holder;
    holder.start(QCoreApplication::applicationFilePath(),
        {"--hold-membership-lease",QString::fromStdString(path.string())});
    QVERIFY(holder.waitForStarted());
    QVERIFY(holder.waitForReadyRead(5000));
    QCOMPARE(QString::fromUtf8(holder.readLine()).trimmed(),QStringLiteral("ready"));

    auto competing=MembershipStore::open(path,actor(94));
    QVERIFY(!competing.ok()); QCOMPARE(competing.error().code(),ErrorCode::writer_conflict);

    holder.closeWriteChannel();
    QVERIFY(holder.waitForFinished(5000));
    QCOMPARE(holder.exitStatus(),QProcess::NormalExit);
    QCOMPARE(holder.exitCode(),0);
    auto after_release=MembershipStore::open(path,actor(94));
    QVERIFY(after_release.ok());
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
      QVERIFY(store.append({1, 1, peer, MembershipEvent::Kind::Enrolled, sha256_hex("parent"), "ack"}).ok());
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
    auto result=store.append({1,1,peer,MembershipEvent::Kind::Enrolled,sha256_hex("parent"),"ack"});
    QVERIFY(!result.ok());
    // The append reached the file before fsync failed. The store has reconciled
    // its sequence from that complete record, so retry cannot reuse sequence 1.
    QCOMPARE(store.last_sequence(),uint64_t(1));
    QVERIFY(store.append({2,2,peer,MembershipEvent::Kind::Retired,sha256_hex("parent"),"confirmed"}).ok());
    auto recovered=store.recover(); QVERIFY(recovered.ok());
    QCOMPARE(recovered.value().size(),size_t(2));
  }

  void retirementCommitRetryConfirmsUncertainPublication() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto producer = actor(14), retiring = actor(15);
    const auto member_a = actor(16), member_b = actor(17);
    const auto descriptor = fixture_descriptor(5, {retiring, member_a, member_b});
    const std::vector<RetirementImpact> impacts{
        {"doc", "text", 5, 1, 0, "branch"}};
    auto preview = preview_retirement(descriptor, retiring, impacts); QVERIFY(preview.ok());
    const std::vector<MembershipAcknowledgement> acks{
        {member_a, "ack-a"}, {member_b, "ack-b"}};
    const auto verifier = [](const ActorId&, std::string_view, std::string_view)
        -> Result<void> { return {}; };
    const std::vector<Step> failure_points{Step::Write, Step::FileSync, Step::DirSync};
    for (const auto point : failure_points) {
      const auto path = std::filesystem::path(dir.path().toStdString()) /
                        (std::string(to_string(point)) + ".log");
      bool fail_once = true;
      StepHook hook;
      hook.fail = [&](Step step, const fs::path&) -> std::optional<Error> {
        if (step == point && fail_once) {
          fail_once = false;
          return Error(ErrorCode::io_error, "injected transition publication failure");
        }
        return std::nullopt;
      };
      auto opened = MembershipStore::open(path, producer, &hook); QVERIFY(opened.ok());
      auto store = std::move(opened.value());
      auto uncertain = store.commit_retirement(preview.value(), descriptor, impacts,
          acks, "confirmed", verifier);
      QVERIFY(!uncertain.ok());
      const uint64_t published = point == Step::Write ? 0 : 1;
      QCOMPARE(store.last_sequence(), published);

      auto retried = store.commit_retirement(preview.value(), descriptor, impacts,
          acks, "confirmed", verifier);
      QVERIFY(retried.ok());
      QCOMPARE(retried.value().sequence, uint64_t(1));
      QCOMPARE(retried.value().epoch, uint64_t(6));
      auto conflicting = store.commit_retirement(preview.value(), descriptor, impacts,
          acks, "different confirmation", verifier);
      QVERIFY(!conflicting.ok());
      QCOMPARE(conflicting.error().code(), ErrorCode::wrong_epoch);
      auto events = store.recover(); QVERIFY(events.ok());
      QCOMPARE(events.value().size(), size_t(1));
    }
  }

  void retirementPreviewMustBeCurrentAndUnanimouslyAcknowledged() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto path=std::filesystem::path(dir.path().toStdString())/"member.log";
    const auto producer=actor(10), retiring=actor(11), member_a=actor(12), member_b=actor(13);
    auto opened=MembershipStore::open(path,producer); QVERIFY(opened.ok());
    auto store=std::move(opened.value());
    auto descriptor=fixture_descriptor(7,{retiring,member_b,member_a});
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
    QVERIFY(replay.ok()); QCOMPARE(replay.value().sequence,uint64_t(1));
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
    auto descriptor=fixture_descriptor(4,{member_b,member_a});
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

    auto throwing_verifier = store.commit_enrollment(
        preview.value(), descriptor,
        {{member_a,"durable-ack"},{member_b,"durable-ack"},{joining,"durable-ack"}},
        [](const ActorId &, std::string_view, std::string_view) -> Result<void> {
          throw std::runtime_error("simulated acknowledgement storage failure");
        });
    QVERIFY(!throwing_verifier.ok());
    QCOMPARE(throwing_verifier.error().code(),ErrorCode::io_error);
    QCOMPARE(store.last_sequence(),uint64_t(0));

    auto committed=store.commit_enrollment(preview.value(),descriptor,
        {{member_a,"durable-ack"},{member_b,"durable-ack"},{joining,"durable-ack"}},verifier);
    QVERIFY(committed.ok()); QCOMPARE(verified,3);
    QCOMPARE(committed.value().kind,MembershipEvent::Kind::Enrolled);
    QCOMPARE(committed.value().epoch,uint64_t(5));
    QCOMPARE(committed.value().subject,joining);
    auto replay=store.commit_enrollment(preview.value(),descriptor,
        {{member_a,"durable-ack"},{member_b,"durable-ack"},{joining,"durable-ack"}},verifier);
    QVERIFY(replay.ok()); QCOMPARE(replay.value().sequence,uint64_t(1));
    QCOMPARE(store.last_sequence(),uint64_t(1));
    auto recovered=store.recover(); QVERIFY(recovered.ok());
    QCOMPARE(recovered.value().size(),size_t(1));
  }

  void membershipTransitionEvidenceDecodesCanonicallyAfterRecovery() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto producer = actor(90), retiring = actor(91);
    const auto member_a = actor(92), member_b = actor(93), joining = actor(94);
    const auto verifier = [](const ActorId &, std::string_view,
                             std::string_view) -> Result<void> { return {}; };
    const std::map<ActorId, std::string> retirement_ack_log{
        {member_a, "signed-a"}, {member_b, "signed-b"}};
    auto parent = fixture_descriptor(12, {retiring, member_a, member_b});
    const std::vector<RetirementImpact> impacts{
        {"document", "text", 12, 2, 1, "branch-hash"}};

    auto retirement_open = MembershipStore::open(
        std::filesystem::path(dir.path().toStdString()) / "retirement-evidence.log",
        producer);
    QVERIFY(retirement_open.ok());
    auto retirement_store = std::move(retirement_open).value();
    auto retirement_preview = preview_retirement(parent, retiring, impacts);
    QVERIFY(retirement_preview.ok());
    const std::vector<MembershipAcknowledgement> retirement_acks{
        {member_a, "signed-a"}, {member_b, "signed-b"}};
    auto committed_retirement = retirement_store.commit_retirement(
        retirement_preview.value(), parent, impacts, retirement_acks,
        "confirmed by operator", verifier);
    QVERIFY(committed_retirement.ok());
    auto retirement_events = retirement_store.recover();
    QVERIFY(retirement_events.ok());
    auto decoded_retirement = decode_membership_transition_evidence(
        retirement_events.value().front());
    QVERIFY(decoded_retirement.ok());
    QCOMPARE(decoded_retirement.value().kind, MembershipEvent::Kind::Retired);
    QCOMPARE(decoded_retirement.value().transition_digest,
             retirement_preview.value().digest);
    QCOMPARE(decoded_retirement.value().explicit_confirmation,
             std::optional<std::string>("confirmed by operator"));
    QCOMPARE(decoded_retirement.value().acknowledgements.size(), size_t(2));
    QCOMPARE(decoded_retirement.value().acknowledgements[0].actor, member_a);
    QCOMPARE(decoded_retirement.value().acknowledgements[0].signature,
             std::string("signed-a"));
    size_t retirement_verifications = 0;
    auto verify_retirement_ledger = [&](const ActorId &ack_actor,
        std::string_view digest, std::string_view signature) -> Result<void> {
      ++retirement_verifications;
      const auto durable = retirement_ack_log.find(ack_actor);
      if (digest != retirement_preview.value().digest ||
          durable == retirement_ack_log.end() || durable->second != signature)
        return Error(ErrorCode::corrupt_record,
                     "recovered retirement acknowledgement is not durable");
      return {};
    };
    QVERIFY(validate_membership_evidence(
        decoded_retirement.value(), retirement_preview.value(),
        "confirmed by operator", verify_retirement_ledger));
    QCOMPARE(retirement_verifications, size_t(2));
    QVERIFY(!validate_membership_evidence(
        decoded_retirement.value(), retirement_preview.value(),
        "different confirmation", verify_retirement_ledger));
    auto changed_retirement_preview = retirement_preview.value();
    changed_retirement_preview.impacts[0].unsynced_operations++;
    QVERIFY(!validate_membership_evidence(
        decoded_retirement.value(), changed_retirement_preview,
        "confirmed by operator", verify_retirement_ledger));
    const auto complete_evidence = retirement_events.value().front().evidence;
    const auto version_end = complete_evidence.find(' ');
    const auto evidence_digest_end = complete_evidence.find(' ', version_end + 1);
    const auto material_end = complete_evidence.find(' ', evidence_digest_end + 1);
    const auto confirmation_end = complete_evidence.find(' ', material_end + 1);
    const auto count_end = complete_evidence.find(' ', confirmation_end + 1);
    auto truncated_digest = retirement_events.value().front();
    truncated_digest.evidence.resize(version_end + 2); // odd-length digest hex
    QVERIFY(!decode_membership_transition_evidence(truncated_digest).ok());
    auto missing_acknowledgements = retirement_events.value().front();
    missing_acknowledgements.evidence.resize(count_end);
    QVERIFY(!decode_membership_transition_evidence(missing_acknowledgements).ok());

    auto retirement_child = parent;
    retirement_child.epoch = 13;
    retirement_child.parent_hash = parent.hash;
    retirement_child.transition_event_hash = membership_event_hash(
        retirement_events.value().front(), producer);
    retirement_child.members.erase(
        std::find(retirement_child.members.begin(), retirement_child.members.end(), retiring));
    retirement_child.base_checkpoint_hash = sha256_hex("retirement-checkpoint");
    retirement_child.hash.clear();
    auto sealed_retirement_child = seal_membership_descriptor(
        std::move(retirement_child));
    QVERIFY(sealed_retirement_child.ok());
    auto revalidate_retirement_evidence = [&](const MembershipDescriptor &,
        const MembershipDescriptor &, const MembershipEvent &event,
        const ActorId &event_producer) -> Result<void> {
      auto evidence = decode_membership_transition_evidence(event);
      if (!evidence.ok()) return evidence.error();
      if (event_producer != producer)
        return Error(ErrorCode::corrupt_record,
                     "recovered transition evidence has the wrong owner");
      return validate_membership_evidence(
          evidence.value(), retirement_preview.value(),
          "confirmed by operator",
          [&](const ActorId &ack_actor, std::string_view digest,
              std::string_view signature) -> Result<void> {
            const auto durable = retirement_ack_log.find(ack_actor);
            if (digest != retirement_preview.value().digest ||
                durable == retirement_ack_log.end() ||
                durable->second != signature)
              return Error(ErrorCode::corrupt_record,
                           "recovered acknowledgement differs from durable caller state");
            return {};
          });
    };
    QVERIFY(retirement_store.validate_transition(
        parent, sealed_retirement_child.value(), revalidate_retirement_evidence,
        [](const MembershipDescriptor &, const MembershipDescriptor &) {
          return Result<void>{};
        }).ok());

    auto bad_digest = retirement_events.value().front();
    const auto digest_start = bad_digest.evidence.find(' ') + 1;
    const auto digest_end = bad_digest.evidence.find(' ', digest_start);
    std::string wrong_digest_hex;
    for (size_t i = 0; i < 64; ++i) wrong_digest_hex += "62"; // 64 ASCII 'b' bytes
    bad_digest.evidence.replace(digest_start, digest_end - digest_start,
                                wrong_digest_hex);
    auto tampered = decode_membership_transition_evidence(bad_digest);
    QVERIFY(!tampered.ok());
    QCOMPARE(tampered.error().code(), ErrorCode::corrupt_record);
    auto noncanonical = retirement_events.value().front();
    noncanonical.evidence += " trailing";
    QVERIFY(!decode_membership_transition_evidence(noncanonical).ok());
    auto wrong_kind = retirement_events.value().front();
    wrong_kind.kind = MembershipEvent::Kind::Enrolled;
    QVERIFY(!decode_membership_transition_evidence(wrong_kind).ok());
    auto oversized = retirement_events.value().front();
    oversized.evidence.resize((1u << 20) + 1, 'x');
    auto over_limit = decode_membership_transition_evidence(oversized);
    QVERIFY(!over_limit.ok());
    QCOMPARE(over_limit.error().code(), ErrorCode::limit_exceeded);

    auto enrollment_parent = fixture_descriptor(20, {member_a, member_b});
    auto enrollment_open = MembershipStore::open(
        std::filesystem::path(dir.path().toStdString()) / "enrollment-evidence.log",
        producer);
    QVERIFY(enrollment_open.ok());
    auto enrollment_store = std::move(enrollment_open).value();
    auto enrollment_preview = preview_enrollment(enrollment_parent, joining);
    QVERIFY(enrollment_preview.ok());
    auto committed_enrollment = enrollment_store.commit_enrollment(
        enrollment_preview.value(), enrollment_parent,
        {{member_a, "signed-a"}, {member_b, "signed-b"}, {joining, "signed-joining"}},
        verifier);
    QVERIFY(committed_enrollment.ok());
    auto enrollment_events = enrollment_store.recover();
    QVERIFY(enrollment_events.ok());
    auto decoded_enrollment = decode_membership_transition_evidence(
        enrollment_events.value().front());
    QVERIFY(decoded_enrollment.ok());
    QCOMPARE(decoded_enrollment.value().kind, MembershipEvent::Kind::Enrolled);
    QCOMPARE(decoded_enrollment.value().transition_digest,
             enrollment_preview.value().digest);
    QVERIFY(!decoded_enrollment.value().explicit_confirmation.has_value());
    QCOMPARE(decoded_enrollment.value().acknowledgements.size(), size_t(3));
    QCOMPARE(decoded_enrollment.value().acknowledgements[2].actor, joining);
    const auto enrollment_ack_log = std::map<ActorId, std::string>{
        {member_a, "signed-a"}, {member_b, "signed-b"},
        {joining, "signed-joining"}};
    auto verify_enrollment_ledger = [&](const ActorId &ack_actor,
        std::string_view digest, std::string_view signature) -> Result<void> {
      const auto durable = enrollment_ack_log.find(ack_actor);
      if (digest != enrollment_preview.value().digest ||
          durable == enrollment_ack_log.end() || durable->second != signature)
        return Error(ErrorCode::corrupt_record,
                     "recovered enrollment acknowledgement is not durable");
      return {};
    };
    QVERIFY(validate_membership_evidence(
        decoded_enrollment.value(), enrollment_preview.value(),
        verify_enrollment_ledger));
    auto missing_enrollment_ack = decoded_enrollment.value();
    missing_enrollment_ack.acknowledgements.pop_back();
    QVERIFY(!validate_membership_evidence(
        missing_enrollment_ack, enrollment_preview.value(),
        verify_enrollment_ledger));
    auto enrollment_child = enrollment_parent;
    enrollment_child.epoch = 21;
    enrollment_child.parent_hash = enrollment_parent.hash;
    enrollment_child.transition_event_hash = membership_event_hash(
        enrollment_events.value().front(), producer);
    enrollment_child.members.push_back(joining);
    std::sort(enrollment_child.members.begin(), enrollment_child.members.end());
    for (auto &frontier : enrollment_child.base_frontiers) {
      frontier.operation.emplace(joining, 0);
      frontier.delivery.emplace(joining, 0);
    }
    enrollment_child.base_checkpoint_hash = sha256_hex("enrollment-checkpoint");
    enrollment_child.hash.clear();
    auto sealed_enrollment_child = seal_membership_descriptor(
        std::move(enrollment_child));
    QVERIFY(sealed_enrollment_child.ok());
    auto revalidate_enrollment_evidence = [&](const MembershipDescriptor &,
        const MembershipDescriptor &, const MembershipEvent &event,
        const ActorId &event_producer) -> Result<void> {
      auto evidence = decode_membership_transition_evidence(event);
      if (!evidence.ok()) return evidence.error();
      if (event_producer != producer)
        return Error(ErrorCode::corrupt_record,
                     "recovered enrollment evidence has the wrong owner");
      return validate_membership_evidence(
          evidence.value(), enrollment_preview.value(),
          [&](const ActorId &ack_actor, std::string_view digest,
              std::string_view signature) -> Result<void> {
            const auto durable = enrollment_ack_log.find(ack_actor);
            if (digest != enrollment_preview.value().digest ||
                durable == enrollment_ack_log.end() ||
                durable->second != signature)
              return Error(ErrorCode::corrupt_record,
                           "recovered enrollment acknowledgement is not durable");
            return {};
          });
    };
    QVERIFY(enrollment_store.validate_transition(
        enrollment_parent, sealed_enrollment_child.value(),
        revalidate_enrollment_evidence,
        [](const MembershipDescriptor &, const MembershipDescriptor &) {
          return Result<void>{};
        }).ok());
  }

  void transitionMustExtendLatestJournalEpoch() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto producer=actor(40), retiring=actor(41), remaining=actor(42), joining=actor(43);
    const auto current=fixture_descriptor(8,{retiring,remaining});
    const auto verifier=[](const ActorId &,std::string_view,std::string_view)->Result<void> {
      return {};
    };

    auto enrollment_open=MembershipStore::open(
        std::filesystem::path(dir.path().toStdString())/"enrollment-gap.log",producer);
    QVERIFY(enrollment_open.ok()); auto enrollment_store=std::move(enrollment_open.value());
    QVERIFY(enrollment_store.append({1,7,remaining,MembershipEvent::Kind::Retired,sha256_hex("parent"),"previous"}).ok());
    auto enrollment=preview_enrollment(current,joining); QVERIFY(enrollment.ok());
    auto enrollment_result=enrollment_store.commit_enrollment(enrollment.value(),current,
        {{retiring,"ack"},{remaining,"ack"},{joining,"ack"}},verifier);
    QVERIFY(!enrollment_result.ok());
    QCOMPARE(enrollment_result.error().code(),ErrorCode::wrong_epoch);
    QCOMPARE(enrollment_store.last_sequence(),uint64_t(1));

    auto retirement_open=MembershipStore::open(
        std::filesystem::path(dir.path().toStdString())/"retirement-gap.log",producer);
    QVERIFY(retirement_open.ok()); auto retirement_store=std::move(retirement_open.value());
    QVERIFY(retirement_store.append({1,7,joining,MembershipEvent::Kind::Enrolled,sha256_hex("parent"),"previous"}).ok());
    auto retirement=preview_retirement(current,retiring,{}); QVERIFY(retirement.ok());
    auto retirement_result=retirement_store.commit_retirement(retirement.value(),current,{},
        {{remaining,"ack"}},"confirm",verifier);
    QVERIFY(!retirement_result.ok());
    QCOMPARE(retirement_result.error().code(),ErrorCode::wrong_epoch);
    QCOMPARE(retirement_store.last_sequence(),uint64_t(1));
  }

  void cannotRetireTheLastMemberOrCommitWithoutConfirmation() {
    const auto lone=actor(20);
    auto one_member=fixture_descriptor(1,{lone});
    QVERIFY(!preview_retirement(one_member,lone,{}).ok());
    QTemporaryDir dir; QVERIFY(dir.isValid());
    auto opened=MembershipStore::open(std::filesystem::path(dir.path().toStdString())/"m.log",actor(21));
    QVERIFY(opened.ok());
    auto store=std::move(opened.value());
    auto two_members=fixture_descriptor(1,{actor(22),actor(23)});
    std::vector<RetirementImpact> impacts{{"doc","text",1,0,0,"branch"}};
    auto preview=preview_retirement(two_members,actor(22),impacts); QVERIFY(preview.ok());
    DurableAcknowledgementVerifier verifier=[](const ActorId&,std::string_view,std::string_view)->Result<void>{return {};};
    auto result=store.commit_retirement(preview.value(),two_members,impacts,
        {{actor(23),"ack"}},"",verifier);
    QVERIFY(!result.ok()); QCOMPARE(store.last_sequence(),uint64_t(0));
    auto throwing_verifier=store.commit_retirement(preview.value(),two_members,impacts,
        {{actor(23),"ack"}},"confirm",
        [](const ActorId &,std::string_view,std::string_view)->Result<void>{
          throw std::runtime_error("simulated acknowledgement storage failure");
        });
    QVERIFY(!throwing_verifier.ok());
    QCOMPARE(throwing_verifier.error().code(),ErrorCode::io_error);
    QCOMPARE(store.last_sequence(),uint64_t(0));
  }

  void deterministicMembershipChurnSurvivesAdversarialCallsAndRestart() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto path=std::filesystem::path(dir.path().toStdString())/"churn.log";
    const auto producer=actor(90);
    auto opened=MembershipStore::open(path,producer); QVERIFY(opened.ok());
    std::optional<MembershipStore> store(std::move(opened.value()));
    auto current=fixture_descriptor(1,{actor(1),actor(2)});
    uint64_t random=UINT64_C(0x9e3779b97f4a7c15);
    auto next_random=[&]() {
      random^=random<<13; random^=random>>7; random^=random<<17;
      return random;
    };
    DurableAcknowledgementVerifier verifier=[](
        const ActorId &who,std::string_view digest,
        std::string_view signature)->Result<void> {
      if(signature!=simulated_ack(who,digest))
        return Error(ErrorCode::corrupt_record,"simulated durable acknowledgement mismatch");
      return {};
    };

    constexpr size_t transitions=48;
    for(size_t i=0;i<transitions;++i) {
      const bool enroll=current.members.size()<2 ||
          (current.members.size()<12 && next_random()%3==0);
      MembershipEvent committed;
      if(enroll) {
        const auto joining=actor(static_cast<uint8_t>(40+i));
        auto preview=preview_enrollment(current,joining); QVERIFY(preview.ok());
        std::vector<MembershipAcknowledgement> acks;
        for(const auto &who:preview.value().required_acknowledgers)
          acks.push_back({who,simulated_ack(who,preview.value().digest)});
        auto incomplete=acks; incomplete.pop_back();
        auto refused=store->commit_enrollment(preview.value(),current,incomplete,verifier);
        QVERIFY(!refused.ok()); QCOMPARE(refused.error().code(),ErrorCode::awaiting_state);
        QCOMPARE(store->last_sequence(),static_cast<uint64_t>(i));
        auto corrupted=acks;
        corrupted.front().signature="corrupted-ack";
        refused=store->commit_enrollment(preview.value(),current,corrupted,verifier);
        QVERIFY(!refused.ok()); QCOMPARE(refused.error().code(),ErrorCode::corrupt_record);
        QCOMPARE(store->last_sequence(),static_cast<uint64_t>(i));
        auto fork=current; fork.hash+="-fork";
        refused=store->commit_enrollment(preview.value(),fork,acks,verifier);
        QVERIFY(!refused.ok()); QCOMPARE(refused.error().code(),ErrorCode::wrong_epoch);
        QCOMPARE(store->last_sequence(),static_cast<uint64_t>(i));
        auto result=store->commit_enrollment(preview.value(),current,acks,verifier);
        QVERIFY(result.ok()); committed=std::move(result.value());
        current.members.push_back(joining);
        std::sort(current.members.begin(),current.members.end());
      } else {
        const auto index=static_cast<size_t>(next_random()%current.members.size());
        const auto retiring=current.members[index];
        std::vector<RetirementImpact> impacts;
        for(size_t doc=0;doc<3;++doc) {
          const auto doc_id="sim-doc-"+std::to_string(doc);
          impacts.push_back({doc_id,"text",current.epoch,
              next_random()%20,next_random()%8,
              "branch-"+std::to_string(i)+"-"+doc_id});
        }
        auto preview=preview_retirement(current,retiring,impacts); QVERIFY(preview.ok());
        std::vector<MembershipAcknowledgement> acks;
        for(const auto &who:preview.value().required_acknowledgers)
          acks.push_back({who,simulated_ack(who,preview.value().digest)});
        auto incomplete=acks; incomplete.pop_back();
        auto refused=store->commit_retirement(preview.value(),current,impacts,
            incomplete,"simulated-user-confirmation",verifier);
        QVERIFY(!refused.ok()); QCOMPARE(refused.error().code(),ErrorCode::awaiting_state);
        QCOMPARE(store->last_sequence(),static_cast<uint64_t>(i));
        auto fork=current; fork.hash+="-fork";
        refused=store->commit_retirement(preview.value(),fork,impacts,acks,
            "simulated-user-confirmation",verifier);
        QVERIFY(!refused.ok()); QCOMPARE(refused.error().code(),ErrorCode::wrong_epoch);
        QCOMPARE(store->last_sequence(),static_cast<uint64_t>(i));
        auto result=store->commit_retirement(preview.value(),current,impacts,acks,
            "simulated-user-confirmation",verifier);
        QVERIFY(result.ok()); committed=std::move(result.value());
        current.members.erase(current.members.begin()+static_cast<std::ptrdiff_t>(index));
      }
      QCOMPARE(committed.sequence,static_cast<uint64_t>(i+1));
      QCOMPARE(committed.epoch,current.epoch+1);
      current=fixture_descriptor(committed.epoch,std::move(current.members));

      // A process restart must recover the just-committed sequence and event.
      store.reset();
      auto reopened=MembershipStore::open(path,producer); QVERIFY(reopened.ok());
      QCOMPARE(reopened.value().last_sequence(),static_cast<uint64_t>(i+1));
      auto recovered=reopened.value().recover(); QVERIFY(recovered.ok());
      QCOMPARE(recovered.value().size(),i+1);
      QCOMPARE(recovered.value().back().epoch,current.epoch);
      QCOMPARE(recovered.value().back().kind,committed.kind);
      QCOMPARE(recovered.value().back().subject,committed.subject);
      store.emplace(std::move(reopened.value()));
    }
  }
};

int main(int argc,char **argv) {
  QCoreApplication app(argc,argv);
  if(argc==3 && std::string_view(argv[1])=="--hold-membership-lease") {
    auto opened=MembershipStore::open(std::filesystem::path(argv[2]),actor(94));
    if(!opened.ok()) {
      std::cerr<<opened.error().message()<<'\n';
      return 2;
    }
    std::cout<<"ready\n"<<std::flush;
    std::cin.get();
    return 0;
  }
  TestMembership test;
  return QTest::qExec(&test,argc,argv);
}
#include "tst_membership.moc"
