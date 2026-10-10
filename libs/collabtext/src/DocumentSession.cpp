#include <collabtext/DocumentSession.h>

#include "crdt/Buffer.h"
#include "core/Sha256.h"
#include "identity/ReplicaAllocator.h"
#include "storage/LocalHistoryLog.h"
#include "transport/StreamSync.h"
#include <collabtext/Serialization.h>

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace CollabText {
using namespace Crdt;

struct DocumentSession::Impl {
  DocumentSessionConfig config;
  std::optional<ReplicaState> replica;
  std::shared_ptr<Identity::ReplicaAllocator> allocator;
  std::unique_ptr<Crdt::Buffer> buffer;
  std::unique_ptr<Crdt::FileOpStream> transport;
  std::unique_ptr<Storage::LocalHistoryLog> local_history;
  SessionRecoveryReport report;
  uint64_t next_envelope = 1;
  std::unordered_set<std::string> replayed_tokens;
  std::unordered_map<std::string, Operation> deferred_tokens;
  bool closed = false;
  bool poisoned = false;

  Result<Operation> publish_local(Operation op) {
    if (next_envelope == UINT64_MAX) {
      poisoned = true;
      return Error(ErrorCode::counter_exhausted,
                   "transport envelope sequence exhausted after local apply");
    }
    auto encoded = encode_operation(op);
    if (!encoded.ok()) {
      poisoned = true;
      return encoded.error().with_context(
          "local operation exists in memory but could not be encoded");
    }
    const auto snapshot = buffer->local_history_snapshot();
    auto intent = local_history->store_pending(
        Storage::PendingLocalHistoryCommit{next_envelope, encoded.value(), snapshot});
    if (!intent.ok()) {
      poisoned = true;
      return intent.error().with_context(
          "local operation applied but recovery intent could not be stored");
    }
    OutboundDelivery delivery{
        DeliveryToken{config.stream, config.epoch, replica->actor(), next_envelope},
        std::move(encoded).value()};
    auto pushed = transport->push(delivery, PushDurability::durable);
    if (!pushed.ok()) {
      poisoned = true;
      report.complete = false;
      report.issue = pushed.error().with_context(
          "local operation applied but log publication failed");
      return pushed.error().with_context(
          "local operation applied but log publication failed; reopen to reconcile");
    }
    replayed_tokens.insert(token_key(delivery.token));
    ++next_envelope;
    auto checkpoint = local_history->store_snapshot(snapshot);
    if (!checkpoint.ok()) {
      poisoned = true;
      return checkpoint.error().with_context(
          "operation published but local undo checkpoint failed");
    }
    auto cleared = local_history->clear_pending();
    if (!cleared.ok()) {
      poisoned = true;
      return cleared.error().with_context(
          "operation and local history committed but recovery intent remains");
    }
    return op;
  }

  [[nodiscard]] bool ready() const noexcept {
    return !closed && !poisoned && report.complete && buffer &&
           buffer->deferred_count() == 0;
  }

  void classify(const ApplyOutcome &outcome) {
    switch (outcome.kind()) {
    case ApplyOutcomeKind::applied:
      ++report.applied;
      break;
    case ApplyOutcomeKind::duplicate:
      ++report.duplicates;
      break;
    case ApplyOutcomeKind::deferred:
      ++report.deferred;
      report.complete = false;
      if (!report.issue)
        report.issue =
            Error(ErrorCode::awaiting_state,
                  "operation history is waiting for causal dependencies");
      break;
    case ApplyOutcomeKind::rejected:
      ++report.rejected;
      report.complete = false;
      if (!report.issue)
        report.issue = outcome.error().value_or(Error(
            ErrorCode::malformed, "operation was rejected during recovery"));
      break;
    }
  }

  static std::string token_key(const DeliveryToken &token) {
    return token.producer.to_string() + ":" + std::to_string(token.sequence);
  }

  static std::filesystem::path history_path(const DocumentSessionConfig &config) {
    std::string key = config.document + std::string(1, '\0') + config.stream +
                      std::string(1, '\0') + std::to_string(config.epoch);
    return config.private_state /
           ("local-history-" + sha256_hex(key) + ".snapshot");
  }

  Result<void> reconcile_pending_history() {
    auto pending = local_history->recover_pending();
    if (!pending.ok()) return pending.error();
    if (!pending.value()) return {};
    const auto &commit = *pending.value();
    if (commit.sequence == UINT64_MAX)
      return Error(ErrorCode::counter_exhausted,
                   "pending local delivery sequence is exhausted");
    auto decoded = decode_operation(commit.operation);
    if (!decoded.ok())
      return decoded.error().with_context("pending local operation record");
    if (op_identity(decoded.value()).actor != replica->actor())
      return Error(ErrorCode::actor_conflict,
                   "pending local operation belongs to another actor");
    OutboundDelivery delivery{
        DeliveryToken{config.stream, config.epoch, replica->actor(),
                      commit.sequence},
        commit.operation};
    auto pushed = transport->push(delivery, PushDurability::durable);
    if (!pushed.ok()) return pushed.error().with_context(
        "reconciling pending local operation publication");
    auto checkpoint = local_history->store_snapshot(commit.snapshot);
    if (!checkpoint.ok()) return checkpoint.error().with_context(
        "reconciling pending local history checkpoint");
    return local_history->clear_pending();
  }

  void update_completeness() {
    if (buffer->deferred_count() == 0 && report.rejected == 0 && !poisoned) {
      report.complete = true;
      if (report.issue && report.issue->code() == ErrorCode::awaiting_state)
        report.issue.reset();
    } else {
      report.complete = false;
    }
  }

  Result<void> replay_delivery(const InboundDelivery &delivery) {
    if (delivery.token.producer == replica->actor()) {
      if (delivery.token.sequence == UINT64_MAX)
        return Error(ErrorCode::counter_exhausted,
                     "local envelope sequence exhausted");
      next_envelope = std::max(next_envelope, delivery.token.sequence + 1);
    }
    const auto key = token_key(delivery.token);
    if (replayed_tokens.contains(key))
      return {};
    auto decoded = decode_operation(delivery.payload);
    if (!decoded.ok()) {
      report.complete = false;
      ++report.rejected;
      if (!report.issue)
        report.issue = decoded.error().with_context("full session replay");
      return {};
    }
    if (op_identity(decoded.value()).actor != delivery.token.producer) {
      report.complete = false;
      ++report.rejected;
      if (!report.issue)
        report.issue =
            Error(ErrorCode::actor_conflict,
                  "operation identity does not match its delivery producer");
      return {};
    }
    ++report.envelopes;
    auto outcomes = buffer->apply_ops({decoded.value()});
    if (!outcomes.empty()) {
      classify(outcomes.front());
      if (outcomes.front().kind() != ApplyOutcomeKind::rejected)
        replayed_tokens.insert(key);
      if (outcomes.front().kind() == ApplyOutcomeKind::deferred)
        deferred_tokens.emplace(key, decoded.value());
    }
    return {};
  }
};

DocumentSession::DocumentSession(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl)) {}
DocumentSession::~DocumentSession() {
  if (m_impl && !m_impl->closed)
    (void)close();
}

