/// Contract tests for P07's transactional transport surface. This in-memory
/// adapter is a test double; it makes no disk or remote durability claim.
#include <collabtext/OpStream.h>

#include <QTest>

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace CollabText;

namespace {

ActorId actor(std::string_view hex) {
    auto parsed = ActorId::parse(hex);
    Q_ASSERT(parsed.ok());
    return parsed.value();
}

const ActorId kAlice = actor("01000000000000000000000000000001");
const ActorId kBob   = actor("02000000000000000000000000000002");
const ActorId kCara  = actor("03000000000000000000000000000003");

bool valid_stream_name(std::string_view name) {
    if (name.empty() || name.size() > 128) return false;
    for (const unsigned char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ':'))
            return false;
    return true;
}

std::string scope_key(const StreamScope& scope) {
    return scope.stream + "#" + std::to_string(scope.epoch);
}
std::string producer_key(const DeliveryToken& token) {
    return scope_key({token.stream, token.epoch}) + "#" + token.producer.to_string();
}
std::string token_key(const DeliveryToken& token) {
    return producer_key(token) + "#" + std::to_string(token.sequence);
}

class MemoryTransport;
class MemoryHub {
public:
    std::vector<MemoryTransport*> members;
    void publish_receipt(const StreamReceipt& receipt);
};

class MemoryTransport final : public OpStream {
public:
    MemoryTransport(MemoryHub& hub, ActorId self) : m_hub(hub), m_self(self) {}

    Result<void> register_stream(const StreamScope& scope) override {
        if (m_in_callback) return Error(ErrorCode::writer_conflict, "reentrant mutation");
        if (!valid_stream_name(scope.stream) || scope.epoch == 0)
            return Error(ErrorCode::malformed, "invalid stream scope");
        auto [it, inserted] = m_scopes.emplace(scope.stream, scope.epoch);
        if (!inserted && it->second != scope.epoch)
            return Error(ErrorCode::wrong_epoch, "stream already registered at another epoch");
        return Result<void>::success();
    }

    Result<void> push(const OutboundDelivery& delivery, PushDurability durability) override {
        if (m_in_callback) return Error(ErrorCode::writer_conflict, "reentrant mutation");
        const auto& token = delivery.token;
        if (auto valid = validate_scope({token.stream, token.epoch}); !valid.ok()) return valid.error();
        if (token.producer != m_self || token.sequence == 0)
            return Error(ErrorCode::invalid_identity, "invalid producer or envelope sequence");
        const auto key = token_key(token);
        if (auto existing = m_outbox.find(key); existing != m_outbox.end()) {
            if (existing->second.payload != delivery.payload)
                return Error(ErrorCode::actor_conflict, "delivery token reused with different bytes");
            if (durability == PushDurability::durable) {
                m_durable_outbox[key] = existing->second;
                m_volatile_outbox.erase(key);
            }
            return Result<void>::success();
        }
        auto& last = m_published[producer_key(token)];
        if (last == UINT64_MAX)
            return Error(ErrorCode::counter_exhausted, "delivery sequence exhausted");
        if (token.sequence != last + 1)
            return Error(ErrorCode::counter_regression, "delivery sequence must be contiguous");
        last = token.sequence;
        m_outbox.emplace(key, delivery);
        if (durability == PushDurability::durable) m_durable_outbox.emplace(key, delivery);
        else m_volatile_outbox.emplace(key, delivery);
        for (auto* peer : m_hub.members) {
            if (peer == this) continue;
            auto peer_scope = peer->m_scopes.find(token.stream);
            if (peer_scope != peer->m_scopes.end() && peer_scope->second == token.epoch)
                peer->m_inbox.push_back({token, delivery.payload});
        }
        return Result<void>::success();
    }

    Result<SubscriptionId> subscribe(const StreamScope& scope, InboundHandler handler) override {
        if (m_in_callback) return Error(ErrorCode::writer_conflict, "reentrant mutation");
        if (auto valid = validate_scope(scope); !valid.ok()) return valid.error();
        if (!handler) return Error(ErrorCode::malformed, "empty inbound handler");
        const auto key = scope_key(scope);
        if (m_inbound.contains(key))
            return Error(ErrorCode::writer_conflict, "inbound handler already registered");
        auto id = allocate_subscription();
        if (!id.ok()) return id.error();
        m_inbound.emplace(key, InboundSubscription{id.value(), std::move(handler)});
        return id.value();
    }

