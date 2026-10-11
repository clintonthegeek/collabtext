#include <QTest>
#include <QTemporaryDir>

#include "storage/CoverageStore.h"
#include "storage/CheckpointCodec.h"
#include "storage/GcCertificate.h"
#include "core/Sha256.h"
#include "reference/GcModel.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <type_traits>

using namespace CollabText;
using namespace CollabText::Storage;

namespace {
static_assert(!std::is_same_v<OperationCoverage, DeliveryCoverage>);

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
  return {member, d.epoch, d.hash, d.base_checkpoint_hash, {}, {}};
}

template <typename Coverage>
GcReference::Coverage reference_coverage(const Coverage &coverage) {
  GcReference::Coverage result;
  for (const auto &[stream, frontier] : coverage)
    for (const auto &[producer, counter] : frontier)
      result[stream].emplace(producer.to_string(), counter);
  return result;
}

GcEligibilityVerifier model_eligibility(
    std::function<void(GcReference::World &)> alter = {}) {
  return [alter = std::move(alter)](
      const MembershipDescriptor &descriptor, std::string_view checkpoint_hash,
      const OperationCoverage &common,
      const DeliveryCoverage &,
      const std::vector<PeerCoverageEvidence> &evidence) {
    GcReference::World world;
    world.epoch.number = descriptor.epoch;
    world.epoch.hash = descriptor.hash;
    world.proposal = {"candidate-1", descriptor.hash,
                      std::string(checkpoint_hash), reference_coverage(common),
                      {{"undo-owner", 1}}, 1, {"translation-manifest"}};
    world.checkpoint.hash = std::string(checkpoint_hash);
    world.checkpoint.epoch_hash = descriptor.hash;
    world.checkpoint.coverage = reference_coverage(common);
    world.checkpoint.durable = true;
    world.checkpoint.integrity_valid = true;
    world.checkpoint.logical_state = true;
    world.checkpoint.deferred_input = true;
    world.checkpoint.coverage_state = true;
    world.checkpoint.pin_projection = true;
    world.pins.complete = true;
    world.pins.frontier = 1;
    const auto retired_undo = GcReference::UndoFloor{{"undo-owner", 1}};
    for (const auto &member_id : descriptor.members) {
      const auto name = member_id.to_string();
      world.epoch.members.insert(name);
      auto item = std::find_if(evidence.begin(), evidence.end(),
          [&](const auto &candidate) { return candidate.member == member_id; });
      if (item == evidence.end()) continue;
      GcReference::Member member;
      member.durable_coverage = reference_coverage(item->operation);
      member.retired_undo_sequence = retired_undo;
      world.members.emplace(name, member);
      world.acknowledgements.emplace(name, GcReference::PeerAck{
          descriptor.hash, world.proposal.id, member.durable_coverage,
          retired_undo, 1, std::string(checkpoint_hash), true, true});
    }
    if (alter) alter(world);
    return GcReference::eligible(world)
        ? Result<void>{}
        : Result<void>(Error(ErrorCode::awaiting_state,
                             "independent G01 reference model rejects eligibility"));
  };
}
}