Result<std::unique_ptr<DocumentSession>>
DocumentSession::open(DocumentSessionConfig config) {
  if (config.shared_store.empty() || config.private_state.empty() ||
      config.document.empty() || config.stream.empty() || config.epoch == 0)
    return Error(ErrorCode::malformed, "session requires store, private state, "
                                       "document, stream and nonzero epoch");
  try {
    auto impl = std::make_unique<Impl>();
    impl->config = std::move(config);
    auto state = ReplicaState::open(impl->config.private_state);
    if (!state.ok())
      return state.error();
    impl->replica.emplace(std::move(state).value());
    auto lease = impl->replica->acquire_writer(impl->config.document,
                                               impl->config.stream);
    if (!lease.ok())
      return lease.error();
    impl->allocator = std::make_shared<Identity::ReplicaAllocator>(
        &*impl->replica, impl->config.document, impl->config.stream,
        impl->config.epoch);
    impl->buffer = std::make_unique<Crdt::Buffer>(impl->allocator);
    impl->buffer->set_max_undo_depth(impl->config.max_undo_steps);
    impl->buffer->set_max_undo_bytes(impl->config.max_undo_bytes);
    auto local_history = Storage::LocalHistoryLog::open(
        Impl::history_path(impl->config), impl->replica->actor());
    if (!local_history.ok()) return local_history.error();
    impl->local_history = std::make_unique<Storage::LocalHistoryLog>(
        std::move(local_history).value());
    impl->transport = std::make_unique<Crdt::FileOpStream>(
        impl->config.shared_store, impl->replica->actor());
    if (auto started = impl->transport->start(); !started.ok())
      return started.error();
    const StreamScope scope{impl->config.stream, impl->config.epoch};
    if (auto registered = impl->transport->register_stream(scope);
        !registered.ok())
      return registered.error();
    if (auto reconciled = impl->reconcile_pending_history();
        !reconciled.ok())
      return reconciled.error();

    auto replayed = impl->transport->replay_all(
        scope, [&](const InboundDelivery &delivery) {
          return impl->replay_delivery(delivery);
        });
    if (!replayed.ok())
      return replayed.error();
    auto history_snapshot = impl->local_history->recover();
    if (!history_snapshot.ok()) return history_snapshot.error();
    if (history_snapshot.value()) {
      auto restored = impl->buffer->restore_local_history(*history_snapshot.value());
      if (!restored.ok()) return restored.error();
    }
    impl->update_completeness();

    auto *raw = impl.get();
    auto subscribed = impl->transport->subscribe(
        scope, [raw](const InboundDelivery &delivery) {
          auto decoded = decode_operation(delivery.payload);
          if (!decoded.ok()) {
            raw->report.complete = false;
            ++raw->report.rejected;
            raw->report.issue = decoded.error();
            return DeliveryDecision::quarantine(decoded.error());
          }
          if (op_identity(decoded.value()).actor != delivery.token.producer) {
            auto error = Error(
                ErrorCode::actor_conflict,
                "operation identity does not match its delivery producer");
            raw->report.complete = false;
            ++raw->report.rejected;
            raw->report.issue = error;
            return DeliveryDecision::quarantine(error);
          }
          const auto token_key = Impl::token_key(delivery.token);
          if (auto waiting = raw->deferred_tokens.find(token_key);
              waiting != raw->deferred_tokens.end()) {
            if (!raw->buffer->version().observed(op_identity(waiting->second)))
              return DeliveryDecision::retry();
            raw->deferred_tokens.erase(waiting);
            raw->replayed_tokens.insert(token_key);
            raw->update_completeness();
            return DeliveryDecision::commit(
                RecoveryPoint::replay_from_beginning());
          }
          auto outcomes = raw->buffer->apply_ops({decoded.value()});
          if (outcomes.empty())
            return DeliveryDecision::retry();
          const auto &outcome = outcomes.front();
          if (outcome.kind() == ApplyOutcomeKind::applied ||
              outcome.kind() == ApplyOutcomeKind::duplicate) {
            raw->replayed_tokens.insert(token_key);
            raw->update_completeness();
            return DeliveryDecision::commit(
                RecoveryPoint::replay_from_beginning());
          }
          raw->report.complete = false;
          raw->classify(outcome);
          if (outcome.kind() == ApplyOutcomeKind::deferred) {
            raw->deferred_tokens.emplace(token_key, decoded.value());
            raw->replayed_tokens.insert(token_key);
            return DeliveryDecision::retry();
          }
          return DeliveryDecision::quarantine(outcome.error().value_or(
              Error(ErrorCode::malformed, "operation was rejected")));
        });
    if (!subscribed.ok())
      return subscribed.error();
    return std::unique_ptr<DocumentSession>(
        new DocumentSession(std::move(impl)));
  } catch (const std::filesystem::filesystem_error &error) {
    return Error(ErrorCode::io_error, error.what(), "opening document session");
  } catch (const std::exception &error) {
    return Error(ErrorCode::io_error, error.what(), "opening document session");
  }
}

