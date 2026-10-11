/// tests/consumer-smoke/main.cpp — an independent consumer of the installed alpha.
///
/// Roadmap task A06a. This file is compiled against `CollabText::Core` resolved
/// from an install prefix, with no reference to the collabtext source tree, and
/// it is what "a consumer can point at the installed library" means as an
/// executable fact.
///
/// It is deliberately *not* a copy of A02a's alpha consumer. That one proves the
/// public surface is sufficient; this one proves the surface is *reachable* from
/// an install tree. Keeping them separate means a packaging regression and an API
/// regression fail as different failures instead of one hiding the other.
///
/// What it checks:
///   * every advertised public header compiles from the installation, alone
///     (the generated TUs beside this one);
///   * `find_package(collabtext CONFIG COMPONENTS core)` needs no Qt, no zstd and
///     no crypto provider, because the project that finds it never asks for them;
///   * the engine can be constructed, edited, transferred and undone using only
///     public types and functions;
///   * the capability manifest is installed next to the headers, because it is
///     part of the contract rather than a note in a repository.
///
/// It does not try anything the manifest says is unavailable. In particular it
/// does not resolve an anchor across a concurrent edit, persist across a reopen,
/// or reclaim anything.

#include <collabtext/ActorId.h>
#include <collabtext/ApplyOutcome.h>
#include <collabtext/CrdtEngine.h>
#ifdef CONSUMER_WANT_TRANSPORT
#include <collabtext/DocumentSession.h>
#endif
#include <collabtext/Engine.h>
#include <collabtext/IdListEngine.h>
#include <collabtext/IdListOperations.h>
#include <collabtext/OpStream.h>
#include <collabtext/Operations.h>
#include <collabtext/Serialization.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <chrono>

