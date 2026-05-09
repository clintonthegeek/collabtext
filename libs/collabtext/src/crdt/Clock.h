#pragma once

#include <cstdint>
#include <cstddef>
#include <compare>
#include <utility>

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
    /// `replica_id` (the field below) is the companion stable surface —
    /// see Operations.h contract block for the explicit carve-out.
    uint64_t counter() const { return value; }

    auto operator<=>(const Lamport &other) const {
        if (auto cmp = value <=> other.value; cmp != 0)
            return cmp;
        return replica_id <=> other.replica_id;
    }
    bool operator==(const Lamport &other) const = default;
};

/// Version vector keyed by replica id. Stored as a sorted, packed sequence of
/// `(replica_id, value)` pairs (one uint64_t per pair, replica_id in the high
/// 32 bits). Sparse: only replicas that have been observed take space. Up to
/// SBO_CAP entries live inline; larger spill to the heap.
///
/// Sparseness matters because the SumTree aggregates a FragmentSummary per
/// edit, each carrying three Globals; a dense-array layout indexed by
/// replica_id would copy and zero ~replica_id*sizeof(uint32_t) bytes per
/// summary, making per-edit cost linear in the magnitude of replica_id.
///
/// `size()` and `operator[]` preserve the legacy dense-view semantics
/// (`size() == max_observed_replica_id + 1`, `operator[](rid) == get(rid)`)
/// so the existing wire format and tests stay unchanged.
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

    /// Dense view: max observed replica_id + 1, or 0 if empty.
    /// Preserved for compatibility with dense-iteration callers
    /// (encode_global, parse_global, identity version-summary encoder).
    size_t size() const noexcept;
    /// Dense view: value at `replica_id == i`, or 0 if absent.
    uint32_t operator[](size_t i) const noexcept;

    /// Sparse iteration: number of present (replica_id, value) pairs.
    size_t pair_count() const noexcept { return m_size; }
    /// Sparse iteration: i-th present pair, sorted by replica_id ascending.
    Lamport pair(size_t i) const noexcept {
        uint64_t p = data()[i];
        return Lamport(static_cast<uint16_t>(p >> 32),
                       static_cast<uint32_t>(p));
    }

private:
    static constexpr uint16_t SBO_CAP = 4;

    static uint64_t pack(uint16_t replica_id, uint32_t value) noexcept {
        return (static_cast<uint64_t>(replica_id) << 32) | value;
    }
    static uint16_t unpack_rid(uint64_t p) noexcept {
        return static_cast<uint16_t>(p >> 32);
    }
    static uint32_t unpack_val(uint64_t p) noexcept {
        return static_cast<uint32_t>(p);
    }

    bool on_heap() const noexcept { return m_capacity > SBO_CAP; }
    uint64_t *data_mut() noexcept { return on_heap() ? m_heap_data : m_inline; }
    const uint64_t *data() const noexcept { return on_heap() ? m_heap_data : m_inline; }

    /// Ensure room for at least `need` pairs. May reallocate; do NOT hold
    /// pointers across this call.
    void ensure_capacity(uint32_t need);

    /// Find the index of `replica_id`, or the insertion point if absent.
    /// Returns (idx, found).
    std::pair<uint32_t, bool> find_index(uint16_t replica_id) const noexcept;

    uint32_t m_size = 0;       ///< number of present pairs
    uint32_t m_capacity = SBO_CAP;
    union {
        uint64_t m_inline[SBO_CAP];
        uint64_t *m_heap_data;
    };
};

} // namespace CollabText::Crdt