Result<SessionRecoveryReport> DocumentSession::recover() {
  if (!m_impl || m_impl->closed)
    return Error(ErrorCode::writer_conflict, "session is closed");
  try {
    const auto scope = StreamScope{m_impl->config.stream, m_impl->config.epoch};
    if (auto reconciled = m_impl->reconcile_pending_history();
        !reconciled.ok())
      return reconciled.error();
    auto replayed = m_impl->transport->replay_all(
        scope, [&](const InboundDelivery &delivery) {
          return m_impl->replay_delivery(delivery);
        });
    if (!replayed.ok())
      return replayed.error();
    auto history_snapshot = m_impl->local_history->recover();
    if (!history_snapshot.ok()) return history_snapshot.error();
    if (history_snapshot.value()) {
      auto restored = m_impl->buffer->restore_local_history(*history_snapshot.value());
      if (!restored.ok()) return restored.error();
    }
    m_impl->update_completeness();
    return m_impl->report;
  } catch (const std::filesystem::filesystem_error &error) {
    return Error(ErrorCode::io_error, error.what(),
                 "recovering document session");
  }
}

Result<Operation> DocumentSession::apply_local(
    const std::vector<std::pair<uint32_t, uint32_t>> &ranges,
    const std::vector<std::string> &replacements) {
  if (!m_impl || !m_impl->ready())
    return Error(ErrorCode::awaiting_state,
                 "session recovery is incomplete; local edits are disabled");
  auto operation = m_impl->buffer->try_apply_local_edit(ranges, replacements);
  if (!operation.ok())
    return operation.error();
  return m_impl->publish_local(std::move(operation).value());
}