namespace {
// Test/consumer assertion, active in Release too. Never erases a failed codec.
template<class T> T checked_codec(CollabText::Result<T> result) {
    if (!result.ok()) {
        std::fprintf(stderr, "codec check failed: %.*s: %.*s\n",
            int(CollabText::to_string(result.error().code()).size()), CollabText::to_string(result.error().code()).data(),
            int(result.error().message().size()), result.error().message().data());
        std::abort();
    }
    return std::move(result).value();
}


int failures = 0;
void check(bool condition, const char *what);

#ifdef CONSUMER_WANT_TRANSPORT
void check_installed_branch_export() {
    using namespace CollabText;
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      ("collabtext-export-consumer-" + std::to_string(unique));
    DocumentSessionConfig config{root / "shared", root / "private",
                                 "consumer-export-doc", "text", 4};
    auto opened = DocumentSession::open(config);
    check(opened.ok(), "installed file-transport consumer opens a DocumentSession");
    if (!opened.ok()) return;
    auto session = std::move(opened).value();
    auto quarantined = session->quarantined_deliveries();
    check(quarantined.ok() && quarantined.value().empty(),
          "installed session exposes an empty quarantine snapshot initially");
    auto edit = session->apply_local({{0, 0}}, {"branch"});
    check(edit.ok(), "installed session accepts a local branch edit");
    LocalBranchExport metadata;
    size_t deliveries = 0;
    const auto exported = session->export_local_branch(
        metadata, [&](const OutboundDelivery& delivery) -> Result<void> {
            ++deliveries;
            check(delivery.token.stream == config.stream &&
                      delivery.token.epoch == config.epoch &&
                      delivery.token.producer == session->actor() &&
                      delivery.token.sequence == 1,
                  "branch export preserves its installed delivery token");
            return {};
        });
    check(exported.ok(), "installed session exports its local branch");
    check(metadata.document == config.document &&
              metadata.scope.stream == config.stream &&
              metadata.scope.epoch == config.epoch &&
              metadata.producer == session->actor() && deliveries == 1,
          "installed branch export reports exact document and original scope");
    check(session->close().ok(), "installed branch export session closes cleanly");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}
#endif

void check(bool condition, const char *what) {
    if (condition) return;
    std::fprintf(stderr, "consumer: FAILED %s\n", what);
    ++failures;
}

/// A consumer-owned transport implementing the installed interface with stable
/// envelope and callback types only. It is intentionally small and in-memory.
class ConsumerOpStream final : public CollabText::OpStream {
public:
    CollabText::Result<void> register_stream(const CollabText::StreamScope& scope) override {
        m_scope = scope;
        return {};
    }
    CollabText::Result<void> push(const CollabText::OutboundDelivery& delivery,
                                  CollabText::PushDurability) override {
        if (!m_scope || delivery.token.stream != m_scope->stream || delivery.token.epoch != m_scope->epoch)
            return CollabText::Error(CollabText::ErrorCode::unknown_stream, "scope mismatch");
        m_last = delivery;
        return {};
    }
    CollabText::Result<CollabText::SubscriptionId> subscribe(
        const CollabText::StreamScope& scope, CollabText::InboundHandler handler) override {
        if (!m_scope || scope != *m_scope)
            return CollabText::Error(CollabText::ErrorCode::unknown_stream, "scope mismatch");
        m_handler = std::move(handler);
        return CollabText::SubscriptionId{1};
    }
    CollabText::Result<CollabText::SubscriptionId> subscribe_receipts(
        CollabText::ReceiptHandler handler) override {
        m_receipt_handler = std::move(handler);
        return CollabText::SubscriptionId{2};
    }
    CollabText::Result<void> unsubscribe(CollabText::SubscriptionId id) override {
        if (id == 1) m_handler = {};
        if (id == 2) m_receipt_handler = {};
        return {};
    }
    CollabText::Result<CollabText::PollReport> poll(
        const CollabText::StreamScope& scope, CollabText::PollBudget) override {
        if (!m_scope || scope != *m_scope)
            return CollabText::Error(CollabText::ErrorCode::unknown_stream, "scope mismatch");
        return CollabText::PollReport{};
    }
    CollabText::Result<std::vector<CollabText::StreamReceipt>> receipts(
        const CollabText::StreamScope& scope) const override {
        if (!m_scope || scope != *m_scope)
            return CollabText::Error(CollabText::ErrorCode::unknown_stream, "scope mismatch");
        return std::vector<CollabText::StreamReceipt>{};
    }
private:
    std::optional<CollabText::StreamScope> m_scope;
    std::optional<CollabText::OutboundDelivery> m_last;
    CollabText::InboundHandler m_handler;
    CollabText::ReceiptHandler m_receipt_handler;
};

/// A consumer-supplied allocator. Durable identity needs a ReplicaState-backed
/// allocator, which A06a installs but this program does not use: it wants two
/// distinct peers in one process and says so rather than pretending otherwise.
class VolatileAllocator final : public CollabText::Crdt::IdentityAllocator {
public:
    explicit VolatileAllocator(const CollabText::ActorId &actor) : m_actor(actor) {}

    CollabText::Result<uint64_t> reserve_run(uint64_t count) override {
        return m_counter.reserve_run(count, "consumer smoke");
    }
    void observe(uint64_t counter) override {
        (void)m_counter.observe(counter, "consumer smoke");
    }
    uint64_t peek() const noexcept override { return m_counter.peek(); }
    const CollabText::ActorId &actor() const noexcept override { return m_actor; }

private:
    CollabText::ActorId m_actor;
    CollabText::CheckedCounter m_counter;
};

CollabText::ActorId actor(uint8_t n) {
    std::array<uint8_t, CollabText::ActorId::kBytes> bytes{};
    bytes[CollabText::ActorId::kBytes - 1] = n;
    return CollabText::ActorId::from_bytes(bytes);
}

} // namespace

