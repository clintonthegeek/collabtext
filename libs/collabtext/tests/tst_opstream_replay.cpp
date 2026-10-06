#include "CodecTest.h"
/// tst_opstream_replay.cpp — Catch-up replay on first attach
///
/// Proves that a brand-new StreamSync instance replays all of a peer's history
/// on its first poll(), because its per-peer read cursors start at the beginning
/// of each peer's segment files.
///
///   - catch_up_replay_on_first_attach:
///       Replica A accumulates many ops. Replica B attaches fresh (no cursors),
///       polls once, and receives all of replica A's history. Final states match.
///
///   - catch_up_then_incremental_sync:
///       After catching up from history, subsequent ops are delivered
///       incrementally on the next flush/poll cycle.

#include <QTest>

#include <QTemporaryDir>
#include <collabtext/CrdtEngine.h>
#include <collabtext/Operations.h>
#include <collabtext/Serialization.h>
#include "transport/StreamSync.h"
#include <collabtext/OpStream.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include "TestActor.h"

using CollabText::Test::actor;

using namespace CollabText;
using namespace CollabText::Crdt;

class TestOpStreamReplay : public QObject {
    Q_OBJECT

private slots:

    void file_opstream_scopes_retry_and_durable_receipt() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const std::filesystem::path shared = tmp.path().toStdString();
        const auto sender_id = actor(41);
        const auto receiver_id = actor(42);
        const StreamScope buffer{"buffer:doc", 7};
        const StreamScope list{"list:doc", 7};
        FileOpStream sender(shared, sender_id), receiver(shared, receiver_id);
        QVERIFY(sender.start().ok());
        QVERIFY(receiver.start().ok());
        QVERIFY(sender.register_stream(buffer).ok());
        QVERIFY(sender.register_stream(list).ok());
        QVERIFY(receiver.register_stream(buffer).ok());
        QVERIFY(receiver.register_stream(list).ok());
        QVERIFY(sender.push({{buffer.stream, buffer.epoch, sender_id, 1}, "b1"}, PushDurability::durable).ok());
        QVERIFY(sender.push({{buffer.stream, buffer.epoch, sender_id, 2}, "b2"}, PushDurability::durable).ok());
        QVERIFY(sender.push({{list.stream, list.epoch, sender_id, 1}, "l1"}, PushDurability::durable).ok());
        auto idle = receiver.poll(buffer, PollBudget{});
        QVERIFY(idle.ok()); QCOMPARE(idle.value().offered, size_t(0));

