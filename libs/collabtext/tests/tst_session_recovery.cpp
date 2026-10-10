#include <QTemporaryDir>
#include <QTest>

#include "crdt/Buffer.h"
#include "core/Sha256.h"
#include "identity/ReplicaAllocator.h"
#include "storage/DurableFile.h"
#include "storage/LocalHistoryLog.h"
#include "transport/StreamSync.h"
#include <collabtext/DocumentSession.h>
#include <collabtext/Serialization.h>
#include "VolatileCounter.h"

#include <fstream>

using namespace CollabText;

namespace {
std::string to_hex(std::string_view bytes) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char b : bytes) {
    out.push_back(digits[b >> 4]);
    out.push_back(digits[b & 15]);
  }
  return out;
}
std::optional<std::string> from_hex(std::string_view text) {
  if (text.size() % 2)
    return {};
  std::string out;
  out.reserve(text.size() / 2);
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    return -1;
  };
  for (size_t i = 0; i < text.size(); i += 2) {
    int hi = nibble(text[i]), lo = nibble(text[i + 1]);
    if (hi < 0 || lo < 0)
      return {};
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return out;
}
Result<void> append_durable_line(const std::filesystem::path &path,
                                 std::string_view line) {
  std::filesystem::create_directories(path.parent_path());
  auto opened = Storage::DurableAppendFile::open(path);
  if (!opened.ok())
    return opened.error();
  auto file = std::move(opened).value();
  std::string framed(line);
  framed.push_back('\n');
  return file.append(framed);
}
class ReadOnlyAllocator final : public Crdt::IdentityAllocator {
public:
  explicit ReadOnlyAllocator(ActorId actor) : m_actor(actor) {}
  Result<uint64_t> reserve_run(uint64_t) override {
    ++reserve_calls;
    return Error(ErrorCode::unsupported_operation,
                 "historical projection cannot mint identities");
  }
  void observe(uint64_t) override {}
  uint64_t peek() const noexcept override { return 1; }
  const ActorId &actor() const noexcept override { return m_actor; }
  size_t reserve_calls = 0;

private:
  ActorId m_actor;
};
} // namespace

