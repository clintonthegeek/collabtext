#pragma once

#include "storage/DurableFile.h"

#include <collabtext/ActorId.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CollabText::Storage {

/// A producer-authored, append-only membership fact. It records intent and
/// provenance; eligibility still requires validation against the G01 protocol
/// and acknowledgements from the complete epoch membership.
struct MembershipEvent {
  enum class Kind { Enrolled, Retired };
  uint64_t sequence = 0;
  uint64_t epoch = 0;
  ActorId subject;
  Kind kind = Kind::Enrolled;
  std::string evidence;
};

struct MembershipDescriptor {
  uint64_t epoch = 0;
  std::string hash;
  std::vector<ActorId> members;
};

/// Consequences of removing a device from the next epoch. Counts and branch
/// hashes are captured by the caller from durable document/session state.
struct RetirementImpact {
  std::string document;
  std::string stream;
  uint64_t epoch = 0;
  uint64_t unsynced_operations = 0;
  uint64_t retained_undo_groups = 0;
  std::string recoverable_branch_hash;
  auto operator<=>(const RetirementImpact &) const = default;
};

struct RetirementPreview {
  ActorId device;
  uint64_t parent_epoch = 0;
  uint64_t next_epoch = 0;
  std::string parent_descriptor_hash;
  std::vector<ActorId> parent_members;
  std::vector<ActorId> required_acknowledgers;
  std::vector<RetirementImpact> impacts;
  std::string digest;
};

struct MembershipAcknowledgement {
  ActorId actor;
  std::string signature;
};

struct EnrollmentPreview {
  ActorId device;
  uint64_t parent_epoch = 0;
  uint64_t next_epoch = 0;
  std::string parent_descriptor_hash;
  std::vector<ActorId> parent_members;
  std::vector<ActorId> required_acknowledgers;
  std::string digest;
};

/// The caller must confirm that each acknowledgement was durably recorded
/// and authenticate its signature over the supplied transition digest.
using DurableAcknowledgementVerifier = std::function<Result<void>(
    const ActorId &actor, std::string_view transition_digest,
    std::string_view signature)>;

[[nodiscard]] Result<RetirementPreview> preview_retirement(
    const MembershipDescriptor &current, const ActorId &device,
    std::vector<RetirementImpact> impacts);
[[nodiscard]] Result<EnrollmentPreview> preview_enrollment(
    const MembershipDescriptor &current, const ActorId &device);

/// Durable per-producer journal for explicit membership transitions. Open
/// holds an OS writer lease for the store lifetime; competing opens fail with
/// writer_conflict instead of risking duplicate event sequences.
class MembershipStore {
public:
  [[nodiscard]] static Result<MembershipStore>
  open(std::filesystem::path path, ActorId producer,
       const StepHook *hook = nullptr);
  MembershipStore(MembershipStore &&) noexcept = default;
  MembershipStore &operator=(MembershipStore &&) noexcept = default;
  MembershipStore(const MembershipStore &) = delete;
  MembershipStore &operator=(const MembershipStore &) = delete;

  [[nodiscard]] Result<std::vector<MembershipEvent>> recover() const;
  [[nodiscard]] Result<void> append(const MembershipEvent &event);
  /// Commits only if the preview still matches current descriptor/impact data,
  /// explicit user confirmation is present, and every remaining member has a
  /// verified acknowledgement from durable caller-owned state.
  [[nodiscard]] Result<MembershipEvent> commit_retirement(
      const RetirementPreview &preview,
      const MembershipDescriptor &current,
      const std::vector<RetirementImpact> &current_impacts,
      const std::vector<MembershipAcknowledgement> &acknowledgements,
      std::string_view explicit_confirmation,
      const DurableAcknowledgementVerifier &verify_acknowledgement);
  /// Commits only against the current parent descriptor and after the joining
  /// device and every current member durably acknowledge the same transition.
  [[nodiscard]] Result<MembershipEvent> commit_enrollment(
      const EnrollmentPreview &preview,
      const MembershipDescriptor &current,
      const std::vector<MembershipAcknowledgement> &acknowledgements,
      const DurableAcknowledgementVerifier &verify_acknowledgement);
  [[nodiscard]] uint64_t last_sequence() const { return m_last_sequence; }

private:
  MembershipStore(std::filesystem::path path, ActorId producer,
                  const StepHook *hook, ProcessLease lease)
      : m_path(std::move(path)), m_producer(producer), m_hook(hook),
        m_lease(std::move(lease)) {}
  std::filesystem::path m_path;
  ActorId m_producer;
  const StepHook *m_hook = nullptr;
  std::optional<ProcessLease> m_lease;
  uint64_t m_last_sequence = 0;
  uint64_t m_last_epoch = 0;
};

} // namespace CollabText::Storage
