#pragma once

#include "storage/MembershipStore.h"

#include <functional>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace CollabText::Storage {

template <typename Tag>
class StreamCoverage : public std::map<std::string, std::map<ActorId, uint64_t>> {
  using Base = std::map<std::string, std::map<ActorId, uint64_t>>;

public:
  using Base::Base;
  using Base::operator=;
};

struct OperationCoverageTag;
/// Per-stream engine operation frontiers. These counters are distinct from
/// envelope delivery sequence numbers and must never be substituted for them.
using OperationCoverage = StreamCoverage<OperationCoverageTag>;

struct DeliveryCoverageTag;
/// Per-stream producer delivery sequence coverage. The outer key preserves
/// stream identity, including IdList streams, when actors reuse counters.
using DeliveryCoverage = StreamCoverage<DeliveryCoverageTag>;

/// Provenance-bearing view of one stream's operation cutoff. It can only be
/// produced from an immutable, fully checked GC certificate, and retains the
/// descriptor/checkpoint binding needed by DocumentSession before installation.
class CertifiedOperationFrontier {
public:
  [[nodiscard]] const std::string &stream() const { return m_stream; }
  [[nodiscard]] const std::string &descriptor_hash() const { return m_descriptor_hash; }
  [[nodiscard]] const std::string &checkpoint_hash() const { return m_checkpoint_hash; }
  [[nodiscard]] const std::map<ActorId, uint64_t> &counters() const { return m_counters; }
  [[nodiscard]] bool matches_context(std::string_view stream,
                                     std::string_view descriptor_hash,
                                     std::string_view checkpoint_hash) const {
    return stream == m_stream && descriptor_hash == m_descriptor_hash &&
           checkpoint_hash == m_checkpoint_hash;
  }

private:
  CertifiedOperationFrontier(std::string stream, std::string descriptor_hash,
                             std::string checkpoint_hash,
                             std::map<ActorId, uint64_t> counters)
      : m_stream(std::move(stream)),
        m_descriptor_hash(std::move(descriptor_hash)),
        m_checkpoint_hash(std::move(checkpoint_hash)),
        m_counters(std::move(counters)) {}

  std::string m_stream;
  std::string m_descriptor_hash;
  std::string m_checkpoint_hash;
  std::map<ActorId, uint64_t> m_counters;
  friend class GcCertificate;
};

struct PeerCoverageEvidence {
  ActorId member;
  uint64_t epoch = 0;
  std::string descriptor_hash;
  std::string checkpoint_hash;
  OperationCoverage operation;
  DeliveryCoverage delivery;
};

/// Must establish that this exact evidence was durably published by `member`
/// and that its checkpoint artifact is complete and valid.
using DurableCoverageVerifier = std::function<Result<void>(
    const MembershipDescriptor &, const PeerCoverageEvidence &)>;

/// Must re-evaluate every G01 eligibility condition for the exact common
/// frontier: complete checkpoint state, deferred input, epoch/proposal and
/// acknowledgement set, durable private history/undo retirement, live pins,
/// and required token translations. Coverage alone can never authorize GC.
using GcEligibilityVerifier = std::function<Result<void>(
    const MembershipDescriptor &, std::string_view checkpoint_hash,
    const OperationCoverage &common_operation,
    const DeliveryCoverage &common_delivery,
    const std::vector<PeerCoverageEvidence> &evidence)>;

/// Immutable result for one exact membership descriptor and checkpoint.
/// Instances can only be created by certify_common_coverage after every
/// current member's durable evidence and the complete G01 eligibility contract
/// have been revalidated.
class GcCertificate {
public:
  [[nodiscard]] const std::string &descriptor_hash() const { return m_descriptor_hash; }
  [[nodiscard]] const std::string &checkpoint_hash() const { return m_checkpoint_hash; }
  [[nodiscard]] const OperationCoverage &common_operation() const { return m_common_operation; }
  [[nodiscard]] const DeliveryCoverage &common_delivery() const { return m_common_delivery; }
  /// Return the certified operation frontier for one exact stream. Missing
  /// streams have no reclamation authority and return nullptr.
  [[nodiscard]] std::optional<CertifiedOperationFrontier>
  operation_frontier(std::string_view stream) const {
    const auto found = m_common_operation.find(std::string(stream));
    if (found == m_common_operation.end()) return std::nullopt;
    return CertifiedOperationFrontier(std::string(stream), m_descriptor_hash,
                                      m_checkpoint_hash, found->second);
  }

private:
  GcCertificate(std::string descriptor_hash, std::string checkpoint_hash,
                OperationCoverage common_operation,
                DeliveryCoverage common_delivery)
      : m_descriptor_hash(std::move(descriptor_hash)),
        m_checkpoint_hash(std::move(checkpoint_hash)),
        m_common_operation(std::move(common_operation)),
        m_common_delivery(std::move(common_delivery)) {}

  std::string m_descriptor_hash;
  std::string m_checkpoint_hash;
  OperationCoverage m_common_operation;
  DeliveryCoverage m_common_delivery;
  friend Result<GcCertificate> certify_common_coverage(
      const MembershipDescriptor &, const std::vector<PeerCoverageEvidence> &,
      const DurableCoverageVerifier &, const GcEligibilityVerifier &);
};

/// Certify the true per-stream intersection for the exact current membership.
/// Missing/extra members, stale descriptors, mismatched checkpoint references,
/// unsupported streams, and absent/failed durable verification all fail closed.
[[nodiscard]] Result<GcCertificate> certify_common_coverage(
    const MembershipDescriptor &descriptor,
    const std::vector<PeerCoverageEvidence> &evidence,
    const DurableCoverageVerifier &verify_durable,
    const GcEligibilityVerifier &verify_g01_eligibility);

} // namespace CollabText::Storage
