#pragma once

#include "storage/CheckpointCodec.h"
#include "storage/DurableFile.h"

#include <functional>
#include <optional>

namespace CollabText::Storage {

/// A checkpoint bundle selected by the durable manifest. `checkpoint_id` is
/// suitable for associating transport receipts with this recoverable state.
struct StoredCheckpoint {
  uint64_t generation = 0;
  std::string checkpoint_id;
  std::string shareable_bytes;
  std::optional<std::string> local_bytes;
};

/// Application validation performed before a stored candidate is selected.
/// Use this to bind the core to membership state and any provider-side pin or
/// translation projection; failure causes recovery to try the older manifest.
using CheckpointCandidateValidator = std::function<Result<void>(
    std::string_view shareable_bytes,
    const std::optional<std::string> &local_bytes)>;

/// Content-addressed checkpoint files with two alternating atomic manifests.
/// A manifest is published only after its referenced artifacts are durable;
/// the other manifest and its artifacts are retained for fallback until G06.
class CheckpointStore {
public:
  [[nodiscard]] static Result<CheckpointStore>
  open(std::filesystem::path directory, const StepHook *hook = nullptr);

  CheckpointStore(CheckpointStore &&) noexcept = default;
  CheckpointStore &operator=(CheckpointStore &&) noexcept = default;
  CheckpointStore(const CheckpointStore &) = delete;
  CheckpointStore &operator=(const CheckpointStore &) = delete;

  /// Validate and durably publish a shareable core and, optionally, its local
  /// companion. A failed manifest replacement leaves the previous manifest
  /// recoverable; commit_uncertain means the new manifest may already be live.
  [[nodiscard]] Result<StoredCheckpoint>
  publish(std::string_view shareable_bytes,
          const std::optional<std::string> &local_bytes,
          const ActorId &local_owner);

  /// Select the newest complete, typed-valid bundle. If its artifact is
  /// missing or corrupt, try the previous alternating manifest. An absent
  /// store returns an empty optional; a present but unusable store is an error.
  /// The caller's validator is mandatory because membership and provider-side
  /// state cannot be established from the artifact bytes alone.
  [[nodiscard]] Result<std::optional<StoredCheckpoint>>
  recover(const ActorId &local_owner,
          const CheckpointCandidateValidator &validate_candidate,
          const std::optional<CheckpointContext> &expected_context = {}) const;

  /// Durably retain a validated content-addressed multi-stream membership
  /// checkpoint bundle. Bundles remain immutable; cleanup belongs to the
  /// reclamation layer after it proves no descriptor still references them.
  [[nodiscard]] Result<void> publish_membership_bundle(
      const MembershipDescriptor &descriptor, std::string_view bundle_bytes);

  /// Resolve a previously published bundle by its descriptor checkpoint hash.
  /// Missing artifacts return awaiting_state; corrupt or mismatched artifacts
  /// fail closed.
  [[nodiscard]] Result<std::string>
  resolve_membership_bundle(std::string_view checkpoint_hash) const;

private:
  CheckpointStore(std::filesystem::path directory, const StepHook *hook,
                  ProcessLease lease)
      : m_directory(std::move(directory)), m_hook(hook),
        m_lease(std::move(lease)) {}

  std::filesystem::path m_directory;
  const StepHook *m_hook = nullptr;
  std::optional<ProcessLease> m_lease;
};

} // namespace CollabText::Storage
