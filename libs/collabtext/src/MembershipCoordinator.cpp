#include <collabtext/MembershipCoordinator.h>

#include <algorithm>
#include <exception>
#include <map>
#include <mutex>
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
    uint64_t pending_epoch = 0;
    std::string pending_identity;
    std::vector<ActorId> pending_members;
  };
  mutable std::shared_mutex mutex;
  std::map<ScopeKey, State> scopes;
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
  if (inside_transition_callback())
    return Error(ErrorCode::reentrant_mutation,
                 "membership transition callback reentered its coordinator");
  if (!durable_commit || transition_identity.empty() ||
      transition_identity.size() > 256 ||
      transition_identity.find('\0') != std::string::npos ||
      expected_epoch == UINT64_MAX ||
      next_epoch != expected_epoch + 1)
    return Error(ErrorCode::malformed,
                 "membership transition identity or next epoch is invalid");
  auto members_valid = validate_members(next_members);
  if (!members_valid.ok()) return members_valid.error();
  auto pending_members = next_members;

  std::unique_lock lock(m_impl->mutex);
  const auto found = m_impl->scopes.find({document, stream});
  if (found == m_impl->scopes.end() || found->second.epoch != expected_epoch)
    return Error(ErrorCode::wrong_epoch,
                 "membership transition parent does not match active scope");
  if (found->second.transition_uncertain &&
      (found->second.pending_epoch != next_epoch ||
       found->second.pending_identity != transition_identity ||
       found->second.pending_members != next_members))
    return Error(ErrorCode::wrong_epoch,
                 "uncertain membership publication can only be retried exactly");
  Result<void> committed;
  try {
    ReentrancyScope callback_scope;
    committed = durable_commit();
  } catch (const std::exception &error) {
    found->second.transition_uncertain = true;
    found->second.pending_epoch = next_epoch;
    found->second.pending_identity = std::move(transition_identity);
    found->second.pending_members = std::move(pending_members);
    return Error(ErrorCode::io_error, error.what(),
                 "committing durable membership transition");
  } catch (...) {
    found->second.transition_uncertain = true;
    found->second.pending_epoch = next_epoch;
    found->second.pending_identity = std::move(transition_identity);
    found->second.pending_members = std::move(pending_members);
    return Error(ErrorCode::io_error,
                 "durable membership transition callback threw an unknown exception");
  }
  if (!committed.ok()) {
    if (committed.error().code() == ErrorCode::commit_uncertain) {
      found->second.transition_uncertain = true;
      found->second.pending_epoch = next_epoch;
      found->second.pending_identity = std::move(transition_identity);
      found->second.pending_members = std::move(pending_members);
    }
    return committed.error();
  }
  found->second.members = std::move(next_members);
  found->second.epoch = next_epoch;
  found->second.transition_uncertain = false;
  found->second.pending_epoch = 0;
  found->second.pending_identity.clear();
  found->second.pending_members.clear();
  return {};
}

} // namespace CollabText