        int buffer_calls = 0;
        bool payload_ok = true;
        int receipt_updates = 0;
        auto receipt_subscription = sender.subscribe_receipts([&](const StreamReceipt& r) {
            if (r.peer == receiver_id && r.through.producer == sender_id) ++receipt_updates;
        });
        QVERIFY(receipt_subscription.ok());
        auto subscribed = receiver.subscribe(buffer, [&](const InboundDelivery& d) {
            ++buffer_calls;
            payload_ok = payload_ok && (d.payload == "b1" || d.payload == "b2");
            return buffer_calls == 1 ? DeliveryDecision::retry()
                                     : DeliveryDecision::commit(RecoveryPoint::replay_from_beginning());
        });
        QVERIFY(subscribed.ok());
        PollBudget budget; budget.max_records = 1;
        auto first = receiver.poll(buffer, budget);
        QVERIFY(first.ok()); QCOMPARE(first.value().retried, size_t(1));
        auto no_receipt = sender.receipts(buffer);
        QVERIFY(no_receipt.ok()); QVERIFY(no_receipt.value().empty());
        auto second = receiver.poll(buffer, budget);
        QVERIFY(second.ok()); QCOMPARE(second.value().committed, size_t(1));
        QVERIFY(second.value().budget_limited);
        auto third = receiver.poll(buffer, budget);
        QVERIFY(third.ok()); QCOMPARE(third.value().committed, size_t(1));
        QVERIFY(payload_ok);
        int list_calls = 0;
        auto list_sub = receiver.subscribe(list, [&](const InboundDelivery&) {
            ++list_calls;
            if (list_calls == 1)
                return DeliveryDecision::quarantine(Error(ErrorCode::malformed, "deferred list payload"));
            return DeliveryDecision::commit(RecoveryPoint::checkpoint("list-recovered"));
        });
        QVERIFY(list_sub.ok());
        auto quarantined = receiver.poll(list);
        QVERIFY(quarantined.ok()); QCOMPARE(quarantined.value().quarantined, size_t(1));
        QVERIFY(sender.poll(list).ok());
        QVERIFY(sender.receipts(list).value().empty());
        auto list_poll = receiver.poll(list);
        QVERIFY(list_poll.ok()); QCOMPARE(list_poll.value().committed, size_t(1));
        QCOMPARE(sender.poll(buffer).value().committed, size_t(0));
        QCOMPARE(sender.poll(list).value().committed, size_t(0));
        QCOMPARE(receipt_updates, 2);
        QVERIFY(sender.receipts(buffer).value().size() == 1);
        QCOMPARE(sender.receipts(buffer).value()[0].through.sequence, uint64_t(2));
        QVERIFY(sender.receipts(list).value().size() == 1);
        QCOMPARE(sender.receipts(buffer).value()[0].through.producer, sender_id);
        QVERIFY(receiver.unsubscribe(subscribed.value()).ok());
        QVERIFY(receiver.unsubscribe(list_sub.value()).ok());
        QVERIFY(sender.unsubscribe(receipt_subscription.value()).ok());
    }

    void file_opstream_receipt_survives_receiver_restart() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const std::filesystem::path shared = tmp.path().toStdString();
        const auto sender_id = actor(51), receiver_id = actor(52);
        const StreamScope scope{"buffer:restart", 3};
        FileOpStream sender(shared, sender_id);
        QVERIFY(sender.start().ok()); QVERIFY(sender.register_stream(scope).ok());
        QVERIFY(sender.push({{scope.stream, scope.epoch, sender_id, 1}, "payload"}, PushDurability::durable).ok());
        {
            FileOpStream receiver(shared, receiver_id);
            QVERIFY(receiver.start().ok()); QVERIFY(receiver.register_stream(scope).ok());
            int calls = 0;
            QVERIFY(receiver.subscribe(scope, [&](const InboundDelivery&) {
                ++calls; return DeliveryDecision::commit(RecoveryPoint::checkpoint("durable-app-state"));
            }).ok());
            auto p = receiver.poll(scope); QVERIFY(p.ok()); QCOMPARE(p.value().committed, size_t(1));
            QCOMPARE(calls, 1);
        }
        FileOpStream restarted(shared, receiver_id);
        QVERIFY(restarted.start().ok()); QVERIFY(restarted.register_stream(scope).ok());
        int calls = 0;
        QVERIFY(restarted.subscribe(scope, [&](const InboundDelivery&) {
            ++calls; return DeliveryDecision::commit(RecoveryPoint::replay_from_beginning());
        }).ok());
        auto replay = restarted.poll(scope); QVERIFY(replay.ok());
        QCOMPARE(replay.value().committed, size_t(0)); QCOMPARE(calls, 0);
        QVERIFY(sender.poll(scope).ok());
        auto frontier = sender.receipts(scope);
        QVERIFY(frontier.ok()); QCOMPARE(frontier.value().size(), size_t(1));
        QCOMPARE(frontier.value()[0].through.sequence, uint64_t(1));
    }

    void file_opstream_sender_restart_recovers_next_sequence() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const std::filesystem::path shared = tmp.path().toStdString();
        const auto sender_id = actor(61);
        const StreamScope scope{"buffer:sender-restart", 9};
        {
            FileOpStream sender(shared, sender_id);
            QVERIFY(sender.start().ok()); QVERIFY(sender.register_stream(scope).ok());
            QVERIFY(sender.push({{scope.stream, scope.epoch, sender_id, 1}, "first"}, PushDurability::durable).ok());
        }
        FileOpStream restarted(shared, sender_id);
        QVERIFY(restarted.start().ok()); QVERIFY(restarted.register_stream(scope).ok());
        auto second = restarted.push({{scope.stream, scope.epoch, sender_id, 2}, "second"}, PushDurability::durable);
        QVERIFY2(second.ok(), second.ok() ? "" : std::string(second.error().message()).c_str());
        auto conflict = restarted.push({{scope.stream, scope.epoch, sender_id, 1}, "different"}, PushDurability::durable);
        QVERIFY(!conflict.ok()); QCOMPARE(conflict.error().code(), ErrorCode::actor_conflict);
    }

    void file_opstream_reconciles_uncertain_durable_retry() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const std::filesystem::path shared = tmp.path().toStdString();
        const auto sender_id = actor(71), receiver_id = actor(72);
        const StreamScope scope{"buffer:uncertain", 4};
        bool fail_sync_once = true;
        Storage::StepHook hook;
        hook.fail = [&](Storage::Step step, const std::filesystem::path& path)
            -> std::optional<Error> {
            if (fail_sync_once && step == Storage::Step::FileSync && path.extension() == ".open") {
                fail_sync_once = false;
                return Error(ErrorCode::io_error, "injected tail sync failure");
            }
            return std::nullopt;
        };
        WriterConfig failed_cfg; failed_cfg.step_hook = &hook;
        {
            FileOpStream sender(shared, sender_id, failed_cfg);
            QVERIFY(sender.start().ok()); QVERIFY(sender.register_stream(scope).ok());
            auto push = sender.push({{scope.stream, scope.epoch, sender_id, 1}, "durable-bytes"},
                                    PushDurability::durable);
            QVERIFY(!push.ok());
        }
        {
            FileOpStream restarted(shared, sender_id);
            QVERIFY(restarted.start().ok()); QVERIFY(restarted.register_stream(scope).ok());
            auto retried = restarted.push({{scope.stream, scope.epoch, sender_id, 1}, "durable-bytes"},
                                          PushDurability::durable);
            QVERIFY(retried.ok());
            QVERIFY(restarted.close().ok());
        }
        FileOpStream receiver(shared, receiver_id);
        QVERIFY(receiver.start().ok()); QVERIFY(receiver.register_stream(scope).ok());
        int calls = 0;
        QVERIFY(receiver.subscribe(scope, [&](const InboundDelivery& d) {
            ++calls;
            return d.payload == "durable-bytes"
                ? DeliveryDecision::commit(RecoveryPoint::replay_from_beginning())
                : DeliveryDecision::quarantine(Error(ErrorCode::malformed, "payload mismatch"));
        }).ok());
        auto polled = receiver.poll(scope);
        QVERIFY(polled.ok()); QCOMPARE(polled.value().committed, size_t(1));
        QCOMPARE(calls, 1);
        QVERIFY(receiver.close().ok());
    }

    void sender_restart_keeps_envelope_identity_for_live_receiver() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const std::filesystem::path shared = tmp.path().toStdString();
        StreamSync receiver(shared, "receiver", 2);
        receiver.register_stream("buf:doc", StreamSync::StreamType::AppendOnly);
        receiver.start();
        std::vector<std::string> received;
        receiver.set_on_inbound([&](const std::string&, uint16_t,
                                    const std::string& payload) {
            received.push_back(payload);
        });

        {
            StreamSync sender(shared, "sender", 1);
            sender.register_stream("buf:doc", StreamSync::StreamType::AppendOnly);
            sender.start();
            sender.push("buf:doc", "a");
            sender.flush();
            QCOMPARE(receiver.poll(), size_t(1));
        }
        {
            StreamSync sender(shared, "sender", 1);
            sender.register_stream("buf:doc", StreamSync::StreamType::AppendOnly);
            sender.start();
            sender.push("buf:doc", "b");
            sender.flush();
        }
        QCOMPARE(receiver.poll(), size_t(1));
        QCOMPARE(received, (std::vector<std::string>{"a", "b"}));
        QCOMPARE(receiver.entries("buf:doc").size(), size_t(2));
    }

    void missing_callback_retains_records_until_handler_is_installed() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const std::filesystem::path shared = tmp.path().toStdString();
        StreamSync sender(shared, "sender", 1);
        sender.register_stream("buf:doc", StreamSync::StreamType::AppendOnly);
        sender.start();
        sender.push("buf:doc", "payload");
        sender.flush();
        QVERIFY(!sender.last_error());

        StreamSync receiver(shared, "receiver", 2);
        receiver.register_stream("buf:doc", StreamSync::StreamType::AppendOnly);
        receiver.start();
        QCOMPARE(receiver.poll(), size_t(1));
        QCOMPARE(receiver.entries("buf:doc").size(), size_t(1));
        std::vector<std::string> received;
        receiver.set_on_inbound([&](const std::string&, uint16_t,
                                    const std::string& payload) {
            received.push_back(payload);
        });
        QCOMPARE(receiver.poll(), size_t(0));
        QCOMPARE(received, (std::vector<std::string>{"payload"}));
    }

    void missing_callback_does_not_publish_acks_and_idle_poll_does_not_rewrite_them() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const std::filesystem::path shared = tmp.path().toStdString();
        StreamSync sender(shared, "sender", 1);
        sender.register_stream("buf:doc", StreamSync::StreamType::AppendOnly);
        sender.start();
        CrdtEngine engine(CollabText::Test::test_counter(1));
        std::string wire;
        engine.setOnLocalOp([&](const Operation& op) {
            wire = checked_codec(encode_operation(op));
        });
        engine.insert(0, "x");
        QVERIFY(!wire.empty());
        sender.push("buf:doc", wire);
        sender.flush();

        StreamSync receiver(shared, "receiver", 2);
        receiver.register_stream("buf:doc", StreamSync::StreamType::AppendOnly);
        receiver.start();
        QCOMPARE(receiver.poll(), size_t(1));
        const auto ack_path = shared / "replicas" / "receiver" / "acks.json";
        QVERIFY(!std::filesystem::exists(ack_path));

        receiver.set_on_inbound([](const std::string&, uint16_t, const std::string&) {});
        QCOMPARE(receiver.poll(), size_t(0));
        QVERIFY(std::filesystem::exists(ack_path));
        const auto before = std::filesystem::last_write_time(ack_path);
        QCOMPARE(receiver.poll(), size_t(0));
        QVERIFY(std::filesystem::last_write_time(ack_path) == before);
    }

    void throwing_callback_leaves_delivery_available_for_retry() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const std::filesystem::path shared = tmp.path().toStdString();
        StreamSync sender(shared, "sender", 1);
        sender.register_stream("buf:doc", StreamSync::StreamType::AppendOnly);
        sender.start();
        sender.push("buf:doc", "payload");
        sender.flush();

        StreamSync receiver(shared, "receiver", 2);
        receiver.register_stream("buf:doc", StreamSync::StreamType::AppendOnly);
        receiver.start();
        receiver.set_on_inbound([](const std::string&, uint16_t, const std::string&) {
            throw std::runtime_error("temporary application failure");
        });
        QCOMPARE(receiver.poll(), size_t(0));
        QVERIFY(receiver.last_error().has_value());

        std::vector<std::string> received;
        receiver.set_on_inbound([&](const std::string&, uint16_t,
                                    const std::string& payload) {
            received.push_back(payload);
        });
        QCOMPARE(receiver.poll(), size_t(1));
        QCOMPARE(received, (std::vector<std::string>{"payload"}));
    }

    // ── catch-up replay on first attach ──────────────────────────────────────
    //
    // Replica A runs and accumulates 10 ops. Replica B attaches fresh against
    // the same shared folder — no read cursors exist — so its first poll()
    // starts from the beginning of replica A's segment files and delivers all
    // 10 ops. Final text must match.

    void catch_up_replay_on_first_attach() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        std::filesystem::path shared = tmp.path().toStdString();

        // ── Replica A: accumulate history ─────────────────────────────────────
        StreamSync sync1(shared, "replica-A", /*replica_id=*/1);
        sync1.register_stream("buf:doc", StreamSync::StreamType::AppendOnly);
        sync1.start();

        CrdtEngine engine1(CollabText::Test::test_counter(1));
        engine1.setOnLocalOp([&](const Operation& op) {
            sync1.push("buf:doc", checked_codec(encode_operation(op)));
        });

        // 10 single-character inserts
        for (int i = 0; i < 10; ++i) {
            engine1.insert(i, std::string(1, static_cast<char>('0' + i)));
        }
        sync1.flush();

        QCOMPARE(engine1.text(), std::string("0123456789"));

        // ── Replica B: fresh attach, no prior cursors ─────────────────────────
        StreamSync sync2(shared, "replica-B", /*replica_id=*/2);
        sync2.register_stream("buf:doc", StreamSync::StreamType::AppendOnly);
        sync2.start();

        CrdtEngine engine2(CollabText::Test::test_counter(2));
        sync2.set_on_inbound([&](const std::string& /*stream*/, uint16_t replica_id,
                                 const std::string& payload) {
            if (replica_id == 2) return;  // skip own ops (none here)
            auto decoded = decode_operation(payload);
            QVERIFY(decoded.ok());
            engine2.applyRemoteOp(decoded.value());
        });

        // First poll — must replay all of replica A's history
        sync2.poll();

        // Both engines converge
        QVERIFY2(!engine2.text().empty(),
                 "engine2 should have received replica A's history");
        QCOMPARE(engine2.text(), engine1.text());
    }

    // ── catch-up then incremental sync ───────────────────────────────────────
    //
    // After the initial catch-up, subsequent ops from replica A are delivered
    // incrementally on the next flush/poll cycle without re-delivering history.

    void catch_up_then_incremental_sync() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        std::filesystem::path shared = tmp.path().toStdString();

        // ── Replica A: accumulate history ─────────────────────────────────────
        StreamSync sync1(shared, "replica-A", /*replica_id=*/1);
        sync1.register_stream("buf:doc", StreamSync::StreamType::AppendOnly);
        sync1.start();

        CrdtEngine engine1(CollabText::Test::test_counter(1));
        engine1.setOnLocalOp([&](const Operation& op) {
            sync1.push("buf:doc", checked_codec(encode_operation(op)));
        });

        // Initial 5 ops
        for (int i = 0; i < 5; ++i) {
            engine1.insert(i, std::string(1, static_cast<char>('a' + i)));
        }
        sync1.flush();

        // ── Replica B: fresh attach and catch-up ──────────────────────────────
        StreamSync sync2(shared, "replica-B", /*replica_id=*/2);
        sync2.register_stream("buf:doc", StreamSync::StreamType::AppendOnly);
        sync2.start();

        CrdtEngine engine2(CollabText::Test::test_counter(2));
        int apply_count = 0;
        sync2.set_on_inbound([&](const std::string& /*stream*/, uint16_t replica_id,
                                 const std::string& payload) {
            if (replica_id == 2) return;
            auto decoded = decode_operation(payload);
            QVERIFY(decoded.ok());
            engine2.applyRemoteOp(decoded.value());
            ++apply_count;
        });

        // First poll — catch-up
        sync2.poll();

        QCOMPARE(engine2.text(), engine1.text());
        int ops_after_catchup = apply_count;
        QVERIFY(ops_after_catchup > 0);

        // ── Replica A: new op after catch-up ──────────────────────────────────
        engine1.insert(5, "Z");
        sync1.flush();

        // Second poll — incremental delivery of only the new op
        sync2.poll();

        QCOMPARE(engine2.text(), engine1.text());
        // At least one more op was applied compared to after catch-up
        QVERIFY(apply_count > ops_after_catchup);
    }
};

QTEST_MAIN(TestOpStreamReplay)
#include "tst_opstream_replay.moc"