    Result<SubscriptionId> subscribe_receipts(ReceiptHandler handler) override {
        if (m_in_callback) return Error(ErrorCode::writer_conflict, "reentrant mutation");
        if (!handler) return Error(ErrorCode::malformed, "empty receipt handler");
        auto id = allocate_subscription();
        if (!id.ok()) return id.error();
        m_receipt_handlers.emplace(id.value(), std::move(handler));
        return id.value();
    }

    Result<void> unsubscribe(SubscriptionId id) override {
        for (auto it = m_inbound.begin(); it != m_inbound.end(); ++it) {
            if (it->second.id == id) {
                m_inbound.erase(it);
                return Result<void>::success();
            }
        }
        m_receipt_handlers.erase(id);
        return Result<void>::success();
    }

    Result<PollReport> poll(const StreamScope& scope, PollBudget budget = {}) override {
        if (m_in_callback) return Error(ErrorCode::writer_conflict, "reentrant poll");
        if (auto valid = validate_scope(scope); !valid.ok()) return valid.error();
        if (budget.max_records == 0 || budget.max_payload_bytes == 0)
            return Error(ErrorCode::limit_exceeded, "poll budget must be positive");
        PollReport report;
        auto subscription = m_inbound.find(scope_key(scope));
        if (subscription == m_inbound.end()) return report;
        std::set<std::string> blocked;
        for (size_t i = 0; i < m_inbox.size();) {
            auto& delivery = m_inbox[i];
            if (delivery.token.stream != scope.stream || delivery.token.epoch != scope.epoch) {
                ++i;
                continue;
            }
            const auto key = producer_key(delivery.token);
            if (blocked.contains(key) || m_quarantined.contains(token_key(delivery.token))) {
                blocked.insert(key);
                ++i;
                continue;
            }
            const auto expected = m_frontiers[key] + 1;
            if (delivery.token.sequence != expected) {
                blocked.insert(key);
                ++i;
                continue;
            }
            if (report.offered == budget.max_records ||
                delivery.payload.size() > budget.max_payload_bytes - report.payload_bytes) {
                report.budget_limited = true;
                break;
            }
            ++report.offered;
            report.payload_bytes += delivery.payload.size();
            const auto handler = subscription->second.handler;
            DeliveryDecision decision = DeliveryDecision::retry();
            try {
                m_in_callback = true;
                decision = handler(delivery);
            } catch (...) {
                decision = DeliveryDecision::retry();
            }
            m_in_callback = false;
            if (decision.kind() == DeliveryDecision::Kind::commit) {
                if (!decision.recovery_point() || !decision.recovery_point()->valid())
                    return Error(ErrorCode::malformed, "commit requires a valid recovery point");
                m_frontiers[key] = delivery.token.sequence;
                StreamReceipt receipt{m_self, delivery.token};
                m_hub.publish_receipt(receipt);
                m_inbox.erase(m_inbox.begin() + static_cast<std::ptrdiff_t>(i));
                ++report.committed;
            } else if (decision.kind() == DeliveryDecision::Kind::retry) {
                ++report.retried;
                blocked.insert(key);
                ++i;
            } else {
                if (!decision.error())
                    return Error(ErrorCode::malformed, "quarantine requires an error");
                m_quarantined.insert(token_key(delivery.token));
                m_quarantine_reasons[token_key(delivery.token)] = decision.error();
                ++report.quarantined;
                blocked.insert(key);
                ++i;
            }
        }
        return report;
    }

    Result<std::vector<StreamReceipt>> receipts(const StreamScope& scope) const override {
        if (auto valid = validate_scope(scope); !valid.ok()) return valid.error();
        std::vector<StreamReceipt> result;
        for (const auto& [key, receipt] : m_receipts) {
            (void)key;
            if (receipt.through.stream == scope.stream && receipt.through.epoch == scope.epoch)
                result.push_back(receipt);
        }
        return result;
    }

    [[nodiscard]] size_t outbox_size() const { return m_outbox.size(); }
    [[nodiscard]] size_t durable_outbox_size() const { return m_durable_outbox.size(); }
    [[nodiscard]] size_t volatile_outbox_size() const { return m_volatile_outbox.size(); }
    void receive_receipt(const StreamReceipt& receipt) {
        if (receipt.through.producer != m_self ||
            !m_outbox.contains(token_key(receipt.through))) return;
        const auto key = receipt.peer.to_string() + "#" + producer_key(receipt.through);
        auto it = m_receipts.find(key);
        if (it != m_receipts.end() &&
            it->second.through.sequence >= receipt.through.sequence) return;
        m_receipts[key] = receipt;
        std::vector<SubscriptionId> ids;
        for (const auto& [id, callback] : m_receipt_handlers) {
            (void)callback;
            ids.push_back(id);
        }
        for (const auto id : ids) {
            auto callback = m_receipt_handlers.find(id);
            if (callback == m_receipt_handlers.end()) continue;
            try {
                m_in_callback = true;
                callback->second(receipt);
            } catch (...) {
                // Receipt state is already committed; observers can query again.
            }
            m_in_callback = false;
        }
    }

private:
    struct InboundSubscription { SubscriptionId id; InboundHandler handler; };

