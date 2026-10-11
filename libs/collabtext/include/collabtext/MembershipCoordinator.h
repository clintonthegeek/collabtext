#pragma once

#include <collabtext/ActorId.h>
#include <collabtext/OpStream.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace CollabText {

/// The active membership for one document stream. A coordinator can own
/// several independent scopes and serializes epoch changes against all
/// outstanding mutation and inbound-delivery permits.
struct MembershipScope {
  DocumentId document;
  StreamId stream;
  uint64_t epoch = 0;
  std::vector<ActorId> members;
};

/// Thread-safe membership gate for composing document sessions with durable
/// membership transitions. Authorization results are RAII leases: retain the
/// returned shared_ptr through the session's durable write/application. Epoch
/// transitions hold the exclusive gate while their durable callback runs.
class MembershipCoordinator {
public:
  [[nodiscard]] static Result<std::shared_ptr<MembershipCoordinator>> create(
      std::vector<MembershipScope> scopes);

  [[nodiscard]] Result<std::shared_ptr<void>> authorize_local_mutation(
      const DocumentId &document, const StreamId &stream, uint64_t epoch,
      const ActorId &producer) const;
  [[nodiscard]] Result<std::shared_ptr<void>> authorize_inbound_delivery(
      const DocumentId &document, const StreamId &stream, uint64_t epoch,
      const DeliveryToken &token) const;

  /// Persist and publish exactly one next-epoch membership while holding the
  /// exclusive gate. The callback must commit the matching durable transition
  /// and must be idempotent when retrying an uncertain publication. It must
  /// not reenter any MembershipCoordinator. A refusal
  /// known to precede publication leaves active membership unchanged. An
  /// ambiguous result or exception blocks authorization for that scope until
  /// the exact transition is retried or state is reconstructed durably.
  [[nodiscard]] Result<void> commit_transition(
      const DocumentId &document, const StreamId &stream,
      uint64_t expected_epoch, uint64_t next_epoch,
      /// Stable, unique identity for every durable transition input. Supply
      /// the same identity only when retrying those exact inputs.
      std::string transition_identity,
      std::vector<ActorId> next_members,
      const std::function<Result<void>()> &durable_commit);

private:
  struct Impl;
  explicit MembershipCoordinator(std::shared_ptr<Impl> impl)
      : m_impl(std::move(impl)) {}
  std::shared_ptr<Impl> m_impl;
};

} // namespace CollabText
