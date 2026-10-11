#pragma once

#include "storage/GcCertificate.h"
#include "storage/DurableFile.h"

#include <filesystem>
#include <functional>
#include <optional>

namespace CollabText::Storage {

using CheckpointCoverageVerifier = std::function<Result<void>(
    const MembershipDescriptor &, std::string_view checkpoint_hash,
    const OperationCoverage &, const DeliveryCoverage &)>;

/// Durable, owner-local operation and delivery coverage for immutable
/// membership/checkpoint pairs. Every read revalidates the named checkpoint
/// through the caller's resolver.
class CoverageStore {
public:
  [[nodiscard]] static Result<CoverageStore>
  open(std::filesystem::path directory, ActorId owner,
       const StepHook *hook = nullptr);
  CoverageStore(CoverageStore &&) noexcept = default;
  CoverageStore &operator=(CoverageStore &&) noexcept = default;
  CoverageStore(const CoverageStore &) = delete;
  CoverageStore &operator=(const CoverageStore &) = delete;

  /// Validate a durable checkpoint before atomically publishing its coverage.
  /// Exact retries rewrite and sync the same immutable evidence.
  [[nodiscard]] Result<PeerCoverageEvidence> publish(
      const MembershipDescriptor &descriptor, std::string_view checkpoint_hash,
      const OperationCoverage &operation, const DeliveryCoverage &delivery,
      const CheckpointCoverageVerifier &verify_checkpoint);

  /// Recover only the exact membership/checkpoint pair. Missing evidence is an
  /// empty optional; corrupt, stale, or no-longer-valid evidence fails closed.
  [[nodiscard]] Result<std::optional<PeerCoverageEvidence>> recover(
      const MembershipDescriptor &descriptor, std::string_view checkpoint_hash,
      const CheckpointCoverageVerifier &verify_checkpoint) const;

private:
  CoverageStore(std::filesystem::path directory, ActorId owner,
                const StepHook *hook, ProcessLease lease)
      : m_directory(std::move(directory)), m_owner(owner), m_hook(hook),
        m_lease(std::move(lease)) {}
  [[nodiscard]] std::filesystem::path record_path(
      std::string_view descriptor_hash, std::string_view checkpoint_hash) const;

  std::filesystem::path m_directory;
  ActorId m_owner;
  const StepHook *m_hook = nullptr;
  std::optional<ProcessLease> m_lease;
};

} // namespace CollabText::Storage