    Result<SubscriptionId> allocate_subscription() {
        if (m_next_subscription == 0)
            return Error(ErrorCode::counter_exhausted, "subscription ids exhausted");
        return m_next_subscription++;
    }

    Result<void> validate_scope(const StreamScope& scope) const {
        auto it = m_scopes.find(scope.stream);
        if (it == m_scopes.end()) return Error(ErrorCode::unknown_stream, "unknown stream");
        if (it->second != scope.epoch) return Error(ErrorCode::wrong_epoch, "wrong stream epoch");
        return Result<void>::success();
    }

    MemoryHub& m_hub;
    ActorId m_self;
    std::map<std::string, uint64_t> m_scopes;
    std::map<std::string, uint64_t> m_published;
    std::map<std::string, uint64_t> m_frontiers;
    std::map<std::string, OutboundDelivery> m_outbox;
    std::map<std::string, OutboundDelivery> m_durable_outbox;
    std::map<std::string, OutboundDelivery> m_volatile_outbox;
    std::map<std::string, StreamReceipt> m_receipts;
    std::map<std::string, InboundSubscription> m_inbound;
    std::map<SubscriptionId, ReceiptHandler> m_receipt_handlers;
    std::vector<InboundDelivery> m_inbox;
    std::set<std::string> m_quarantined;
    std::map<std::string, Error> m_quarantine_reasons;
    SubscriptionId m_next_subscription = 1;
    bool m_in_callback = false;
};

void join(MemoryHub& hub, MemoryTransport& member) { hub.members.push_back(&member); }
void MemoryHub::publish_receipt(const StreamReceipt& receipt) {
    for (auto* member : members) member->receive_receipt(receipt);
}

DeliveryToken token(std::string stream, ActorId producer, uint64_t sequence, uint64_t epoch = 1) {
    return {std::move(stream), epoch, producer, sequence};
}

} // namespace

class TestTransportCommitContract : public QObject {
    Q_OBJECT
private slots:
    void no_handler_and_retry_do_not_publish_receipts_then_durable_commit_does() {
        MemoryHub hub;
        MemoryTransport sender(hub, kAlice), receiver(hub, kBob);
        join(hub, sender); join(hub, receiver);
        const StreamScope scope{"text", 1};
        QVERIFY(sender.register_stream(scope).ok());
        QVERIFY(receiver.register_stream(scope).ok());
        OutboundDelivery message{token("text", kAlice, 1), "operation-one"};
        QVERIFY(sender.push(message, PushDurability::durable).ok());
        QCOMPARE(sender.outbox_size(), size_t(1));
        QCOMPARE(sender.durable_outbox_size(), size_t(1));
        QCOMPARE(sender.volatile_outbox_size(), size_t(0));

        auto idle = receiver.poll(scope);
        QVERIFY(idle.ok());
        QCOMPARE(idle.value().offered, size_t(0));
        QVERIFY(sender.receipts(scope).value().empty());

        bool application_state_durable = false;
        bool incoming_token_matches = false;
        bool reentrant_poll_rejected = false;
        auto registration = receiver.subscribe(scope, [&](const InboundDelivery& incoming) {
            incoming_token_matches = incoming.token == message.token;
            auto nested_poll = receiver.poll(scope);
            reentrant_poll_rejected = !nested_poll.ok() &&
                nested_poll.error().code() == ErrorCode::writer_conflict;
            if (!application_state_durable) return DeliveryDecision::retry();
            return DeliveryDecision::commit(RecoveryPoint::checkpoint("state-1"));
        });
        QVERIFY(registration.ok());
        auto retry = receiver.poll(scope);
        QVERIFY(retry.ok());
        QCOMPARE(retry.value().retried, size_t(1));
        QVERIFY(incoming_token_matches);
        QVERIFY(reentrant_poll_rejected);
        QVERIFY(sender.receipts(scope).value().empty());

        application_state_durable = true;
        size_t receipt_updates = 0;
        bool reentrant_push_rejected = false;
        auto receipt_sub = sender.subscribe_receipts([&](const StreamReceipt& receipt) {
            ++receipt_updates;
            QCOMPARE(receipt.peer, kBob);
            QVERIFY(receipt.through == message.token);
            auto nested_push = sender.push(
                {token("text", kAlice, 2), "reentrant"}, PushDurability::queued);
            reentrant_push_rejected = !nested_push.ok() &&
                nested_push.error().code() == ErrorCode::writer_conflict;
        });
        QVERIFY(receipt_sub.ok());
        auto committed = receiver.poll(scope);
        QVERIFY(committed.ok());
        QCOMPARE(committed.value().committed, size_t(1));
        QCOMPARE(receipt_updates, size_t(1));
        QVERIFY(reentrant_push_rejected);
        auto receipts = sender.receipts(scope);
        QVERIFY(receipts.ok());
        QCOMPARE(receipts.value().size(), size_t(1));
        QCOMPARE(receipts.value().front().through, message.token);
    }