class TestGcCertificate final : public QObject {
  Q_OBJECT
private slots:
  void intersectionMatchesReferenceAcrossStreamsAndCounterReuse() {
    const auto d = descriptor();
    const auto candidate_checkpoint = sha256_hex("candidate checkpoint");
    const auto p = actor(20);
    const auto q = actor(21);
    std::vector<PeerCoverageEvidence> peers;
    for (const auto &member : d.members) peers.push_back(evidence(d, member));
    for (auto &peer : peers) peer.checkpoint_hash = candidate_checkpoint;
    peers[0].operation = {{"text", {{p, 12}, {q, 2}}},
                         {"comments", {{p, 8}}},
                         {"ids", {{p, 5}}}};
    peers[1].operation = {{"text", {{p, 9}, {q, 4}}},
                         {"comments", {{p, 10}}},
                         {"ids", {{p, 3}}}};
    peers[2].operation = {{"text", {{p, 11}}},
                         {"comments", {{p, 7}}},
                         {"ids", {{p, 6}}}};
    peers[0].delivery = {{"text", {{p, 30}, {q, 2}}},
                         {"comments", {{p, 18}}}, {"ids", {{p, 15}}}};
    peers[1].delivery = {{"text", {{p, 20}, {q, 4}}},
                         {"comments", {{p, 10}}}, {"ids", {{p, 13}}}};
    peers[2].delivery = {{"text", {{p, 25}}},
                         {"comments", {{p, 17}}}, {"ids", {{p, 16}}}};
    const DurableCoverageVerifier verify = [](const auto &, const auto &) {
      return Result<void>{};
    };
    auto certificate = certify_common_coverage(d, peers, verify,
                                               model_eligibility());
    QVERIFY(certificate.ok());

    std::vector<GcReference::Coverage> vectors;
    for (const auto &peer : peers) {
      GcReference::Coverage item;
      for (const auto &[stream, frontier] : peer.operation) {
        auto &dest = item[stream];
        for (const auto &[producer, count] : frontier)
          dest.emplace(producer.to_string(), count);
      }
      vectors.push_back(std::move(item));
    }
    const auto expected = GcReference::meet(vectors);
    QCOMPARE(certificate.value().descriptor_hash(), d.hash);
    QCOMPARE(certificate.value().checkpoint_hash(), candidate_checkpoint);
    const auto &actual = certificate.value().common_operation();
    QCOMPARE(actual.at("text").at(p), uint64_t(9));
    QCOMPARE(actual.at("text").find(q), actual.at("text").end());
    QCOMPARE(actual.at("comments").at(p), uint64_t(7));
    QCOMPARE(actual.at("ids").at(p), uint64_t(3));
    GcReference::Coverage converted;
    for (const auto &[stream, frontier] : actual)
      for (const auto &[producer, count] : frontier)
        converted[stream].emplace(producer.to_string(), count);
    QCOMPARE(converted, expected);
    const auto &delivery = certificate.value().common_delivery();
    QCOMPARE(delivery.at("text").at(p), uint64_t(20));
    QCOMPARE(delivery.at("comments").at(p), uint64_t(10));
    QCOMPARE(delivery.at("ids").at(p), uint64_t(13));
  }

  void missingStaleAndUnverifiedMembersCannotCertify() {
    const auto d = descriptor();
    std::vector<PeerCoverageEvidence> peers;
    for (const auto &member : d.members) peers.push_back(evidence(d, member));
    auto no_verifier = certify_common_coverage(d, peers, {}, model_eligibility());
    QVERIFY(!no_verifier.ok());
    auto no_eligibility_verifier = certify_common_coverage(
        d, peers, [](const auto &, const auto &) { return Result<void>{}; }, {});
    QVERIFY(!no_eligibility_verifier.ok());
    auto deferred_input = certify_common_coverage(
        d, peers, [](const auto &, const auto &) { return Result<void>{}; },
        model_eligibility([](auto &world) {
          world.checkpoint.deferred_input = false;
        }));
    QVERIFY(!deferred_input.ok());
    auto private_undo_unverified = certify_common_coverage(
        d, peers, [](const auto &, const auto &) { return Result<void>{}; },
        model_eligibility([](auto &world) {
          world.acknowledgements.begin()->second.private_history_durable = false;
        }));
    QVERIFY(!private_undo_unverified.ok());
    auto incomplete_pins = certify_common_coverage(
        d, peers, [](const auto &, const auto &) { return Result<void>{}; },
        model_eligibility([](auto &world) { world.pins.complete = false; }));
    QVERIFY(!incomplete_pins.ok());
    auto eligibility_threw = certify_common_coverage(
        d, peers, [](const auto &, const auto &) { return Result<void>{}; },
        [](const auto &, std::string_view, const OperationCoverage &,
           const DeliveryCoverage &,
           const std::vector<PeerCoverageEvidence> &) -> Result<void> {
          throw std::runtime_error("eligibility verifier fault");
        });
    QVERIFY(!eligibility_threw.ok());
    auto missing = peers;
    missing.pop_back();
    QVERIFY(!certify_common_coverage(d, missing, [](const auto &, const auto &) {
      return Result<void>{};
    }, model_eligibility()).ok());
    auto stale = peers;
    stale.front().descriptor_hash = sha256_hex("old membership");
    QVERIFY(!certify_common_coverage(d, stale, [](const auto &, const auto &) {
      return Result<void>{};
    }, model_eligibility()).ok());
    auto rejected = certify_common_coverage(d, peers, [](const auto &, const auto &) {
      return Result<void>(Error(ErrorCode::corrupt_record, "checkpoint not durable"));
    }, model_eligibility());
    QVERIFY(!rejected.ok());
    auto throwing = certify_common_coverage(d, peers, [](const auto &, const auto &) -> Result<void> {
      throw std::runtime_error("verifier fault");
    }, model_eligibility());
    QVERIFY(!throwing.ok());
    auto mixed_checkpoint = peers;
    mixed_checkpoint.back().checkpoint_hash = sha256_hex("different checkpoint");
    QVERIFY(!certify_common_coverage(d, mixed_checkpoint,
        [](const auto &, const auto &) { return Result<void>{}; },
        model_eligibility()).ok());
  }

