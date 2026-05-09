#include "crdt/Clock.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace CollabText::Crdt {

Global::Global(const Global &other) {
    if (other.m_size == 0) return;
    if (other.m_size <= SBO_CAP) {
        std::memcpy(m_inline, other.data(), other.m_size * sizeof(uint64_t));
        m_capacity = SBO_CAP;
    } else {
        m_heap_data = static_cast<uint64_t *>(
            std::malloc(other.m_capacity * sizeof(uint64_t)));
        std::memcpy(m_heap_data, other.m_heap_data,
                    other.m_size * sizeof(uint64_t));
        m_capacity = other.m_capacity;
    }
    m_size = other.m_size;
}

Global::Global(Global &&other) noexcept {
    if (other.on_heap()) {
        m_heap_data = other.m_heap_data;
        m_capacity = other.m_capacity;
    } else {
        std::memcpy(m_inline, other.m_inline, other.m_size * sizeof(uint64_t));
        m_capacity = SBO_CAP;
    }
    m_size = other.m_size;
    other.m_size = 0;
    other.m_capacity = SBO_CAP;
}

Global &Global::operator=(const Global &other) {
    if (this == &other) return *this;
    if (on_heap()) std::free(m_heap_data);
    m_capacity = SBO_CAP;
    m_size = 0;
    if (other.m_size == 0) return *this;
    if (other.m_size <= SBO_CAP) {
        std::memcpy(m_inline, other.data(), other.m_size * sizeof(uint64_t));
    } else {
        m_heap_data = static_cast<uint64_t *>(
            std::malloc(other.m_capacity * sizeof(uint64_t)));
        std::memcpy(m_heap_data, other.m_heap_data,
                    other.m_size * sizeof(uint64_t));
        m_capacity = other.m_capacity;
    }
    m_size = other.m_size;
    return *this;
}

Global &Global::operator=(Global &&other) noexcept {
    if (this == &other) return *this;
    if (on_heap()) std::free(m_heap_data);
    if (other.on_heap()) {
        m_heap_data = other.m_heap_data;
        m_capacity = other.m_capacity;
    } else {
        m_capacity = SBO_CAP;
        std::memcpy(m_inline, other.m_inline, other.m_size * sizeof(uint64_t));
    }
    m_size = other.m_size;
    other.m_size = 0;
    other.m_capacity = SBO_CAP;
    return *this;
}

Global::~Global() {
    if (on_heap()) std::free(m_heap_data);
}

void Global::ensure_capacity(uint32_t need) {
    if (need <= m_capacity) return;
    uint32_t new_cap = m_capacity == 0 ? SBO_CAP : m_capacity;
    while (new_cap < need) {
        // Double, with a safety cap at uint16_t-max+1 for replica space.
        new_cap = (new_cap >= (uint32_t{1} << 31)) ? UINT32_MAX : new_cap * 2;
    }
    uint64_t *new_data = static_cast<uint64_t *>(
        std::malloc(new_cap * sizeof(uint64_t)));
    if (m_size > 0) {
        std::memcpy(new_data, data(), m_size * sizeof(uint64_t));
    }
    if (on_heap()) std::free(m_heap_data);
    m_heap_data = new_data;
    m_capacity = new_cap;
}

std::pair<uint32_t, bool> Global::find_index(uint16_t replica_id) const noexcept {
    // Binary search for replica_id in the high-32-bits-keyed packed pairs.
    // pack(replica_id, 0) is the smallest packed value for that replica_id.
    const uint64_t target = pack(replica_id, 0);
    const uint64_t *d = data();
    uint32_t lo = 0;
    uint32_t hi = m_size;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (d[mid] < target) lo = mid + 1;
        else hi = mid;
    }
    if (lo < m_size && unpack_rid(d[lo]) == replica_id) {
        return {lo, true};
    }
    return {lo, false};
}

uint32_t Global::get(uint16_t replica_id) const {
    auto [idx, found] = find_index(replica_id);
    return found ? unpack_val(data()[idx]) : 0;
}

void Global::observe(Lamport ts) {
    if (ts.value == 0) return;
    auto [idx, found] = find_index(ts.replica_id);
    if (found) {
        uint64_t *d = data_mut();
        if (unpack_val(d[idx]) < ts.value) {
            d[idx] = pack(ts.replica_id, ts.value);
        }
        return;
    }
    // Insert new pair at idx, shifting existing entries right.
    ensure_capacity(m_size + 1);
    uint64_t *d = data_mut();
    if (idx < m_size) {
        std::memmove(d + idx + 1, d + idx,
                     (m_size - idx) * sizeof(uint64_t));
    }
    d[idx] = pack(ts.replica_id, ts.value);
    ++m_size;
}

bool Global::observed(Lamport ts) const {
    return get(ts.replica_id) >= ts.value;
}

bool Global::observed_all(const Global &other) const {
    // self ⊇ other iff for every (rid, val) in other, self.get(rid) >= val.
    const uint64_t *b = other.data();
    for (uint32_t i = 0; i < other.m_size; ++i) {
        uint16_t rid = unpack_rid(b[i]);
        uint32_t val = unpack_val(b[i]);
        if (get(rid) < val) return false;
    }
    return true;
}

void Global::join(const Global &other) {
    if (other.m_size == 0) return;
    // Other is sorted by replica_id; merge in order to keep self sorted.
    // Worst case O(n + m); typical case O(m * log n) when other is a small
    // delta — both fine.
    const uint64_t *b = other.data();
    for (uint32_t i = 0; i < other.m_size; ++i) {
        uint16_t rid = unpack_rid(b[i]);
        uint32_t val = unpack_val(b[i]);
        observe(Lamport(rid, val));
    }
}

void Global::meet(const Global &other) {
    // Asymmetric meet: for each pair in self, if other has the same replica,
    // take min(self, other); otherwise leave self's value unchanged. This
    // matches the prior dense semantics, where absent entries (zero) didn't
    // pull self's value down. Self never gains entries.
    uint64_t *a = data_mut();
    for (uint32_t i = 0; i < m_size; ++i) {
        uint16_t rid = unpack_rid(a[i]);
        uint32_t self_val = unpack_val(a[i]);
        uint32_t other_val = other.get(rid);
        if (other_val > 0 && other_val < self_val) {
            a[i] = pack(rid, other_val);
        }
    }
}

bool Global::operator==(const Global &other) const {
    if (m_size != other.m_size) return false;
    return std::memcmp(data(), other.data(),
                       m_size * sizeof(uint64_t)) == 0;
}

size_t Global::size() const noexcept {
    if (m_size == 0) return 0;
    // Pairs are sorted ascending by replica_id; the dense "length" is the
    // largest observed replica_id plus one.
    return static_cast<size_t>(unpack_rid(data()[m_size - 1])) + 1;
}

uint32_t Global::operator[](size_t i) const noexcept {
    if (i > UINT16_MAX) return 0;
    return get(static_cast<uint16_t>(i));
}

} // namespace CollabText::Crdt
