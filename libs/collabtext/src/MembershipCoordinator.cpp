#include <collabtext/MembershipCoordinator.h>

#include <algorithm>
#include <exception>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>

namespace CollabText {
namespace {

using ScopeKey = std::pair<DocumentId, StreamId>;
struct ActiveCoordinator {
  ActiveCoordinator *previous;
};
thread_local ActiveCoordinator *active_coordinators = nullptr;

bool inside_transition_callback() {
  return active_coordinators != nullptr;
}

struct ReentrancyScope {
  ReentrancyScope() : active{active_coordinators} {
    active_coordinators = &active;
  }
  ~ReentrancyScope() { active_coordinators = active.previous; }
  ActiveCoordinator active;
};

struct SharedPermit {
  SharedPermit(std::shared_ptr<void> keep_alive, std::shared_mutex &mutex)
      : owner(std::move(keep_alive)), lock(mutex) {}
  std::shared_ptr<void> owner;
  std::shared_lock<std::shared_mutex> lock;
};

Result<void> validate_members(std::vector<ActorId> &members) {
  if (members.empty() || members.size() > 4096)
    return Error(ErrorCode::malformed,
                 "membership scope must contain between 1 and 4096 actors");
  std::sort(members.begin(), members.end());
  if (std::any_of(members.begin(), members.end(),
                  [](const ActorId &actor) { return !actor.valid(); }) ||
      std::adjacent_find(members.begin(), members.end()) != members.end())
    return Error(ErrorCode::malformed,
                 "membership scope contains an invalid or duplicate actor");
  return {};
}

} // namespace

struct MembershipCoordinator::Impl {
  struct State {
    uint64_t epoch = 0;
    std::vector<ActorId> members;
    bool transition_uncertain = false;
  };
  struct Update {
    ScopeKey key;
    uint64_t expected_epoch = 0;
    uint64_t next_epoch = 0;
    std::vector<ActorId> next_members;
    bool operator==(const Update &) const = default;
  };
  struct PendingBatch {
    std::string identity;
    std::vector<Update> updates;
  };
  mutable std::shared_mutex mutex;
  std::map<ScopeKey, State> scopes;
  std::optional<PendingBatch> pending;
};

Result<std::shared_ptr<MembershipCoordinator>> MembershipCoordinator::create(
    std::vector<MembershipScope> scopes) {
  if (scopes.empty() || scopes.size() > 4096)
    return Error(ErrorCode::malformed,
                 "membership coordinator requires between 1 and 4096 scopes");
  auto impl = std::make_shared<Impl>();
  for (auto &scope : scopes) {
    if (scope.document.empty() || scope.document.size() > 4096 ||
        scope.stream.empty() || scope.stream.size() > 4096 || scope.epoch == 0)
      return Error(ErrorCode::malformed,
                   "membership coordinator scope identity or epoch is invalid");
    auto members_valid = validate_members(scope.members);
    if (!members_valid.ok()) return members_valid.error();
    const ScopeKey key{std::move(scope.document), std::move(scope.stream)};
    Impl::State state;
    state.epoch = scope.epoch;
    state.members = std::move(scope.members);
    if (!impl->scopes.emplace(key, std::move(state)).second)
      return Error(ErrorCode::malformed,
                   "membership coordinator repeats a document stream scope");
  }
  return std::shared_ptr<MembershipCoordinator>(
      new MembershipCoordinator(std::move(impl)));
}

Result<std::shared_ptr<void>> MembershipCoordinator::authorize_local_mutation(
    const DocumentId &document, const StreamId &stream, uint64_t epoch,
    const ActorId &producer) const {
  if (inside_transition_callback())
    return Error(ErrorCode::reentrant_mutation,
                 "membership transition callback reentered its coordinator");
  auto permit = std::make_shared<SharedPermit>(
      std::static_pointer_cast<void>(m_impl), m_impl->mutex);
  const auto found = m_impl->scopes.find({document, stream});
  if (found != m_impl->scopes.end() && found->second.transition_uncertain)
    return Error(ErrorCode::commit_uncertain,
                 "membership transition publication is uncertain; writes are blocked until recovery");
  if (found == m_impl->scopes.end() || found->second.epoch != epoch ||
      !std::binary_search(found->second.members.begin(),
                          found->second.members.end(), producer))
    return Error(ErrorCode::wrong_epoch,
                 "local producer is not enrolled in this document stream epoch");
  return std::static_pointer_cast<void>(std::move(permit));
}

Result<std::shared_ptr<void>> MembershipCoordinator::authorize_inbound_delivery(
    const DocumentId &document, const StreamId &stream, uint64_t epoch,
    const DeliveryToken &token) const {
  if (token.stream != stream || token.epoch != epoch || token.sequence == 0 ||
      !token.producer.valid())
    return Error(ErrorCode::wrong_epoch,
                 "inbound delivery token does not match its authorized scope");
  return authorize_local_mutation(document, stream, epoch, token.producer);
}

Result<void> MembershipCoordinator::commit_transition(
    const DocumentId &document, const StreamId &stream,
    uint64_t expected_epoch, uint64_t next_epoch,
    std::string transition_identity,
    std::vector<ActorId> next_members,
    const std::function<Result<void>()> &durable_commit) {
  std::vector<MembershipScopeTransition> transitions;
  transitions.push_back({document, stream, expected_epoch, next_epoch,
                         std::move(next_members)});
  return commit_transitions(std::move(transition_identity),
                            std::move(transitions), durable_commit);
}

Result<void> MembershipCoordinator::commit_transitions(
    std::string transition_identity,
    std::vector<MembershipScopeTransition> transitions,
    const std::function<Result<void>()> &durable_commit) {
  if (inside_transition_callback())
    return Error(ErrorCode::reentrant_mutation,
                 "membership transition callback reentered its coordinator");
  if (!durable_commit || transitions.empty() || transitions.size() > 4096 ||
      transition_identity.empty() ||
      transition_identity.size() > 256 ||
      transition_identity.find('\0') != std::string::npos)
    return Error(ErrorCode::malformed,
                 "membership transition identity or scope set is invalid");
  std::vector<Impl::Update> updates;
  updates.reserve(transitions.size());
  for (auto &transition : transitions) {
    if (transition.document.empty() || transition.document.size() > 4096 ||
        transition.stream.empty() || transition.stream.size() > 4096 ||
        transition.expected_epoch == UINT64_MAX ||
        transition.next_epoch != transition.expected_epoch + 1)
      return Error(ErrorCode::malformed,
                   "membership transition scope or next epoch is invalid");
    auto members_valid = validate_members(transition.next_members);
    if (!members_valid.ok()) return members_valid.error();
    updates.push_back({{std::move(transition.document),
                        std::move(transition.stream)},
                       transition.expected_epoch, transition.next_epoch,
                       std::move(transition.next_members)});
  }
  std::sort(updates.begin(), updates.end(),
            [](const Impl::Update &left, const Impl::Update &right) {
              return left.key < right.key;
            });
  if (std::adjacent_find(updates.begin(), updates.end(),
          [](const Impl::Update &left, const Impl::Update &right) {
            return left.key == right.key;
          }) != updates.end())
    return Error(ErrorCode::malformed,
                 "membership transition repeats a document stream scope");

  std::unique_lock lock(m_impl->mutex);
  if (m_impl->pending &&
      (m_impl->pending->identity != transition_identity ||
       m_impl->pending->updates != updates))
    return Error(ErrorCode::wrong_epoch,
                 "uncertain membership batch can only be retried exactly");
  for (const auto &update : updates) {
    const auto found = m_impl->scopes.find(update.key);
    if (found == m_impl->scopes.end() ||
        found->second.epoch != update.expected_epoch)
      return Error(ErrorCode::wrong_epoch,
                   "membership transition parent does not match every active scope");
    if (found->second.transition_uncertain && !m_impl->pending)
      return Error(ErrorCode::commit_uncertain,
                   "membership scope is blocked pending durable recovery");
  }

  // Allocate the recovery record before invoking a callback that may publish
  // durably. Marking an uncertain result must not itself require copying the
  // complete batch after publication may already have happened.
  Impl::PendingBatch pending_candidate{transition_identity, updates};
  const auto mark_uncertain = [&] {
    m_impl->pending = std::move(pending_candidate);
    for (const auto &update : updates)
      m_impl->scopes.at(update.key).transition_uncertain = true;
  };
  Result<void> committed;
  try {
    ReentrancyScope callback_scope;
    committed = durable_commit();
  } catch (const std::exception &error) {
    mark_uncertain();
    return Error(ErrorCode::io_error, error.what(),
                 "committing durable membership batch");
  } catch (...) {
    mark_uncertain();
    return Error(ErrorCode::io_error,
                 "durable membership batch callback threw an unknown exception");
  }
  if (!committed.ok()) {
    if (committed.error().code() == ErrorCode::commit_uncertain)
      mark_uncertain();
    return committed.error();
  }
  for (auto &update : updates) {
    auto &state = m_impl->scopes.at(update.key);
    state.members = std::move(update.next_members);
    state.epoch = update.next_epoch;
    state.transition_uncertain = false;
  }
  m_impl->pending.reset();
  return {};
}

} // namespace CollabText
