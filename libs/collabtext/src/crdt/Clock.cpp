#include "crdt/Clock.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

namespace CollabText::Crdt {

Global::Global(const Global &other) {
    if (other.m_size == 0) return;
    if (other.m_size <= SBO_CAP) {
        std::memcpy(m_inline_storage, other.data(),
                    other.m_size * sizeof(Pair));
        m_capacity = SBO_CAP;
    } else {
        m_heap_data = static_cast<Pair *>(
            std::malloc(other.m_capacity * sizeof(Pair)));
        std::memcpy(m_heap_data, other.m_heap_data,
                    other.m_size * sizeof(Pair));
        m_capacity = other.m_capacity;
    }
    m_size = other.m_size;
}

Global::Global(Global &&other) noexcept {
    if (other.on_heap()) {
        m_heap_data = other.m_heap_data;
        m_capacity = other.m_capacity;
    } else {
        std::memcpy(m_inline_storage, other.m_inline_storage,
                    other.m_size * sizeof(Pair));
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
        std::memcpy(m_inline_storage, other.data(),
                    other.m_size * sizeof(Pair));
    } else {
        m_heap_data = static_cast<Pair *>(
            std::malloc(other.m_capacity * sizeof(Pair)));
        std::memcpy(m_heap_data, other.m_heap_data,
                    other.m_size * sizeof(Pair));
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
        std::memcpy(m_inline_storage, other.m_inline_storage,
                    other.m_size * sizeof(Pair));
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
        if (new_cap > UINT32_MAX / 2) {
            new_cap = need;
            break;
        }
        new_cap *= 2;
    }
    Pair *new_data = static_cast<Pair *>(std::malloc(new_cap * sizeof(Pair)));
    if (!new_data) std::terminate();
    if (m_size > 0) {
        std::memcpy(new_data, data(), m_size * sizeof(Pair));
    }
    if (on_heap()) std::free(m_heap_data);
    m_heap_data = new_data;
    m_capacity = new_cap;
}

std::pair<uint32_t, bool> Global::find_index(const ActorId &actor) const noexcept {
    // Binary search over pairs sorted by ActorId::compare.
    const Pair *d = data();
    uint32_t lo = 0;
    uint32_t hi = m_size;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (d[mid].first < actor) lo = mid + 1;
        else hi = mid;
    }
    if (lo < m_size && d[lo].first == actor) {
        return {lo, true};
    }
    return {lo, false};
}

uint64_t Global::get(const ActorId &actor) const {
    auto [idx, found] = find_index(actor);
    return found ? data()[idx].second : 0;
}

void Global::observe(Identity id) {
    if (id.counter == 0) return;
    auto [idx, found] = find_index(id.actor);
    if (found) {
        Pair *d = data_mut();
        if (d[idx].second < id.counter) {
            d[idx].second = id.counter;
        }
        return;
    }
    // Insert new pair at idx, shifting existing entries right.
    ensure_capacity(m_size + 1);
    Pair *d = data_mut();
    if (idx < m_size) {
        std::memmove(d + idx + 1, d + idx, (m_size - idx) * sizeof(Pair));
    }
    d[idx] = Pair{id.actor, id.counter};
    ++m_size;
}

bool Global::observed(Identity id) const {
    return get(id.actor) >= id.counter;
}

bool Global::observed_all(const Global &other) const {
    // self ⊇ other iff for every (actor, counter) in other,
    // self.get(actor) >= counter.
    const Pair *b = other.data();
    for (uint32_t i = 0; i < other.m_size; ++i) {
        if (get(b[i].first) < b[i].second) return false;
    }
    return true;
}

void Global::join(const Global &other) {
    if (other.m_size == 0) return;
    const Pair *b = other.data();
    for (uint32_t i = 0; i < other.m_size; ++i) {
        observe(Identity{b[i].first, b[i].second});
    }
}

void Global::meet(const Global &other) {
    // Asymmetric meet: for each pair in self, if other has the same actor,
    // take min(self, other); otherwise leave self's value unchanged. This
    // matches the prior dense semantics, where absent entries (zero) didn't
    // pull self's value down. Self never gains entries.
    Pair *a = data_mut();
    for (uint32_t i = 0; i < m_size; ++i) {
        const ActorId &who = a[i].first;
        uint64_t self_val = a[i].second;
        uint64_t other_val = other.get(who);
        if (other_val > 0 && other_val < self_val) {
            a[i].second = other_val;
        }
    }
}

Global Global::coverage_intersection(const Global &other) const {
    // Walk the union of both sorted pair lists once, taking the minimum, and
    // drop anything that comes out zero: absence is zero coverage, and a zero
    // component is not a fact worth carrying.
    Global result;
    uint32_t capacity = m_size + other.m_size;
    result.ensure_capacity(capacity);
    Pair *out = result.data_mut();

    const Pair *a = data();
    const Pair *b = other.data();
    uint32_t i = 0;
    uint32_t j = 0;
    while (i < m_size || j < other.m_size) {
        ActorId who;
        uint64_t left = 0;
        uint64_t right = 0;
        if (j >= other.m_size ||
            (i < m_size && a[i].first < b[j].first)) {
            who = a[i].first;
            left = a[i].second;
            ++i;
        } else if (i >= m_size || b[j].first < a[i].first) {
            who = b[j].first;
            right = b[j].second;
            ++j;
        } else {
            who = a[i].first;
            left = a[i].second;
            right = b[j].second;
            ++i;
            ++j;
        }
        const uint64_t value = left < right ? left : right;
        if (value > 0) out[result.m_size++] = Pair{who, value};
    }
    return result;
}

bool Global::operator==(const Global &other) const {
    if (m_size != other.m_size) return false;
    const Pair *a = data();
    const Pair *b = other.data();
    for (uint32_t i = 0; i < m_size; ++i) {
        if (a[i].first != b[i].first || a[i].second != b[i].second) return false;
    }
    return true;
}

} // namespace CollabText::Crdt
