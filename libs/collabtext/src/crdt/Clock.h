#pragma once

#include <cstdint>
#include <compare>
#include <vector>

namespace CollabText::Crdt {

struct Lamport {
    uint32_t value = 0;
    uint16_t replica_id = 0;

    Lamport() = default;
    Lamport(uint16_t replica, uint32_t val) : value(val), replica_id(replica) {}

    static Lamport min() { return {0, 0}; }
    static Lamport max() { return {UINT16_MAX, UINT32_MAX}; }

    Lamport tick() {
        Lamport result = *this;
        ++value;
        return result;
    }

    void observe(Lamport other) {
        if (other.value >= value)
            value = other.value + 1;
    }

    auto operator<=>(const Lamport &other) const {
        if (auto cmp = value <=> other.value; cmp != 0)
            return cmp;
        return replica_id <=> other.replica_id;
    }
    bool operator==(const Lamport &other) const = default;
};

class Global {
public:
    Global() = default;

    uint32_t get(uint16_t replica_id) const;
    void observe(Lamport ts);
    bool observed(Lamport ts) const;
    bool observed_all(const Global &other) const;
    void join(const Global &other);
    void meet(const Global &other);

    bool operator==(const Global &other) const = default;
    const std::vector<uint32_t> &values() const { return m_values; }

private:
    std::vector<uint32_t> m_values;
};

} // namespace CollabText::Crdt
