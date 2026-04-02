#include "crdt/Clock.h"
#include <algorithm>

namespace CollabText::Crdt {

uint32_t Global::get(uint16_t replica_id) const {
    if (replica_id < m_values.size())
        return m_values[replica_id];
    return 0;
}

void Global::observe(Lamport ts) {
    if (ts.value == 0) return;
    if (ts.replica_id >= m_values.size())
        m_values.resize(ts.replica_id + 1, 0);
    m_values[ts.replica_id] = std::max(m_values[ts.replica_id], ts.value);
}

bool Global::observed(Lamport ts) const {
    return get(ts.replica_id) >= ts.value;
}

bool Global::observed_all(const Global &other) const {
    if (m_values.size() < other.m_values.size())
        return false;
    for (size_t i = 0; i < other.m_values.size(); ++i) {
        if (m_values[i] < other.m_values[i])
            return false;
    }
    return true;
}

void Global::join(const Global &other) {
    if (other.m_values.size() > m_values.size())
        m_values.resize(other.m_values.size(), 0);
    for (size_t i = 0; i < other.m_values.size(); ++i)
        m_values[i] = std::max(m_values[i], other.m_values[i]);
}

void Global::meet(const Global &other) {
    size_t minLen = std::min(m_values.size(), other.m_values.size());
    for (size_t i = 0; i < minLen; ++i) {
        if (m_values[i] > 0 && other.m_values[i] > 0)
            m_values[i] = std::min(m_values[i], other.m_values[i]);
    }
    while (!m_values.empty() && m_values.back() == 0)
        m_values.pop_back();
}

} // namespace CollabText::Crdt
