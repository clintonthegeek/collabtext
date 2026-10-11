#include "storage/GcCertificate.h"

#include <algorithm>
#include <exception>
#include <set>

namespace CollabText::Storage {

Result<GcCertificate> certify_common_coverage(
    const MembershipDescriptor &descriptor,
    const std::vector<PeerCoverageEvidence> &evidence,
    const DurableCoverageVerifier &verify_durable) {
  if (auto valid = validate_membership_descriptor(descriptor); !valid)
    return valid.error().with_context("GC coverage certificate");
  if (descriptor.members.empty() || evidence.size() != descriptor.members.size() ||
      !verify_durable)
    return Error(ErrorCode::awaiting_state,
                 "certificate requires durable evidence from every current member");

  std::set<ActorId> seen;
  std::vector<const PeerCoverageEvidence *> ordered;
  ordered.reserve(evidence.size());
  for (const auto &item : evidence) {
    if (!item.member.valid() || !seen.insert(item.member).second ||
        !std::binary_search(descriptor.members.begin(), descriptor.members.end(),
                            item.member) ||
        item.epoch != descriptor.epoch ||
        item.descriptor_hash != descriptor.hash ||
        item.checkpoint_hash != descriptor.base_checkpoint_hash)
      return Error(ErrorCode::wrong_epoch,
                   "coverage evidence is duplicate, stale, or outside membership");
    for (const auto &[stream, frontier] : item.delivery) {
      const auto declared = std::lower_bound(
          descriptor.base_frontiers.begin(), descriptor.base_frontiers.end(), stream,
          [](const auto &entry, const auto &name) { return entry.stream < name; });
      if (declared == descriptor.base_frontiers.end() || declared->stream != stream)
        return Error(ErrorCode::corrupt_record,
                     "coverage names a stream absent from the membership descriptor",
                     stream);
      for (const auto &[producer, through] : frontier) {
        if (!producer.valid() ||
            !declared->delivery.contains(producer))
          return Error(ErrorCode::corrupt_record,
                       "coverage producer is absent from the descriptor frontier",
                       stream);
        (void)through;
      }
    }
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
  DeliveryCoverage common;
  for (const auto &stream_frontier : descriptor.base_frontiers) {
    std::set<ActorId> producers;
    for (const auto *peer : ordered) {
      const auto stream = peer->delivery.find(stream_frontier.stream);
      if (stream != peer->delivery.end())
        for (const auto &[producer, _] : stream->second) producers.insert(producer);
    }
    auto &frontier = common[stream_frontier.stream];
    for (const auto &producer : producers) {
      uint64_t through = UINT64_MAX;
      for (const auto *peer : ordered) {
        const auto stream = peer->delivery.find(stream_frontier.stream);
        if (stream == peer->delivery.end()) { through = 0; break; }
        const auto found = stream->second.find(producer);
        through = std::min(through, found == stream->second.end() ? 0 : found->second);
      }
      if (through != 0) frontier.emplace(producer, through);
    }
    if (frontier.empty()) common.erase(stream_frontier.stream);
  }
  return GcCertificate(descriptor.hash, descriptor.base_checkpoint_hash,
                       std::move(common));
}

} // namespace CollabText::Storage
