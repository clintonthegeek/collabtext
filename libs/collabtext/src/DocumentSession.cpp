#include <collabtext/DocumentSession.h>

#include "crdt/Buffer.h"
#include "crdt/SeedOp.h"
#include "core/Sha256.h"
#include "identity/ReplicaAllocator.h"
#include "storage/CheckpointCodec.h"
#include "storage/LocalHistoryLog.h"
#include "transport/StreamSync.h"
#ifdef COLLABTEXT_ENABLE_DOCUMENT_SESSION_TEST_ACCESS
#include "DocumentSessionTestAccess.h"
#endif
#include <collabtext/Serialization.h>

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace CollabText {
using namespace Crdt;

struct DocumentSession::Impl {
  struct DeferredState {
    DeliveryToken token;
    Operation operation;
    std::string payload;
  };
  DocumentSessionConfig config;
  std::optional<ReplicaState> replica;
  std::shared_ptr<CollabText::Identity::ReplicaAllocator> allocator;
  std::unique_ptr<Crdt::Buffer> buffer;
  std::unique_ptr<Crdt::FileOpStream> transport;
  std::unique_ptr<Storage::LocalHistoryLog> local_history;
  SessionRecoveryReport report;
  Global history_floor;
  uint64_t next_envelope = 1;
  std::unordered_map<std::string, std::string> replayed_payload_hashes;
  std::unordered_map<std::string, DeferredState> deferred_tokens;
  std::vector<SessionQuarantinedDelivery> quarantined_deliveries;
  size_t quarantined_bytes = 0;
  bool quarantine_snapshot_complete = true;
  bool closed = false;
  bool poisoned = false;

  Result<std::shared_ptr<void>> authorize_local_mutation() const {
    if (!config.authorize_local_mutation) return std::shared_ptr<void>{};
    try {
      auto permit = config.authorize_local_mutation(
          config.document, config.stream, config.epoch, replica->actor());
      if (!permit.ok()) return permit.error();
      if (!permit.value())
        return Error(ErrorCode::malformed,
                     "membership authorization returned no mutation lease");
      return permit;
    } catch (const std::exception &error) {
      return Error(ErrorCode::io_error, error.what(),
                   "checking local membership authorization");
    } catch (...) {
      return Error(ErrorCode::io_error,
                   "membership authorization callback threw an unknown exception");
    }
  }

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
    if (auto remembered = remember_replayed_delivery(
            InboundDelivery{delivery.token, delivery.payload});
        !remembered.ok()) {
      poisoned = true;
      return remembered.error().with_context(
          "durable local delivery conflicts with represented checkpoint input");
    }
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

  void retain_quarantine(const InboundDelivery &delivery, const Error &reason) {
    const auto duplicate = std::find_if(
        quarantined_deliveries.begin(), quarantined_deliveries.end(),
        [&](const SessionQuarantinedDelivery &record) {
          return record.delivery.token == delivery.token &&
                 record.delivery.payload == delivery.payload;
        });
    if (duplicate == quarantined_deliveries.end()) {
      size_t record_bytes = delivery.payload.size();
      const auto add_bytes = [&record_bytes](size_t amount) {
        if (amount > std::numeric_limits<size_t>::max() - record_bytes)
          return false;
        record_bytes += amount;
        return true;
      };
      const auto context = reason.context();
      const bool size_valid = add_bytes(reason.message().size()) &&
          (!context || add_bytes(context->size()));
      if (quarantined_deliveries.size() >=
              config.max_quarantined_deliveries ||
          !size_valid || record_bytes > config.max_quarantined_bytes -
              std::min(quarantined_bytes, config.max_quarantined_bytes)) {
        quarantine_snapshot_complete = false;
      } else {
        quarantined_deliveries.push_back({delivery, reason});
        quarantined_bytes += record_bytes;
      }
    }
  }

  void quarantine(const InboundDelivery &delivery, const Error &reason) {
    report.complete = false;
    ++report.rejected;
    if (!report.issue) report.issue = reason;
    retain_quarantine(delivery, reason);
  }

  static std::string token_key(const DeliveryToken &token) {
    return token.producer.to_string() + ":" + std::to_string(token.sequence);
  }

  Result<void> remember_replayed_delivery(const InboundDelivery &delivery) {
    const auto key = token_key(delivery.token);
    const auto payload_hash = sha256_hex(delivery.payload);
    auto [found, inserted] = replayed_payload_hashes.emplace(key, payload_hash);
    if (!inserted && found->second != payload_hash)
      return Error(ErrorCode::corrupt_record,
                   "replayed delivery token was reused with different payload bytes");
    return {};
  }

  Result<Storage::CheckpointShareableCore>
  checkpoint_core(Storage::CheckpointShareableCore core) const {
    if (closed || poisoned || !buffer || !transport)
      return Error(ErrorCode::writer_conflict,
                   "session cannot capture a checkpoint in its current state");
    if (core.context.document != config.document ||
        core.context.stream != config.stream || core.context.epoch != config.epoch ||
        core.context.engine != Storage::CheckpointEngineKind::Text)
      return Error(ErrorCode::wrong_epoch,
                   "checkpoint provider context does not match the document session");
    if (!quarantine_snapshot_complete)
      return Error(ErrorCode::limit_exceeded,
                   "checkpoint quarantine snapshot exceeded its configured budget");

    const auto state = buffer->checkpoint_state();
    core.engine.kind = Storage::CheckpointEngineKind::Text;
    core.engine.fragments = state.fragments;
    core.engine.entries.clear();
    core.causal.version = state.version;
    core.causal.undo_entries = state.undo_entries;
    core.coverage.operation.clear();
    for (size_t i = 0; i < state.version.pair_count(); ++i) {
      const auto [actor, counter] = state.version.pair(i);
      core.coverage.operation.emplace(actor, counter);
    }

    core.deferred.clear();
    core.deferred.reserve(deferred_tokens.size() + quarantined_deliveries.size());
    for (const auto &[_, deferred] : deferred_tokens) {
      if (state.version.observed(get_op_timestamp(deferred.operation)))
        continue; // Buffer drained this deferred operation after its dependency arrived.
      core.deferred.push_back({deferred.token, deferred.payload, false, {}});
    }
    for (const auto &quarantined : quarantined_deliveries)
      core.deferred.push_back({quarantined.delivery.token,
                               quarantined.delivery.payload, true,
                               std::string(quarantined.reason.message())});
    std::sort(core.deferred.begin(), core.deferred.end(),
              [](const auto &left, const auto &right) {
                if (left.token.producer != right.token.producer)
                  return left.token.producer < right.token.producer;
                return left.token.sequence < right.token.sequence;
              });

    core.coverage.delivery.clear();
    std::unordered_set<std::string> producer_prefix_broken;
    std::unordered_map<std::string, const SessionQuarantinedDelivery *>
        quarantined_by_token;
    quarantined_by_token.reserve(quarantined_deliveries.size());
    for (const auto &record : quarantined_deliveries)
      quarantined_by_token.emplace(token_key(record.delivery.token), &record);
    auto scanned = transport->replay_all(
        StreamScope{config.stream, config.epoch},
        [&](const InboundDelivery &delivery) -> Result<void> {
          const auto key = token_key(delivery.token);
          bool checkpointed_outstanding = false;
          if (const auto deferred = deferred_tokens.find(key);
              deferred != deferred_tokens.end() &&
              !state.version.observed(get_op_timestamp(deferred->second.operation))) {
            if (deferred->second.payload != delivery.payload)
              return Error(ErrorCode::corrupt_record,
                           "checkpoint log payload differs from deferred session input");
            checkpointed_outstanding = true;
          }
          if (const auto quarantined = quarantined_by_token.find(key);
              quarantined != quarantined_by_token.end()) {
            if (quarantined->second->delivery.payload != delivery.payload)
              return Error(ErrorCode::corrupt_record,
                           "checkpoint log payload differs from quarantined session input");
            checkpointed_outstanding = true;
          }
          bool represented = false;
          if (!checkpointed_outstanding) {
            if (const auto applied = replayed_payload_hashes.find(key);
                applied != replayed_payload_hashes.end()) {
              if (applied->second != sha256_hex(delivery.payload))
                return Error(ErrorCode::corrupt_record,
                             "checkpoint log payload differs from replayed session input");
              represented = true;
            }
          }
          const auto producer = delivery.token.producer.to_string();
          if (!represented) {
            producer_prefix_broken.insert(producer);
          } else if (!producer_prefix_broken.contains(producer)) {
            core.coverage.delivery[delivery.token.producer] =
                delivery.token.sequence;
          }
          return {};
        });
    if (!scanned.ok()) return scanned.error();
    return core;
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
    if (const auto known = replayed_payload_hashes.find(key);
        known != replayed_payload_hashes.end()) {
      if (known->second != sha256_hex(delivery.payload))
        return Error(ErrorCode::corrupt_record,
                     "replayed delivery token was reused with different payload bytes");
      return {};
    }
    auto decoded = decode_operation(delivery.payload);
    if (!decoded.ok()) {
      quarantine(delivery, decoded.error().with_context("full session replay"));
      return {};
    }
    if (op_identity(decoded.value()).actor != delivery.token.producer) {
      quarantine(delivery,
                 Error(ErrorCode::actor_conflict,
                       "operation identity does not match its delivery producer"));
      return {};
    }
    ++report.envelopes;
    auto outcomes = buffer->apply_ops({decoded.value()});
    if (!outcomes.empty()) {
      classify(outcomes.front());
      if (outcomes.front().kind() != ApplyOutcomeKind::rejected) {
        if (auto remembered = remember_replayed_delivery(delivery); !remembered.ok())
          return remembered.error();
      } else {
        retain_quarantine(delivery, outcomes.front().error().value_or(
            Error(ErrorCode::malformed, "operation was rejected during recovery")));
      }
      if (outcomes.front().kind() == ApplyOutcomeKind::deferred)
        deferred_tokens.emplace(
            key, DeferredState{delivery.token, decoded.value(), delivery.payload});
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

Result<Storage::CheckpointShareableCore>
DocumentSessionTestAccess::checkpoint_core(
    DocumentSession &session, Storage::CheckpointShareableCore core) {
  if (!session.m_impl)
    return Error(ErrorCode::writer_conflict,
                 "session is unavailable for checkpoint capture");
  auto captured = session.m_impl->checkpoint_core(std::move(core));
  if (!captured.ok()) return captured.error();
  auto encoded = Storage::encode_checkpoint_core(captured.value());
  if (!encoded.ok()) return encoded.error();
  return std::move(captured).value();
}

Result<std::unique_ptr<DocumentSession>>
DocumentSession::open(DocumentSessionConfig config) {
  return open_with_writer_config(std::move(config), Crdt::WriterConfig{});
}

Result<std::unique_ptr<DocumentSession>>
DocumentSession::open_with_writer_config(
    DocumentSessionConfig config, const Crdt::WriterConfig &writer_config) {
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
    impl->allocator = std::make_shared<CollabText::Identity::ReplicaAllocator>(
        &*impl->replica, impl->config.document, impl->config.stream,
        impl->config.epoch);
    impl->buffer = std::make_unique<Crdt::Buffer>(impl->allocator);
    auto depth_limit = impl->buffer->set_max_undo_depth(
        impl->config.max_undo_steps);
    if (!depth_limit.ok()) return depth_limit.error();
    auto byte_limit = impl->buffer->set_max_undo_bytes(
        impl->config.max_undo_bytes);
    if (!byte_limit.ok()) return byte_limit.error();
    if (!impl->config.initial_text.empty())
      impl->buffer->apply_ops({Crdt::op_for_seed(impl->config.initial_text)});
    impl->history_floor = impl->buffer->version();
    auto local_history = Storage::LocalHistoryLog::open(
        Impl::history_path(impl->config), impl->replica->actor());
    if (!local_history.ok()) return local_history.error();
    impl->local_history = std::make_unique<Storage::LocalHistoryLog>(
        std::move(local_history).value());
    impl->transport = std::make_unique<Crdt::FileOpStream>(
        impl->config.shared_store, impl->replica->actor(), writer_config);
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
            raw->quarantine(delivery, decoded.error());
            return DeliveryDecision::quarantine(decoded.error());
          }
          if (op_identity(decoded.value()).actor != delivery.token.producer) {
            auto error = Error(
                ErrorCode::actor_conflict,
                "operation identity does not match its delivery producer");
            raw->quarantine(delivery, error);
            return DeliveryDecision::quarantine(error);
          }
          const auto token_key = Impl::token_key(delivery.token);
          if (auto waiting = raw->deferred_tokens.find(token_key);
              waiting != raw->deferred_tokens.end()) {
            if (waiting->second.payload != delivery.payload) {
              auto error = Error(
                  ErrorCode::corrupt_record,
                  "deferred delivery token was reused with different payload bytes");
              raw->quarantine(delivery, error);
              return DeliveryDecision::quarantine(std::move(error));
            }
            if (!raw->buffer->version().observed(
                    op_identity(waiting->second.operation)))
              return DeliveryDecision::retry();
            raw->deferred_tokens.erase(waiting);
            if (auto remembered = raw->remember_replayed_delivery(delivery);
                !remembered.ok()) {
              raw->quarantine(delivery, remembered.error());
              return DeliveryDecision::quarantine(remembered.error());
            }
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
            if (auto remembered = raw->remember_replayed_delivery(delivery);
                !remembered.ok()) {
              raw->quarantine(delivery, remembered.error());
              return DeliveryDecision::quarantine(remembered.error());
            }
            raw->update_completeness();
            return DeliveryDecision::commit(
                RecoveryPoint::replay_from_beginning());
          }
          raw->report.complete = false;
          raw->classify(outcome);
          if (outcome.kind() == ApplyOutcomeKind::deferred) {
            raw->deferred_tokens.emplace(
                token_key, Impl::DeferredState{delivery.token, decoded.value(),
                                               delivery.payload});
            if (auto remembered = raw->remember_replayed_delivery(delivery);
                !remembered.ok()) {
              raw->quarantine(delivery, remembered.error());
              return DeliveryDecision::quarantine(remembered.error());
            }
            return DeliveryDecision::retry();
          }
          const auto error = outcome.error().value_or(
              Error(ErrorCode::malformed, "operation was rejected"));
          raw->retain_quarantine(delivery, error);
          return DeliveryDecision::quarantine(error);
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
  auto authorized = m_impl->authorize_local_mutation();
  if (!authorized.ok()) return authorized.error();
  auto operation = m_impl->buffer->try_apply_local_edit(ranges, replacements);
  if (!operation.ok())
    return operation.error();
  return m_impl->publish_local(std::move(operation).value());
}

Result<Operation> DocumentSession::undo() {
  if (!m_impl || !m_impl->ready())
    return Error(ErrorCode::awaiting_state,
                 "session recovery is incomplete; undo is disabled");
  auto authorized = m_impl->authorize_local_mutation();
  if (!authorized.ok()) return authorized.error();
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
  auto authorized = m_impl->authorize_local_mutation();
  if (!authorized.ok()) return authorized.error();
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

std::vector<Crdt::LocalUndoEntrySnapshot>
DocumentSession::pending_undo_retirements() const {
  if (!m_impl || !m_impl->buffer) return {};
  return m_impl->buffer->local_history_snapshot().retired_entries;
}

Result<void> DocumentSession::acknowledge_undo_retirements(
    const std::vector<Crdt::Identity> &group_ids) {
  if (!m_impl || !m_impl->ready())
    return Error(ErrorCode::awaiting_state,
                 "session recovery is incomplete; undo retirement is disabled");
  auto acknowledged = m_impl->buffer->acknowledge_undo_retirements(group_ids);
  if (!acknowledged.ok()) return acknowledged.error();
  auto checkpoint = m_impl->local_history->store_snapshot(
      m_impl->buffer->local_history_snapshot());
  if (!checkpoint.ok()) {
    m_impl->poisoned = true;
    return checkpoint.error().with_context(
        "undo retirement acknowledged in memory but checkpoint failed; reopen to retry");
  }
  return Result<void>::success();
}

Result<bool> DocumentSession::coalesce_last_undo() {
  if (!m_impl || !m_impl->ready())
    return Error(ErrorCode::awaiting_state,
                 "session recovery is incomplete; undo coalescing is disabled");
  auto authorized = m_impl->authorize_local_mutation();
  if (!authorized.ok()) return authorized.error();
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

Result<SessionShareableCheckpoint>
DocumentSession::capture_shareable_checkpoint(
    const SessionCheckpointProviderState &provider) const {
  if (!m_impl)
    return Error(ErrorCode::writer_conflict,
                 "session is unavailable for checkpoint capture");
  if (!provider.pin_projection_complete ||
      provider.projected_pin_frontier != provider.canonical_pin_frontier)
    return Error(ErrorCode::awaiting_state,
                 "provider pin projection is incomplete");

  Storage::CheckpointShareableCore core;
  core.context.document = m_impl->config.document;
  core.context.stream = m_impl->config.stream;
  core.context.epoch = m_impl->config.epoch;
  core.context.parent_descriptor_hash = provider.parent_descriptor_hash;
  core.context.engine = Storage::CheckpointEngineKind::Text;
  core.pins.projected_frontier = provider.projected_pin_frontier;
  core.pins.canonical_frontier = provider.canonical_pin_frontier;
  core.pins.complete = provider.pin_projection_complete;
  core.pins.canonical_projection_hash =
      provider.canonical_pin_projection_hash;
  core.pins.live_pins = provider.live_pins;
  core.translations.supported_token_versions =
      provider.supported_token_versions;
  core.translations.manifest_hash =
      provider.expected_translation_manifest_hash;
  core.translations.entries.reserve(provider.translations.size());
  for (const auto &translation : provider.translations) {
    core.translations.entries.push_back({
        translation.reference_id, translation.source_epoch,
        translation.source_token_bytes, translation.current_token_bytes});
  }

  auto captured = m_impl->checkpoint_core(std::move(core));
  if (!captured.ok()) return captured.error();
  auto translation_section = Storage::encode_checkpoint_translations(
      captured.value().translations, captured.value().context,
      captured.value().pins);
  if (!translation_section.ok()) return translation_section.error();
  auto canonical_translations = Storage::decode_checkpoint_translations(
      translation_section.value(), captured.value().context,
      captured.value().pins);
  if (!canonical_translations.ok()) return canonical_translations.error();
  captured.value().translations.manifest_hash =
      canonical_translations.value().manifest_hash;
  auto encoded = Storage::encode_checkpoint_core(captured.value());
  if (!encoded.ok()) return encoded.error();
  auto artifact_bytes = std::move(encoded).value();
  auto artifact_hash = sha256_hex(artifact_bytes);
  return SessionShareableCheckpoint{
      std::move(artifact_bytes), std::move(artifact_hash),
      std::move(captured.value().coverage.operation),
      std::move(captured.value().coverage.delivery),
      std::move(captured.value().pins.canonical_projection_hash),
      std::move(captured.value().translations.manifest_hash)};
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

Result<void> DocumentSession::export_local_branch(
    LocalBranchExport& metadata,
    const std::function<Result<void>(const OutboundDelivery&)>& visitor) {
  if (!m_impl || m_impl->closed)
    return Error(ErrorCode::writer_conflict, "session is closed");
  if (!visitor)
    return Error(ErrorCode::malformed, "local branch export visitor is empty");

  const ActorId producer = m_impl->replica->actor();
  const StreamScope scope{m_impl->config.stream, m_impl->config.epoch};
  std::optional<OutboundDelivery> pending_delivery;
  auto pending = m_impl->local_history->recover_pending();
  if (!pending.ok()) return pending.error();
  if (pending.value()) {
    const auto& commit = *pending.value();
    if (commit.sequence == 0 || commit.sequence == UINT64_MAX)
      return Error(ErrorCode::corrupt_record,
                   "pending local branch delivery sequence is invalid");
    auto operation = decode_operation(commit.operation);
    if (!operation.ok())
      return operation.error().with_context("decoding pending branch operation");
    if (op_identity(operation.value()).actor != producer)
      return Error(ErrorCode::actor_conflict,
                   "pending branch operation belongs to another producer");
    pending_delivery.emplace(OutboundDelivery{
        DeliveryToken{scope.stream, scope.epoch, producer, commit.sequence},
        commit.operation});
  }

  metadata = LocalBranchExport{m_impl->config.document, scope, producer};
  uint64_t expected = 1;
  bool pending_seen_in_log = false;
  const auto validate_delivery = [&](const OutboundDelivery& delivery) -> Result<void> {
    if (delivery.token.stream != scope.stream || delivery.token.epoch != scope.epoch
        || delivery.token.producer != producer || delivery.token.sequence == 0
        || delivery.token.sequence == UINT64_MAX)
      return Error(ErrorCode::corrupt_record,
                   "local branch contains a delivery outside its original scope");
    auto operation = decode_operation(delivery.payload);
    if (!operation.ok())
      return operation.error().with_context("decoding local branch operation");
    if (op_identity(operation.value()).actor != producer)
      return Error(ErrorCode::actor_conflict,
                   "local branch operation identity differs from its producer");
    return {};
  };

  try {
    auto replayed = m_impl->transport->replay_producer(
        scope, producer, [&](const InboundDelivery& delivery) -> Result<void> {
          OutboundDelivery local{delivery.token, delivery.payload};
          auto valid = validate_delivery(local);
          if (!valid.ok()) return valid.error();
          if (local.token.sequence != expected)
            return Result<void>(Error(ErrorCode::corrupt_record,
                                      "local branch delivery sequence has a gap"));
          ++expected;
          if (pending_delivery &&
              local.token.sequence == pending_delivery->token.sequence) {
            if (local.payload != pending_delivery->payload)
              return Result<void>(Error(
                  ErrorCode::actor_conflict,
                  "pending local branch conflicts with retained delivery bytes"));
            pending_seen_in_log = true;
          } else if (pending_delivery &&
                     local.token.sequence > pending_delivery->token.sequence) {
            return Result<void>(Error(
                ErrorCode::corrupt_record,
                "pending local branch delivery is missing before retained history"));
          }
          return visitor(local);
        });
    if (!replayed.ok()) return replayed.error();

    if (pending_delivery && !pending_seen_in_log) {
      if (pending_delivery->token.sequence != expected)
        return Error(ErrorCode::corrupt_record,
                     "pending local branch delivery does not extend retained history");
      auto delivered = visitor(*pending_delivery);
      if (!delivered.ok()) return delivered.error();
    }
  } catch (const std::filesystem::filesystem_error& error) {
    return Error(ErrorCode::io_error, error.what(), "exporting local branch");
  } catch (const std::exception& error) {
    return Error(ErrorCode::io_error, error.what(), "exporting local branch");
  }
  return {};
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
uint32_t DocumentSession::visible_length() const {
  return m_impl && m_impl->buffer ? m_impl->buffer->visible_length() : 0;
}
size_t DocumentSession::undo_depth() const {
  return m_impl && m_impl->buffer ? m_impl->buffer->undo_depth() : 0;
}
Crdt::Anchor DocumentSession::anchor_at(uint32_t byte_offset,
                                         Crdt::Bias bias) const {
  return m_impl && m_impl->buffer
             ? m_impl->buffer->anchor_at(byte_offset, bias)
             : Crdt::Anchor::min();
}
uint32_t DocumentSession::resolve_anchor(const Crdt::Anchor &anchor) const {
  return m_impl && m_impl->buffer ? m_impl->buffer->resolve_anchor(anchor) : 0;
}
Global DocumentSession::recovery_frontier() const {
  return m_impl && m_impl->buffer ? m_impl->buffer->version() : Global{};
}
Result<std::vector<SessionTextEdit>>
DocumentSession::edits_since(const Global &since) const {
  std::vector<SessionTextEdit> out;
  if (!m_impl || !m_impl->buffer)
    return Error(ErrorCode::history_unavailable, "session history is unavailable");
  const auto current = m_impl->buffer->version();
  if (!current.observed_all(since))
    return Error(ErrorCode::awaiting_state,
                 "requested edit version is ahead of the session frontier");
  if (!since.observed_all(m_impl->history_floor))
    return Error(ErrorCode::history_unavailable,
                 "requested edit version is below the retained history floor");
  for (auto &edit : m_impl->buffer->edits_since(since))
    out.push_back({edit.old_start, edit.old_end, std::move(edit.new_text)});
  return out;
}
const SessionRecoveryReport &DocumentSession::recovery_report() const noexcept {
  return m_impl->report;
}
std::vector<InboundDelivery> DocumentSession::deferred_deliveries() const {
  std::vector<InboundDelivery> deliveries;
  if (!m_impl) return deliveries;
  deliveries.reserve(m_impl->deferred_tokens.size());
  for (const auto &[_, deferred] : m_impl->deferred_tokens)
    deliveries.push_back({deferred.token, deferred.payload});
  std::sort(deliveries.begin(), deliveries.end(),
            [](const InboundDelivery &left, const InboundDelivery &right) {
              if (left.token.stream != right.token.stream)
                return left.token.stream < right.token.stream;
              if (left.token.epoch != right.token.epoch)
                return left.token.epoch < right.token.epoch;
              if (left.token.producer != right.token.producer)
                return left.token.producer < right.token.producer;
              return left.token.sequence < right.token.sequence;
            });
  return deliveries;
}
Result<std::vector<SessionQuarantinedDelivery>>
DocumentSession::quarantined_deliveries() const {
  if (!m_impl)
    return Error(ErrorCode::writer_conflict,
                 "session is unavailable for quarantine inspection");
  if (!m_impl->quarantine_snapshot_complete)
    return Error(ErrorCode::limit_exceeded,
                 "quarantined delivery snapshot exceeded its configured budget");
  auto deliveries = m_impl->quarantined_deliveries;
  std::sort(deliveries.begin(), deliveries.end(),
            [](const SessionQuarantinedDelivery &left,
               const SessionQuarantinedDelivery &right) {
              if (left.delivery.token.stream != right.delivery.token.stream)
                return left.delivery.token.stream < right.delivery.token.stream;
              if (left.delivery.token.epoch != right.delivery.token.epoch)
                return left.delivery.token.epoch < right.delivery.token.epoch;
              if (left.delivery.token.producer != right.delivery.token.producer)
                return left.delivery.token.producer < right.delivery.token.producer;
              if (left.delivery.token.sequence != right.delivery.token.sequence)
                return left.delivery.token.sequence < right.delivery.token.sequence;
              if (left.delivery.payload != right.delivery.payload)
                return left.delivery.payload < right.delivery.payload;
              return left.reason.message() < right.reason.message();
            });
  return deliveries;
}
const ActorId &DocumentSession::actor() const noexcept {
  return m_impl->replica->actor();
}
bool DocumentSession::ready() const noexcept {
  return m_impl && m_impl->ready();
}

} // namespace CollabText