Result<Operation> DocumentSession::undo() {
  if (!m_impl || !m_impl->ready())
    return Error(ErrorCode::awaiting_state,
                 "session recovery is incomplete; undo is disabled");
  auto operation = m_impl->buffer->try_undo();
  if (!operation.ok()) return operation.error();
  if (!operation.value())
    return Error(ErrorCode::not_found, "no undo operation is available");
  return m_impl->publish_local(std::move(*operation.value()));
}

Result<Operation> DocumentSession::redo() {
  if (!m_impl || !m_impl->ready())
    return Error(ErrorCode::awaiting_state,
                 "session recovery is incomplete; redo is disabled");
  auto operation = m_impl->buffer->try_redo();
  if (!operation.ok()) return operation.error();
  if (!operation.value())
    return Error(ErrorCode::not_found, "no redo operation is available");
  return m_impl->publish_local(std::move(*operation.value()));
}

std::optional<LocalUndoGroupMetadata>
DocumentSession::next_undo_group() const {
  if (!m_impl || !m_impl->buffer) return std::nullopt;
  auto entry = m_impl->buffer->next_undo_group();
  if (!entry) return std::nullopt;
  return LocalUndoGroupMetadata{entry->group_id, std::move(entry->operation_ids)};
}

std::optional<LocalUndoGroupMetadata>
DocumentSession::next_redo_group() const {
  if (!m_impl || !m_impl->buffer) return std::nullopt;
  auto entry = m_impl->buffer->next_redo_group();
  if (!entry) return std::nullopt;
  return LocalUndoGroupMetadata{entry->group_id, std::move(entry->operation_ids)};
}

Result<bool> DocumentSession::coalesce_last_undo() {
  if (!m_impl || !m_impl->ready())
    return Error(ErrorCode::awaiting_state,
                 "session recovery is incomplete; undo coalescing is disabled");
  if (!m_impl->buffer->coalesce_last_undo()) return false;
  auto checkpoint = m_impl->local_history->store_snapshot(
      m_impl->buffer->local_history_snapshot());
  if (!checkpoint.ok()) {
    m_impl->poisoned = true;
    return checkpoint.error().with_context(
        "undo groups coalesced in memory but checkpoint failed; reopen to reconcile");
  }
  return true;
}

Result<PollReport> DocumentSession::poll(PollBudget budget) {
  if (!m_impl || m_impl->closed)
    return Error(ErrorCode::writer_conflict, "session is closed");
  try {
    auto report = m_impl->transport->poll(
        StreamScope{m_impl->config.stream, m_impl->config.epoch}, budget);
    if (report)
      m_impl->update_completeness();
    return report;
  } catch (const std::filesystem::filesystem_error &error) {
    return Error(ErrorCode::io_error, error.what(), "polling document session");
  }
}

Result<void> DocumentSession::save() {
  if (!m_impl || m_impl->closed)
    return Error(ErrorCode::writer_conflict, "session is closed");
  auto flushed = m_impl->transport->flush();
  if (!flushed.ok())
    return flushed.error();
  auto history = m_impl->local_history->store_snapshot(
      m_impl->buffer->local_history_snapshot());
  if (!history.ok()) return history.error();
  return m_impl->replica->flush();
}

Result<void> DocumentSession::close() {
  if (!m_impl || m_impl->closed)
    return {};
  auto saved = save();
  if (!saved.ok())
    return saved.error();
  auto closed = m_impl->transport->close();
  if (!closed.ok())
    return closed.error();
  m_impl->replica->release_writer();
  auto result = m_impl->replica->close();
  if (!result.ok())
    return result.error();
  m_impl->closed = true;
  return {};
}

std::string DocumentSession::text() const {
  return m_impl && m_impl->buffer ? m_impl->buffer->text() : std::string{};
}
Global DocumentSession::recovery_frontier() const {
  return m_impl && m_impl->buffer ? m_impl->buffer->version() : Global{};
}
std::vector<SessionTextEdit>
DocumentSession::edits_since(const Global &since) const {
  std::vector<SessionTextEdit> out;
  if (!m_impl || !m_impl->buffer)
    return out;
  for (auto &edit : m_impl->buffer->edits_since(since))
    out.push_back({edit.old_start, edit.old_end, std::move(edit.new_text)});
  return out;
}
const SessionRecoveryReport &DocumentSession::recovery_report() const noexcept {
  return m_impl->report;
}
const ActorId &DocumentSession::actor() const noexcept {
  return m_impl->replica->actor();
}
bool DocumentSession::ready() const noexcept {
  return m_impl && m_impl->ready();
}

} // namespace CollabText