  void unsupportedStreamAndDuplicateMemberAreRejected() {
    const auto d = descriptor();
    std::vector<PeerCoverageEvidence> peers;
    for (const auto &member : d.members) peers.push_back(evidence(d, member));
    peers.front().delivery["not-in-descriptor"][actor(9)] = 1;
    QVERIFY(!certify_common_coverage(d, peers, [](const auto &, const auto &) {
      return Result<void>{};
    }, model_eligibility()).ok());
    peers.front().delivery.clear();
    peers.front().operation["not-in-descriptor"][actor(9)] = 1;
    QVERIFY(!certify_common_coverage(d, peers, [](const auto &, const auto &) {
      return Result<void>{};
    }, model_eligibility()).ok());
    peers.front().operation.clear();
    peers.front().operation["text"][actor(9)] = 1;
    QVERIFY(!certify_common_coverage(d, peers, [](const auto &, const auto &) {
      return Result<void>{};
    }, model_eligibility()).ok());
    peers.front().operation.clear();
    peers.front().delivery["text"][ActorId{}] = 1;
    QVERIFY(!certify_common_coverage(d, peers, [](const auto &, const auto &) {
      return Result<void>{};
    }, model_eligibility()).ok());
    peers.front().delivery.clear();
    peers.back().member = peers.front().member;
    QVERIFY(!certify_common_coverage(d, peers, [](const auto &, const auto &) {
      return Result<void>{};
    }, model_eligibility()).ok());
  }

  void silentCurrentMemberConstrainsCoverageToZero() {
    const auto d = descriptor();
    std::vector<PeerCoverageEvidence> peers;
    for (const auto &member : d.members) peers.push_back(evidence(d, member));
    peers[0].delivery["text"][actor(20)] = 9;
    peers[1].delivery["text"][actor(20)] = 8;
    // The third member has a valid durable checkpoint, but no delivery for
    // this producer. It stays in membership, so the common frontier is zero.
    auto certificate = certify_common_coverage(
        d, peers, [](const auto &, const auto &) { return Result<void>{}; },
        model_eligibility());
    QVERIFY(certificate.ok());
    QVERIFY(!certificate.value().common_delivery().contains("text"));
  }

  void coverageCannotRegressMembershipBaseFrontiers() {
    auto base = descriptor();
    auto text = std::find_if(base.base_frontiers.begin(), base.base_frontiers.end(),
        [](const auto &frontier) { return frontier.stream == "text"; });
    QVERIFY(text != base.base_frontiers.end());
    text->operation[actor(20)] = 4;
    text->delivery[actor(20)] = 7;
    auto sealed = seal_membership_descriptor(std::move(base));
    QVERIFY(sealed.ok());
    const auto d = std::move(sealed.value());

    std::vector<PeerCoverageEvidence> peers;
    for (const auto &member : d.members) {
      auto peer = evidence(d, member);
      peer.operation["text"][actor(20)] = 4;
      peer.delivery["text"][actor(20)] = 7;
      peers.push_back(std::move(peer));
    }
    auto at_base = certify_common_coverage(
        d, peers, [](const auto &, const auto &) { return Result<void>{}; },
        model_eligibility());
    QVERIFY(at_base.ok());

    auto operation_regression = peers;
    operation_regression.front().operation["text"][actor(20)] = 3;
    QVERIFY(!certify_common_coverage(
        d, operation_regression,
        [](const auto &, const auto &) { return Result<void>{}; },
        model_eligibility()).ok());

    auto delivery_regression = peers;
    delivery_regression.front().delivery["text"][actor(20)] = 6;
    QVERIFY(!certify_common_coverage(
        d, delivery_regression,
        [](const auto &, const auto &) { return Result<void>{}; },
        model_eligibility()).ok());
  }