int main() {
    using namespace CollabText;
    namespace Crdt = CollabText::Crdt;

    auto alice_counter = std::make_shared<VolatileAllocator>(actor(1));
    auto bob_counter = std::make_shared<VolatileAllocator>(actor(2));

    CrdtEngine alice(alice_counter);
    CrdtEngine bob(bob_counter);

    std::vector<std::string> from_alice;
    std::vector<std::string> from_bob;
    alice.setOnLocalOp([&](const Crdt::Operation &op) {
        from_alice.push_back(checked_codec(Crdt::encode_operation(op)));
    });
    bob.setOnLocalOp([&](const Crdt::Operation &op) {
        from_bob.push_back(checked_codec(Crdt::encode_operation(op)));
    });

    // Local edit, read, undo, redo -- offsets are UTF-16 code units.
    alice.insert(0, "hello");
    check(alice.text() == "hello", "a local insert reads back");
    check(alice.undo(), "undo succeeds");
    check(alice.text().empty(), "undo removes the edit");
    check(alice.redo(), "redo succeeds");
    check(alice.text() == "hello", "redo restores it");
    check(alice.set_max_undo_depth(32).ok() && alice.max_undo_depth() == 32,
          "installed text facade exposes a checked undo depth limit");
    check(alice.set_max_undo_bytes(4096).ok() && alice.max_undo_bytes() == 4096,
          "installed text facade exposes a checked undo byte limit");

    CrdtEngine batch_peer(std::make_shared<VolatileAllocator>(actor(4)));
    const auto batch_outcomes = batch_peer.apply_ops({});
    check(batch_outcomes.empty(), "empty text batch returns an empty ordered result");
    std::vector<Crdt::Operation> alice_batch;
    for (const std::string &bytes : from_alice) {
        auto decoded = Crdt::decode_operation(bytes);
        check(decoded.ok(), "batch source operation decodes");
        if (decoded) alice_batch.push_back(decoded.value());
    }
    const auto batch_results = batch_peer.apply_ops(alice_batch);
    check(batch_results.size() == alice_batch.size(), "text batch returns one result per operation");
    check(batch_peer.text() == alice.text(), "ordered text batch reaches the same final state");
    check(alice.set_max_undo_depth(0).ok(), "installed undo depth can be bounded to zero");
    const auto no_undo = alice.try_undo();
    check(no_undo.ok() && !no_undo.value().has_value(),
          "checked undo reports an empty history without fabricating an operation");

    bob.insert(0, "world");
    for (const std::string &bytes : from_alice) {
        auto decoded = Crdt::decode_operation(bytes);
        check(decoded.ok(), "an operation from a peer decodes");
        if (decoded) check(bob.apply_remote(decoded.value()).kind() == ApplyOutcomeKind::applied,
                           "text facade reports a typed first application");
    }
    for (const std::string &bytes : from_bob) {
        auto decoded = Crdt::decode_operation(bytes);
        check(decoded.ok(), "an operation from a peer decodes");
        if (decoded) check(alice.apply_remote(decoded.value()).kind() == ApplyOutcomeKind::applied,
                           "text facade reports a typed first application");
    }
    check(alice.text() == bob.text(), "two peers converge over the wire codec");
    // Concurrent inserts at one position interleave (F8); the manifest says so.
    check(alice.text().size() == bob.text().size(),
          "both peers hold the same number of characters");

    ConsumerOpStream stream;
    const StreamScope stream_scope{"smoke", 1};
    check(stream.register_stream(stream_scope).ok(), "consumer transport registers a scope");
    const DeliveryToken delivery_token{"smoke", 1, actor(1), 1};
    check(stream.push({delivery_token, "opaque"}, PushDurability::queued).ok(),
          "consumer transport accepts stable delivery values");
    auto subscription = stream.subscribe(stream_scope, [](const InboundDelivery&) {
        return DeliveryDecision::commit(RecoveryPoint::replay_from_beginning());
    });
    check(subscription.ok(), "consumer transport implements inbound subscription");
    auto poll = stream.poll(stream_scope, PollBudget{});
    check(poll.ok(), "consumer transport implements bounded polling");
    check(stream.receipts(stream_scope).ok(), "consumer transport exposes receipt snapshots");
    if (subscription.ok()) check(stream.unsubscribe(subscription.value()).ok(),
                                 "consumer transport removes a subscription");

    // A structural list, through the public facade.
    IdListEngine list(alice_counter);
    check(list.try_insert_after(Crdt::Anchor::min(), 7).ok(), "list insert succeeds");
    check(list.size() == 1, "the list has one visible element");
    const auto anchor = list.try_anchor_of(7);
    check(anchor.ok(), "the element has an anchor");
    if (anchor.ok()) {
        const Crdt::Anchor at = anchor.value();
        check(list.try_insert_after(at, 8).ok(), "list insert-after succeeds");
        check(list.try_remove_at(at).ok(), "list remove succeeds");
    }
    check(list.undo().has_value(), "list undo succeeds");
    const auto missing = list.try_anchor_of(4242);
    check(!missing.ok(), "an unknown element is an explicit miss");

    // Installed-only versioned codec contract, both primitives + gap metadata.
    const auto list_op = list.try_insert_after(Crdt::Anchor::min(), UINT64_MAX);
    check(list_op.ok(), "list creates full-width application value");
    if (list_op.ok()) {
        auto encoded = Crdt::encode_idlist_operation(list_op.value());
        check(encoded.ok(), "list encoding succeeds");
        if (encoded.ok()) {
            auto decoded = Crdt::decode_idlist_operation(encoded.value());
            check(decoded.ok(), "installed list decoder reads installed encoder");
            if (decoded.ok()) check(Crdt::idlist_op_identity(decoded.value()) ==
                                  Crdt::idlist_op_identity(list_op.value()), "list identity stays exact");
        }
    }
    Crdt::CounterGap gap{actor(1), UINT64_MAX - 1, 1, {}};
    auto gap_wire = Crdt::encode_counter_gap(gap);
    check(gap_wire.ok(), "full-width gap encoding succeeds");
    if (gap_wire.ok()) {
        auto decoded = Crdt::decode_counter_gap(gap_wire.value());
        check(decoded.ok(), "gap decodes through installed headers");
        if (decoded.ok()) check(decoded.value().first == gap.first && decoded.value().count == 1,
                               "gap metadata remains exact");
    }
    auto newer = Crdt::decode_operation("CTBO0003:{}");
    check(!newer.ok() && newer.error().code() == ErrorCode::unsupported_version,
          "newer format has a typed verdict");
    Crdt::Global frontier;
    frontier.observe({actor(1), UINT64_MAX - 1});
    auto frontier_wire = Crdt::encode_global(frontier);
    check(frontier_wire.ok(), "frontier encoding succeeds");
    if (frontier_wire.ok()) {
        auto decoded = Crdt::decode_global(frontier_wire.value());
        check(decoded.ok(), "frontier decoding succeeds");
        if (decoded.ok()) check(decoded.value() == frontier, "frontier stays exact");
    }

    auto scoped = CrdtEngine::create(std::make_shared<VolatileAllocator>(actor(3)),
                                    {"doc", "block", Primitive::text, 1});
    check(scoped.ok(), "installed scope factory is available");
    if (scoped.ok()) {
        auto& engine = *scoped.value();
        engine.insert(0, "ab");
        auto left = engine.boundary_at(1, Affinity::left);
        auto right = engine.boundary_at(1, Affinity::right);
        check(left.ok() && right.ok(), "installed boundary mint succeeds");
        if (left.ok() && right.ok()) {
            auto bytes = encode_position(left.value());
            check(bytes.ok(), "installed position encoder succeeds");
            engine.insert(1, "X");
            auto l = engine.resolve(left.value()), r = engine.resolve(right.value());
            check(l.ok() && r.ok(), "installed scoped resolution succeeds");
            if (l.ok() && r.ok()) check(l.value().byte_offset == 1 && r.value().byte_offset == 2,
                                      "installed exact affinity is 1/2");
            if (bytes.ok()) {
                auto decoded = decode_position_as<TextBoundary>(bytes.value());
                check(decoded.ok(), "installed token decoder succeeds");
                if (decoded.ok()) check(engine.resolve(decoded.value()).ok(), "decoded token owns its value");
            }
        }
    }

#ifdef CONSUMER_WANT_TRANSPORT
    check_installed_branch_export();
#endif

    if (failures == 0)
        std::printf("consumer: ok (text=%zu bytes, list=%zu ids)\n",
                    alice.text().size(), list.ids().size());
    else
        std::fprintf(stderr, "consumer: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
