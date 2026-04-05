#include "crdt/NetworkSim.h"
#include <QTest>
#include <cassert>

namespace CollabText::Crdt {

NetworkSim::NetworkSim(int num_replicas, NetworkConfig config, uint64_t seed)
    : m_config(config), m_rng(seed)
{
    m_replicas.reserve(num_replicas);
    for (int i = 0; i < num_replicas; ++i)
        m_replicas.emplace_back(static_cast<uint16_t>(i + 1));
    m_queues.resize(num_replicas);
    m_offline.resize(num_replicas, false);
    m_pending_offline.resize(num_replicas);
}

uint64_t NetworkSim::random_latency() {
    if (m_config.min_latency_ms >= m_config.max_latency_ms)
        return m_config.min_latency_ms;
    uint32_t range = m_config.max_latency_ms - m_config.min_latency_ms;
    return m_config.min_latency_ms + (m_rng() % (range + 1));
}

void NetworkSim::broadcast(int source, const Operation& op) {
    for (int r = 0; r < static_cast<int>(m_replicas.size()); ++r) {
        if (r == source) continue;
        ScheduledOp sop;
        sop.deliver_at_ms = m_clock + random_latency();
        sop.from_replica = static_cast<uint16_t>(source + 1);
        sop.op = op;

        if (m_offline[r]) {
            m_pending_offline[r].push_back(sop);
        } else {
            m_queues[r].push(sop);
            // Duplicate with configured probability
            double roll = static_cast<double>(m_rng() % 10000) / 10000.0;
            if (roll < m_config.duplicate_probability) {
                ScheduledOp dup = sop;
                dup.deliver_at_ms = m_clock + random_latency();
                m_queues[r].push(dup);
            }
        }
    }
}

Operation NetworkSim::edit(int replica,
                           const std::vector<std::pair<uint32_t, uint32_t>>& ranges,
                           const std::vector<std::string>& new_text) {
    auto op = m_replicas[replica].apply_local_edit(ranges, new_text);
    broadcast(replica, op);
    return op;
}

Operation NetworkSim::edit(int replica, const EditAction& action) {
    return edit(replica, action.ranges, action.new_text);
}

std::optional<Operation> NetworkSim::undo(int replica) {
    auto op = m_replicas[replica].undo();
    if (op) broadcast(replica, *op);
    return op;
}

std::optional<Operation> NetworkSim::redo(int replica) {
    auto op = m_replicas[replica].redo();
    if (op) broadcast(replica, *op);
    return op;
}

void NetworkSim::disconnect(int replica) {
    m_offline[replica] = true;
}

void NetworkSim::reconnect(int replica) {
    m_offline[replica] = false;
    for (auto& sop : m_pending_offline[replica]) {
        sop.deliver_at_ms = m_clock + random_latency();
        m_queues[replica].push(sop);
    }
    m_pending_offline[replica].clear();
}

void NetworkSim::tick(uint64_t ms) {
    m_clock += ms;
    for (int r = 0; r < static_cast<int>(m_replicas.size()); ++r) {
        if (m_offline[r]) continue;
        std::vector<Operation> batch;
        while (!m_queues[r].empty() &&
               m_queues[r].top().deliver_at_ms <= m_clock) {
            batch.push_back(m_queues[r].top().op);
            m_queues[r].pop();
        }
        if (!batch.empty())
            m_replicas[r].apply_ops(batch);
    }
}

void NetworkSim::drain() {
    // Deliver everything
    m_clock = UINT64_MAX / 2;  // large but not overflow-prone
    tick(0);
    // Retry deferred ops
    for (int pass = 0; pass < 30; ++pass) {
        for (auto& buf : m_replicas)
            buf.apply_ops({});
    }
}

void NetworkSim::check_all_invariants(const char* context) const {
    for (int r = 0; r < static_cast<int>(m_replicas.size()); ++r) {
        const auto& buf = m_replicas[r];
        auto frags = buf.fragments();
        std::string text = buf.text();

        // INV-1: visible_length matches text
        if (buf.visible_length() != static_cast<uint32_t>(text.size()))
            QFAIL(qPrintable(QString("INV-1 at %1 r%2").arg(context).arg(r)));

        // INV-2 + INV-8: fragment byte sums match rope lengths
        uint32_t vis = 0, del = 0;
        for (auto& f : frags) {
            if (f.visible) vis += f.byte_length;
            else del += f.byte_length;
        }
        if (vis != buf.visible_rope_len())
            QFAIL(qPrintable(QString("INV-8 vis at %1 r%2").arg(context).arg(r)));
        if (del != buf.deleted_rope_len())
            QFAIL(qPrintable(QString("INV-8 del at %1 r%2").arg(context).arg(r)));

        // INV-4: fragment ordering
        for (size_t i = 1; i < frags.size(); ++i) {
            auto cmp = frags[i].locator <=> frags[i-1].locator;
            if (cmp < 0)
                QFAIL(qPrintable(QString("INV-4 at %1 r%2").arg(context).arg(r)));
            if (cmp == 0 && frags[i].origin <= frags[i-1].origin)
                QFAIL(qPrintable(QString("INV-4 at %1 r%2").arg(context).arg(r)));
        }

        // INV-5: non-empty fragments
        for (size_t i = 0; i < frags.size(); ++i) {
            if (frags[i].byte_length == 0 || frags[i].length == 0)
                QFAIL(qPrintable(QString("INV-5 at %1 r%2 i%3").arg(context).arg(r).arg(i)));
        }
    }
}

void NetworkSim::assert_convergence(const char* context) const {
    if (m_replicas.size() < 2) return;
    std::string expected = m_replicas[0].text();
    for (size_t r = 1; r < m_replicas.size(); ++r) {
        if (m_replicas[r].text() != expected) {
            qWarning("CONVERGENCE FAILURE at %s: r0 len=%zu, r%zu len=%zu",
                     context,
                     expected.size(), r, m_replicas[r].text().size());
            QFAIL(qPrintable(QString("Convergence failed at %1: r0 != r%2")
                .arg(context).arg(r)));
        }
    }
}

size_t NetworkSim::collect_garbage(int replica) {
    return m_replicas[replica].collect_garbage();
}

size_t NetworkSim::compact_all() {
    if (m_replicas.empty()) return 0;
    Global watermark = m_replicas[0].version();
    for (size_t r = 1; r < m_replicas.size(); ++r)
        watermark.meet(m_replicas[r].version());
    size_t total = 0;
    for (auto& buf : m_replicas)
        total += buf.compact(watermark);
    return total;
}

const Buffer& NetworkSim::buffer(int replica) const {
    return m_replicas[replica];
}

Buffer& NetworkSim::buffer(int replica) {
    return m_replicas[replica];
}

int NetworkSim::num_replicas() const {
    return static_cast<int>(m_replicas.size());
}

uint64_t NetworkSim::clock() const {
    return m_clock;
}

} // namespace CollabText::Crdt
