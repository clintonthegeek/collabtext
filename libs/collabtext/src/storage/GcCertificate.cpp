#include "storage/GcCertificate.h"

#include <algorithm>
#include <exception>
#include <set>

namespace CollabText::Storage {
namespace {
template <typename Coverage, typename StreamSelector>
Result<void> validate_frontier(const MembershipDescriptor &descriptor,
                               const Coverage &coverage,
                               StreamSelector select) {
  for (const auto &[stream, frontier] : coverage) {
    const auto declared = std::lower_bound(
        descriptor.base_frontiers.begin(), descriptor.base_frontiers.end(), stream,
        [](const auto &entry, const auto &name) { return entry.stream < name; });
    if (declared == descriptor.base_frontiers.end() || declared->stream != stream)
      return Error(ErrorCode::corrupt_record,
                   "coverage names a stream absent from the membership descriptor", stream);
    const auto &producers = select(*declared);
    for (const auto &[producer, through] : frontier)
      if (!producer.valid() || !producers.contains(producer) || through == 0 ||
          through == UINT64_MAX)
        return Error(ErrorCode::corrupt_record,
                     "coverage producer is absent from the descriptor frontier", stream);
  }
  for (const auto &base : descriptor.base_frontiers) {
    const auto &required = select(base);
    const auto stream = coverage.find(base.stream);
    for (const auto &[producer, through] : required) {
      if (through == 0) continue;
      if (stream == coverage.end())
        return Error(ErrorCode::corrupt_record,
                     "coverage regresses below the membership base frontier", base.stream);
      const auto observed = stream->second.find(producer);
      if (observed == stream->second.end() || observed->second < through)
        return Error(ErrorCode::corrupt_record,
                     "coverage regresses below the membership base frontier", base.stream);
    }
  }
  return {};
}

template <typename Coverage, typename StreamSelector>
Coverage common_frontier(const MembershipDescriptor &descriptor,
                         const std::vector<const PeerCoverageEvidence *> &peers,
                         StreamSelector select) {
  Coverage common;
  for (const auto &stream_frontier : descriptor.base_frontiers) {
    std::set<ActorId> producers;
    for (const auto *peer : peers) {
      const auto &coverage = select(*peer);
      const auto stream = coverage.find(stream_frontier.stream);
      if (stream != coverage.end())
        for (const auto &[producer, _] : stream->second) producers.insert(producer);
    }
    auto &frontier = common[stream_frontier.stream];
    for (const auto &producer : producers) {
      uint64_t through = UINT64_MAX;
      for (const auto *peer : peers) {
        const auto &coverage = select(*peer);
        const auto stream = coverage.find(stream_frontier.stream);
        if (stream == coverage.end()) { through = 0; break; }
        const auto found = stream->second.find(producer);
        through = std::min(through, found == stream->second.end() ? 0 : found->second);
      }
      if (through != 0) frontier.emplace(producer, through);
    }
    if (frontier.empty()) common.erase(stream_frontier.stream);
  }
  return common;
}

bool is_sha256(std::string_view value) {
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
} // namespace

Result<GcCertificate> certify_common_coverage(
    const MembershipDescriptor &descriptor,
    const std::vector<PeerCoverageEvidence> &evidence,
    const DurableCoverageVerifier &verify_durable,
    const GcEligibilityVerifier &verify_g01_eligibility) {
  if (auto valid = validate_membership_descriptor(descriptor); !valid)
    return valid.error().with_context("GC coverage certificate");
  if (descriptor.members.empty() || evidence.size() != descriptor.members.size() ||
      !verify_durable || !verify_g01_eligibility)
    return Error(ErrorCode::awaiting_state,
                 "certificate requires durable evidence and full G01 eligibility verification");

  std::set<ActorId> seen;
  std::string checkpoint_hash;
  std::vector<const PeerCoverageEvidence *> ordered;
  ordered.reserve(evidence.size());
  for (const auto &item : evidence) {
    if (!item.member.valid() || !seen.insert(item.member).second ||
        !std::binary_search(descriptor.members.begin(), descriptor.members.end(),
                            item.member) ||
        item.epoch != descriptor.epoch ||
        item.descriptor_hash != descriptor.hash ||
        !is_sha256(item.checkpoint_hash) ||
        (!checkpoint_hash.empty() && item.checkpoint_hash != checkpoint_hash))
      return Error(ErrorCode::wrong_epoch,
                   "coverage evidence is duplicate, stale, or outside membership");
    checkpoint_hash = item.checkpoint_hash;
    if (auto valid = validate_frontier(descriptor, item.operation,
          [](const auto &base) -> const auto & { return base.operation; }); !valid)
      return valid.error();
    if (auto valid = validate_frontier(descriptor, item.delivery,
          [](const auto &base) -> const auto & { return base.delivery; }); !valid)
      return valid.error();
    Result<void> verified;
    try {
      verified = verify_durable(descriptor, item);
    } catch (const std::exception &error) {
      return Error(ErrorCode::io_error, error.what(),
                   "durable peer coverage verifier threw");
    } catch (...) {
      return Error(ErrorCode::io_error,
                   "durable peer coverage verifier threw an unknown exception");
    }
    if (!verified.ok()) return verified.error().with_context("verifying peer coverage");
    ordered.push_back(&item);
  }
  if (seen.size() != descriptor.members.size())
    return Error(ErrorCode::awaiting_state,
                 "coverage evidence omits a current member");

  // The meet ranges over every stream and producer mentioned by any member.
  // Absence at even one member is zero; it must not disappear from the result
  // by taking a union or maximum.
  const auto common_operation = common_frontier<OperationCoverage>(
      descriptor, ordered, [](const auto &peer) -> const auto & { return peer.operation; });
  const auto common_delivery = common_frontier<DeliveryCoverage>(
      descriptor, ordered, [](const auto &peer) -> const auto & { return peer.delivery; });
  Result<void> eligible;
  try {
    eligible = verify_g01_eligibility(descriptor, checkpoint_hash, common_operation,
                                      common_delivery, evidence);
  } catch (const std::exception &error) {
    return Error(ErrorCode::io_error, error.what(),
                 "G01 reclamation eligibility verifier threw");
  } catch (...) {
    return Error(ErrorCode::io_error,
                 "G01 reclamation eligibility verifier threw an unknown exception");
  }
  if (!eligible.ok()) return eligible.error().with_context("G01 reclamation eligibility");
  return GcCertificate(descriptor.hash, std::move(checkpoint_hash),
                       common_operation, common_delivery);
}

} // namespace CollabText::Storage
