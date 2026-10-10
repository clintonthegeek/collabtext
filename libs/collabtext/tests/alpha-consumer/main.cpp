/// alpha-consumer/main.cpp — a consumer of the alpha surface, and nothing else.
///
/// Roadmap task A02a: "Build a small plain C++ consumer that edits text and list
/// content and transfers actual available operations." Its acceptance is that
/// this program can create and edit both primitives using public types and
/// receive and apply operations **without inspecting fields**.
///
/// Three rules this file follows, and each one is a test:
///
///   1. It includes only `<collabtext/...>` headers. No `crdt/`, no `src/`. If
///      the public surface were not sufficient, this would not compile.
///   2. It never reads an operation's fields. Operations travel as values through
///      `encode_operation` / `decode_operation` and `ApplyOutcome`, which is the
///      shape the whole contract was designed around — a consumer that had to
///      read `EditOperation::timestamp` to make progress would prove the fields
///      were the contract after all.
///   3. It links `CollabText::Core` and nothing else: no Qt, no zstd, no test
///      support, no transport.
///
/// What it deliberately does not do is exercise anything unavailable. The
/// capability manifest beside this file says what the alpha can and cannot do,
/// and this program stays inside that list: it does not resolve anchors across
/// concurrent edits, persist across a reopen, or recover from a crash, because
/// those are recorded as unavailable rather than demonstrated optimistically.

#include <collabtext/ActorId.h>
#include <collabtext/ApplyOutcome.h>
#include <collabtext/CrdtEngine.h>
#include <collabtext/Engine.h>
#include <collabtext/IdListEngine.h>
#include <collabtext/IdListOperations.h>
#include <collabtext/Operations.h>
#include <collabtext/Serialization.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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

void check(bool condition, const char *what) {
    if (condition) return;
    std::fprintf(stderr, "alpha-consumer: FAILED %s\n", what);
    ++failures;
}

/// A caller-supplied allocator. A consumer that wants durable identity opens a
/// ReplicaState-backed allocator (A06a installs the adapter); this one is
/// explicitly volatile, which is the honest choice for a program that only wants
/// two distinct peers in one process.
class VolatileAllocator final : public CollabText::Crdt::IdentityAllocator {
public:
    explicit VolatileAllocator(const CollabText::ActorId &actor) : m_actor(actor) {}

