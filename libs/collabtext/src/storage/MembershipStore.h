#pragma once

#include "storage/DurableFile.h"

#include <collabtext/ActorId.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
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
  std::string parent_descriptor_hash;
  std::string evidence;
};

struct MembershipDescriptor {
  uint64_t epoch = 0;
  /// Empty only for the genesis descriptor; otherwise the exact parent hash.
  std::string parent_hash;
  /// Empty only for genesis; otherwise the hash of the transition record.
  std::string transition_event_hash;
  std::string base_checkpoint_hash;
  /// SHA-256 of the canonical encoding of every field below and above.
  std::string hash;
  std::vector<ActorId> members;
  /// Sorted by stream; maps include every member and may retain historical
  /// producer frontiers for actors no longer enrolled.
  struct StreamFrontier {
    std::string stream;
    std::map<ActorId, uint64_t> operation;
    std::map<ActorId, uint64_t> delivery;
    auto operator<=>(const StreamFrontier &) const = default;
  };
  std::vector<StreamFrontier> base_frontiers;
  std::vector<uint32_t> supported_token_versions;
  std::string translation_manifest_hash;
};

/// Canonicalize a descriptor's sorted fields and compute its content hash.
/// Epoch one is the only descriptor without parent/event hashes.
[[nodiscard]] Result<MembershipDescriptor> seal_membership_descriptor(
    MembershipDescriptor descriptor);
[[nodiscard]] Result<void> validate_membership_descriptor(
    const MembershipDescriptor &descriptor);
[[nodiscard]] std::string membership_event_hash(
    const MembershipEvent &event, const ActorId &producer);
[[nodiscard]] Result<void> validate_membership_transition_link(
    const MembershipDescriptor &parent,
    const MembershipDescriptor &next,
    const MembershipEvent &event,
    const ActorId &producer);

/// The caller must resolve and validate the durable base checkpoint named by
/// `next`, including its integrity, completeness, and exact represented
/// frontiers. MembershipStore has no checkpoint codec of its own.
using MembershipCheckpointVerifier = std::function<Result<void>(
    const MembershipDescriptor &parent,
    const MembershipDescriptor &next)>;

/// Revalidate the stored transition evidence after recovery. The caller checks
/// the exact proposal/acknowledger set and durable acknowledgement proofs;
/// commit-time verification alone is not enough for later eligibility checks.
using MembershipTransitionEvidenceVerifier = std::function<Result<void>(
    const MembershipDescriptor &parent,
    const MembershipDescriptor &next,
    const MembershipEvent &event,
    const ActorId &producer)>;

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
  /// Confirm that `next` names a transition event actually present in this
  /// durable producer journal and that the event extends `parent` membership.
  [[nodiscard]] Result<void> validate_transition_link(
      const MembershipDescriptor &parent,
      const MembershipDescriptor &next) const;
  /// Full transition validation fails closed without evidence and checkpoint
  /// verifiers.
  /// The event-link check runs first, then the caller revalidates durable
  /// transition evidence and base checkpoint bytes.
  [[nodiscard]] Result<void> validate_transition(
      const MembershipDescriptor &parent,
      const MembershipDescriptor &next,
      const MembershipTransitionEvidenceVerifier &verify_evidence,
      const MembershipCheckpointVerifier &verify_checkpoint) const;
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