    void same_actor_counter_values_on_distinct_streams_and_producers_are_isolated() {
        MemoryHub hub;
        MemoryTransport a(hub, kAlice), b(hub, kBob), c(hub, kCara);
        join(hub, a); join(hub, b); join(hub, c);
        const StreamScope text{"text", 1}, comments{"comments", 1};
        for (auto* transport : {&a, &b, &c}) {
            QVERIFY(transport->register_stream(text).ok());
            QVERIFY(transport->register_stream(comments).ok());
        }
        auto accept_all = [](const InboundDelivery&) {
            return DeliveryDecision::commit(RecoveryPoint::replay_from_beginning());
        };
        QVERIFY(c.subscribe(text, accept_all).ok());
        QVERIFY(c.subscribe(comments, accept_all).ok());
        QVERIFY(a.push({token("text", kAlice, 1), "a-text"}, PushDurability::queued).ok());
        QVERIFY(b.push({token("text", kBob, 1), "b-text"}, PushDurability::durable).ok());
        QCOMPARE(a.volatile_outbox_size(), size_t(1));
        QCOMPARE(a.durable_outbox_size(), size_t(0));
        QCOMPARE(b.durable_outbox_size(), size_t(1));
        QVERIFY(a.push({token("comments", kAlice, 1), "a-comment"}, PushDurability::durable).ok());
        QCOMPARE(c.poll(text).value().committed, size_t(2));
        QCOMPARE(c.poll(comments).value().committed, size_t(1));
        const auto a_text_receipts = a.receipts(text).value();
        const auto b_text_receipts = b.receipts(text).value();
        const auto a_comment_receipts = a.receipts(comments).value();
        QCOMPARE(a_text_receipts.size(), size_t(1));
        QCOMPARE(b_text_receipts.size(), size_t(1));
        QCOMPARE(a_comment_receipts.size(), size_t(1));
        QCOMPARE(a_text_receipts.front().peer, kCara);
        QCOMPARE(a_text_receipts.front().through.producer, kAlice);
        QCOMPARE(a_text_receipts.front().through.sequence, uint64_t(1));
        QCOMPARE(b_text_receipts.front().peer, kCara);
        QCOMPARE(b_text_receipts.front().through.producer, kBob);
        QCOMPARE(a_comment_receipts.front().through.stream, std::string("comments"));
    }

    void quarantine_preserves_blocking_and_never_acknowledges_rejected_or_later_records() {
        MemoryHub hub;
        MemoryTransport sender(hub, kAlice), receiver(hub, kBob);
        join(hub, sender); join(hub, receiver);
        const StreamScope scope{"text", 1};
        QVERIFY(sender.register_stream(scope).ok());
        QVERIFY(receiver.register_stream(scope).ok());
        QVERIFY(sender.push({token("text", kAlice, 1), "bad"}, PushDurability::durable).ok());
        QVERIFY(sender.push({token("text", kAlice, 2), "later"}, PushDurability::durable).ok());
        QVERIFY(receiver.subscribe(scope, [](const InboundDelivery&) {
            return DeliveryDecision::quarantine(Error(ErrorCode::malformed, "invalid operation"));
        }).ok());
        auto report = receiver.poll(scope);
        QVERIFY(report.ok());
        QCOMPARE(report.value().quarantined, size_t(1));
        QCOMPARE(report.value().offered, size_t(1));
        QVERIFY(sender.receipts(scope).value().empty());
        auto repeated = receiver.poll(scope);
        QVERIFY(repeated.ok());
        QCOMPARE(repeated.value().offered, size_t(0));
        QVERIFY(sender.receipts(scope).value().empty());
    }

