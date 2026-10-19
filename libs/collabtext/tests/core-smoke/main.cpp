/// core-smoke/main.cpp — proves CollabText::Core stands on its own.
///
/// Roadmap task A01's acceptance criterion is that a Core-only configuration
/// builds and *runs* something, with Qt6, zstd and a crypto provider all
/// explicitly unavailable. Configuring without Qt proves the target graph is
/// right; running a program that edits text, exchanges operations between two
/// engines, and round-trips an operation through the wire codec proves the
/// component actually links on its own.
///
/// Deliberately plain C++: no QtTest, no test support, no assertion macro that
/// a consumer would have to provide. It returns non-zero and prints why, because
/// "exit zero" is the only thing a CTest row can record.
///
/// The assertions are deliberately about *composition*, not about behaviour:
/// a behavioural change that broke two-replica convergence belongs in the
/// convergence suites. What this catches is a dependency that crept back in.

#include <collabtext/ActorId.h>
#include <collabtext/CrdtEngine.h>
#include <collabtext/Operations.h>
#include <collabtext/Serialization.h>

#include <collabtext/Engine.h> // IdentityAllocator, and nothing internal

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
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
    if (!condition) {
        std::fprintf(stderr, "core-smoke: FAILED %s\n", what);
        ++failures;
    }
}

/// A volatile counter, declared here rather than borrowed from the test
/// harness: a consumer standing up an engine has to be able to do this without
/// linking anything, and the ability to do it wrongly is the point of the
/// allocator being an explicit dependency.
class SmokeCounter final : public CollabText::Crdt::IdentityAllocator {
public:
    explicit SmokeCounter(const CollabText::ActorId &actor) : m_actor(actor) {}

    CollabText::Result<uint64_t> reserve_run(uint64_t count) override {
        return m_counter.reserve_run(count, "core smoke");
    }
    void observe(uint64_t counter) override {
        (void)m_counter.observe(counter, "core smoke");
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

    // Note what is *not* here: any include of a `crdt/` or `src/` header. This
    // program is a consumer of the public surface and nothing else, which is the
    // property A02a's header-independence check turns into a build. The portable
    // hash Core uses internally is covered by tst_sha256, not by a consumer.

    // --- two engines, one actor each, converging over the wire codec ---
    auto first = std::make_shared<SmokeCounter>(actor(1));
    auto second = std::make_shared<SmokeCounter>(actor(2));
    CrdtEngine a(first);
    CrdtEngine b(second);

    std::vector<std::string> wire;
    a.setOnLocalOp([&](const Crdt::Operation &op) {
        wire.push_back(checked_codec(Crdt::encode_operation(op)));
    });
    b.setOnLocalOp([&](const Crdt::Operation &op) {
        wire.push_back(checked_codec(Crdt::encode_operation(op)));
    });

    a.insert(0, "hello ");
    b.insert(0, "world");
    check(wire.size() == 2, "each engine emitted one operation");

    for (const std::string &bytes : wire) {
        auto decoded = Crdt::decode_operation(bytes);
        check(decoded.ok(), "an encoded operation decodes");
        if (!decoded) continue;
        check(checked_codec(Crdt::encode_operation(decoded.value())) == bytes,
              "an operation survives encode/decode unchanged");
        const auto outcome_a = a.apply_remote(decoded.value());
        const auto outcome_b = b.apply_remote(decoded.value());
        check(outcome_a.kind() == ApplyOutcomeKind::applied ||
                  outcome_a.kind() == ApplyOutcomeKind::duplicate,
              "first replica accepts the operation");
        check(outcome_b.kind() == ApplyOutcomeKind::applied ||
                  outcome_b.kind() == ApplyOutcomeKind::duplicate,
              "second replica accepts the operation");
    }

    check(a.text() == b.text(), "two engines converge after exchange");

    // Both edits are present, as characters. Not as substrings: two engines that
    // insert at the same position produce a character-interleaved document
    // today -- convergent, and wrong for prose. That is finding F8 and it is
    // S01/S02/S03's subject, so asserting contiguity here would quietly turn a
    // component smoke test into an ordering gate that is supposed to be red for
    // several more tasks. The multiset check says what this test is actually
    // about: the component links, the codec round-trips, and both operations
    // survived.
    std::string sorted = a.text();
    std::sort(sorted.begin(), sorted.end());
    std::string expected = "hello world";
    std::sort(expected.begin(), expected.end());
    check(sorted == expected, "both edits are present after the exchange");
    check(first->peek() > 1, "the first engine reserved identities durably-ish");
    check(second->peek() > 1, "the second engine reserved identities");
    check(first->actor() != second->actor(), "the two engines have distinct actors");

    if (failures == 0)
        std::printf("core-smoke: ok (%s)\n", a.text().c_str());
    else
        std::fprintf(stderr, "core-smoke: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