    CollabText::Result<uint64_t> reserve_run(uint64_t count) override {
        return m_counter.reserve_run(count, "alpha consumer");
    }
    void observe(uint64_t counter) override {
        (void)m_counter.observe(counter, "alpha consumer");
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

/// Everything one peer emits, in order. A consumer's own log; the library never
/// keeps it for you.
struct Wire {
    std::vector<std::string> text_ops;
    std::vector<std::string> list_ops;

    void attach_text(CollabText::CrdtEngine &engine) {
        engine.setOnLocalOp([this](const CollabText::Crdt::Operation &op) {
            text_ops.push_back(checked_codec(CollabText::Crdt::encode_operation(op)));
        });
    }
    void attach_list(CollabText::IdListEngine &list) {
        list.set_on_local_op([this](const CollabText::Crdt::IdListOperation &op) {
            list_ops.push_back(checked_codec(CollabText::Crdt::encode_idlist_operation(op)));
        });
    }
};

/// Deliver everything one side produced to the other, and report what happened.
/// Nothing here inspects an operation's fields: the payload is opaque bytes, the
/// outcome is a typed verdict.
void deliver_text(const std::string &bytes, CollabText::CrdtEngine &target) {
    auto decoded = CollabText::Crdt::decode_operation(bytes);
    check(decoded.ok(), "an encoded text operation decodes");
    if (!decoded) return;
    check(checked_codec(CollabText::Crdt::encode_operation(decoded.value())) == bytes,
          "a text operation survives a round trip unchanged");
    const auto outcome = target.apply_remote(decoded.value());
    check(outcome.kind() == CollabText::ApplyOutcomeKind::applied,
          "a freshly delivered text operation applies");
}

void deliver_list(const std::string &bytes, CollabText::IdListEngine &target) {
    auto decoded = CollabText::Crdt::decode_idlist_operation(bytes);
    check(decoded.ok(), "an encoded list operation decodes");
    if (!decoded) return;
    const auto outcomes = target.apply_ops({decoded.value()});
    check(outcomes.size() == size_t(1), "one operation produces one outcome");
    if (outcomes.empty()) return;
    check(outcomes.front().kind() == CollabText::ApplyOutcomeKind::applied,
          "a freshly delivered operation applies");
}

} // namespace

int main() {
    using namespace CollabText;
    namespace Crdt = CollabText::Crdt;

    auto alice_counter = std::make_shared<VolatileAllocator>(actor(1));
    auto bob_counter = std::make_shared<VolatileAllocator>(actor(2));
    CrdtEngine alice(alice_counter);
    CrdtEngine bob(bob_counter);
    Wire alice_wire, bob_wire;
    alice_wire.attach_text(alice);
    bob_wire.attach_text(bob);

    // ---- text: local edits, reads, undo ----
    alice.insert(0, "hello");
    check(alice.length() == 5, "a local insert is visible to its author");
    check(alice.text() == "hello", "text reads back what was written");

    alice.insert(5, " world");
    check(alice.text() == "hello world", "a second insert appends");

    check(alice.undo(), "undo succeeds while there is history");
    check(alice.text() == "hello", "undo removes the most recent edit");
    check(alice.redo(), "redo succeeds after an undo");
    check(alice.text() == "hello world", "redo restores the edit");

    // Offsets are UTF-16 code units, and this says so rather than leaving the
    // consumer to discover it.
    alice.insert(0, "\xF0\x9F\x8E\x89");  // U+1F389, one code point, two units
    check(alice.length() == 13, "an astral insert counts as two UTF-16 units");

    // ---- text: transfer without touching fields ----
    for (const std::string &bytes : alice_wire.text_ops) deliver_text(bytes, bob);
    for (const std::string &bytes : bob_wire.text_ops) deliver_text(bytes, alice);
    check(alice.text() == bob.text(), "two peers converge on the same text");

    // A duplicate is reported as a duplicate, not applied twice.
    if (!alice_wire.text_ops.empty()) {
        const auto duplicate = bob.apply_remote(
            checked_codec(Crdt::decode_operation(alice_wire.text_ops.front())));
        check(duplicate.kind() == ApplyOutcomeKind::duplicate,
              "a re-delivered text operation is a duplicate");
        check(bob.text() == alice.text(), "a re-delivered operation is idempotent");
    }

    // ---- text: a refusal is a refusal, not an assert ----
    check(alice.apply_remote(Crdt::Operation{}).kind() == ApplyOutcomeKind::rejected,
          "a default-constructed operation is refused rather than applied");

    // ---- structural list: construction, edits, reads, undo ----
    IdListEngine list_a(alice_counter);
    IdListEngine list_b(bob_counter);
    Wire list_wire_a, list_wire_b;
    list_wire_a.attach_list(list_a);
    list_wire_b.attach_list(list_b);

    const auto head = Crdt::Anchor::min();
    check(list_a.try_insert_after(head, 100).ok(), "insert at the head succeeds");
    auto anchor_100 = list_a.try_anchor_of(100);
    check(anchor_100.ok(), "the inserted element has an anchor");
    if (anchor_100.ok()) {
        const Crdt::Anchor at = anchor_100.value();
        check(list_a.try_insert_after(at, 200).ok(),
              "insert after an element succeeds");
        check(list_a.try_remove_at(at).ok(), "remove an element succeeds");
    }
    check(list_a.size() == 1, "one element remains visible after the removal");
    check(list_a.tombstone_count() == 1, "the removed element is a tombstone");
    // A removed element keeps its identity but is no longer visible, so it is
    // not in ids(). It is still counted, which is the distinction a consumer needs
    // in order to tell "removed" from "never existed".
    check(list_a.entry_count() == 2, "the removed element is still an entry");
    check(list_a.tombstone_count() == 1, "exactly one element is a tombstone");

    // A missing element is an explicit outcome, never a fabricated position.
    const auto missing = list_a.try_anchor_of(999);
    check(!missing.ok(), "an unknown id is an explicit miss");
    if (!missing.ok())
        check(missing.error().code() == ErrorCode::unresolvable_anchor,
              "the miss is typed as unresolvable_anchor");

    // ---- list: transfer without touching fields ----
    for (const std::string &bytes : list_wire_a.list_ops) deliver_list(bytes, list_b);
    for (const std::string &bytes : list_wire_b.list_ops) deliver_list(bytes, list_a);
    check(list_a.ids() == list_b.ids(), "two peers converge on the same list");

    // ---- identity is adopted, and it is the actors' own ----
    check(alice_counter->actor() != bob_counter->actor(),
          "two peers have distinct actors");
    check(alice_counter->peek() > 1, "the first peer reserved identities");
    check(list_a.actor_id() == alice_counter->actor(),
          "the list reports its allocator's actor");
    // ---- outcome vocabulary is typed where the facade offers it ----
    //
    // Both facades expose the same typed operation outcome vocabulary.
    if (!list_wire_a.list_ops.empty()) {
        auto decoded = Crdt::decode_idlist_operation(list_wire_a.list_ops.front());
        if (decoded) {
            const auto outcomes = list_b.apply_ops({decoded.value()});
            check(outcomes.size() == size_t(1), "a re-delivery reports one outcome");
            if (!outcomes.empty())
                check(outcomes.front().kind() == ApplyOutcomeKind::duplicate,
                      "a re-delivered list operation is reported as a duplicate");
        }
    }

    if (failures == 0)
        std::printf("alpha-consumer: ok (text=%zu bytes, list=%zu ids)\n",
                    alice.text().size(), list_a.ids().size());
    else
        std::fprintf(stderr, "alpha-consumer: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
