#include <QTemporaryDir>
#include <QTest>

#include <collabtext/Anchor.h>
#include <collabtext/CrdtEngine.h>
#include <collabtext/MembershipCoordinator.h>
#include "crdt/Buffer.h"
#include "crdt/SeedOp.h"
#include "core/Sha256.h"
#include "identity/ReplicaAllocator.h"
#include "storage/DurableFile.h"
#include "storage/CheckpointStore.h"
#include "storage/GcCertificate.h"
#include "storage/LocalHistoryLog.h"
#include "storage/MembershipStore.h"
#include "storage/RetentionState.h"
#include "transport/StreamSync.h"
#include <collabtext/DocumentSession.h>
#include "DocumentSessionTestAccess.h"
#include "CodecTest.h"
#include "TestActor.h"
#include <collabtext/Serialization.h>
#include "VolatileCounter.h"

#include <fstream>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

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
  void localMutationAuthorizationBlocksBeforeChangingSessionState() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::filesystem::path root = tmp.path().toStdString();
    bool enrolled = false;
    bool omit_lease = false;
    bool throw_from_callback = false;
    bool publication_had_lease = false;
    std::weak_ptr<void> outstanding_lease;
    size_t checks = 0;
    Storage::StepHook hook;
    hook.observe = [&](Storage::Step step, const std::filesystem::path &) {
      if (step == Storage::Step::Write && !outstanding_lease.expired())
        publication_had_lease = true;
    };
    Crdt::WriterConfig writer_config;
    writer_config.step_hook = &hook;
    DocumentSessionConfig config{root / "shared", root / "device",
                                 "doc-membership-gate", "text", 4};
    config.authorize_local_mutation = [&](const DocumentId &document,
                                          const StreamId &stream,
                                          uint64_t epoch,
                                          const ActorId &producer)
      -> Result<std::shared_ptr<void>> {
      ++checks;
      if (document != config.document || stream != config.stream || epoch != config.epoch ||
          !producer.valid())
        return Error(ErrorCode::malformed, "membership callback received the wrong scope");
      if (throw_from_callback) throw std::runtime_error("membership lookup failed");
      if (!enrolled)
        return Error(ErrorCode::wrong_epoch, "producer has been retired from this epoch");
      if (omit_lease) return std::shared_ptr<void>{};
      auto lease = std::make_shared<int>(1);
      outstanding_lease = lease;
      return std::static_pointer_cast<void>(lease);
    };

    auto opened = DocumentSessionTestAccess::open(config, writer_config);
    QVERIFY(opened.ok());
    auto session = std::move(opened).value();
    auto denied_edit = session->apply_local({{0, 0}}, {"a"});
    QVERIFY(!denied_edit.ok());
    QCOMPARE(denied_edit.error().code(), ErrorCode::wrong_epoch);
    QCOMPARE(session->text(), std::string());
    QCOMPARE(session->undo_depth(), size_t(0));

    enrolled = true;
    omit_lease = true;
    auto missing_lease_edit = session->apply_local({{0, 0}}, {"a"});
    QVERIFY(!missing_lease_edit.ok());
    QCOMPARE(missing_lease_edit.error().code(), ErrorCode::malformed);
    omit_lease = false;
    throw_from_callback = true;
    auto throwing_callback_edit = session->apply_local({{0, 0}}, {"a"});
    QVERIFY(!throwing_callback_edit.ok());
    QCOMPARE(throwing_callback_edit.error().code(), ErrorCode::io_error);
    throw_from_callback = false;
    QCOMPARE(session->text(), std::string());
    QCOMPARE(session->undo_depth(), size_t(0));
    QVERIFY(session->apply_local({{0, 0}}, {"a"}).ok());
    QVERIFY(publication_had_lease);
    QVERIFY(outstanding_lease.expired());
    QVERIFY(session->apply_local({{1, 1}}, {"b"}).ok());
    const auto depth_before_denials = session->undo_depth();
    enrolled = false;
    auto denied_undo = session->undo();
    QVERIFY(!denied_undo.ok()); QCOMPARE(denied_undo.error().code(), ErrorCode::wrong_epoch);
    auto denied_coalesce = session->coalesce_last_undo();
    QVERIFY(!denied_coalesce.ok());
    QCOMPARE(denied_coalesce.error().code(), ErrorCode::wrong_epoch);
    QCOMPARE(session->text(), std::string("ab"));
    QCOMPARE(session->undo_depth(), depth_before_denials);

    enrolled = true;
    QVERIFY(session->undo().ok());
    enrolled = false;
    auto denied_redo = session->redo();
    QVERIFY(!denied_redo.ok()); QCOMPARE(denied_redo.error().code(), ErrorCode::wrong_epoch);
    QCOMPARE(session->text(), std::string("a"));
    enrolled = true;
    QVERIFY(session->redo().ok());
    QCOMPARE(session->text(), std::string("ab"));
    QVERIFY(checks >= size_t(8));
    QVERIFY(session->close().ok());
  }

  void inboundAuthorizationCallbackCannotReenterSessionMutation() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::filesystem::path root = tmp.path().toStdString();
    DocumentSessionConfig sender_config{root / "shared", root / "sender",
                                        "doc-auth-reentry", "text-auth-reentry", 3};
    auto sender_opened = DocumentSession::open(sender_config);
    QVERIFY(sender_opened.ok());
    auto sender = std::move(sender_opened).value();
    QVERIFY(sender->apply_local({{0, 0}}, {"a"}).ok());
    QVERIFY(sender->close().ok());

    DocumentSession *receiver_ptr = nullptr;
    ErrorCode nested_error = ErrorCode::malformed;
    DocumentSessionConfig receiver_config{root / "shared", root / "receiver",
                                          sender_config.document,
                                          sender_config.stream,
                                          sender_config.epoch};
    receiver_config.authorize_inbound_delivery =
        [&](const DocumentId &, const StreamId &, uint64_t,
            const DeliveryToken &) -> Result<std::shared_ptr<void>> {
      if (receiver_ptr) {
        auto nested = receiver_ptr->apply_local({{0, 0}}, {"x"});
        nested_error = nested.ok() ? ErrorCode::malformed : nested.error().code();
      }
      return std::static_pointer_cast<void>(std::make_shared<int>(1));
    };
    auto receiver_opened = DocumentSession::open(receiver_config);
    QVERIFY(receiver_opened.ok());
    auto receiver = std::move(receiver_opened).value();
    receiver_ptr = receiver.get();
    QVERIFY(receiver->ready());
    QCOMPARE(receiver->text(), std::string("a"));

    sender_opened = DocumentSession::open(sender_config);
    QVERIFY(sender_opened.ok());
    sender = std::move(sender_opened).value();
    QVERIFY(sender->apply_local({{1, 1}}, {"b"}).ok());
    auto polled = receiver->poll();
    QVERIFY(polled.ok());
    QCOMPARE(polled.value().committed, size_t(2));
    QCOMPARE(nested_error, ErrorCode::reentrant_mutation);
    QCOMPARE(receiver->text(), std::string("ab"));
    QVERIFY(sender->close().ok());
    QVERIFY(receiver->close().ok());
  }

  void mutationPermitSerializesPublicationAgainstRetirement() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::filesystem::path root = tmp.path().toStdString();
    std::shared_ptr<MembershipCoordinator> coordinator;
    std::atomic<bool> block_next_write{false};
    std::mutex barrier_mutex;
    std::condition_variable barrier_cv;
    bool write_entered = false;
    bool allow_write = false;
    bool retirement_attempted = false;
    bool retired = false;
    bool retirement_waited_for_permit = false;
    bool retirement_commit_ok = false;

    Storage::StepHook hook;
    hook.observe = [&](Storage::Step step, const std::filesystem::path &) {
      if (step != Storage::Step::Write || !block_next_write.exchange(false)) return;
      std::unique_lock lock(barrier_mutex);
      write_entered = true;
      barrier_cv.notify_all();
      barrier_cv.wait(lock, [&] { return allow_write; });
    };
    Crdt::WriterConfig writer_config;
    writer_config.step_hook = &hook;
    DocumentSessionConfig config{root / "shared", root / "device",
                                 "doc-membership-race", "text", 8};
    config.authorize_local_mutation = [&](const DocumentId &, const StreamId &,
                                          uint64_t, const ActorId &producer)
      -> Result<std::shared_ptr<void>> {
      if (!coordinator)
        return Error(ErrorCode::awaiting_state,
                     "membership coordinator is not initialized");
      return coordinator->authorize_local_mutation(
          config.document, config.stream, config.epoch, producer);
    };

    auto opened = DocumentSessionTestAccess::open(config, writer_config);
    QVERIFY(opened.ok());
    auto session = std::move(opened).value();
    const auto producer = session->actor();
    const auto peer = Test::actor(222);
    auto coordinator_result = MembershipCoordinator::create({
        {config.document, config.stream, config.epoch, {producer, peer}}});
    QVERIFY(coordinator_result.ok());
    coordinator = std::move(coordinator_result).value();
    block_next_write = true;
    bool edit_published = false;
    std::thread editor([&] {
      edit_published = session->apply_local({{0, 0}}, {"x"}).ok();
    });
    bool saw_blocked_write = false;
    {
      std::unique_lock lock(barrier_mutex);
      saw_blocked_write = barrier_cv.wait_for(
          lock, std::chrono::seconds(5), [&] { return write_entered; });
    }

    std::thread retirer([&] {
      {
        std::lock_guard lock(barrier_mutex);
        retirement_attempted = true;
      }
      barrier_cv.notify_all();
      auto transition = coordinator->commit_transition(
          config.document, config.stream, config.epoch, config.epoch + 1,
          "session-race-retirement-v1",
          {peer}, [&]() -> Result<void> {
            std::lock_guard lock(barrier_mutex);
            retired = true;
            barrier_cv.notify_all();
            return {};
          });
      retirement_commit_ok = transition.ok();
    });
    {
      std::unique_lock lock(barrier_mutex);
      barrier_cv.wait_for(lock, std::chrono::seconds(5),
                          [&] { return retirement_attempted; });
      if (saw_blocked_write)
        retirement_waited_for_permit = !retired;
      allow_write = true;
    }
    barrier_cv.notify_all();
    editor.join();
    retirer.join();

    QVERIFY(saw_blocked_write);
    QVERIFY(retirement_waited_for_permit);
    QVERIFY(edit_published);
    QVERIFY(retired);
    QVERIFY(retirement_commit_ok);
    QCOMPARE(session->text(), std::string("x"));
    auto rejected = session->apply_local({{1, 1}}, {"y"});
    QVERIFY(!rejected.ok());
    QCOMPARE(rejected.error().code(), ErrorCode::wrong_epoch);
    QCOMPARE(session->text(), std::string("x"));
    QVERIFY(session->close().ok());
  }

  void membershipCoordinatorFailsClosedAndPublishesOnlyDurableTransitions() {
    const auto member_a = Test::actor(221);
    const auto member_b = Test::actor(222);
    const std::string transition_id = "coordinator-transition-7-8-v1";
    auto created = MembershipCoordinator::create(
        {{"coordinator-doc", "text", 7, {member_a, member_b}}});
    QVERIFY(created.ok());
    auto coordinator = std::move(created).value();

    auto local = coordinator->authorize_local_mutation(
        "coordinator-doc", "text", 7, member_a);
    QVERIFY(local.ok());
    QVERIFY(!coordinator->authorize_local_mutation(
        "coordinator-doc", "text", 8, member_a));
    QVERIFY(!coordinator->authorize_local_mutation(
        "coordinator-doc", "text", 7, Test::actor(223)));
    DeliveryToken token{"text", 7, member_a, 1};
    auto inbound = coordinator->authorize_inbound_delivery(
        "coordinator-doc", "text", 7, token);
    QVERIFY(inbound.ok());
    token.stream = "other";
    QVERIFY(!coordinator->authorize_inbound_delivery(
        "coordinator-doc", "text", 7, token));
    inbound.value().reset();
    local.value().reset();

    const auto rejected_transition = coordinator->commit_transition(
        "coordinator-doc", "text", 7, 8, transition_id, {member_b}, [] {
          return Result<void>(Error(ErrorCode::io_error,
                                    "simulated durable publication failure"));
        });
    QVERIFY(!rejected_transition.ok());
    QVERIFY(coordinator->authorize_local_mutation(
        "coordinator-doc", "text", 7, member_a));

    const auto uncertain_transition = coordinator->commit_transition(
        "coordinator-doc", "text", 7, 8, transition_id, {member_b}, [] {
          return Result<void>(Error(ErrorCode::commit_uncertain,
                                    "simulated uncertain durable publication"));
        });
    QVERIFY(!uncertain_transition.ok());
    auto blocked_actor = coordinator->authorize_local_mutation(
        "coordinator-doc", "text", 7, member_a);
    QVERIFY(!blocked_actor.ok());
    QCOMPARE(blocked_actor.error().code(), ErrorCode::commit_uncertain);
    auto conflicting_retry = coordinator->commit_transition(
        "coordinator-doc", "text", 7, 8, "different-transition-id", {member_b}, [] {
          return Result<void>{};
        });
    QVERIFY(!conflicting_retry.ok());
    QCOMPARE(conflicting_retry.error().code(), ErrorCode::wrong_epoch);

    ErrorCode nested_authorization_error = ErrorCode::malformed;
    const auto reentrant_transition = coordinator->commit_transition(
        "coordinator-doc", "text", 7, 8, transition_id, {member_b}, [&] {
          auto nested = coordinator->authorize_local_mutation(
              "coordinator-doc", "text", 7, member_a);
          if (nested.ok())
            return Result<void>(Error(ErrorCode::malformed,
                                      "reentrant authorization unexpectedly succeeded"));
          nested_authorization_error = nested.error().code();
          return Result<void>(nested.error());
        });
    QVERIFY(!reentrant_transition.ok());
    QCOMPARE(reentrant_transition.error().code(), ErrorCode::reentrant_mutation);
    QCOMPARE(nested_authorization_error, ErrorCode::reentrant_mutation);

    auto other_created = MembershipCoordinator::create(
        {{"other-doc", "text", 1, {member_a}}});
    QVERIFY(other_created.ok());
    auto other = std::move(other_created).value();
    auto cross_coordinator_reentrancy = coordinator->commit_transition(
        "coordinator-doc", "text", 7, 8, transition_id, {member_b}, [&] {
          return other->commit_transition("other-doc", "text", 1, 2,
              "other-transition-v1",
              {member_a}, [&]() -> Result<void> {
                auto nested = coordinator->authorize_local_mutation(
                    "coordinator-doc", "text", 7, member_a);
                if (nested.ok())
                  return Error(ErrorCode::malformed,
                               "cross-coordinator reentrancy unexpectedly succeeded");
                return nested.error();
              });
        });
    QVERIFY(!cross_coordinator_reentrancy.ok());
    QCOMPARE(cross_coordinator_reentrancy.error().code(),
             ErrorCode::reentrant_mutation);

    const auto throwing_retry = coordinator->commit_transition(
        "coordinator-doc", "text", 7, 8, transition_id, {member_b}, []() -> Result<void> {
          throw std::runtime_error("simulated transition callback interruption");
        });
    QVERIFY(!throwing_retry.ok());
    QCOMPARE(throwing_retry.error().code(), ErrorCode::io_error);
    auto still_blocked = coordinator->authorize_local_mutation(
        "coordinator-doc", "text", 7, member_a);
    QVERIFY(!still_blocked.ok());
    QCOMPARE(still_blocked.error().code(), ErrorCode::commit_uncertain);

    bool durable = false;
    QVERIFY(coordinator->commit_transition(
        "coordinator-doc", "text", 7, 8, transition_id, {member_b}, [&] {
          durable = true;
          return Result<void>{};
        }).ok());
    QVERIFY(durable);
    QVERIFY(!coordinator->authorize_local_mutation(
        "coordinator-doc", "text", 7, member_a));
    QVERIFY(!coordinator->authorize_inbound_delivery(
        "coordinator-doc", "text", 7,
        DeliveryToken{"text", 7, member_b, 1}));
    QVERIFY(coordinator->authorize_local_mutation(
        "coordinator-doc", "text", 8, member_b));
    QVERIFY(coordinator->authorize_inbound_delivery(
        "coordinator-doc", "text", 8,
        DeliveryToken{"text", 8, member_b, 1}));

    auto temporary_created = MembershipCoordinator::create(
        {{"temporary-doc", "text", 1, {member_a}}});
    QVERIFY(temporary_created.ok());
    auto temporary_coordinator = std::move(temporary_created).value();
    auto temporary_permit = temporary_coordinator->authorize_local_mutation(
        "temporary-doc", "text", 1, member_a);
    QVERIFY(temporary_permit.ok());
    auto lease_outliving_coordinator = std::move(temporary_permit).value();
    temporary_coordinator.reset();
    lease_outliving_coordinator.reset();
  }

  void membershipCoordinatorTransitionsMultipleScopesAsOneGate() {
    const auto member_a = Test::actor(224);
    const auto member_b = Test::actor(225);
    auto created = MembershipCoordinator::create({
        {"batch-doc", "text", 2, {member_a, member_b}},
        {"batch-doc", "comments", 2, {member_a, member_b}}});
    QVERIFY(created.ok());
    auto coordinator = std::move(created).value();
    auto in_flight = coordinator->authorize_local_mutation(
        "batch-doc", "comments", 2, member_a);
    QVERIFY(in_flight.ok());

    std::mutex mutex;
    std::condition_variable changed;
    bool attempted = false;
    std::atomic<bool> durable_callback_entered{false};
    bool committed = false;
    std::thread transition([&] {
      {
        std::lock_guard lock(mutex);
        attempted = true;
      }
      changed.notify_all();
      committed = coordinator->commit_transitions(
          "batch-doc-retirement-v2",
          {{"batch-doc", "comments", 2, 3, {member_a}},
           {"batch-doc", "text", 2, 3, {member_a}}},
          [&]() -> Result<void> {
            durable_callback_entered = true;
            return {};
          }).ok();
    });
    bool saw_attempted = false;
    {
      std::unique_lock lock(mutex);
      saw_attempted = changed.wait_for(lock, std::chrono::seconds(5),
                                       [&] { return attempted; });
    }
    const bool waited_for_lease = !durable_callback_entered.load();
    in_flight.value().reset();
    transition.join();

    QVERIFY(saw_attempted);
    QVERIFY(waited_for_lease);
    QVERIFY(committed);
    QVERIFY(durable_callback_entered.load());
    for (const auto &stream : {StreamId("text"), StreamId("comments")}) {
      QVERIFY(!coordinator->authorize_local_mutation(
          "batch-doc", stream, 2, member_a));
      QVERIFY(!coordinator->authorize_local_mutation(
          "batch-doc", stream, 3, member_b));
      QVERIFY(coordinator->authorize_local_mutation(
          "batch-doc", stream, 3, member_a));
    }
  }

  void uncertainMembershipBatchRequiresExactWholeBatchRetry() {
    const auto member_a = Test::actor(226);
    const auto member_b = Test::actor(227);
    auto created = MembershipCoordinator::create({
        {"batch-retry-doc", "text", 5, {member_a, member_b}},
        {"batch-retry-doc", "comments", 5, {member_a, member_b}}});
    QVERIFY(created.ok());
    auto coordinator = std::move(created).value();
    const std::vector<MembershipScopeTransition> batch{
        {"batch-retry-doc", "text", 5, 6, {member_a}},
        {"batch-retry-doc", "comments", 5, 6, {member_a}}};
    unsigned durable_calls = 0;
    auto uncertain = coordinator->commit_transitions(
        "cross-stream-retirement-v5-v1", batch, [&]() -> Result<void> {
          ++durable_calls;
          return Error(ErrorCode::commit_uncertain,
                       "checkpoint directory sync is uncertain");
        });
    QVERIFY(!uncertain.ok());
    QCOMPARE(durable_calls, 1u);
    for (const auto &stream : {StreamId("text"), StreamId("comments")}) {
      auto blocked = coordinator->authorize_local_mutation(
          "batch-retry-doc", stream, 5, member_a);
      QVERIFY(!blocked.ok());
      QCOMPARE(blocked.error().code(), ErrorCode::commit_uncertain);
    }

    auto partial_retry = coordinator->commit_transitions(
        "cross-stream-retirement-v5-v1", {batch.front()}, [&] {
          ++durable_calls;
          return Result<void>{};
        });
    QVERIFY(!partial_retry.ok());
    QCOMPARE(partial_retry.error().code(), ErrorCode::wrong_epoch);
    QCOMPARE(durable_calls, 1u);

    auto exact_retry = coordinator->commit_transitions(
        "cross-stream-retirement-v5-v1", batch, [&] {
          ++durable_calls;
          return Result<void>{};
        });
    QVERIFY(exact_retry.ok());
    QCOMPARE(durable_calls, 2u);
    for (const auto &stream : {StreamId("text"), StreamId("comments")}) {
      QVERIFY(coordinator->authorize_local_mutation(
          "batch-retry-doc", stream, 6, member_a));
      QVERIFY(!coordinator->authorize_local_mutation(
          "batch-retry-doc", stream, 5, member_a));
    }
  }

  void external_token_roundtrip_reopen_durable_file_replay() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::filesystem::path root = tmp.path().toStdString();
    const std::string document = "durable-position-document";
    const StreamScope stream{"text", 1};
    const PositionScope position_scope{document, stream.stream, Primitive::text, stream.epoch};
    const auto seed_op = Crdt::op_for_seed("ab");
    const auto actor_a = Test::actor(101);
    const auto actor_b = Test::actor(102);

    std::string left_bytes, right_bytes;
    {
      auto opened_engine = CrdtEngine::create(Test::test_counter(101), position_scope);
      QVERIFY(opened_engine.ok());
      auto engine = std::move(opened_engine).value();
      QVERIFY(CollabText::Test::apply_succeeded(engine->apply_remote(seed_op)));
      auto left = engine->boundary_at(1, Affinity::left);
      auto right = engine->boundary_at(1, Affinity::right);
      QVERIFY(left.ok()); QVERIFY(right.ok());
      left_bytes = checked_codec(encode_position(left.value()));
      right_bytes = checked_codec(encode_position(right.value()));

      Crdt::FileOpStream writer(root, actor_a);
      QVERIFY(writer.start().ok());
      QVERIFY(writer.register_stream(stream).ok());
      uint64_t sequence = 1;
      std::optional<Error> write_error;
      engine->setOnLocalOp([&](const Crdt::Operation &op) {
        auto encoded = Crdt::encode_operation(op);
        if (!encoded.ok()) { write_error = encoded.error(); return; }
        OutboundDelivery delivery{{stream.stream, stream.epoch, actor_a, sequence++},
                                  encoded.value()};
        auto pushed = writer.push(delivery, PushDurability::durable);
        if (!pushed.ok()) write_error = pushed.error();
      });
      engine->insert(1, "X");
      QVERIFY(!write_error.has_value());
      QVERIFY(writer.close().ok());
    }
    {
      auto opened_engine = CrdtEngine::create(Test::test_counter(102), position_scope);
      QVERIFY(opened_engine.ok());
      auto engine = std::move(opened_engine).value();
      QVERIFY(CollabText::Test::apply_succeeded(engine->apply_remote(seed_op)));
      Crdt::FileOpStream writer(root, actor_b);
      QVERIFY(writer.start().ok());
      QVERIFY(writer.register_stream(stream).ok());
      std::optional<Error> write_error;
      engine->setOnLocalOp([&](const Crdt::Operation &op) {
        auto encoded = Crdt::encode_operation(op);
        if (!encoded.ok()) { write_error = encoded.error(); return; }
        OutboundDelivery delivery{{stream.stream, stream.epoch, actor_b, 1},
                                  encoded.value()};
        auto pushed = writer.push(delivery, PushDurability::durable);
        if (!pushed.ok()) write_error = pushed.error();
      });
      engine->insert(1, "Y");
      QVERIFY(!write_error.has_value());
      QVERIFY(writer.close().ok());
    }

    auto recover_replica = [&](uint8_t local_actor) -> Result<std::unique_ptr<CrdtEngine>> {
      auto engine = CrdtEngine::create(Test::test_counter(local_actor), position_scope);
      if (!engine.ok()) return engine.error();
      if (!CollabText::Test::apply_succeeded(engine.value()->apply_remote(seed_op)))
        return Error(ErrorCode::malformed, "deterministic seed was not accepted");
      Crdt::FileOpStream reader(root, Test::actor(local_actor));
      auto started = reader.start(); if (!started.ok()) return started.error();
      auto registered = reader.register_stream(stream); if (!registered.ok()) return registered.error();
      auto replayed = reader.replay_all(stream, [&](const InboundDelivery &delivery) -> Result<void> {
        auto op = Crdt::decode_operation(delivery.payload);
        if (!op.ok()) return op.error();
        if (!CollabText::Test::apply_succeeded(engine.value()->apply_remote(op.value())))
          return Error(ErrorCode::malformed, "durable operation replay failed");
        return {};
      });
      if (!replayed.ok()) return replayed.error();
      auto closed = reader.close(); if (!closed.ok()) return closed.error();
      return std::move(engine).value();
    };

    auto recovered_a = recover_replica(103);
    auto recovered_b = recover_replica(104);
    QVERIFY(recovered_a.ok()); QVERIFY(recovered_b.ok());
    QCOMPARE(recovered_a.value()->text(), std::string("aXYb"));
    QCOMPARE(recovered_b.value()->text(), recovered_a.value()->text());
    for (const auto *recovered : {recovered_a.value().get(), recovered_b.value().get()}) {
      auto left = decode_position_as<TextBoundary>(left_bytes);
      auto right = decode_position_as<TextBoundary>(right_bytes);
      QVERIFY(left.ok()); QVERIFY(right.ok());
      auto left_result = recovered->resolve(left.value());
      auto right_result = recovered->resolve(right.value());
      QVERIFY(left_result.ok()); QVERIFY(right_result.ok());
      QCOMPARE(left_result.value().byte_offset, uint32_t(1));
      QCOMPARE(right_result.value().byte_offset, uint32_t(3));
      QCOMPARE(left_result.value().collapsed(), false);
      QCOMPARE(right_result.value().collapsed(), false);
    }
  }

  void wp6_split_pastes_survive_durable_replay_with_original_tokens() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::filesystem::path root = tmp.path().toStdString();
    const std::string document = "wp6-durable-boundary-document";
    const StreamScope stream{"text", 1};
    const PositionScope position_scope{document, stream.stream, Primitive::text, stream.epoch};
    const auto actor_a = Test::actor(105);
    const auto actor_b = Test::actor(106);
    std::string left_bytes, right_bytes;

    {
      auto created = CrdtEngine::create(Test::test_counter(105), position_scope);
      QVERIFY(created.ok());
      auto engine = std::move(created).value();
      auto left = engine->boundary_at(0, Affinity::left);
      auto right = engine->boundary_at(0, Affinity::right);
      QVERIFY(left.ok()); QVERIFY(right.ok());
      left_bytes = checked_codec(encode_position(left.value()));
      right_bytes = checked_codec(encode_position(right.value()));
    }

    auto persist_paste = [&](ActorId actor, uint8_t actor_index,
                             const std::string &passage) -> Result<void> {
      auto created = CrdtEngine::create(Test::test_counter(actor_index), position_scope);
      if (!created.ok()) return created.error();
      auto engine = std::move(created).value();
      Crdt::FileOpStream writer(root, actor);
      auto started = writer.start(); if (!started.ok()) return started.error();
      auto registered = writer.register_stream(stream);
      if (!registered.ok()) return registered.error();
      std::optional<Error> write_error;
      engine->setOnLocalOp([&](const Crdt::Operation &op) {
        auto encoded = Crdt::encode_operation(op);
        if (!encoded.ok()) { write_error = encoded.error(); return; }
        OutboundDelivery delivery{{stream.stream, stream.epoch, actor, 1}, encoded.value()};
        auto pushed = writer.push(delivery, PushDurability::durable);
        if (!pushed.ok()) write_error = pushed.error();
      });
      engine->insert(0, passage);
      if (write_error) return *write_error;
      return writer.close();
    };
    auto persisted_a = persist_paste(actor_a, 105, "abc");
    auto persisted_b = persist_paste(actor_b, 106, "XYZ");
    QVERIFY(persisted_a.ok()); QVERIFY(persisted_b.ok());

    auto created = CrdtEngine::create(Test::test_counter(107), position_scope);
    QVERIFY(created.ok());
    auto recovered = std::move(created).value();
    Crdt::FileOpStream reader(root, Test::actor(107));
    QVERIFY(reader.start().ok());
    QVERIFY(reader.register_stream(stream).ok());
    size_t applied = 0;
    auto replayed = reader.replay_all(stream, [&](const InboundDelivery &delivery) -> Result<void> {
      auto op = Crdt::decode_operation(delivery.payload);
      if (!op.ok()) return op.error();
      if (!CollabText::Test::apply_succeeded(recovered->apply_remote(op.value())))
        return Error(ErrorCode::malformed, "WP6 durable passage operation was not integrated");
      ++applied;
      return {};
    });
    QVERIFY(replayed.ok());
    QCOMPARE(applied, size_t(2));
    QCOMPARE(recovered->text(), std::string("abcXYZ"));
    QVERIFY(reader.close().ok());

    auto left = decode_position_as<TextBoundary>(left_bytes);
    auto right = decode_position_as<TextBoundary>(right_bytes);
    QVERIFY(left.ok()); QVERIFY(right.ok());
    auto left_result = recovered->resolve(left.value());
    auto right_result = recovered->resolve(right.value());
    QVERIFY(left_result.ok()); QVERIFY(right_result.ok());
    QCOMPARE(left_result.value().byte_offset, uint32_t(0));
    QCOMPARE(right_result.value().byte_offset, uint32_t(6));
  }

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
      QVERIFY(deltas.ok());
      QCOMPARE(deltas.value().size(), size_t(1));
      QCOMPARE(deltas.value()[0].new_text, std::string("hello"));
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

  void localBranchExportKeepsOriginalScopeAndExcludesPeerOperations() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const auto root = std::filesystem::path(tmp.path().toStdString());
    DocumentSessionConfig local_config{root / "shared", root / "local",
                                       "doc-branch", "text", 3};
    local_config.initial_text = "seed";
    DocumentSessionConfig peer_config{root / "shared", root / "peer",
                                      "doc-branch", "text", 3};
    peer_config.initial_text = "seed";

    auto local_opened = DocumentSession::open(local_config);
    QVERIFY(local_opened.ok());
    auto local = std::move(local_opened).value();
    const auto local_actor = local->actor();
    auto local_edit = local->apply_local({{4, 4}}, {" local"});
    QVERIFY(local_edit.ok());

    auto peer_opened = DocumentSession::open(peer_config);
    QVERIFY(peer_opened.ok());
    auto peer = std::move(peer_opened).value();
    QCOMPARE(peer->text(), std::string("seed local"));
    auto peer_edit = peer->apply_local({{10, 10}}, {" peer"});
    QVERIFY(peer_edit.ok());
    QVERIFY(peer->close().ok());
    auto polled = local->poll();
    QVERIFY(polled.ok());
    QCOMPARE(local->text(), std::string("seed local peer"));

    LocalBranchExport export_metadata;
    std::vector<OutboundDelivery> exported_deliveries;
    auto exported = local->export_local_branch(export_metadata,
        [&](const OutboundDelivery& delivery) -> Result<void> {
          exported_deliveries.push_back(delivery);
          return {};
        });
    QVERIFY(exported.ok());
    QCOMPARE(export_metadata.document, local_config.document);
    QCOMPARE(export_metadata.scope.stream, local_config.stream);
    QCOMPARE(export_metadata.scope.epoch, uint64_t(3));
    QCOMPARE(export_metadata.producer, local_actor);
    QCOMPARE(exported_deliveries.size(), size_t(1));
    const auto& delivery = exported_deliveries[0];
    QCOMPARE(delivery.token.stream, local_config.stream);
    QCOMPARE(delivery.token.epoch, uint64_t(3));
    QCOMPARE(delivery.token.producer, local_actor);
    QCOMPARE(delivery.token.sequence, uint64_t(1));
    QCOMPARE(delivery.payload, Crdt::encode_operation(local_edit.value()).value());
    auto decoded = Crdt::decode_operation(delivery.payload);
    QVERIFY(decoded.ok());
    QCOMPARE(Crdt::op_identity(decoded.value()).actor, local_actor);
    QVERIFY(local->close().ok());
  }

  void retiredReturningBranchStaysExportableButCannotResumeOldEpoch() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const auto root = std::filesystem::path(tmp.path().toStdString());
    DocumentSessionConfig old_config{root / "shared", root / "returning-device",
                                     "doc-returning-branch", "text", 4};
    old_config.initial_text = "base";
    auto original_opened = DocumentSession::open(old_config);
    QVERIFY(original_opened.ok());
    auto original = std::move(original_opened).value();
    const auto returning_actor = original->actor();
    auto local = original->apply_local({{4, 4}}, {" offline"});
    QVERIFY(local.ok());
    auto local_bytes = Crdt::encode_operation(local.value());
    QVERIFY(local_bytes.ok());
    QVERIFY(original->close().ok());

    const auto remaining_actor = Test::actor(231);
    auto coordinator_result = MembershipCoordinator::create({
        {old_config.document, old_config.stream, 5, {remaining_actor}}});
    QVERIFY(coordinator_result.ok());
    auto coordinator = std::move(coordinator_result).value();
    old_config.authorize_local_mutation =
        [coordinator](const DocumentId &document, const StreamId &stream,
                      uint64_t epoch, const ActorId &producer) {
          return coordinator->authorize_local_mutation(
              document, stream, epoch, producer);
        };
    old_config.authorize_inbound_delivery =
        [coordinator](const DocumentId &document, const StreamId &stream,
                      uint64_t epoch, const DeliveryToken &token) {
          return coordinator->authorize_inbound_delivery(
              document, stream, epoch, token);
        };
    auto returning_opened = DocumentSession::open(old_config);
    const std::string open_error = returning_opened.ok()
        ? std::string{} : std::string(returning_opened.error().message());
    QVERIFY2(returning_opened.ok(), open_error.c_str());
    auto returning = std::move(returning_opened).value();
    QVERIFY(!returning->ready());
    auto refused_edit = returning->apply_local({{4, 4}}, {"!"});
    QVERIFY(!refused_edit.ok());

    auto quarantined = returning->quarantined_deliveries();
    QVERIFY(quarantined.ok());
    QCOMPARE(quarantined.value().size(), size_t(1));
    QCOMPARE(quarantined.value().front().delivery.token.epoch, uint64_t(4));
    QCOMPARE(quarantined.value().front().delivery.token.producer, returning_actor);

    LocalBranchExport metadata;
    std::vector<OutboundDelivery> exported;
    auto export_result = returning->export_local_branch(
        metadata, [&](const OutboundDelivery &delivery) -> Result<void> {
          exported.push_back(delivery);
          return {};
        });
    QVERIFY(export_result.ok());
    QCOMPARE(metadata.document, old_config.document);
    QCOMPARE(metadata.scope.stream, old_config.stream);
    QCOMPARE(metadata.scope.epoch, uint64_t(4));
    QCOMPARE(metadata.producer, returning_actor);
    QCOMPARE(exported.size(), size_t(1));
    QCOMPARE(exported.front().token.epoch, uint64_t(4));
    QCOMPARE(exported.front().token.producer, returning_actor);
    QCOMPARE(exported.front().payload, local_bytes.value());
    QVERIFY(returning->close().ok());

    old_config.max_quarantined_deliveries = 0;
    auto bounded_opened = DocumentSession::open(old_config);
    QVERIFY(bounded_opened.ok());
    auto bounded = std::move(bounded_opened).value();
    QVERIFY(!bounded->ready());
    auto incomplete_snapshot = bounded->quarantined_deliveries();
    QVERIFY(!incomplete_snapshot.ok());
    QCOMPARE(incomplete_snapshot.error().code(), ErrorCode::limit_exceeded);
    std::vector<OutboundDelivery> bounded_export;
    QVERIFY(bounded->export_local_branch(metadata,
        [&](const OutboundDelivery &delivery) -> Result<void> {
          bounded_export.push_back(delivery);
          return {};
        }).ok());
    QCOMPARE(bounded_export.size(), size_t(1));
    QCOMPARE(bounded_export.front().payload, local_bytes.value());
    QVERIFY(bounded->close().ok());
  }

  void localBranchExportVisitorFailureLeavesSourceRetryable() {
    QTemporaryDir tmp; QVERIFY(tmp.isValid());
    const auto root = std::filesystem::path(tmp.path().toStdString());
    DocumentSessionConfig config{root / "shared", root / "device",
                                 "doc-branch-retry", "text", 1};
    config.initial_text = "seed";
    auto opened = DocumentSession::open(config); QVERIFY(opened.ok());
    auto session = std::move(opened).value();
    QVERIFY(session->apply_local({{4, 4}}, {"!"}).ok());
    QVERIFY(session->apply_local({{5, 5}}, {"?"}).ok());

    LocalBranchExport metadata;
    size_t failed_visits = 0;
    auto failed = session->export_local_branch(metadata,
        [&](const OutboundDelivery&) -> Result<void> {
          ++failed_visits;
          return Error(ErrorCode::io_error, "staging destination failed");
        });
    QVERIFY(!failed.ok()); QCOMPARE(failed.error().code(), ErrorCode::io_error);
    QCOMPARE(failed_visits, size_t(1));

    auto threw = session->export_local_branch(metadata,
        [](const OutboundDelivery&) -> Result<void> {
          throw std::runtime_error("staging callback threw");
        });
    QVERIFY(!threw.ok()); QCOMPARE(threw.error().code(), ErrorCode::io_error);

    std::vector<OutboundDelivery> retry;
    auto retried = session->export_local_branch(metadata,
        [&](const OutboundDelivery& delivery) -> Result<void> {
          retry.push_back(delivery);
          return {};
        });
    QVERIFY(retried.ok()); QCOMPARE(retry.size(), size_t(2));
    QCOMPARE(retry[0].token.sequence, uint64_t(1));
    QCOMPARE(retry[1].token.sequence, uint64_t(2));
    QVERIFY(session->close().ok());
  }

  void localBranchExportIncludesPendingIntentWithoutWrittenSegment() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const auto root = std::filesystem::path(tmp.path().toStdString());
    const DocumentSessionConfig config{root / "shared", root / "device",
                                       "doc-pending-branch", "text", 2};
    bool failed_write = false;
    Storage::StepHook hook;
    hook.fail = [&](Storage::Step step, const std::filesystem::path &path)
        -> std::optional<Error> {
      if (!failed_write && step == Storage::Step::Write &&
          path.extension() == ".open") {
        failed_write = true;
        return Error(ErrorCode::io_error, "injected branch segment write failure");
      }
      return std::nullopt;
    };
    Crdt::WriterConfig writer_config;
    writer_config.step_hook = &hook;
    auto opened = DocumentSessionTestAccess::open(config, writer_config);
    QVERIFY(opened.ok());
    auto session = std::move(opened).value();
    const auto local_actor = session->actor();
    auto failed = session->apply_local({{0, 0}}, {"pending"});
    QVERIFY(!failed.ok());
    QVERIFY(failed_write);

    LocalBranchExport metadata;
    std::vector<OutboundDelivery> deliveries;
    auto exported = session->export_local_branch(metadata,
        [&](const OutboundDelivery &delivery) -> Result<void> {
          deliveries.push_back(delivery);
          return {};
        });
    QVERIFY(exported.ok());
    QCOMPARE(metadata.document, config.document);
    QCOMPARE(metadata.scope.epoch, config.epoch);
    QCOMPARE(metadata.producer, local_actor);
    QCOMPARE(deliveries.size(), size_t(1));
    QCOMPARE(deliveries[0].token.producer, local_actor);
    QCOMPARE(deliveries[0].token.sequence, uint64_t(1));
    auto decoded = Crdt::decode_operation(deliveries[0].payload);
    QVERIFY(decoded.ok());
    QCOMPARE(Crdt::op_identity(decoded.value()).actor, local_actor);
    // The injected segment writer failure leaves this session poisoned; the
    // exported operation is recoverable from its durable local intent.
  }

  void edits_since_reports_history_floor_and_future_frontier() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const auto root = std::filesystem::path(tmp.path().toStdString());
    DocumentSessionConfig config{root / "shared", root / "device", "doc-floor",
                                 "stream-floor", 1};
    config.initial_text = "seed";
    auto opened = DocumentSession::open(config);
    QVERIFY(opened.ok());
    auto session = std::move(opened).value();
    Crdt::Global before_seed;
    auto unavailable = session->edits_since(before_seed);
    QVERIFY(!unavailable.ok());
    QCOMPARE(unavailable.error().code(), ErrorCode::history_unavailable);

    auto ahead = session->recovery_frontier();
    ahead.observe({Test::actor(123), 1});
    auto waiting = session->edits_since(ahead);
    QVERIFY(!waiting.ok());
    QCOMPARE(waiting.error().code(), ErrorCode::awaiting_state);
    QVERIFY(session->close().ok());
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

  void session_coalesced_group_metadata_survives_reopen() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::filesystem::path root = tmp.path().toStdString();
    const DocumentSessionConfig config{root / "shared", root / "device",
                                       "doc-coalesce", "text-coalesce", 1};
    Crdt::Identity stable_group_id;
    std::vector<Crdt::Identity> members;
    {
      auto opened = DocumentSession::open(config);
      QVERIFY(opened.ok());
      auto session = std::move(opened).value();
      QVERIFY(session->apply_local({{0, 0}}, {"a"}).ok());
      const auto first_group = session->next_undo_group();
      QVERIFY(first_group.has_value());
      stable_group_id = first_group->id;
      QVERIFY(session->apply_local({{1, 1}}, {"b"}).ok());
      QVERIFY(session->coalesce_last_undo().ok());
      auto after = session->next_undo_group();
      QVERIFY(after.has_value());
      QCOMPARE(after->operations.size(), size_t(2));
      QCOMPARE(after->id, stable_group_id);
      members = after->operations;
      QVERIFY(session->close().ok());
    }
    auto reopened = DocumentSession::open(config);
    QVERIFY(reopened.ok());
    auto session = std::move(reopened).value();
    const auto group = session->next_undo_group();
    QVERIFY(group.has_value());
    QCOMPARE(group->id, stable_group_id);
    QCOMPARE(group->operations, members);
    QVERIFY(session->undo().ok());
    QCOMPARE(session->text(), std::string{});
    QVERIFY(session->close().ok());
  }

  void session_replays_pending_retirement_until_consumer_acknowledges_it() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::filesystem::path root = tmp.path().toStdString();
    DocumentSessionConfig config{root / "shared", root / "device",
                                 "doc-retirement", "text-retirement", 1};
    config.max_undo_steps = 1;
    const auto retirement_log = root / "consumer" / "retirements.log";
    Crdt::Identity retired_group;
    {
      auto opened = DocumentSession::open(config);
      QVERIFY(opened.ok());
      auto session = std::move(opened).value();
      QVERIFY(session->apply_local({{0, 0}}, {"a"}).ok());
      const auto original_group = session->next_undo_group();
      QVERIFY(original_group.has_value());
      retired_group = original_group->id;
      QVERIFY(session->apply_local({{1, 1}}, {"b"}).ok());
      const auto pending = session->pending_undo_retirements();
      QCOMPARE(pending.size(), size_t(1));
      QCOMPARE(pending[0].group_id, retired_group);
      QVERIFY(append_durable_line(
                  retirement_log, "retired:" + retired_group.actor.to_string() +
                                      "/" + std::to_string(retired_group.counter))
                  .ok());
      // Simulate restart after external publication but before local ack.
      QVERIFY(session->close().ok());
    }
    {
      auto reopened = DocumentSession::open(config);
      QVERIFY(reopened.ok());
      auto session = std::move(reopened).value();
      const auto retry = session->pending_undo_retirements();
      QCOMPARE(retry.size(), size_t(1));
      QCOMPARE(retry[0].group_id, retired_group);
      std::ifstream events(retirement_log);
      std::string row;
      QVERIFY(static_cast<bool>(std::getline(events, row)));
      QVERIFY(session->acknowledge_undo_retirements({retired_group}).ok());
      QVERIFY(session->pending_undo_retirements().empty());
      QVERIFY(session->close().ok());
    }
    auto reopened = DocumentSession::open(config);
    QVERIFY(reopened.ok());
    auto session = std::move(reopened).value();
    QVERIFY(session->pending_undo_retirements().empty());
    QVERIFY(session->undo().ok());
    QCOMPARE(session->text(), std::string("a"));
    QVERIFY(!session->undo().ok());
    QVERIFY(session->close().ok());
    std::ifstream events(retirement_log);
    std::string row, extra;
    QVERIFY(static_cast<bool>(std::getline(events, row)));
    QVERIFY(!static_cast<bool>(std::getline(events, extra)));
  }

  void retentionPublicationAndSessionHistoryRecoverAcrossUncertainSync() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::filesystem::path root = tmp.path().toStdString();
    DocumentSessionConfig config{root / "shared", root / "device",
                                 "doc-retention-integration", "text", 1};
    config.max_undo_steps = 1;
    const auto retention_path = root / "device" / "retention.log";
    Crdt::Identity group;
    unsigned sync_failures_remaining = 2;
    Storage::StepHook hook;
    hook.fail = [&](Storage::Step step, const std::filesystem::path &)
        -> std::optional<Error> {
      if (step == Storage::Step::FileSync && sync_failures_remaining > 0) {
        --sync_failures_remaining;
        return Error(ErrorCode::io_error, "injected retention publication sync failure");
      }
      return std::nullopt;
    };

    {
      auto opened = DocumentSession::open(config);
      QVERIFY(opened.ok());
      auto session = std::move(opened).value();
      QVERIFY(session->apply_local({{0, 0}}, {"a"}).ok());
      const auto undo_group = session->next_undo_group();
      QVERIFY(undo_group.has_value());
      group = undo_group->id;
      QVERIFY(session->apply_local({{1, 1}}, {"b"}).ok());
      QCOMPARE(session->pending_undo_retirements().size(), size_t(1));

      auto retention_open = Storage::RetentionState::open(
          retention_path, session->actor(), &hook);
      QVERIFY(retention_open.ok());
      auto retention = std::move(retention_open).value();
      auto uncertain = retention.publish_after_durable_trim(
          config.stream, {group}, [&]() { return session->save(); });
      QVERIFY(!uncertain.ok());
      // The journal recovered the complete line, but the failed durability
      // barrier means session history must keep advertising the retirement.
      QVERIFY(retention.is_retired(config.stream, group));
      QCOMPARE(session->pending_undo_retirements().size(), size_t(1));
      QVERIFY(session->close().ok());
    }

    {
      auto reopened = DocumentSession::open(config);
      QVERIFY(reopened.ok());
      auto session = std::move(reopened).value();
      const auto pending = session->pending_undo_retirements();
      QCOMPARE(pending.size(), size_t(1));
      QCOMPARE(pending[0].group_id, group);

      auto retention_open = Storage::RetentionState::open(
          retention_path, session->actor(), &hook);
      QVERIFY(retention_open.ok());
      auto retention = std::move(retention_open).value();
      QVERIFY(retention.is_retired(config.stream, group));
      // Retry must establish a fresh sync barrier even though recovery found
      // the complete line. A second injected failure must leave local history
      // pending; only a later successful retry can acknowledge it.
      auto retry = retention.publish_after_durable_trim(
          config.stream, {group}, [&]() { return session->save(); });
      QVERIFY(!retry.ok());
      QCOMPARE(session->pending_undo_retirements().size(), size_t(1));
      QVERIFY(retention.publish_after_durable_trim(
          config.stream, {group}, [&]() { return session->save(); }).ok());
      QVERIFY(session->acknowledge_undo_retirements({group}).ok());
      QVERIFY(session->pending_undo_retirements().empty());
      QVERIFY(session->close().ok());
    }

    auto reopened = DocumentSession::open(config);
    QVERIFY(reopened.ok());
    auto session = std::move(reopened).value();
    QVERIFY(session->pending_undo_retirements().empty());
    QVERIFY(session->undo().ok());
    QCOMPARE(session->text(), std::string("a"));
    QVERIFY(session->close().ok());
    auto retention = Storage::RetentionState::open(retention_path, session->actor());
    QVERIFY(retention.ok());
    QVERIFY(retention.value().is_retired(config.stream, group));
  }

  void multiStreamRetirementPublishesAllHistoryBeforeMembership() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const auto root = std::filesystem::path(tmp.path().toStdString());
    DocumentSessionConfig text_config{root / "shared", root / "device",
                                      "doc-multistream-retirement", "text", 1};
    DocumentSessionConfig comments_config{root / "shared", root / "device",
                                          text_config.document, "comments", 1};
    text_config.max_undo_steps = comments_config.max_undo_steps = 1;

    ActorId owner;
    {
      auto seeded = DocumentSession::open(text_config);
      QVERIFY(seeded.ok());
      auto session = std::move(seeded).value();
      owner = session->actor();
      QVERIFY(session->close().ok());
    }
    {
      auto seeded = DocumentSession::open(comments_config);
      QVERIFY(seeded.ok());
      auto session = std::move(seeded).value();
      QCOMPARE(session->actor(), owner);
      QVERIFY(session->close().ok());
    }
    const auto retired_device = Test::actor(232);
    auto coordinator_result = MembershipCoordinator::create({
        {text_config.document, text_config.stream, 1, {owner, retired_device}},
        {comments_config.document, comments_config.stream, 1, {owner, retired_device}}});
    QVERIFY(coordinator_result.ok());
    auto coordinator = std::move(coordinator_result).value();
    const auto configure_membership = [coordinator](DocumentSessionConfig &config) {
      config.authorize_local_mutation =
          [coordinator](const DocumentId &document, const StreamId &stream,
                        uint64_t epoch, const ActorId &actor) {
            return coordinator->authorize_local_mutation(
                document, stream, epoch, actor);
          };
      config.authorize_inbound_delivery =
          [coordinator](const DocumentId &document, const StreamId &stream,
                        uint64_t epoch, const DeliveryToken &token) {
            return coordinator->authorize_inbound_delivery(
                document, stream, epoch, token);
          };
    };
    configure_membership(text_config);
    configure_membership(comments_config);

    Crdt::Identity text_group, comments_group;
    {
      auto opened = DocumentSession::open(text_config);
      QVERIFY(opened.ok());
      auto session = std::move(opened).value();
      QCOMPARE(session->actor(), owner);
      QVERIFY(session->apply_local({{0, 0}}, {"a"}).ok());
      QVERIFY(session->apply_local({{1, 1}}, {"b"}).ok());
      QCOMPARE(session->pending_undo_retirements().size(), size_t(1));
      text_group = session->pending_undo_retirements().front().group_id;
      QVERIFY(session->close().ok());
    }
    {
      auto opened = DocumentSession::open(comments_config);
      QVERIFY(opened.ok());
      auto session = std::move(opened).value();
      QCOMPARE(session->actor(), owner);
      QVERIFY(session->apply_local({{0, 0}}, {"c"}).ok());
      QVERIFY(session->apply_local({{1, 1}}, {"d"}).ok());
      QCOMPARE(session->pending_undo_retirements().size(), size_t(1));
      comments_group = session->pending_undo_retirements().front().group_id;
      QVERIFY(session->close().ok());
    }

    const auto save_both_stream_histories = [&]() -> Result<void> {
      auto text_opened = DocumentSession::open(text_config);
      if (!text_opened.ok()) return text_opened.error();
      auto text_session = std::move(text_opened).value();
      auto text_saved = text_session->save();
      if (!text_saved.ok()) return text_saved.error();
      auto text_closed = text_session->close();
      if (!text_closed.ok()) return text_closed.error();

      auto comments_opened = DocumentSession::open(comments_config);
      if (!comments_opened.ok()) return comments_opened.error();
      auto comments_session = std::move(comments_opened).value();
      auto comments_saved = comments_session->save();
      if (!comments_saved.ok()) return comments_saved.error();
      return comments_session->close();
    };

    Storage::MembershipDescriptor descriptor;
    descriptor.epoch = 1;
    descriptor.base_checkpoint_hash = sha256_hex("multistream-base-checkpoint");
    descriptor.members = {owner, retired_device};
    std::sort(descriptor.members.begin(), descriptor.members.end());
    for (const auto &stream : {comments_config.stream, text_config.stream}) {
      Storage::MembershipDescriptor::StreamFrontier frontier;
      frontier.stream = stream;
      for (const auto &member : descriptor.members) {
        frontier.operation.emplace(member, 0);
        frontier.delivery.emplace(member, 0);
      }
      descriptor.base_frontiers.push_back(std::move(frontier));
    }
    descriptor.supported_token_versions = {1};
    descriptor.translation_manifest_hash = sha256_hex("multistream-token-manifest");
    auto sealed = Storage::seal_membership_descriptor(std::move(descriptor));
    QVERIFY(sealed.ok());
    descriptor = std::move(sealed).value();

    const std::vector<Storage::RetirementImpact> impacts{
        {text_config.document, comments_config.stream, 1, 2, 1,
         sha256_hex("comments-branch")},
        {text_config.document, text_config.stream, 1, 2, 1,
         sha256_hex("text-branch")}};
    auto preview = Storage::preview_retirement(descriptor, retired_device, impacts);
    QVERIFY(preview.ok());
    const std::vector<Storage::MembershipAcknowledgement> acknowledgements{
        {owner, "durable-retirement-ack"}};
    const Storage::DurableAcknowledgementVerifier verify_ack =
        [owner, digest = preview.value().digest](
            const ActorId &actor, std::string_view supplied_digest,
            std::string_view signature) -> Result<void> {
      if (actor != owner || supplied_digest != digest ||
          signature != "durable-retirement-ack")
        return Error(ErrorCode::corrupt_record,
                     "multi-stream retirement acknowledgement does not match");
      return {};
    };
    auto membership_opened = Storage::MembershipStore::open(
        root / "device" / "membership.log", owner);
    QVERIFY(membership_opened.ok());
    auto membership = std::move(membership_opened).value();

    const std::vector<Storage::StreamUndoRetirement> retirements{
        {comments_config.stream, {comments_group}},
        {text_config.stream, {text_group}}};
    const auto retention_path = root / "device" / "retention.log";
    unsigned writes = 0;
    Storage::StepHook failure;
    failure.fail = [&](Storage::Step step, const std::filesystem::path &)
        -> std::optional<Error> {
      if (step == Storage::Step::Write && ++writes == 2)
        return Error(ErrorCode::io_error,
                     "injected failure before second stream retirement record");
      return std::nullopt;
    };
    {
      auto retention_opened = Storage::RetentionState::open(
          retention_path, owner, &failure);
      QVERIFY(retention_opened.ok());
      auto retention = std::move(retention_opened).value();
      auto partial = retention.publish_after_durable_trim(
          retirements, save_both_stream_histories);
      QVERIFY(!partial.ok());
      QVERIFY(retention.is_retired(comments_config.stream, comments_group));
      QVERIFY(!retention.is_retired(text_config.stream, text_group));
      auto before_membership = membership.recover();
      QVERIFY(before_membership.ok());
      QVERIFY(before_membership.value().empty());
      QVERIFY(coordinator->authorize_local_mutation(
          text_config.document, text_config.stream, 1, retired_device));
      QVERIFY(coordinator->authorize_local_mutation(
          comments_config.document, comments_config.stream, 1, retired_device));
    }

    {
      auto opened = DocumentSession::open(text_config);
      QVERIFY(opened.ok());
      auto session = std::move(opened).value();
      QCOMPARE(session->pending_undo_retirements().size(), size_t(1));
      QCOMPARE(session->pending_undo_retirements().front().group_id, text_group);
      QVERIFY(session->close().ok());
    }
    {
      auto opened = DocumentSession::open(comments_config);
      QVERIFY(opened.ok());
      auto session = std::move(opened).value();
      QCOMPARE(session->pending_undo_retirements().size(), size_t(1));
      QCOMPARE(session->pending_undo_retirements().front().group_id, comments_group);
      QVERIFY(session->close().ok());
    }

    auto retention_opened = Storage::RetentionState::open(retention_path, owner);
    QVERIFY(retention_opened.ok());
    auto retention = std::move(retention_opened).value();
    QVERIFY(retention.is_retired(comments_config.stream, comments_group));
    QVERIFY(!retention.is_retired(text_config.stream, text_group));
    QVERIFY(retention.publish_after_durable_trim(
        retirements, save_both_stream_histories).ok());
    QVERIFY(retention.is_retired(text_config.stream, text_group));
    auto before_membership = membership.recover();
    QVERIFY(before_membership.ok());
    QVERIFY(before_membership.value().empty());

    // The consumer may checkpoint the local acknowledgment only after both
    // stream retirement records have crossed their durability barriers.
    {
      auto opened = DocumentSession::open(text_config);
      QVERIFY(opened.ok());
      auto session = std::move(opened).value();
      QVERIFY(session->acknowledge_undo_retirements({text_group}).ok());
      QVERIFY(session->pending_undo_retirements().empty());
      QVERIFY(session->close().ok());
    }
    {
      auto opened = DocumentSession::open(comments_config);
      QVERIFY(opened.ok());
      auto session = std::move(opened).value();
      QVERIFY(session->acknowledge_undo_retirements({comments_group}).ok());
      QVERIFY(session->pending_undo_retirements().empty());
      QVERIFY(session->close().ok());
    }

    std::optional<Storage::MembershipEvent> retired_event;
    auto membership_transition = coordinator->commit_transitions(
        "retirement-" + preview.value().digest,
        {{text_config.document, text_config.stream, 1, 2, {owner}},
         {comments_config.document, comments_config.stream, 1, 2, {owner}}},
        [&]() -> Result<void> {
          auto event = membership.commit_retirement(
              preview.value(), descriptor, impacts, acknowledgements,
              "explicit two-stream retirement confirmation", verify_ack);
          if (!event.ok()) return event.error();
          retired_event = event.value();
          return {};
        });
    QVERIFY(membership_transition.ok());
    QVERIFY(retired_event.has_value());
    QCOMPARE(retired_event->epoch, uint64_t(2));
    const auto events = membership.recover();
    QVERIFY(events.ok());
    QCOMPARE(events.value().size(), size_t(1));

    for (const auto &stream : {text_config.stream, comments_config.stream}) {
      QVERIFY(!coordinator->authorize_local_mutation(
          text_config.document, stream, 1, retired_device));
      QVERIFY(coordinator->authorize_local_mutation(
          text_config.document, stream, 2, owner));
    }
  }

  void session_recovers_pending_intent_after_transport_sync_failure() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::filesystem::path root = tmp.path().toStdString();
    const DocumentSessionConfig config{root / "shared", root / "device",
                                       "doc-push-failure", "text-push-failure", 1};
    bool fail_segment_sync = true;
    Storage::StepHook hook;
    hook.fail = [&](Storage::Step step, const std::filesystem::path &path)
        -> std::optional<Error> {
      if (fail_segment_sync && step == Storage::Step::FileSync &&
          path.extension() == ".open") {
        fail_segment_sync = false;
        return Error(ErrorCode::io_error, "injected segment sync failure");
      }
      return std::nullopt;
    };
    Crdt::WriterConfig writer_config;
    writer_config.step_hook = &hook;
    {
      auto opened = DocumentSessionTestAccess::open(config, writer_config);
      QVERIFY(opened.ok());
      auto session = std::move(opened).value();
      QVERIFY(session->ready());
      auto failed = session->apply_local({{0, 0}}, {"survives"});
      QVERIFY(!failed.ok());
      QCOMPARE(failed.error().code(), ErrorCode::io_error);
      QVERIFY(!session->ready());
      LocalBranchExport metadata;
      std::vector<OutboundDelivery> deliveries;
      auto exported = session->export_local_branch(metadata,
          [&](const OutboundDelivery& delivery) -> Result<void> {
            deliveries.push_back(delivery);
            return {};
          });
      QVERIFY2(exported.ok(), exported.ok() ? "" : exported.error().message().data());
      QCOMPARE(deliveries.size(), size_t(1));
      QCOMPARE(deliveries[0].token.producer, session->actor());
      QCOMPARE(deliveries[0].token.sequence, uint64_t(1));
    }

    auto reopened = DocumentSession::open(config);
    QVERIFY(reopened.ok());
    auto session = std::move(reopened).value();
    QVERIFY(session->ready());
    QCOMPARE(session->text(), std::string("survives"));
    QCOMPARE(session->recovery_report().applied, size_t(1));
    auto group = session->next_undo_group();
    QVERIFY(group.has_value());
    QCOMPARE(group->operations.size(), size_t(1));
    QVERIFY(session->undo().ok());
    QCOMPARE(session->text(), std::string{});
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

  void retired_actorDoesNotPublishPendingCommitIntoOldEpoch() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::filesystem::path root = tmp.path().toStdString();
    DocumentSessionConfig config{root / "shared", root / "device",
                                 "doc-retired-pending", "text-retired-pending", 9};
    ActorId owner;
    {
      auto opened = DocumentSession::open(config);
      QVERIFY(opened.ok());
      auto session = std::move(opened).value();
      owner = session->actor();
      QVERIFY(session->close().ok());
    }

    auto allocator = std::make_shared<Test::VolatileCounter>(owner);
    Crdt::Buffer source(allocator);
    auto local = source.try_apply_local_edit({{0, 0}}, {"unsynced"});
    QVERIFY(local.ok());
    auto wire = encode_operation(local.value());
    QVERIFY(wire.ok());
    const std::string history_key = config.document + std::string(1, '\0') +
                                    config.stream + std::string(1, '\0') +
                                    std::to_string(config.epoch);
    auto history_open = Storage::LocalHistoryLog::open(
        root / "device" /
            ("local-history-" + sha256_hex(history_key) + ".snapshot"), owner);
    QVERIFY(history_open.ok());
    auto history = std::move(history_open).value();
    QVERIFY(history.store_pending(
        {1, wire.value(), source.local_history_snapshot()}).ok());
    QVERIFY(history.close().ok());

    bool enrolled = false;
    config.authorize_local_mutation = [&](const DocumentId &, const StreamId &,
                                          uint64_t epoch, const ActorId &actor)
        -> Result<std::shared_ptr<void>> {
      if (epoch != config.epoch || actor != owner)
        return Error(ErrorCode::wrong_epoch, "unexpected mutation scope");
      if (!enrolled)
        return Error(ErrorCode::wrong_epoch, "actor was retired from this epoch");
      return std::static_pointer_cast<void>(std::make_shared<int>(1));
    };

    auto reopened = DocumentSession::open(config);
    QVERIFY(reopened.ok());
    auto session = std::move(reopened).value();
    QVERIFY(!session->ready());
    QCOMPARE(session->text(), std::string());
    LocalBranchExport metadata;
    std::vector<OutboundDelivery> exported;
    QVERIFY(session->export_local_branch(metadata,
        [&](const OutboundDelivery &delivery) -> Result<void> {
          exported.push_back(delivery);
          return {};
        }).ok());
    QCOMPARE(exported.size(), size_t(1));
    QCOMPARE(exported.front().token.producer, owner);
    QCOMPARE(exported.front().token.epoch, config.epoch);
    QCOMPARE(exported.front().payload, wire.value());

    DocumentSessionConfig observer_config{root / "shared", root / "observer",
                                           config.document, config.stream, config.epoch};
    auto observer_opened = DocumentSession::open(observer_config);
    QVERIFY(observer_opened.ok());
    auto observer = std::move(observer_opened).value();
    QCOMPARE(observer->text(), std::string());
    QVERIFY(observer->close().ok());

    enrolled = true;
    auto recovered = session->recover();
    QVERIFY(recovered.ok());
    QVERIFY(session->ready());
    QCOMPARE(session->text(), std::string("unsynced"));
    QVERIFY(session->pending_undo_retirements().empty());
    QVERIFY(session->close().ok());
  }

  void inboundRetiredEpochDeliveryIsQuarantinedBeforeApply() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::filesystem::path root = tmp.path().toStdString();
    DocumentSessionConfig sender_config{root / "shared", root / "sender",
                                        "doc-inbound-membership",
                                        "text-inbound-membership", 5};
    auto sender_opened = DocumentSession::open(sender_config);
    QVERIFY(sender_opened.ok());
    auto sender = std::move(sender_opened).value();
    const auto sender_actor = sender->actor();
    auto edit = sender->apply_local({{0, 0}}, {"retired"});
    QVERIFY(edit.ok());
    auto wire = encode_operation(edit.value());
    QVERIFY(wire.ok());
    QVERIFY(sender->close().ok());

    DocumentSessionConfig receiver_config{root / "shared", root / "receiver",
                                          sender_config.document,
                                          sender_config.stream,
                                          sender_config.epoch};
    bool sender_allowed = false;
    receiver_config.authorize_inbound_delivery =
        [&](const DocumentId &document, const StreamId &stream, uint64_t epoch,
            const DeliveryToken &token) -> Result<std::shared_ptr<void>> {
      if (document != sender_config.document || stream != sender_config.stream ||
          epoch != sender_config.epoch || token.producer != sender_actor)
        return Error(ErrorCode::malformed,
                     "inbound authorization received an unexpected delivery");
      if (!sender_allowed)
        return Error(ErrorCode::wrong_epoch,
                     "sender no longer belongs to the active epoch");
      return std::static_pointer_cast<void>(std::make_shared<int>(1));
    };

    auto denied_opened = DocumentSession::open(receiver_config);
    QVERIFY(denied_opened.ok());
    auto denied = std::move(denied_opened).value();
    QVERIFY(!denied->ready());
    QCOMPARE(denied->text(), std::string());
    auto quarantined = denied->quarantined_deliveries();
    QVERIFY(quarantined.ok());
    QCOMPARE(quarantined.value().size(), size_t(1));
    QCOMPARE(quarantined.value().front().delivery.payload, wire.value());
    QCOMPARE(quarantined.value().front().delivery.token.producer, sender_actor);
    QVERIFY(denied->close().ok());

    sender_allowed = true;
    auto accepted_opened = DocumentSession::open(receiver_config);
    QVERIFY(accepted_opened.ok());
    auto accepted = std::move(accepted_opened).value();
    QVERIFY(accepted->ready());
    QCOMPARE(accepted->text(), std::string("retired"));
    QVERIFY(accepted->close().ok());
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
    std::optional<OutboundDelivery> expected_deferred;
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
      LocalBranchExport metadata;
      auto exported = b->export_local_branch(
          metadata, [&](const OutboundDelivery &delivery) -> Result<void> {
            expected_deferred = delivery;
            return {};
          });
      QVERIFY(exported.ok());
      QVERIFY(expected_deferred.has_value());
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
    auto deferred = c->deferred_deliveries();
    QCOMPARE(deferred.size(), size_t(1));
    QCOMPARE(deferred[0].token, expected_deferred->token);
    QCOMPARE(deferred[0].payload, expected_deferred->payload);
    Storage::CheckpointShareableCore deferred_core;
    deferred_core.context.document = c_config.document;
    deferred_core.context.stream = c_config.stream;
    deferred_core.context.epoch = c_config.epoch;
    deferred_core.pins.canonical_projection_hash = sha256_hex("");
    deferred_core.translations.supported_token_versions = {1};
    auto captured_deferred = DocumentSessionTestAccess::checkpoint_core(
        *c, deferred_core);
    QVERIFY(captured_deferred.ok());
    QCOMPARE(captured_deferred.value().deferred.size(), size_t(1));
    QCOMPARE(captured_deferred.value().deferred.front().operation_bytes,
             expected_deferred->payload);
    QVERIFY(!captured_deferred.value().deferred.front().quarantined);
    QVERIFY(!captured_deferred.value().coverage.delivery.contains(
        expected_deferred->token.producer));
    auto edit_while_incomplete = c->apply_local({{0, 0}}, {"unsafe"});
    QVERIFY(!edit_while_incomplete.ok());

    std::filesystem::copy(source / "replicas" / actor_a.to_string(),
                          partial / "replicas" / actor_a.to_string(),
                          std::filesystem::copy_options::recursive);
    auto delivery_poll = c->poll();
    QVERIFY(delivery_poll.ok());
    QVERIFY(c->ready());
    QCOMPARE(c->text(), std::string("AB"));
    auto captured_after_dependency = DocumentSessionTestAccess::checkpoint_core(
        *c, deferred_core);
    QVERIFY(captured_after_dependency.ok());
    QVERIFY(captured_after_dependency.value().deferred.empty());
    QCOMPARE(captured_after_dependency.value().coverage.delivery.at(
                 expected_deferred->token.producer),
             uint64_t(1));
    QVERIFY(c->poll().ok()); // commits any earlier deferred envelope after its
                             // dependency arrived
    QVERIFY(c->deferred_deliveries().empty());
    auto recovered = c->recover();
    QVERIFY(recovered.ok());
    QVERIFY(c->close().ok());
  }

  void malformed_inbound_deliveryRetainsExactBytesAndReasonAcrossReopen() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::filesystem::path root = tmp.path().toStdString();
    const auto producer = Test::actor(137);
    const StreamScope scope{"quarantine-stream", 1};
    const std::string payload{static_cast<char>(0xff), 'x', '\0', 'y'};
    const InboundDelivery expected{
        DeliveryToken{scope.stream, scope.epoch, producer, 1}, payload};
    {
      Crdt::FileOpStream writer(root / "shared", producer);
      QVERIFY(writer.start().ok());
      QVERIFY(writer.register_stream(scope).ok());
      QVERIFY(writer.push(OutboundDelivery{expected.token, payload},
                          PushDurability::durable).ok());
      QVERIFY(writer.close().ok());
    }

    const DocumentSessionConfig config{root / "shared", root / "device",
                                       "quarantine-doc", scope.stream,
                                       scope.epoch};
    std::optional<Error> expected_reason;
    for (int reopen = 0; reopen != 2; ++reopen) {
      auto opened = DocumentSession::open(config);
      QVERIFY(opened.ok());
      auto session = std::move(opened).value();
      QVERIFY(!session->ready());
      QCOMPARE(session->recovery_report().rejected, size_t(1));
      auto quarantined = session->quarantined_deliveries();
      QVERIFY(quarantined.ok());
      QCOMPARE(quarantined.value().size(), size_t(1));
      QCOMPARE(quarantined.value().front().delivery.token, expected.token);
      QCOMPARE(quarantined.value().front().delivery.payload, expected.payload);
      QVERIFY(!quarantined.value().front().reason.message().empty());
      if (expected_reason)
        QCOMPARE(quarantined.value().front().reason, *expected_reason);
      else
        expected_reason = quarantined.value().front().reason;
      if (reopen == 0) {
        auto polled = session->poll();
        QVERIFY(polled.ok());
        QCOMPARE(polled.value().quarantined, size_t(1));
        QCOMPARE(polled.value().committed, size_t(0));
        auto after_poll = session->quarantined_deliveries();
        QVERIFY(after_poll.ok());
        QCOMPARE(after_poll.value().size(), size_t(1));
        auto retried = session->recover();
        QVERIFY(retried.ok());
        QCOMPARE(session->recovery_report().rejected, size_t(3));
        auto deduplicated = session->quarantined_deliveries();
        QVERIFY(deduplicated.ok());
        QCOMPARE(deduplicated.value().size(), size_t(1));
      }
      QVERIFY(session->close().ok());
    }

    auto limited_config = config;
    limited_config.private_state = root / "limited-device";
    limited_config.max_quarantined_deliveries = 0;
    auto limited_open = DocumentSession::open(limited_config);
    QVERIFY(limited_open.ok());
    auto limited = std::move(limited_open).value();
    QVERIFY(!limited->ready());
    auto omitted = limited->quarantined_deliveries();
    QVERIFY(!omitted.ok());
    QCOMPARE(omitted.error().code(), ErrorCode::limit_exceeded);
    QVERIFY(limited->close().ok());

    auto byte_limited_config = config;
    byte_limited_config.private_state = root / "byte-limited-device";
    byte_limited_config.max_quarantined_bytes = 0;
    auto byte_limited_open = DocumentSession::open(byte_limited_config);
    QVERIFY(byte_limited_open.ok());
    auto byte_limited = std::move(byte_limited_open).value();
    auto omitted_by_bytes = byte_limited->quarantined_deliveries();
    QVERIFY(!omitted_by_bytes.ok());
    QCOMPARE(omitted_by_bytes.error().code(), ErrorCode::limit_exceeded);
    QVERIFY(byte_limited->close().ok());
  }

  void sessionCheckpointCapturesOnlyRepresentedDeliveryPrefix() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::filesystem::path root = tmp.path().toStdString();
    const StreamScope scope{"checkpoint-capture-stream", 1};
    const auto producer = Test::actor(138);
    const std::string payload{static_cast<char>(0xff), 'c', '\0', 'p'};
    const InboundDelivery inbound{
        DeliveryToken{scope.stream, scope.epoch, producer, 1}, payload};
    DocumentSessionConfig config{root / "shared", root / "device",
                                 "checkpoint-capture-doc", scope.stream,
                                 scope.epoch};
    config.checkpoint_store = root / "checkpoint-store";
    bool provider_accepts_checkpoint = true;
    config.validate_checkpoint_candidate =
        [&provider_accepts_checkpoint](
            std::string_view shareable,
            const std::optional<std::string> &local) -> Result<void> {
      if (!provider_accepts_checkpoint || shareable.empty() || !local)
        return Error(ErrorCode::malformed,
                     "provider rejected or received an incomplete "
                     "checkpoint bundle");
      return {};
    };
    config.checkpoint_delivery_threshold = 1;
    auto opened = DocumentSession::open(config);
    QVERIFY(opened.ok());
    auto session = std::move(opened).value();
    auto local = session->apply_local({{0, 0}}, {"local"});
    QVERIFY(local.ok());

    {
      Crdt::FileOpStream writer(root / "shared", producer);
      QVERIFY(writer.start().ok());
      QVERIFY(writer.register_stream(scope).ok());
      QVERIFY(writer.push(OutboundDelivery{inbound.token, payload},
                          PushDurability::durable).ok());
      QVERIFY(writer.close().ok());
    }

    Storage::CheckpointShareableCore provider;
    provider.context.document = config.document;
    provider.context.stream = config.stream;
    provider.context.epoch = config.epoch;
    provider.context.engine = Storage::CheckpointEngineKind::Text;
    provider.pins.canonical_projection_hash = sha256_hex("");
    provider.translations.supported_token_versions = {1};
    auto before_poll = DocumentSessionTestAccess::checkpoint_core(
        *session, provider);
    QVERIFY(before_poll.ok());
    QCOMPARE(before_poll.value().coverage.delivery.at(session->actor()),
             uint64_t(1));
    QVERIFY(!before_poll.value().coverage.delivery.contains(producer));
    QVERIFY(before_poll.value().deferred.empty());

    auto polled = session->poll();
    QVERIFY(polled.ok());
    QCOMPARE(polled.value().quarantined, size_t(1));
    auto due_before_publish = session->checkpoint_due();
    QVERIFY(due_before_publish.ok());
    QVERIFY(due_before_publish.value());
    auto after_poll = DocumentSessionTestAccess::checkpoint_core(
        *session, provider);
    QVERIFY2(after_poll.ok(), after_poll.error().message().data());
    QVERIFY(!after_poll.value().coverage.delivery.contains(producer));
    QCOMPARE(after_poll.value().deferred.size(), size_t(1));
    QCOMPARE(after_poll.value().deferred.front().token, inbound.token);
    QCOMPARE(after_poll.value().deferred.front().operation_bytes, payload);
    QVERIFY(after_poll.value().deferred.front().quarantined);

    SessionCheckpointProviderState public_provider;
    public_provider.canonical_pin_projection_hash = sha256_hex("");
    auto public_capture = session->capture_checkpoint(public_provider);
    QVERIFY(public_capture.ok());
    QVERIFY(!public_capture.value().shareable_artifact_bytes.empty());
    QVERIFY(!public_capture.value().local_companion_artifact_bytes.empty());
    QCOMPARE(public_capture.value().shareable_artifact_hash,
             sha256_hex(public_capture.value().shareable_artifact_bytes));
    QCOMPARE(public_capture.value().operation_coverage,
             after_poll.value().coverage.operation);
    QCOMPARE(public_capture.value().delivery_coverage,
             after_poll.value().coverage.delivery);
    QCOMPARE(public_capture.value().canonical_pin_projection_hash,
             public_provider.canonical_pin_projection_hash);
    QCOMPARE(public_capture.value().translation_manifest_hash.size(), size_t(64));
    auto local_artifact = Storage::decode_checkpoint(
        public_capture.value().local_companion_artifact_bytes,
        Storage::CheckpointKind::Local);
    QVERIFY(local_artifact.ok());
    auto local_core = Storage::decode_checkpoint_local_core(
        local_artifact.value(), session->actor(),
        public_capture.value().shareable_artifact_hash,
        after_poll.value().context);
    QVERIFY(local_core.ok());
    QCOMPARE(local_core.value().writer.durable_delivery_frontier, uint64_t(1));
    QCOMPARE(local_core.value().writer.next_identity_counter,
             public_capture.value().operation_coverage.at(session->actor()) + 1);
    auto published_capture = session->publish_checkpoint(public_provider);
    QVERIFY(published_capture.ok());
    QCOMPARE(published_capture.value().shareable_artifact_bytes,
             public_capture.value().shareable_artifact_bytes);
    QCOMPARE(published_capture.value().local_companion_artifact_bytes,
             public_capture.value().local_companion_artifact_bytes);
    auto due_after_publish = session->checkpoint_due();
    QVERIFY(due_after_publish.ok());
    QVERIFY(!due_after_publish.value());
    {
      auto checkpoint_store = Storage::CheckpointStore::open(
          root / "checkpoint-store");
      QVERIFY(checkpoint_store.ok());
      auto stored = checkpoint_store.value().recover(
          session->actor(),
          [](std::string_view,
             const std::optional<std::string> &) -> Result<void> {
            return {};
          },
          after_poll.value().context);
      QVERIFY(stored.ok());
      QVERIFY(stored.value().has_value());
      QCOMPARE(stored.value()->shareable_bytes,
               public_capture.value().shareable_artifact_bytes);
      QCOMPARE(stored.value()->local_bytes,
               std::optional<std::string>(
                   public_capture.value().local_companion_artifact_bytes));
      provider_accepts_checkpoint = false;
      auto rejected_publication = session->publish_checkpoint(public_provider);
      QVERIFY(!rejected_publication.ok());
      QCOMPARE(rejected_publication.error().code(), ErrorCode::malformed);
      provider_accepts_checkpoint = true;
      auto still_stored = checkpoint_store.value().recover(
          session->actor(),
          [](std::string_view,
             const std::optional<std::string> &) -> Result<void> {
            return {};
          },
          after_poll.value().context);
      QVERIFY(still_stored.ok());
      QVERIFY(still_stored.value().has_value());
      QCOMPARE(still_stored.value()->shareable_bytes,
               public_capture.value().shareable_artifact_bytes);
    }
    auto incomplete_provider = public_provider;
    incomplete_provider.pin_projection_complete = false;
    auto incomplete_capture = session->capture_checkpoint(
        incomplete_provider);
    QVERIFY(!incomplete_capture.ok());
    QCOMPARE(incomplete_capture.error().code(), ErrorCode::awaiting_state);
    auto mismatched_manifest = public_provider;
    mismatched_manifest.expected_translation_manifest_hash = std::string(64, '0');
    auto rejected_manifest = session->capture_checkpoint(
        mismatched_manifest);
    QVERIFY(!rejected_manifest.ok());

    auto encoded = Storage::encode_checkpoint_core(after_poll.value());
    QVERIFY(encoded.ok());
    auto artifact = Storage::decode_checkpoint(
        encoded.value(), Storage::CheckpointKind::Shareable);
    QVERIFY(artifact.ok());
    auto decoded = Storage::decode_checkpoint_core(artifact.value());
    QVERIFY(decoded.ok());
    QCOMPARE(decoded.value().deferred, after_poll.value().deferred);
    QCOMPARE(decoded.value().coverage.delivery,
             after_poll.value().coverage.delivery);
    QVERIFY(session->close().ok());

    auto checkpointed_config = config;
    checkpointed_config.checkpoint_store = root / "checkpoint-store";
    checkpointed_config.validate_checkpoint_candidate =
        [](std::string_view shareable,
           const std::optional<std::string> &local) -> Result<void> {
          if (shareable.empty() || !local)
            return Error(ErrorCode::malformed,
                         "expected a complete local checkpoint bundle");
          return {};
        };
    auto checkpointed_open = DocumentSession::open(checkpointed_config);
    QVERIFY(checkpointed_open.ok());
    auto checkpointed = std::move(checkpointed_open).value();
    QCOMPARE(checkpointed->text(), std::string("local"));
    QCOMPARE(checkpointed->actor(), local_core.value().writer.actor);
    QVERIFY(checkpointed->recovery_report().diagnostics.empty());
    auto restored_quarantine = checkpointed->quarantined_deliveries();
    QVERIFY(restored_quarantine.ok());
    QCOMPARE(restored_quarantine.value().size(), size_t(1));
    QCOMPARE(restored_quarantine.value().front().delivery.payload, payload);
    QVERIFY(checkpointed->close().ok());

    auto rejected_config = checkpointed_config;
    rejected_config.validate_checkpoint_candidate =
        [](std::string_view, const std::optional<std::string> &) -> Result<void> {
      return Error(ErrorCode::malformed,
                   "provider rejected checkpoint candidate");
    };
    auto rejected_open = DocumentSession::open(rejected_config);
    QVERIFY(rejected_open.ok());
    auto rejected = std::move(rejected_open).value();
    QCOMPARE(rejected->text(), std::string("local"));
    QVERIFY(!rejected->recovery_report().diagnostics.empty());
    QVERIFY(rejected->close().ok());

  }

  void checkpointStartupMatchesFullReplayForFutureEdits() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::filesystem::path root = tmp.path().toStdString();
    DocumentSessionConfig source_config{root / "source-shared",
                                        root / "source-device", "doc-equivalent",
                                        "stream-equivalent", 1};
    auto source_open = DocumentSession::open(source_config);
    QVERIFY(source_open.ok());
    auto source = std::move(source_open).value();
    auto initial_edit = source->apply_local({{0, 0}}, {"base"});
    QVERIFY(initial_edit.ok());

    SessionCheckpointProviderState provider;
    provider.canonical_pin_projection_hash = sha256_hex("");
    auto capture = source->capture_checkpoint(provider);
    QVERIFY2(capture.ok(), capture.error().message().data());
    {
      auto store_open = Storage::CheckpointStore::open(root / "source-checkpoint");
      QVERIFY(store_open.ok());
      auto store = std::move(store_open).value();
      auto published = store.publish(capture.value().shareable_artifact_bytes,
                                     capture.value().local_companion_artifact_bytes,
                                     source->actor());
      QVERIFY(published.ok());
    }
    QVERIFY(source->close().ok());

    const auto copy_tree = [](const std::filesystem::path &from,
                              const std::filesystem::path &to) {
      std::error_code error;
      std::filesystem::copy(from, to,
                            std::filesystem::copy_options::recursive, error);
      return !error;
    };
    const auto full_shared = root / "full-shared";
    const auto full_device = root / "full-device";
    QVERIFY(copy_tree(root / "source-shared", full_shared));
    QVERIFY(copy_tree(root / "source-device", full_device));

    auto peer_config = source_config;
    peer_config.private_state = root / "peer-device";
    auto peer_open = DocumentSession::open(peer_config);
    QVERIFY(peer_open.ok());
    auto peer = std::move(peer_open).value();
    QCOMPARE(peer->text(), std::string("base"));
    auto peer_edit = peer->apply_local({{0, 0}}, {"peer"});
    QVERIFY(peer_edit.ok());
    const auto peer_actor = peer->actor();
    auto peer_bytes = encode_operation(peer_edit.value());
    QVERIFY(peer_bytes.ok());
    QVERIFY(peer->close().ok());
    {
      Crdt::FileOpStream full_branch_writer(full_shared, peer_actor);
      QVERIFY(full_branch_writer.start().ok());
      const StreamScope branch_scope{source_config.stream, source_config.epoch};
      QVERIFY(full_branch_writer.register_stream(branch_scope).ok());
      QVERIFY(full_branch_writer.push(
          {{branch_scope.stream, branch_scope.epoch, peer_actor, 1},
           peer_bytes.value()},
          PushDurability::durable).ok());
      QVERIFY(full_branch_writer.close().ok());
    }

    auto checkpoint_config = source_config;
    checkpoint_config.checkpoint_store = root / "source-checkpoint";
    checkpoint_config.validate_checkpoint_candidate =
        [](std::string_view shareable,
           const std::optional<std::string> &local) -> Result<void> {
      if (shareable.empty() || !local)
        return Error(ErrorCode::malformed, "incomplete checkpoint bundle");
      return {};
    };
    auto checkpoint_open = DocumentSession::open(checkpoint_config);
    QVERIFY(checkpoint_open.ok());
    auto checkpointed = std::move(checkpoint_open).value();
    const auto checkpoint_base_text = checkpointed->text();
    QCOMPARE(checkpoint_base_text.size(), size_t(8));
    auto checkpoint_edit = checkpointed->apply_local({{8, 8}}, {"!"});
    QVERIFY(checkpoint_edit.ok());
    auto checkpoint_bytes = encode_operation(checkpoint_edit.value());
    QVERIFY(checkpoint_bytes.ok());
    const auto checkpoint_text = checkpointed->text();
    QCOMPARE(checkpoint_text.size(), size_t(9));

    auto full_config = source_config;
    full_config.shared_store = full_shared;
    full_config.private_state = full_device;
    auto full_open = DocumentSession::open(full_config);
    QVERIFY(full_open.ok());
    auto replayed = std::move(full_open).value();
    QCOMPARE(replayed->text().size(), size_t(8));
    QCOMPARE(replayed->text(), checkpoint_base_text);
    auto full_edit = replayed->apply_local({{8, 8}}, {"!"});
    QVERIFY(full_edit.ok());
    auto full_bytes = encode_operation(full_edit.value());
    QVERIFY(full_bytes.ok());
    QCOMPARE(checkpoint_bytes.value(), full_bytes.value());
    QCOMPARE(replayed->text(), checkpoint_text);

    auto checkpoint_undo = checkpointed->undo();
    QVERIFY(checkpoint_undo.ok());
    auto checkpoint_undo_bytes = encode_operation(checkpoint_undo.value());
    QVERIFY(checkpoint_undo_bytes.ok());
    auto full_undo = replayed->undo();
    QVERIFY(full_undo.ok());
    auto full_undo_bytes = encode_operation(full_undo.value());
    QVERIFY(full_undo_bytes.ok());
    QCOMPARE(checkpoint_undo_bytes.value(), full_undo_bytes.value());
    QCOMPARE(checkpointed->text(), checkpoint_base_text);
    QCOMPARE(replayed->text(), checkpoint_base_text);

    auto checkpoint_redo = checkpointed->redo();
    QVERIFY(checkpoint_redo.ok());
    auto checkpoint_redo_bytes = encode_operation(checkpoint_redo.value());
    QVERIFY(checkpoint_redo_bytes.ok());
    auto full_redo = replayed->redo();
    QVERIFY(full_redo.ok());
    auto full_redo_bytes = encode_operation(full_redo.value());
    QVERIFY(full_redo_bytes.ok());
    QCOMPARE(checkpoint_redo_bytes.value(), full_redo_bytes.value());
    QCOMPARE(checkpointed->text(), checkpoint_text);
    QCOMPARE(replayed->text(), checkpoint_text);

    QVERIFY(checkpointed->close().ok());
    QVERIFY(replayed->close().ok());
  }

  void sessionCheckpointBindsCoveredPeerReceiptToPublishedDigest() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::filesystem::path root = tmp.path().toStdString();
    const StreamScope scope{"checkpoint-receipt-stream", 1};
    DocumentSessionConfig config{root / "shared", root / "device",
                                 "checkpoint-receipt-doc", scope.stream,
                                 scope.epoch};
    config.checkpoint_store = root / "checkpoints";
    struct ExpectedReclamationBinding {
      std::string descriptor_hash;
      std::string checkpoint_hash;
      bool refuse = false;
    };
    auto expected_binding = std::make_shared<ExpectedReclamationBinding>();
    config.validate_checkpoint_candidate =
        [](std::string_view shareable,
           const std::optional<std::string> &) -> Result<void> {
      if (shareable.empty())
        return Error(ErrorCode::malformed, "empty checkpoint artifact");
      return {};
    };
    config.validate_reclamation_frontier =
        [expected_binding, scope](
            const DocumentId &document, const StreamId &stream, uint64_t epoch,
            const Storage::CertifiedOperationFrontier &frontier) -> Result<void> {
      if (expected_binding->refuse || document != "checkpoint-receipt-doc" ||
          stream != scope.stream || epoch != scope.epoch ||
          !frontier.matches_context(stream, expected_binding->descriptor_hash,
                                    expected_binding->checkpoint_hash))
        return Error(ErrorCode::awaiting_state,
                     "test provider rejected the reclamation certificate");
      return {};
    };
    config.checkpoint_delivery_threshold = 1;
    auto opened = DocumentSession::open(config);
    QVERIFY(opened.ok());
    auto session = std::move(opened).value();
    auto initially_due = session->checkpoint_due();
    QVERIFY(initially_due.ok());
    QVERIFY(!initially_due.value());

    const auto producer = Test::actor(188);
    Crdt::Buffer peer_engine(Test::test_counter(188));
    auto peer_operation = peer_engine.try_apply_local_edit({{0, 0}}, {"peer"});
    QVERIFY(peer_operation.ok());
    auto peer_payload = encode_operation(peer_operation.value());
    QVERIFY(peer_payload.ok());
    {
      Crdt::FileOpStream peer(root / "shared", producer);
      QVERIFY(peer.start().ok());
      QVERIFY(peer.register_stream(scope).ok());
      QVERIFY(peer.push({{scope.stream, scope.epoch, producer, 1},
                         peer_payload.value()},
                        PushDurability::durable).ok());
      QVERIFY(peer.close().ok());
    }
    auto delivery_due = session->checkpoint_due();
    QVERIFY(delivery_due.ok());
    QVERIFY(delivery_due.value());
    auto polled = session->poll();
    QVERIFY(polled.ok());
    QCOMPARE(polled.value().committed, size_t(1));
    QCOMPARE(session->text(), std::string("peer"));

    SessionCheckpointProviderState provider;
    provider.canonical_pin_projection_hash = sha256_hex("");
    auto captured = session->capture_checkpoint(provider);
    QVERIFY(captured.ok());
    QCOMPARE(captured.value().delivery_coverage.at(producer), uint64_t(1));
    auto published = session->publish_checkpoint(provider);
    QVERIFY(published.ok());
    QCOMPARE(published.value().shareable_artifact_hash,
             captured.value().shareable_artifact_hash);

    const auto session_actor = session->actor();
    Storage::MembershipDescriptor descriptor;
    descriptor.epoch = scope.epoch;
    descriptor.base_checkpoint_hash = sha256_hex("reclamation-base-checkpoint");
    descriptor.members = {session_actor};
    Storage::MembershipDescriptor::StreamFrontier base;
    base.stream = scope.stream;
    base.operation.emplace(session_actor, 0);
    base.delivery.emplace(session_actor, 0);
    for (const auto &[actor, _] : captured.value().operation_coverage)
      base.operation.emplace(actor, 0);
    for (const auto &[actor, _] : captured.value().delivery_coverage)
      base.delivery.emplace(actor, 0);
    descriptor.base_frontiers.push_back(std::move(base));
    descriptor.supported_token_versions = {1};
    descriptor.translation_manifest_hash = sha256_hex("reclamation-translations");
    auto sealed_descriptor = Storage::seal_membership_descriptor(
        std::move(descriptor));
    QVERIFY(sealed_descriptor.ok());
    descriptor = std::move(sealed_descriptor).value();

    Storage::PeerCoverageEvidence evidence;
    evidence.member = session_actor;
    evidence.epoch = descriptor.epoch;
    evidence.descriptor_hash = descriptor.hash;
    evidence.checkpoint_hash = published.value().shareable_artifact_hash;
    evidence.operation[scope.stream] = captured.value().operation_coverage;
    evidence.delivery[scope.stream] = captured.value().delivery_coverage;
    auto certificate = Storage::certify_common_coverage(
        descriptor, {evidence},
        [](const auto &, const auto &) { return Result<void>{}; },
        [](const auto &, auto, const auto &, const auto &, const auto &) {
          return Result<void>{};
        });
    QVERIFY(certificate.ok());
    auto certified_frontier =
        certificate.value().operation_frontier(scope.stream);
    QVERIFY(certified_frontier.has_value());
    expected_binding->descriptor_hash = descriptor.hash;
    expected_binding->checkpoint_hash = published.value().shareable_artifact_hash;
    QVERIFY(session->validate_reclamation_frontier(*certified_frontier).ok());

    evidence.checkpoint_hash = sha256_hex("stale-reclamation-checkpoint");
    auto stale_certificate = Storage::certify_common_coverage(
        descriptor, {evidence},
        [](const auto &, const auto &) { return Result<void>{}; },
        [](const auto &, auto, const auto &, const auto &, const auto &) {
          return Result<void>{};
        });
    QVERIFY(stale_certificate.ok());
    auto stale_frontier = stale_certificate.value().operation_frontier(scope.stream);
    QVERIFY(stale_frontier.has_value());
    auto stale_result = session->validate_reclamation_frontier(*stale_frontier);
    QVERIFY(!stale_result.ok());
    QCOMPARE(stale_result.error().code(), ErrorCode::wrong_epoch);

    expected_binding->refuse = true;
    auto refused = session->validate_reclamation_frontier(*certified_frontier);
    QVERIFY(!refused.ok());
    QCOMPARE(refused.error().code(), ErrorCode::awaiting_state);
    expected_binding->refuse = false;

    auto reset_due = session->checkpoint_due();
    QVERIFY(reset_due.ok());
    QVERIFY(!reset_due.value());

    std::string stream_component;
    static constexpr char hex[] = "0123456789abcdef";
    for (unsigned char byte : scope.stream) {
      stream_component.push_back(hex[byte >> 4]);
      stream_component.push_back(hex[byte & 15]);
    }
    const auto receipt_path = root / "shared" / "local"
        / session->actor().to_string() / "receipts" / stream_component
        / std::to_string(scope.epoch) / producer.to_string();
    std::ifstream input(receipt_path, std::ios::binary);
    QVERIFY(input.good());
    const std::string actual(std::istreambuf_iterator<char>(input), {});
    std::string expected("CTRC\1", 5);
    const auto append_u64 = [&expected](uint64_t value) {
      for (int shift = 56; shift >= 0; shift -= 8)
        expected.push_back(static_cast<char>(value >> shift));
    };
    append_u64(1);
    expected.push_back(1);
    append_u64(published.value().shareable_artifact_hash.size());
    expected += published.value().shareable_artifact_hash;
    QCOMPARE(actual, expected);
    QVERIFY(session->close().ok());
    auto reopened_result = DocumentSession::open(config);
    QVERIFY(reopened_result.ok());
    auto reopened = std::move(reopened_result).value();
    QVERIFY(reopened->validate_reclamation_frontier(*certified_frontier).ok());
    const auto local_actor = reopened->actor();
    QVERIFY(reopened->close().ok());

    auto bound_artifact = Storage::decode_checkpoint(
        published.value().shareable_artifact_bytes,
        Storage::CheckpointKind::Shareable);
    QVERIFY(bound_artifact.ok());
    auto bound_core = Storage::decode_checkpoint_core(bound_artifact.value());
    QVERIFY(bound_core.ok());
    bound_core.value().engine.reclamation.stream = scope.stream;
    bound_core.value().engine.reclamation.descriptor_hash = descriptor.hash;
    bound_core.value().engine.reclamation.certificate_checkpoint_hash =
        published.value().shareable_artifact_hash;
    bound_core.value().engine.reclamation.operation_frontier =
        captured.value().operation_coverage;
    auto bound_bytes = Storage::encode_checkpoint_core(bound_core.value());
    QVERIFY(bound_bytes.ok());
    {
      auto store = Storage::CheckpointStore::open(root / "checkpoints");
      QVERIFY(store.ok());
      auto bound_checkpoint =
          store.value().publish(bound_bytes.value(), {}, local_actor);
      QVERIFY(bound_checkpoint.ok());
      auto selected_bound = store.value().recover(
          local_actor,
          [](std::string_view, const std::optional<std::string> &) -> Result<void> {
            return {};
          }, bound_core.value().context);
      QVERIFY(selected_bound.ok());
      QVERIFY(selected_bound.value().has_value());
      QCOMPARE(selected_bound.value()->shareable_bytes, bound_bytes.value());
    }

    bool binding_validated = false;
    config.validate_reclamation_binding =
        [&binding_validated, expected_binding, scope](
            const DocumentId &document, const StreamId &stream, uint64_t epoch,
            std::string_view descriptor_hash, std::string_view checkpoint_hash,
            const std::map<ActorId, uint64_t> &frontier) -> Result<void> {
      binding_validated = true;
      if (expected_binding->refuse || document != "checkpoint-receipt-doc" ||
          stream != scope.stream || epoch != scope.epoch ||
          descriptor_hash != expected_binding->descriptor_hash ||
          checkpoint_hash != expected_binding->checkpoint_hash || frontier.empty())
        return Error(ErrorCode::awaiting_state,
                     "test provider rejected persisted reclamation binding");
      return {};
    };
    auto bound_open = DocumentSession::open(config);
    QVERIFY(bound_open.ok());
    auto bound_session = std::move(bound_open).value();
    QVERIFY(binding_validated);
    QCOMPARE(bound_session->text(), std::string("peer"));
    QVERIFY(bound_session->recovery_report().diagnostics.empty());
    QVERIFY(bound_session->close().ok());

    expected_binding->refuse = true;
    binding_validated = false;
    auto refused_open = DocumentSession::open(config);
    QVERIFY(refused_open.ok());
    auto refused_session = std::move(refused_open).value();
    QVERIFY(binding_validated);
    QCOMPARE(refused_session->text(), std::string("peer"));
    QVERIFY(refused_session->close().ok());
  }

  void invalidCheckpointCoverageResetsBeforeFullReplayFallback() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::filesystem::path root = tmp.path().toStdString();
    DocumentSessionConfig config{root / "shared", root / "device",
                                 "doc-fallback-reset", "stream-fallback-reset", 1};
    config.checkpoint_store = root / "checkpoints";
    config.validate_checkpoint_candidate =
        [](std::string_view, const std::optional<std::string> &) -> Result<void> {
      return {};
    };
    auto opened = DocumentSession::open(config);
    QVERIFY(opened.ok());
    auto source = std::move(opened).value();
    auto edit = source->apply_local({{0, 0}}, {"logged"});
    QVERIFY(edit.ok());
    SessionCheckpointProviderState provider;
    provider.canonical_pin_projection_hash = sha256_hex("");
    auto capture = source->capture_checkpoint(provider);
    QVERIFY(capture.ok());

    auto shareable_artifact = Storage::decode_checkpoint(
        capture.value().shareable_artifact_bytes,
        Storage::CheckpointKind::Shareable);
    QVERIFY(shareable_artifact.ok());
    auto core = Storage::decode_checkpoint_core(shareable_artifact.value());
    QVERIFY(core.ok());
    bool changed_engine_state = false;
    for (auto &fragment : core.value().engine.fragments) {
      if (fragment.text == "logged") {
        fragment.text = "mutant";
        changed_engine_state = true;
      }
    }
    QVERIFY(changed_engine_state);
    const auto absent_producer = Test::actor(186);
    core.value().coverage.delivery[absent_producer] = 1;
    auto invalid_shareable = Storage::encode_checkpoint_core(core.value());
    QVERIFY(invalid_shareable.ok());
    const auto invalid_hash = sha256_hex(invalid_shareable.value());

    auto local_artifact = Storage::decode_checkpoint(
        capture.value().local_companion_artifact_bytes,
        Storage::CheckpointKind::Local);
    QVERIFY(local_artifact.ok());
    auto local_core = Storage::decode_checkpoint_local_core(
        local_artifact.value(), source->actor(),
        capture.value().shareable_artifact_hash, core.value().context);
    QVERIFY(local_core.ok());
    local_core.value().owner.shareable_checkpoint_hash = invalid_hash;
    auto invalid_local = Storage::encode_checkpoint_local_core(
        local_core.value(), core.value().context, invalid_shareable.value());
    QVERIFY(invalid_local.ok());
    const auto owner = source->actor();
    QVERIFY(source->close().ok());
    {
      auto store_open = Storage::CheckpointStore::open(config.checkpoint_store);
      QVERIFY(store_open.ok());
      auto store = std::move(store_open).value();
      auto published = store.publish(invalid_shareable.value(),
                                     invalid_local.value(), owner);
      QVERIFY(published.ok());
    }

    auto fallback_open = DocumentSession::open(config);
    QVERIFY(fallback_open.ok());
    auto fallback = std::move(fallback_open).value();
    QCOMPARE(fallback->text(), std::string("logged"));
    QVERIFY(!fallback->recovery_report().diagnostics.empty());
    QVERIFY(fallback->close().ok());
  }

  void consumer_event_log_correlates_undo_and_redo_groups_append_only() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::filesystem::path root = tmp.path().toStdString();
    const DocumentSessionConfig config{root / "shared", root / "device",
                                       "doc-events", "text-events", 1};
    const auto canonical = root / "consumer" / "events.log";
    const auto identity_text = [](const Crdt::Identity &id) {
      return id.actor.to_string() + "/" + std::to_string(id.counter);
    };
    std::string original_row;
    std::string undo_row;
    std::string group_id;
    {
      auto opened = DocumentSession::open(config);
      QVERIFY(opened.ok());
      auto session = std::move(opened).value();
      auto edit = session->apply_local({{0, 0}}, {"word"});
      QVERIFY(edit.ok());
      const auto group = session->next_undo_group();
      QVERIFY(group.has_value());
      group_id = identity_text(group->id);
      auto edit_wire = encode_operation(edit.value());
      QVERIFY(edit_wire.ok());
      original_row = "accepted:event-1:" + group_id + ":" +
                     identity_text(Crdt::op_identity(edit.value())) + ":" +
                     to_hex(edit_wire.value());
      QVERIFY(append_durable_line(canonical, original_row).ok());

      auto undo = session->undo();
      QVERIFY(undo.ok());
      auto undo_wire = encode_operation(undo.value());
      QVERIFY(undo_wire.ok());
      undo_row = "accepted:event-2:" + group_id + ":" +
                 identity_text(Crdt::op_identity(undo.value())) + ":" +
                 to_hex(undo_wire.value());
      QVERIFY(append_durable_line(canonical, undo_row).ok());
      QCOMPARE(session->text(), std::string{});
      QVERIFY(session->close().ok());
    }
    {
      auto reopened = DocumentSession::open(config);
      QVERIFY(reopened.ok());
      auto session = std::move(reopened).value();
      const auto group = session->next_redo_group();
      QVERIFY(group.has_value());
      QCOMPARE(identity_text(group->id), group_id);
      auto redo = session->redo();
      QVERIFY(redo.ok());
      auto redo_wire = encode_operation(redo.value());
      QVERIFY(redo_wire.ok());
      const auto redo_row = "accepted:event-3:" + group_id + ":" +
                            identity_text(Crdt::op_identity(redo.value())) + ":" +
                            to_hex(redo_wire.value());
      QVERIFY(append_durable_line(canonical, redo_row).ok());
      QCOMPARE(session->text(), std::string("word"));
      QVERIFY(session->close().ok());
    }
    std::ifstream events(canonical);
    std::vector<std::string> rows;
    for (std::string row; std::getline(events, row);)
      rows.push_back(std::move(row));
    QCOMPARE(rows.size(), size_t(3));
    QCOMPARE(rows[0], original_row);
    QCOMPARE(rows[1], undo_row);
    QVERIFY(rows[2].starts_with("accepted:event-3:" + group_id + ":"));
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
