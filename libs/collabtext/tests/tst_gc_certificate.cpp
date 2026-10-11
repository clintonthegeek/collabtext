#include <QTest>

#include "storage/GcCertificate.h"
#include "core/Sha256.h"
#include "reference/GcModel.h"

#include <algorithm>
#include <array>
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

MembershipDescriptor descriptor() {
  MembershipDescriptor value;
  value.epoch = 4;
  value.parent_hash = sha256_hex("coverage-parent");
  value.transition_event_hash = sha256_hex("coverage-event");
  value.base_checkpoint_hash = sha256_hex("coverage-checkpoint");
  value.members = {actor(1), actor(2), actor(3)};
  std::sort(value.members.begin(), value.members.end());
  for (const auto *stream : {"text", "comments", "ids"}) {
    MembershipDescriptor::StreamFrontier frontier;
    frontier.stream = stream;
    for (const auto &member : value.members) {
      frontier.operation.emplace(member, 0);
      frontier.delivery.emplace(member, 0);
    }
    frontier.operation.emplace(actor(20), 0);
    frontier.operation.emplace(actor(21), 0);
    frontier.delivery.emplace(actor(20), 0);
    frontier.delivery.emplace(actor(21), 0);
    value.base_frontiers.push_back(std::move(frontier));
  }
  value.supported_token_versions = {1};
  value.translation_manifest_hash = sha256_hex("coverage-translations");
  auto sealed = seal_membership_descriptor(std::move(value));
  if (!sealed.ok()) throw std::runtime_error(std::string(sealed.error().message()));
  return std::move(sealed.value());
}

PeerCoverageEvidence evidence(const MembershipDescriptor &d, const ActorId &member) {
  return {member, d.epoch, d.hash, d.base_checkpoint_hash, {}};
}
}

class TestGcCertificate final : public QObject {
  Q_OBJECT
private slots:
  void intersectionMatchesReferenceAcrossStreamsAndCounterReuse() {
    const auto d = descriptor();
    const auto p = actor(20);
    const auto q = actor(21);
    std::vector<PeerCoverageEvidence> peers;
    for (const auto &member : d.members) peers.push_back(evidence(d, member));
    peers[0].delivery = {{"text", {{p, 12}, {q, 2}}},
                         {"comments", {{p, 8}}},
                         {"ids", {{p, 5}}}};
    peers[1].delivery = {{"text", {{p, 9}, {q, 4}}},
                         {"comments", {{p, 10}}},
                         {"ids", {{p, 3}}}};
    peers[2].delivery = {{"text", {{p, 11}}},
                         {"comments", {{p, 7}}},
                         {"ids", {{p, 6}}}};
    const DurableCoverageVerifier verify = [](const auto &, const auto &) {
      return Result<void>{};
    };
    auto certificate = certify_common_coverage(d, peers, verify);
    QVERIFY(certificate.ok());

    std::vector<GcReference::Coverage> vectors;
    for (const auto &peer : peers) {
      GcReference::Coverage item;
      for (const auto &[stream, frontier] : peer.delivery) {
        auto &dest = item[stream];
        for (const auto &[producer, count] : frontier)
          dest.emplace(producer.to_string(), count);
      }
      vectors.push_back(std::move(item));
    }
    const auto expected = GcReference::meet(vectors);
    QCOMPARE(certificate.value().descriptor_hash(), d.hash);
    QCOMPARE(certificate.value().checkpoint_hash(), d.base_checkpoint_hash);
    const auto &actual = certificate.value().common_delivery();
    QCOMPARE(actual.at("text").at(p), uint64_t(9));
    QCOMPARE(actual.at("text").find(q), actual.at("text").end());
    QCOMPARE(actual.at("comments").at(p), uint64_t(7));
    QCOMPARE(actual.at("ids").at(p), uint64_t(3));
    GcReference::Coverage converted;
    for (const auto &[stream, frontier] : actual)
      for (const auto &[producer, count] : frontier)
        converted[stream].emplace(producer.to_string(), count);
    QCOMPARE(converted, expected);
  }

  void missingStaleAndUnverifiedMembersCannotCertify() {
    const auto d = descriptor();
    std::vector<PeerCoverageEvidence> peers;
    for (const auto &member : d.members) peers.push_back(evidence(d, member));
    auto no_verifier = certify_common_coverage(d, peers, {});
    QVERIFY(!no_verifier.ok());
    auto missing = peers;
    missing.pop_back();
    QVERIFY(!certify_common_coverage(d, missing, [](const auto &, const auto &) {
      return Result<void>{};
    }).ok());
    auto stale = peers;
    stale.front().descriptor_hash = sha256_hex("old membership");
    QVERIFY(!certify_common_coverage(d, stale, [](const auto &, const auto &) {
      return Result<void>{};
    }).ok());
    auto rejected = certify_common_coverage(d, peers, [](const auto &, const auto &) {
      return Result<void>(Error(ErrorCode::corrupt_record, "checkpoint not durable"));
    });
    QVERIFY(!rejected.ok());
    auto throwing = certify_common_coverage(d, peers, [](const auto &, const auto &) -> Result<void> {
      throw std::runtime_error("verifier fault");
    });
    QVERIFY(!throwing.ok());
  }

  void unsupportedStreamAndDuplicateMemberAreRejected() {
    const auto d = descriptor();
    std::vector<PeerCoverageEvidence> peers;
    for (const auto &member : d.members) peers.push_back(evidence(d, member));
    peers.front().delivery["not-in-descriptor"][actor(9)] = 1;
    QVERIFY(!certify_common_coverage(d, peers, [](const auto &, const auto &) {
      return Result<void>{};
    }).ok());
    peers.front().delivery.clear();
    peers.front().delivery["text"][ActorId{}] = 1;
    QVERIFY(!certify_common_coverage(d, peers, [](const auto &, const auto &) {
      return Result<void>{};
    }).ok());
    peers.front().delivery.clear();
    peers.back().member = peers.front().member;
    QVERIFY(!certify_common_coverage(d, peers, [](const auto &, const auto &) {
      return Result<void>{};
    }).ok());
  }

  void silentCurrentMemberConstrainsCoverageToZero() {
    const auto d = descriptor();
    std::vector<PeerCoverageEvidence> peers;
    for (const auto &member : d.members) peers.push_back(evidence(d, member));
    peers[0].delivery["text"][actor(20)] = 9;
    peers[1].delivery["text"][actor(20)] = 8;
    // The third member has a valid durable checkpoint, but no delivery for
    // this producer. It stays in membership, so the common frontier is zero.
    auto certificate = certify_common_coverage(d, peers, [](const auto &, const auto &) {
      return Result<void>{};
    });
    QVERIFY(certificate.ok());
    QVERIFY(!certificate.value().common_delivery().contains("text"));
  }
};

QTEST_GUILESS_MAIN(TestGcCertificate)
#include "tst_gc_certificate.moc"