    void poll_budget_and_subscription_lifetime_are_enforced() {
        MemoryHub hub;
        MemoryTransport sender(hub, kAlice), receiver(hub, kBob);
        join(hub, sender); join(hub, receiver);
        const StreamScope scope{"text", 1};
        QVERIFY(sender.register_stream(scope).ok());
        QVERIFY(receiver.register_stream(scope).ok());
        QVERIFY(sender.push({token("text", kAlice, 1), "1234"}, PushDurability::durable).ok());
        QVERIFY(sender.push({token("text", kAlice, 2), "5678"}, PushDurability::durable).ok());
        size_t calls = 0;
        SubscriptionId subscription = 0;
        bool unsubscribe_succeeded = false;
        auto registered = receiver.subscribe(scope, [&](const InboundDelivery&) {
            ++calls;
            unsubscribe_succeeded = receiver.unsubscribe(subscription).ok();
            return DeliveryDecision::commit(RecoveryPoint::replay_from_beginning());
        });
        QVERIFY(registered.ok());
        subscription = registered.value();
        auto first = receiver.poll(scope, PollBudget{10, 4});
        QVERIFY(first.ok());
        QCOMPARE(first.value().committed, size_t(1));
        QCOMPARE(first.value().payload_bytes, size_t(4));
        QVERIFY(first.value().budget_limited);
        QCOMPARE(calls, size_t(1));
        QVERIFY(unsubscribe_succeeded);
        QVERIFY(receiver.unsubscribe(subscription).ok()); // repeated removal is idempotent
        auto after_unsubscribe = receiver.poll(scope);
        QVERIFY(after_unsubscribe.ok());
        QCOMPARE(after_unsubscribe.value().offered, size_t(0));
        QCOMPARE(sender.receipts(scope).value().front().through.sequence, uint64_t(1));

        auto empty = receiver.poll(scope, PollBudget{0, 10});
        QVERIFY(!empty.ok());
        QCOMPARE(empty.error().code(), ErrorCode::limit_exceeded);
    }

    void unknown_scopes_token_conflicts_and_sequence_gaps_are_typed() {
        MemoryHub hub;
        MemoryTransport sender(hub, kAlice);
        join(hub, sender);
        const StreamScope scope{"text", 1};
        auto unknown_push = sender.push(
            {token("missing", kAlice, 1), "x"}, PushDurability::durable);
        QVERIFY(!unknown_push.ok());
        QCOMPARE(unknown_push.error().code(), ErrorCode::unknown_stream);
        const StreamScope missing{"missing", 1};
        auto unknown_poll = sender.poll(missing);
        auto unknown_receipts = sender.receipts(missing);
        auto unknown_subscription = sender.subscribe(missing, [](const InboundDelivery&) {
            return DeliveryDecision::retry();
        });
        QVERIFY(!unknown_poll.ok());
        QVERIFY(!unknown_receipts.ok());
        QVERIFY(!unknown_subscription.ok());
        QCOMPARE(unknown_poll.error().code(), ErrorCode::unknown_stream);
        QCOMPARE(unknown_receipts.error().code(), ErrorCode::unknown_stream);
        QCOMPARE(unknown_subscription.error().code(), ErrorCode::unknown_stream);
        QVERIFY(!sender.register_stream({std::string("bad\0name", 8), 1}).ok());
        QVERIFY(!sender.register_stream({std::string("\xC0\xAF", 2), 1}).ok());
        QVERIFY(!sender.register_stream({std::string(129, 'a'), 1}).ok());
        QVERIFY(sender.register_stream(scope).ok());
        QVERIFY(!sender.register_stream({"text", 2}).ok());
        QVERIFY(!sender.push({token("text", kAlice, 2), "gap"}, PushDurability::durable).ok());
        QVERIFY(sender.push({token("text", kAlice, 1), "original"}, PushDurability::durable).ok());
        QVERIFY(sender.push({token("text", kAlice, 1), "original"}, PushDurability::durable).ok());
        auto conflict = sender.push({token("text", kAlice, 1), "different"}, PushDurability::durable);
        QVERIFY(!conflict.ok());
        QCOMPARE(conflict.error().code(), ErrorCode::actor_conflict);
    }
};

QTEST_GUILESS_MAIN(TestTransportCommitContract)
#include "tst_transport_commit_contract.moc"