  void verifiedMembershipCheckpointCoverageCertifiesAfterRecovery() {
    auto d = descriptor();
    const OperationCoverage operation{{"text", {{actor(20), 8}}},
                                      {"ids", {{actor(21), 3}}}};
    const DeliveryCoverage delivery{{"text", {{actor(20), 14}}},
                                    {"ids", {{actor(21), 9}}}};
    std::vector<MembershipCheckpointBundleEntry> entries;
    std::string translation_manifest_hash;
    for (const auto &base : d.base_frontiers) {
      CheckpointShareableCore core;
      core.context = CheckpointContext{
          "coverage-document", base.stream, d.epoch, d.parent_hash,
          CheckpointEngineKind::Text};
      core.coverage.operation = operation.contains(base.stream)
                                    ? operation.at(base.stream)
                                    : std::map<ActorId, uint64_t>{};
      core.coverage.delivery = delivery.contains(base.stream)
                                   ? delivery.at(base.stream)
                                   : std::map<ActorId, uint64_t>{};
      for (const auto &[producer, through] : core.coverage.operation)
        core.causal.version.observe(Crdt::Identity{producer, through});
      core.pins.canonical_projection_hash = sha256_hex("");
      core.translations.supported_token_versions = d.supported_token_versions;
      auto bytes = encode_checkpoint_core(core);
      QVERIFY(bytes.ok());
      auto frame = decode_checkpoint(bytes.value(), CheckpointKind::Shareable);
      QVERIFY(frame.ok());
      auto decoded = decode_checkpoint_core(frame.value());
      QVERIFY(decoded.ok());
      if (translation_manifest_hash.empty())
        translation_manifest_hash = decoded.value().translations.manifest_hash;
      QCOMPARE(decoded.value().translations.manifest_hash,
               translation_manifest_hash);
      entries.push_back({base.stream, std::move(bytes.value())});
    }
    d.translation_manifest_hash = translation_manifest_hash;
    auto sealed = seal_membership_descriptor(std::move(d));
    QVERIFY(sealed.ok());
    d = std::move(sealed.value());
    auto bundle = encode_membership_checkpoint_bundle(std::move(entries));
    QVERIFY(bundle.ok());
    const auto checkpoint_hash = sha256_hex(bundle.value());
    const auto verifier = make_membership_checkpoint_coverage_verifier(
        [&](std::string_view requested_hash) -> Result<std::string> {
          if (requested_hash != checkpoint_hash)
            return Error(ErrorCode::awaiting_state,
                         "candidate checkpoint bundle is unavailable");
          return bundle.value();
        });
    const auto evidence_verifier = make_membership_checkpoint_evidence_verifier(
        [&](std::string_view requested_hash) -> Result<std::string> {
          if (requested_hash != checkpoint_hash)
            return Error(ErrorCode::awaiting_state,
                         "candidate checkpoint bundle is unavailable");
          return bundle.value();
        });
    QVERIFY(static_cast<bool>(verifier));
    QVERIFY(static_cast<bool>(evidence_verifier));
    QVERIFY(verifier(d, checkpoint_hash, operation, delivery).ok());
    auto below_checkpoint = operation;
    below_checkpoint["text"][actor(20)] = 7;
    QVERIFY(!evidence_verifier(d, checkpoint_hash, below_checkpoint, delivery).ok());
    QVERIFY(!verifier(d, checkpoint_hash, below_checkpoint, delivery).ok());
    auto below_delivery_checkpoint = delivery;
    below_delivery_checkpoint["text"][actor(20)] = 13;
    QVERIFY(!evidence_verifier(d, checkpoint_hash, operation,
                               below_delivery_checkpoint).ok());

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdString());
    for (size_t i = 0; i < d.members.size(); ++i) {
      const auto &member = d.members[i];
      const auto advance = static_cast<uint64_t>(d.members.size() - i - 1);
      auto member_operation = operation;
      auto member_delivery = delivery;
      for (auto &[stream, frontier] : member_operation) {
        (void)stream;
        for (auto &[producer, through] : frontier) {
          (void)producer;
          through += advance;
        }
      }
      for (auto &[stream, frontier] : member_delivery) {
        (void)stream;
        for (auto &[producer, through] : frontier) {
          (void)producer;
          through += advance;
        }
      }
      auto store = CoverageStore::open(root, member);
      QVERIFY(store.ok());
      auto published = store.value().publish(
          d, checkpoint_hash, member_operation, member_delivery,
          evidence_verifier);
      QVERIFY(published.ok());
      QCOMPARE(published.value().operation, member_operation);
      QCOMPARE(published.value().delivery, member_delivery);
    }

