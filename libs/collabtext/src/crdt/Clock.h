#pragma once

#include <cstdint>
#include <cstddef>
#include <compare>

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

    /// Stable public accessor: returns the logical clock value as uint64_t.
    /// Part of the OpStream narrow contract; safe to depend on.
    /// replica_id is the companion stable field (directly accessible).
    uint64_t counter() const { return value; }

    auto operator<=>(const Lamport &other) const {
        if (auto cmp = value <=> other.value; cmp != 0)
            return cmp;
        return replica_id <=> other.replica_id;
    }
    bool operator==(const Lamport &other) const = default;
};

/// Version vector keyed by replica id. Uses small-buffer optimization: up
/// to SBO_CAP entries live inline; larger sizes spill to the heap. The
/// inline path matters because the SumTree aggregates many FragmentSummary
/// values per edit, each carrying three Globals.
class Global {
public:
    Global() noexcept = default;
    Global(const Global &other);
    Global(Global &&other) noexcept;
    Global &operator=(const Global &other);
    Global &operator=(Global &&other) noexcept;
    ~Global();

    uint32_t get(uint16_t replica_id) const;
    void observe(Lamport ts);
    bool observed(Lamport ts) const;
    bool observed_all(const Global &other) const;
    void join(const Global &other);
    void meet(const Global &other);

    bool operator==(const Global &other) const;

    size_t size() const noexcept { return m_size; }
    uint32_t operator[](size_t i) const noexcept { return data()[i]; }

private:
    static constexpr uint16_t SBO_CAP = 4;

    bool on_heap() const noexcept { return m_capacity > SBO_CAP; }
    uint32_t *data_mut() noexcept { return on_heap() ? m_heap_data : m_inline; }
    const uint32_t *data() const noexcept { return on_heap() ? m_heap_data : m_inline; }
    void resize_zero(uint32_t new_size);

    uint32_t m_size = 0;
    uint32_t m_capacity = SBO_CAP;
    union {
        uint32_t m_inline[SBO_CAP];
        uint32_t *m_heap_data;
    };
};

} // namespace CollabText::Crdt