class TestSessionRecovery : public QObject {
  Q_OBJECT
private slots:
  void own_log_replays_and_counter_continues_after_two_reopens() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::filesystem::path root = tmp.path().toStdString();
    const DocumentSessionConfig config{root / "shared", root / "device",
                                       "doc-1", "text-1", 1};
    ActorId original_actor;
    std::string original_operation;
    {
      auto opened = DocumentSession::open(config);
      QVERIFY(opened.ok());
      auto session = std::move(opened).value();
      QVERIFY(session->ready());
      original_actor = session->actor();
      const auto before = session->recovery_frontier();
      auto edit = session->apply_local({{0, 0}}, {"hello"});
      QVERIFY(edit.ok());
      QCOMPARE(session->text(), std::string("hello"));
      auto wire = encode_operation(edit.value());
      QVERIFY(wire.ok());
      original_operation = wire.value();
      auto deltas = session->edits_since(before);
      QCOMPARE(deltas.size(), size_t(1));
      QCOMPARE(deltas[0].new_text, std::string("hello"));
      QVERIFY(session->save().ok());
      QVERIFY(session->close().ok());
    }
    {
      Crdt::FileOpStream duplicate_log(config.shared_store, original_actor);
      const StreamScope scope{config.stream, config.epoch};
      QVERIFY(duplicate_log.start().ok());
      QVERIFY(duplicate_log.register_stream(scope).ok());
      QVERIFY(duplicate_log
                  .push({{scope.stream, scope.epoch, original_actor, 2},
                         original_operation},
                        PushDurability::durable)
                  .ok());
      QVERIFY(duplicate_log.close().ok());
    }
    {
      auto opened = DocumentSession::open(config);
      QVERIFY(opened.ok());
      auto session = std::move(opened).value();
      QVERIFY(session->ready());
      QCOMPARE(session->actor(), original_actor);
      QCOMPARE(session->text(), std::string("hello"));
      QVERIFY(session->recovery_report().envelopes >= size_t(1));
      QVERIFY(session->recovery_report().duplicates >= size_t(1));
      auto edit = session->apply_local({{5, 5}}, {"!"});
      QVERIFY(edit.ok());
      QVERIFY(op_identity(edit.value()).counter > 1);
      QCOMPARE(session->text(), std::string("hello!"));
      QVERIFY(session->close().ok());
    }
    {
      auto opened = DocumentSession::open(config);
      QVERIFY(opened.ok());
      auto session = std::move(opened).value();
      QVERIFY(session->ready());
      QCOMPARE(session->text(), std::string("hello!"));
      QVERIFY(session->close().ok());
    }
  }

  void session_undo_and_redo_survive_reopen() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::filesystem::path root = tmp.path().toStdString();
    const DocumentSessionConfig config{root / "shared", root / "device",
                                       "doc-undo", "text-undo", 1};
    Crdt::Identity stable_group_id;
    std::vector<Crdt::Identity> stable_operations;
    {
      auto opened = DocumentSession::open(config);
      QVERIFY(opened.ok());
      auto session = std::move(opened).value();
      QVERIFY(session->apply_local({{0, 0}}, {"local"}).ok());
      QCOMPARE(session->text(), std::string("local"));
      const auto group = session->next_undo_group();
      QVERIFY(group.has_value());
      QCOMPARE(group->operations.size(), size_t(1));
      stable_group_id = group->id;
      stable_operations = group->operations;
      QVERIFY(session->close().ok());
    }
    {
      auto reopened = DocumentSession::open(config);
      QVERIFY(reopened.ok());
      auto session = std::move(reopened).value();
      QCOMPARE(session->text(), std::string("local"));
      const auto recovered_group = session->next_undo_group();
      QVERIFY(recovered_group.has_value());
      QCOMPARE(recovered_group->id, stable_group_id);
      QCOMPARE(recovered_group->operations, stable_operations);
      QVERIFY(session->undo().ok());
      QCOMPARE(session->text(), std::string{});
      const auto redo_group = session->next_redo_group();
      QVERIFY(redo_group.has_value());
      QCOMPARE(redo_group->id, stable_group_id);
      QVERIFY(session->close().ok());
    }
    {
      auto opened = DocumentSession::open(config);
      QVERIFY(opened.ok());
      auto session = std::move(opened).value();
      QCOMPARE(session->text(), std::string{});
      QVERIFY(session->redo().ok());
      QCOMPARE(session->text(), std::string("local"));
      QVERIFY(session->close().ok());
    }
    auto reopened = DocumentSession::open(config);
    QVERIFY(reopened.ok());
    auto session = std::move(reopened).value();
    QCOMPARE(session->text(), std::string("local"));
    QVERIFY(!session->redo().ok());
    QVERIFY(session->close().ok());
  }

  void session_reopens_with_configured_undo_budget() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::filesystem::path root = tmp.path().toStdString();
    DocumentSessionConfig config{root / "shared", root / "device",
                                 "doc-undo-budget", "text-undo-budget", 1};
    config.max_undo_steps = 10;
    config.max_undo_bytes = 40;
    {
      auto opened = DocumentSession::open(config);
      QVERIFY(opened.ok());
      auto session = std::move(opened).value();
      QVERIFY(session->apply_local({{0, 0}}, {"A"}).ok());
      QVERIFY(session->apply_local({{1, 1}}, {"B"}).ok());
      QCOMPARE(session->text(), std::string("AB"));
      QVERIFY(session->close().ok());
    }
    auto opened = DocumentSession::open(config);
    QVERIFY(opened.ok());
    auto session = std::move(opened).value();
    QVERIFY(session->undo().ok());
    QCOMPARE(session->text(), std::string("A"));
    QVERIFY(session->close().ok());
  }

  void session_reconciles_pending_local_commit_before_replay() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::filesystem::path root = tmp.path().toStdString();
    const DocumentSessionConfig config{root / "shared", root / "device",
                                       "doc-pending", "text-pending", 1};
    ActorId owner;
    {
      auto opened = DocumentSession::open(config);
      QVERIFY(opened.ok());
      auto session = std::move(opened).value();
      owner = session->actor();
      QVERIFY(session->close().ok());
    }
    auto test_state = ReplicaState::open(config.private_state);
    QVERIFY(test_state.ok());
    QVERIFY(test_state.value().close().ok());

    auto allocator = std::make_shared<Test::VolatileCounter>(owner);
    Crdt::Buffer source(allocator);
    auto local = source.try_apply_local_edit({{0, 0}}, {"recovered"});
    QVERIFY(local.ok());
    auto wire = encode_operation(local.value());
    QVERIFY(wire.ok());
    const std::string history_key = config.document + std::string(1, '\0') +
                                    config.stream + std::string(1, '\0') +
                                    std::to_string(config.epoch);
    const auto history_path = config.private_state /
        ("local-history-" + sha256_hex(history_key) + ".snapshot");
    auto history_open = Storage::LocalHistoryLog::open(history_path, owner);
    QVERIFY(history_open.ok());
    auto history = std::move(history_open).value();
    QVERIFY(history.store_pending({1, wire.value(), source.local_history_snapshot()})
                .ok());

    // Simulates death after the intent is fsynced and before the operation is
    // published. Opening retries the same producer/sequence and then restores
    // its saved undo stack.
    auto reopened = DocumentSession::open(config);
    QVERIFY(reopened.ok());
    auto recovered = std::move(reopened).value();
    QCOMPARE(recovered->text(), std::string("recovered"));
    QVERIFY(recovered->undo().ok());
    QCOMPARE(recovered->text(), std::string{});
    QVERIFY(recovered->close().ok());
  }

  void reopened_local_undo_preserves_a_peers_edit() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::filesystem::path root = tmp.path().toStdString();
    const DocumentSessionConfig a_config{root / "shared", root / "device-a",
                                         "doc-undo-peer", "text-undo-peer", 1};
    const DocumentSessionConfig b_config{root / "shared", root / "device-b",
                                         "doc-undo-peer", "text-undo-peer", 1};
    auto opened_a = DocumentSession::open(a_config);
    QVERIFY(opened_a.ok());
    auto a = std::move(opened_a).value();
    QVERIFY(a->apply_local({{0, 0}}, {"A"}).ok());
    auto opened_b = DocumentSession::open(b_config);
    QVERIFY(opened_b.ok());
    auto b = std::move(opened_b).value();
    QCOMPARE(b->text(), std::string("A"));
    QVERIFY(b->apply_local({{1, 1}}, {"B"}).ok());
    QVERIFY(a->poll().ok());
    QCOMPARE(a->text(), std::string("AB"));
    QVERIFY(b->close().ok());
    QVERIFY(a->close().ok());

    auto reopened = DocumentSession::open(a_config);
    QVERIFY(reopened.ok());
    auto recovered = std::move(reopened).value();
    QCOMPARE(recovered->text(), std::string("AB"));
    QVERIFY(recovered->undo().ok());
    QCOMPARE(recovered->text(), std::string("B"));
    QVERIFY(recovered->close().ok());
  }

  void mixed_peer_replay_survives_prior_transport_receipt() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::filesystem::path root = tmp.path().toStdString();
    const DocumentSessionConfig a_config{root / "shared", root / "device-a",
                                         "doc-2", "text-2", 1};
    const DocumentSessionConfig b_config{root / "shared", root / "device-b",
                                         "doc-2", "text-2", 1};
    auto opened_a = DocumentSession::open(a_config);
    QVERIFY(opened_a.ok());
    auto a = std::move(opened_a).value();
    QVERIFY(a->apply_local({{0, 0}}, {"A"}).ok());
    QVERIFY(a->save().ok());
    auto opened_b = DocumentSession::open(b_config);
    QVERIFY(opened_b.ok());
    auto b = std::move(opened_b).value();
    QVERIFY(b->ready());
    QCOMPARE(b->text(), a->text());
    auto receipt_poll = b->poll();
    QVERIFY(receipt_poll.ok());
    QCOMPARE(receipt_poll.value().committed, size_t(1));
    QVERIFY(b->apply_local({{1, 1}}, {"B"}).ok());
    auto delivered = a->poll();
    QVERIFY(delivered.ok());
    QCOMPARE(a->text(), b->text());
    QVERIFY(b->close().ok());
    QVERIFY(a->close().ok());

    auto reopened = DocumentSession::open(b_config);
    QVERIFY(reopened.ok());
    auto recovered = std::move(reopened).value();
    QVERIFY(recovered->ready());
    QCOMPARE(recovered->text(), std::string("AB"));
    QVERIFY(recovered->close().ok());
  }

  void missing_causal_log_keeps_session_incomplete_until_arrival() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::filesystem::path root = tmp.path().toStdString();
    const auto source = root / "source";
    const DocumentSessionConfig a_config{source, root / "device-a", "doc-3",
                                         "text-3", 1};
    const DocumentSessionConfig b_config{source, root / "device-b", "doc-3",
                                         "text-3", 1};
    ActorId actor_a;
    {
      auto opened = DocumentSession::open(a_config);
      QVERIFY(opened.ok());
      auto a = std::move(opened).value();
      actor_a = a->actor();
      QVERIFY(a->apply_local({{0, 0}}, {"A"}).ok());
      QVERIFY(a->close().ok());
    }
    {
      auto opened = DocumentSession::open(b_config);
      QVERIFY(opened.ok());
      auto b = std::move(opened).value();
      QCOMPARE(b->text(), std::string("A"));
      QVERIFY(b->apply_local({{1, 1}}, {"B"}).ok());
      QVERIFY(b->close().ok());
    }

    const auto partial = root / "partial";
    std::filesystem::copy(source, partial,
                          std::filesystem::copy_options::recursive);
    std::filesystem::remove_all(partial / "replicas" / actor_a.to_string());
    const DocumentSessionConfig c_config{partial, root / "device-c", "doc-3",
                                         "text-3", 1};
    auto opened_c = DocumentSession::open(c_config);
    QVERIFY(opened_c.ok());
    auto c = std::move(opened_c).value();
    QVERIFY(!c->ready());
    QVERIFY(!c->recovery_report().complete);
    QVERIFY(c->recovery_report().deferred > 0);
    auto edit_while_incomplete = c->apply_local({{0, 0}}, {"unsafe"});
    QVERIFY(!edit_while_incomplete.ok());

    std::filesystem::copy(source / "replicas" / actor_a.to_string(),
                          partial / "replicas" / actor_a.to_string(),
                          std::filesystem::copy_options::recursive);
    auto delivery_poll = c->poll();
    QVERIFY(delivery_poll.ok());
    QVERIFY(c->ready());
    QCOMPARE(c->text(), std::string("AB"));
    QVERIFY(c->poll().ok()); // commits any earlier deferred envelope after its
                             // dependency arrived
    auto recovered = c->recover();
    QVERIFY(recovered.ok());
    QVERIFY(c->close().ok());
  }

  void consumer_outbox_reconciles_without_rewinding_canonical_history() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::filesystem::path root = tmp.path().toStdString();
    const auto state_dir = root / "private";
    const auto canonical = root / "consumer" / "accepted.log";
    const auto outbox = root / "consumer" / "outbox.log";
    const auto decisions = root / "consumer" / "decisions.log";
    ActorId owner;
    std::string pending_wire;
    uint64_t pending_counter = 0;
    {
      auto opened = ReplicaState::open(state_dir);
      QVERIFY(opened.ok());
      auto state = std::move(opened).value();
      owner = state.actor();
      QVERIFY(state.acquire_writer("doc-canonical", "stream-canonical").ok());
      auto allocator = std::make_shared<Identity::ReplicaAllocator>(
          &state, "doc-canonical", "stream-canonical", 1);
      Crdt::Buffer optimistic(allocator);
      auto accepted = optimistic.try_apply_local_edit({{0, 0}}, {"A"});
      QVERIFY(accepted.ok());
      auto accepted_wire = Crdt::encode_operation(accepted.value());
      QVERIFY(accepted_wire.ok());
      QVERIFY(append_durable_line(canonical,
                                  "A:event-1:" + to_hex(accepted_wire.value()))
                  .ok());
      auto pending = optimistic.try_apply_local_edit({{1, 1}}, {"B"});
      QVERIFY(pending.ok());
      pending_counter = Crdt::op_identity(pending.value()).counter;
      auto pending_encoded = Crdt::encode_operation(pending.value());
      QVERIFY(pending_encoded.ok());
      pending_wire = pending_encoded.value();
      QVERIFY(append_durable_line(outbox, "P:event-2:" + to_hex(pending_wire))
                  .ok());
      QVERIFY(state.close().ok());
    }

    // Reopen and reconcile the durable pending outbox against canonical
    // acceptance. The rejection marker does not remove either source row.
    auto reopened = ReplicaState::open(state_dir);
    QVERIFY(reopened.ok());
    auto state = std::move(reopened).value();
    QVERIFY(state.acquire_writer("doc-canonical", "stream-canonical").ok());
    auto allocator = std::make_shared<Identity::ReplicaAllocator>(
        &state, "doc-canonical", "stream-canonical", 1);
    Crdt::Buffer current(allocator);
    std::ifstream accepted_file(canonical), outbox_file(outbox);
    std::string accepted_line, pending_line;
    QVERIFY(static_cast<bool>(std::getline(accepted_file, accepted_line)));
    QVERIFY(static_cast<bool>(std::getline(outbox_file, pending_line)));
    const auto accepted_separator = accepted_line.rfind(':');
    const auto pending_separator = pending_line.rfind(':');
    QVERIFY(accepted_separator != std::string::npos &&
            pending_separator != std::string::npos);
    auto accepted_bytes = from_hex(
        std::string_view(accepted_line).substr(accepted_separator + 1));
    auto pending_bytes =
        from_hex(std::string_view(pending_line).substr(pending_separator + 1));
    QVERIFY(accepted_bytes && pending_bytes);
    QCOMPARE(*pending_bytes, pending_wire);
    auto accepted_op = Crdt::decode_operation(*accepted_bytes);
    QVERIFY(accepted_op.ok());
    auto pending_op = Crdt::decode_operation(*pending_bytes);
    QVERIFY(pending_op.ok());
    QCOMPARE(Crdt::op_identity(pending_op.value()).counter, pending_counter);
    const auto applied = current.apply_ops({accepted_op.value()});
    QCOMPARE(applied[0].kind(), ApplyOutcomeKind::applied);
    QCOMPARE(current.text(), std::string("A"));
    QVERIFY(append_durable_line(decisions, "R:event-2:server-rejected").ok());

    // Historical playback has no allocator capable of issuing identities.
    auto read_only = std::make_shared<ReadOnlyAllocator>(owner);
    Crdt::Buffer historical(read_only);
    auto historic_apply = historical.apply_ops({accepted_op.value()});
    QCOMPARE(historic_apply[0].kind(), ApplyOutcomeKind::applied);
    QCOMPARE(historical.text(), std::string("A"));
    QCOMPARE(read_only->reserve_calls, size_t(0));

    // A fresh current projection continues beyond the rejected reservation.
    auto next = current.try_apply_local_edit({{1, 1}}, {"C"});
    QVERIFY(next.ok());
    QVERIFY(Crdt::op_identity(next.value()).counter > pending_counter);
    QCOMPARE(current.text(), std::string("AC"));
    std::ifstream canonical_after(canonical);
    std::string retained_acceptance;
    QVERIFY(
        static_cast<bool>(std::getline(canonical_after, retained_acceptance)));
    QCOMPARE(retained_acceptance, accepted_line);
    QVERIFY(state.close().ok());
  }

  void consumer_commit_pin_crash_matrix_rebuilds_projection_before_gc() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::filesystem::path root = tmp.path().toStdString();
    const DocumentSessionConfig seed_config{root / "seed-shared",
                                             root / "seed-private", "doc-pin",
                                             "stream-pin", 1};
    std::string event_wire;
    {
      auto opened = DocumentSession::open(seed_config);
      QVERIFY(opened.ok());
      auto session = std::move(opened).value();
      auto event = session->apply_local({{0, 0}}, {"reply"});
      QVERIFY(event.ok());
      auto encoded = encode_operation(event.value());
      QVERIFY(encoded.ok());
      event_wire = encoded.value();
      QVERIFY(session->close().ok());
    }

    // Each row models a crash after the named durable consumer write. Recovery
    // must preserve accepted history and rebuild the projection and its pin
    // before the consumer enables collection.
    for (int crash_stage = 0; crash_stage != 4; ++crash_stage) {
      const auto case_dir = root / ("stage-" + std::to_string(crash_stage));
      const auto outbox = case_dir / "outbox.log";
      const auto canonical = case_dir / "canonical.log";
      const auto projection = case_dir / "projection.log";
      const auto pins = case_dir / "pins.log";
      QVERIFY(append_durable_line(outbox, "pending:event-1:" + to_hex(event_wire))
                  .ok());
      if (crash_stage >= 1)
        QVERIFY(append_durable_line(
                    canonical, "accepted:event-1:" + to_hex(event_wire))
                    .ok());
      if (crash_stage >= 2)
        QVERIFY(append_durable_line(projection, "projected:event-1").ok());
      if (crash_stage >= 3)
        QVERIFY(append_durable_line(pins, "pinned:event-1:frontier-1").ok());

      bool gc_allowed = false;
      std::string canonical_row;
      std::ifstream canonical_in(canonical);
      const bool has_canonical =
          static_cast<bool>(std::getline(canonical_in, canonical_row));
      if (has_canonical) {
        std::string projection_row;
        std::ifstream projection_in(projection);
        if (!std::getline(projection_in, projection_row))
          QVERIFY(append_durable_line(projection, "projected:event-1").ok());
        std::string pin_row;
        std::ifstream pins_in(pins);
        if (!std::getline(pins_in, pin_row))
          QVERIFY(append_durable_line(pins, "pinned:event-1:frontier-1").ok());
        // The simulated collector gate opens only after recovered projection
        // and pin records are durable.
        std::ifstream recovered_pin(pins);
        QVERIFY(static_cast<bool>(std::getline(recovered_pin, pin_row)));
        gc_allowed = pin_row == "pinned:event-1:frontier-1";
      }
      QCOMPARE(gc_allowed, has_canonical);

      // Rejection/reconciliation never removes an accepted canonical row or
      // its pending outbox evidence.
      std::ifstream canonical_after(canonical), outbox_after(outbox);
      std::string retained_canonical, retained_outbox;
      QCOMPARE(static_cast<bool>(std::getline(canonical_after, retained_canonical)),
               has_canonical);
      QVERIFY(static_cast<bool>(std::getline(outbox_after, retained_outbox)));
      if (has_canonical)
        QCOMPARE(retained_canonical,
                 "accepted:event-1:" + to_hex(event_wire));
      QCOMPARE(retained_outbox, "pending:event-1:" + to_hex(event_wire));
    }
  }

  void invalid_store_path_returns_typed_open_error() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::filesystem::path root = tmp.path().toStdString();
    const auto blocked_path = root / "shared-file";
    {
      std::ofstream file(blocked_path);
      QVERIFY(file.good());
      file << "not a directory";
    }
    auto opened = DocumentSession::open(
        {blocked_path, root / "private", "doc", "stream", 1});
    QVERIFY(!opened.ok());
    QCOMPARE(opened.error().code(), ErrorCode::io_error);
  }
};

QTEST_MAIN(TestSessionRecovery)
#include "tst_session_recovery.moc"