    std::vector<PeerCoverageEvidence> recovered_peers;
    for (size_t i = 0; i < d.members.size(); ++i) {
      const auto &member = d.members[i];
      const auto advance = static_cast<uint64_t>(d.members.size() - i - 1);
      auto member_operation = operation;
      auto member_delivery = delivery;
      for (auto &[stream, frontier] : member_operation) {
        (void)stream;
        for (auto &[producer, through] : frontier) {
          (void)producer;
          through += advance;
        }
      }
      for (auto &[stream, frontier] : member_delivery) {
        (void)stream;
        for (auto &[producer, through] : frontier) {
          (void)producer;
          through += advance;
        }
      }
      auto store = CoverageStore::open(root, member);
      QVERIFY(store.ok());
      auto recovered = store.value().recover(d, checkpoint_hash, evidence_verifier);
      QVERIFY(recovered.ok());
      QVERIFY(recovered.value().has_value());
      QCOMPARE(recovered.value()->operation, member_operation);
      QCOMPARE(recovered.value()->delivery, member_delivery);
      recovered_peers.push_back(std::move(*recovered.value()));
    }
    const DurableCoverageVerifier durable = [&](const auto &descriptor,
                                                 const auto &item) -> Result<void> {
      if (descriptor.hash != d.hash)
        return Error(ErrorCode::wrong_epoch, "membership descriptor changed");
      auto store = CoverageStore::open(root, item.member);
      if (!store) return store.error();
      auto recovered = store.value().recover(d, checkpoint_hash, evidence_verifier);
      if (!recovered) return recovered.error();
      if (!recovered.value() || recovered.value()->operation != item.operation ||
          recovered.value()->delivery != item.delivery)
        return Error(ErrorCode::corrupt_record,
                     "peer coverage differs from its durable record");
      return {};
    };
    const GcEligibilityVerifier eligibility =
        [&](const auto &candidate, std::string_view hash,
            const auto &common_operation, const auto &common_delivery,
            const auto &peers) -> Result<void> {
          auto checkpoint_matches = verifier(candidate, hash, common_operation,
                                             common_delivery);
          if (!checkpoint_matches.ok()) return checkpoint_matches.error();
          return model_eligibility()(candidate, hash, common_operation,
                                    common_delivery, peers);
        };
    auto certificate = certify_common_coverage(
        d, recovered_peers, durable, eligibility);
    QVERIFY(certificate.ok());
    QCOMPARE(certificate.value().checkpoint_hash(), checkpoint_hash);
    QCOMPARE(certificate.value().common_operation(), operation);
    QCOMPARE(certificate.value().common_delivery(), delivery);

