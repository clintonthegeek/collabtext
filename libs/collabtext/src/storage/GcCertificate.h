#pragma once

#include "storage/MembershipStore.h"

#include <functional>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace CollabText::Storage {

/// Per-stream producer delivery sequence coverage. This is deliberately not
/// an engine operation vector: the outer key prevents counters from different
/// streams (including IdList streams) from being conflated.
using DeliveryCoverage =
    std::map<std::string, std::map<ActorId, uint64_t>>;

struct PeerCoverageEvidence {
  ActorId member;
  uint64_t epoch = 0;
  std::string descriptor_hash;
  std::string checkpoint_hash;
  DeliveryCoverage delivery;
};

/// Must establish that this exact evidence was durably published by `member`
/// and that its checkpoint artifact is complete and valid.
using DurableCoverageVerifier = std::function<Result<void>(
    const MembershipDescriptor &, const PeerCoverageEvidence &)>;

/// Immutable result for one exact membership descriptor and checkpoint.
/// Instances can only be created by certify_common_coverage after every
/// current member's durable evidence has been revalidated.
class GcCertificate {
public:
  [[nodiscard]] const std::string &descriptor_hash() const { return m_descriptor_hash; }
  [[nodiscard]] const std::string &checkpoint_hash() const { return m_checkpoint_hash; }
  [[nodiscard]] const DeliveryCoverage &common_delivery() const { return m_common_delivery; }

private:
  GcCertificate(std::string descriptor_hash, std::string checkpoint_hash,
                DeliveryCoverage common_delivery)
      : m_descriptor_hash(std::move(descriptor_hash)),
        m_checkpoint_hash(std::move(checkpoint_hash)),
        m_common_delivery(std::move(common_delivery)) {}

  std::string m_descriptor_hash;
  std::string m_checkpoint_hash;
  DeliveryCoverage m_common_delivery;
  friend Result<GcCertificate> certify_common_coverage(
      const MembershipDescriptor &, const std::vector<PeerCoverageEvidence> &,
      const DurableCoverageVerifier &);
};

/// Certify the true per-stream intersection for the exact current membership.
/// Missing/extra members, stale descriptors, mismatched checkpoint references,
/// unsupported streams, and absent/failed durable verification all fail closed.
[[nodiscard]] Result<GcCertificate> certify_common_coverage(
    const MembershipDescriptor &descriptor,
    const std::vector<PeerCoverageEvidence> &evidence,
    const DurableCoverageVerifier &verify_durable);

} // namespace CollabText::Storage
