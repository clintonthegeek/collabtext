/// VolatileCounter.h — an explicitly non-durable allocator, for tests only.
///
/// docs/specs/vnext-identity.md §3.7: "Tests that need a throwaway counter must
/// say so explicitly as a volatile test counter — never by reusing a production
/// allocator path."
///
/// This is that saying-so. It is backed by the same checked `CheckedCounter`
/// production uses, so exhaustion and the all-or-nothing bound behave
/// identically, and it logs its reservations so a test can assert *how many*
/// allocations an operation performed rather than inferring it from the values
/// that came back.
///
/// What it deliberately does not have: persistence. An identity from this
/// allocator is not durable, and a test that needs durability must open a
/// `ReplicaState`. The name is the warning.

#pragma once

#include <collabtext/ActorId.h>

#include <collabtext/Engine.h>

#include <cstdint>
#include <vector>

namespace CollabText::Test {

/// A volatile, in-memory allocator for one actor.
class VolatileCounter final : public CollabText::Crdt::IdentityAllocator {
public:
    explicit VolatileCounter(const CollabText::ActorId &actor,
                             uint64_t start = 1)
        : m_actor(actor), m_counter(start) {}

    [[nodiscard]] CollabText::Result<uint64_t> reserve_run(uint64_t count) override {
        auto base = m_counter.reserve_run(count, "volatile test counter");
        if (!base.ok()) return base.error();
        m_reservations.push_back(count);
        m_first_base = base.value();
        return base;
    }

    void observe(uint64_t counter) override {
        (void)m_counter.observe(counter, "volatile test counter");
    }

    [[nodiscard]] uint64_t peek() const noexcept override { return m_counter.peek(); }

    [[nodiscard]] const CollabText::ActorId &actor() const noexcept override {
        return m_actor;
    }

    // ---- test-facing observation ----

    /// How many reservations have been asked for since the last reset.
    [[nodiscard]] size_t reservation_count() const noexcept { return m_reservations.size(); }
    /// The size of the most recent reservation.
    [[nodiscard]] uint64_t last_reserved_count() const noexcept {
        return m_reservations.empty() ? 0 : m_reservations.back();
    }
    /// The base of the most recent reservation.
    [[nodiscard]] uint64_t first_reserved_base() const noexcept { return m_first_base; }
    [[nodiscard]] const std::vector<uint64_t> &reservations() const noexcept {
        return m_reservations;
    }

    void reset_call_log() noexcept {
        m_reservations.clear();
        m_first_base = 0;
    }

private:
    CollabText::ActorId m_actor;
    CollabText::CheckedCounter m_counter;
    std::vector<uint64_t> m_reservations;
    uint64_t m_first_base = 0;
};

} // namespace CollabText::Test