    auto wrong_operation = operation;
    wrong_operation["text"][actor(20)]++;
    QVERIFY(!verifier(d, checkpoint_hash, wrong_operation, delivery).ok());
    auto wrong_delivery = delivery;
    wrong_delivery["ids"][actor(21)]++;
    QVERIFY(!verifier(d, checkpoint_hash, operation, wrong_delivery).ok());
    auto unsupported = operation;
    unsupported["unknown"][actor(20)] = 1;
    QVERIFY(!verifier(d, checkpoint_hash, unsupported, delivery).ok());
    QVERIFY(!verifier(d, sha256_hex("different candidate"), operation, delivery).ok());
  }

  void coveragePublicationRecoversOnlyAfterCheckpointRevalidation() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    namespace fs = std::filesystem;
    const auto root = fs::path(temporary.path().toStdString());
    const auto candidate_checkpoint = sha256_hex("stored candidate checkpoint");
    const DeliveryCoverage coverage{{"text", {{actor(20), 9}}},
                                    {"ids", {{actor(21), 3}}}};
    const OperationCoverage operations{{"text", {{actor(20), 5}}},
                                       {"ids", {{actor(21), 2}}}};
    auto base = descriptor();
    for (auto &frontier : base.base_frontiers) {
      if (auto found = operations.find(frontier.stream); found != operations.end())
        for (const auto &[producer, through] : found->second)
          frontier.operation[producer] = through;
      if (auto found = coverage.find(frontier.stream); found != coverage.end())
        for (const auto &[producer, through] : found->second)
          frontier.delivery[producer] = through;
    }
    auto sealed = seal_membership_descriptor(std::move(base));
    QVERIFY(sealed.ok());
    const auto d = std::move(sealed.value());
    const auto owner = d.members.front();
    size_t validations = 0;
    const CheckpointCoverageVerifier valid_checkpoint =
        [&](const auto &checked_descriptor, std::string_view checked_hash,
            const auto &checked_operations, const auto &checked_coverage) {
          ++validations;
          if (checked_descriptor.hash != d.hash ||
              checked_hash != candidate_checkpoint || checked_operations != operations ||
              checked_coverage != coverage)
            return Result<void>(Error(ErrorCode::corrupt_record,
                                      "checkpoint coverage mismatch"));
          return Result<void>{};
        };
    bool fail_directory_sync = true;
    StepHook hook;
    hook.fail = [&](Step step, const fs::path &) -> std::optional<Error> {
      if (step == Step::DirSync && fail_directory_sync) {
        fail_directory_sync = false;
        return Error(ErrorCode::io_error, "injected directory sync failure");
      }
      return std::nullopt;
    };
    {
      auto opened = CoverageStore::open(root, owner, &hook);
      QVERIFY(opened.ok());
      auto regressed_operations = operations;
      regressed_operations["text"][actor(20)] = 4;
      QVERIFY(!opened.value().publish(
          d, candidate_checkpoint, regressed_operations, coverage,
          [](const auto &, std::string_view, const auto &, const auto &) {
            return Result<void>{};
          }).ok());
      auto regressed_delivery = coverage;
      regressed_delivery["text"][actor(20)] = 8;
      QVERIFY(!opened.value().publish(
          d, candidate_checkpoint, operations, regressed_delivery,
          [](const auto &, std::string_view, const auto &, const auto &) {
            return Result<void>{};
          }).ok());
      auto uncertain = opened.value().publish(d, candidate_checkpoint,
                                              operations, coverage, valid_checkpoint);
      QVERIFY(!uncertain.ok());
      auto retried = opened.value().publish(d, candidate_checkpoint,
                                            operations, coverage, valid_checkpoint);
      QVERIFY(retried.ok());
      QCOMPARE(retried.value().delivery, coverage);
      auto conflicting = coverage;
      conflicting["text"][actor(20)] = 10;
      QVERIFY(!opened.value().publish(
          d, candidate_checkpoint, operations, conflicting,
          [](const auto &, std::string_view, const auto &, const auto &) {
            return Result<void>{};
          }).ok());
      auto conflicting_operations = operations;
      conflicting_operations["text"][actor(20)] = 6;
      QVERIFY(!opened.value().publish(
          d, candidate_checkpoint, conflicting_operations, coverage,
          [](const auto &, std::string_view, const auto &, const auto &) {
            return Result<void>{};
          }).ok());
      auto refused = opened.value().publish(
          d, sha256_hex("bad candidate"), operations, coverage,
          [](const auto &, std::string_view, const auto &, const auto &) {
            return Result<void>(Error(ErrorCode::corrupt_record,
                                      "checkpoint was not durable"));
          });
      QVERIFY(!refused.ok());
    }
    QCOMPARE(validations, size_t(2));
    {
      auto reopened = CoverageStore::open(root, owner);
      QVERIFY(reopened.ok());
      auto recovered = reopened.value().recover(d, candidate_checkpoint,
                                                valid_checkpoint);
      QVERIFY(recovered.ok());
      QVERIFY(recovered.value().has_value());
      QCOMPARE(recovered.value()->delivery, coverage);
      QCOMPARE(recovered.value()->operation, operations);
      QVERIFY(!reopened.value().recover(d, candidate_checkpoint, {}).ok());
      auto stale_checkpoint = reopened.value().recover(
          d, candidate_checkpoint, [](const auto &, std::string_view,
                                      const auto &, const auto &) {
            return Result<void>(Error(ErrorCode::corrupt_record,
                                      "checkpoint no longer validates"));
          });
      QVERIFY(!stale_checkpoint.ok());
      auto absent = reopened.value().recover(
          d, sha256_hex("missing checkpoint"), valid_checkpoint);
      QVERIFY(absent.ok());
      QVERIFY(!absent.value().has_value());
    }
    QCOMPARE(validations, size_t(3));

    fs::path record;
    for (const auto &entry : fs::recursive_directory_iterator(root))
      if (entry.is_regular_file() && entry.path().extension() == ".coverage")
        record = entry.path();
    QVERIFY(!record.empty());
    {
      std::fstream file(record, std::ios::binary | std::ios::in | std::ios::out);
      QVERIFY(file.good());
      file.seekg(-2, std::ios::end);
      char byte = 0;
      file.read(&byte, 1);
      QVERIFY(file.good());
      file.seekp(-2, std::ios::end);
      const char changed = byte == '0' ? '1' : '0';
      file.write(&changed, 1);
      QVERIFY(file.good());
    }
    auto reopened = CoverageStore::open(root, owner);
    QVERIFY(reopened.ok());
    auto corrupt = reopened.value().recover(d, candidate_checkpoint,
                                            valid_checkpoint);
    QVERIFY(!corrupt.ok());
  }

  void recoveredDurablePeerEvidenceFeedsCertificateConstruction() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    namespace fs = std::filesystem;
    const auto root = fs::path(temporary.path().toStdString());
    const auto d = descriptor();
    const auto candidate_checkpoint = sha256_hex("shared certificate checkpoint");
    std::map<ActorId, DeliveryCoverage> expected;
    std::map<ActorId, OperationCoverage> expected_operations;
    for (size_t i = 0; i < d.members.size(); ++i) {
      const auto member = d.members[i];
      expected[member] = {{"text", {{actor(20), 10 - i}}},
                          {"ids", {{actor(21), 6 - i}}}};
      expected_operations[member] = {{"text", {{actor(20), 5 - i}}},
                                     {"ids", {{actor(21), 3 - i}}}};
      auto store = CoverageStore::open(root, member);
      QVERIFY(store.ok());
      auto published = store.value().publish(
          d, candidate_checkpoint, expected_operations.at(member), expected.at(member),
          [&](const auto &checked_descriptor, std::string_view checked_hash,
              const auto &operation, const auto &coverage) {
            if (checked_descriptor.hash != d.hash ||
                checked_hash != candidate_checkpoint ||
                operation != expected_operations.at(member) ||
                coverage != expected.at(member))
              return Result<void>(Error(ErrorCode::corrupt_record,
                                        "durable candidate does not match"));
            return Result<void>{};
          });
      QVERIFY(published.ok());
    }
    std::vector<PeerCoverageEvidence> recovered_evidence;
    for (const auto &member : d.members) {
      auto store = CoverageStore::open(root, member);
      QVERIFY(store.ok());
      auto recovered = store.value().recover(
          d, candidate_checkpoint,
          [&](const auto &checked_descriptor, std::string_view checked_hash,
              const auto &operation, const auto &coverage) {
            if (checked_descriptor.hash != d.hash ||
                checked_hash != candidate_checkpoint ||
                operation != expected_operations.at(member) ||
                coverage != expected.at(member))
              return Result<void>(Error(ErrorCode::corrupt_record,
                                        "recovered checkpoint does not match"));
            return Result<void>{};
          });
      QVERIFY(recovered.ok());
      QVERIFY(recovered.value().has_value());
      recovered_evidence.push_back(std::move(*recovered.value()));
    }
    auto certificate = certify_common_coverage(
        d, recovered_evidence,
        [&](const auto &checked_descriptor, const auto &item) {
          if (checked_descriptor.hash != d.hash ||
              expected.at(item.member) != item.delivery ||
              expected_operations.at(item.member) != item.operation)
            return Result<void>(Error(ErrorCode::corrupt_record,
                                      "peer evidence differs from durable copy"));
          return Result<void>{};
        },
        model_eligibility());
    QVERIFY(certificate.ok());
    QCOMPARE(certificate.value().checkpoint_hash(), candidate_checkpoint);
    QCOMPARE(certificate.value().common_delivery().at("text").at(actor(20)),
             uint64_t(8));
    QCOMPARE(certificate.value().common_delivery().at("ids").at(actor(21)),
             uint64_t(4));
    QCOMPARE(certificate.value().common_operation().at("text").at(actor(20)),
             uint64_t(3));
    QCOMPARE(certificate.value().common_operation().at("ids").at(actor(21)),
             uint64_t(1));
  }
};

QTEST_GUILESS_MAIN(TestGcCertificate)
#include "tst_gc_certificate.moc"
