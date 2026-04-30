#include "crdt/Clock.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace CollabText::Crdt {

Global::Global(const Global &other) {
    if (other.m_size == 0) return;
    if (other.m_size <= SBO_CAP) {
        std::memcpy(m_inline, other.data(), other.m_size * sizeof(uint32_t));
    } else {
        m_heap_data = static_cast<uint32_t *>(std::malloc(other.m_capacity * sizeof(uint32_t)));
        std::memcpy(m_heap_data, other.m_heap_data, other.m_size * sizeof(uint32_t));
        m_capacity = other.m_capacity;
    }
    m_size = other.m_size;
}

Global::Global(Global &&other) noexcept {
    if (other.on_heap()) {
        m_heap_data = other.m_heap_data;
        m_capacity = other.m_capacity;
    } else {
        std::memcpy(m_inline, other.m_inline, other.m_size * sizeof(uint32_t));
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
        std::memcpy(m_inline, other.data(), other.m_size * sizeof(uint32_t));
    } else {
        m_heap_data = static_cast<uint32_t *>(std::malloc(other.m_capacity * sizeof(uint32_t)));
        std::memcpy(m_heap_data, other.m_heap_data, other.m_size * sizeof(uint32_t));
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
        std::memcpy(m_inline, other.m_inline, other.m_size * sizeof(uint32_t));
    }
    m_size = other.m_size;
    other.m_size = 0;
    other.m_capacity = SBO_CAP;
    return *this;
}

Global::~Global() {
    if (on_heap()) std::free(m_heap_data);
}

void Global::resize_zero(uint32_t new_size) {
    if (new_size <= m_size) {
        m_size = new_size;
        return;
    }
    if (new_size > m_capacity) {
        uint32_t new_cap = m_capacity;
        while (new_cap < new_size) new_cap *= 2;
        uint32_t *new_data = static_cast<uint32_t *>(std::malloc(new_cap * sizeof(uint32_t)));
        if (m_size > 0)
            std::memcpy(new_data, data(), m_size * sizeof(uint32_t));
        std::memset(new_data + m_size, 0, (new_cap - m_size) * sizeof(uint32_t));
        if (on_heap()) std::free(m_heap_data);
        m_heap_data = new_data;
        m_capacity = new_cap;
    } else {
        std::memset(data_mut() + m_size, 0, (new_size - m_size) * sizeof(uint32_t));
    }
    m_size = new_size;
}

uint32_t Global::get(uint16_t replica_id) const {
    if (replica_id < m_size) return data()[replica_id];
    return 0;
}

void Global::observe(Lamport ts) {
    if (ts.value == 0) return;
    if (ts.replica_id >= m_size) resize_zero(ts.replica_id + 1);
    uint32_t *d = data_mut();
    if (d[ts.replica_id] < ts.value) d[ts.replica_id] = ts.value;
}

bool Global::observed(Lamport ts) const {
    return get(ts.replica_id) >= ts.value;
}

bool Global::observed_all(const Global &other) const {
    if (m_size < other.m_size) return false;
    const uint32_t *a = data();
    const uint32_t *b = other.data();
    for (uint32_t i = 0; i < other.m_size; ++i)
        if (a[i] < b[i]) return false;
    return true;
}

void Global::join(const Global &other) {
    if (other.m_size == 0) return;
    if (other.m_size > m_size) resize_zero(other.m_size);
    uint32_t *a = data_mut();
    const uint32_t *b = other.data();
    for (uint32_t i = 0; i < other.m_size; ++i)
        if (b[i] > a[i]) a[i] = b[i];
}

void Global::meet(const Global &other) {
    uint32_t minLen = std::min(m_size, other.m_size);
    uint32_t *a = data_mut();
    const uint32_t *b = other.data();
    for (uint32_t i = 0; i < minLen; ++i) {
        if (a[i] > 0 && b[i] > 0)
            a[i] = std::min(a[i], b[i]);
    }
    while (m_size > 0 && a[m_size - 1] == 0)
        --m_size;
}

bool Global::operator==(const Global &other) const {
    if (m_size != other.m_size) return false;
    const uint32_t *a = data();
    const uint32_t *b = other.data();
    for (uint32_t i = 0; i < m_size; ++i)
        if (a[i] != b[i]) return false;
    return true;
}

} // namespace CollabText::Crdt
