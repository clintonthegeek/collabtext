#include "crdt/NetworkSim.h"
#include "crdt/DerivedActor.h"
#include <QTest>
#include <algorithm>
#include <cassert>

namespace CollabText::Crdt {

/// A checked, in-memory counter. Simulator-only; see simulator_allocator().
class SimulatorCounter final : public IdentityAllocator {
public:
    explicit SimulatorCounter(const ActorId &actor) : m_actor(actor) {}

    [[nodiscard]] Result<uint64_t> reserve_run(uint64_t count) override {
        return m_counter.reserve_run(count, "simulator counter");
    }
    void observe(uint64_t counter) override {
        (void)m_counter.observe(counter, "simulator counter");
    }
    [[nodiscard]] uint64_t peek() const noexcept override { return m_counter.peek(); }
    [[nodiscard]] const ActorId &actor() const noexcept override { return m_actor; }

private:
    ActorId m_actor;
    CheckedCounter m_counter;
};

NetworkSim::NetworkSim(int num_replicas, NetworkConfig config, uint64_t seed)
    : m_config(config), m_rng(seed)
{
    m_replicas.reserve(num_replicas);
    for (int i = 0; i < num_replicas; ++i)
        m_replicas.emplace_back(simulator_allocator(i));
    m_queues.resize(num_replicas);
    m_offline.resize(num_replicas, false);
    m_pending_outbound.resize(num_replicas);
    m_links.assign(num_replicas, std::vector<bool>(num_replicas, false));
    m_last_scheduled.assign(num_replicas,
                            std::vector<uint64_t>(num_replicas, 0));
}

ActorId simulator_actor(int index) {
    return derived_actor("collabtext/simulator-actor/v1",
                         std::to_string(index));
}

/// A volatile allocator per simulated replica.
///
/// The simulator is not a device: it exists to make convergence scenarios
/// reproducible, and its actors are derived from the replica index precisely so
/// they are stable across runs. So its counters are volatile too — persisted
/// counters would outlive the scenario that made them and a rerun would start
/// from wherever the last one stopped. Durability is exercised where it is real:
/// against ReplicaState.
SharedAllocator simulator_allocator(int index) {
    return std::make_shared<SimulatorCounter>(simulator_actor(index));
}

uint64_t NetworkSim::random_latency() {
    if (m_config.min_latency_ms >= m_config.max_latency_ms)
        return m_config.min_latency_ms;
    uint32_t range = m_config.max_latency_ms - m_config.min_latency_ms;
    return m_config.min_latency_ms + (m_rng() % (range + 1));
}

void NetworkSim::schedule_delivery(int target, const ScheduledOp& proto) {
    ScheduledOp sop = proto;
    // from_replica is the one-based Buffer replica id; lanes are zero-based.
    const size_t producer =
        static_cast<size_t>(sop.from_lane) - 1;
    sop.deliver_at_ms = m_clock + random_latency();
    if (!m_config.allow_reorder) {
        // Per-(target, producer) monotonicity: this operation may not come due
        // before the previous operation from the same producer. Latency stays
        // random and other producers still interleave freely.
        uint64_t& lane = m_last_scheduled[target][producer];
        if (sop.deliver_at_ms <= lane) sop.deliver_at_ms = lane + 1;
        lane = sop.deliver_at_ms;
    }
    m_queues[target].push(sop);
    // Retransmission follows the same lane, so a duplicate can never overtake
    // the original it duplicates.
    double roll = static_cast<double>(m_rng() % 10000) / 10000.0;
    if (roll < m_config.duplicate_probability) {
        ScheduledOp dup = sop;
        dup.deliver_at_ms = m_clock + random_latency();
        if (!m_config.allow_reorder) {
            uint64_t& lane = m_last_scheduled[target][producer];
            if (dup.deliver_at_ms <= lane) dup.deliver_at_ms = lane + 1;
            lane = dup.deliver_at_ms;
        }
        m_queues[target].push(dup);
    }
}

void NetworkSim::schedule_to_peers(int source, const Operation& op) {
    for (int r = 0; r < static_cast<int>(m_replicas.size()); ++r) {
        if (r == source) continue;
        ScheduledOp sop;
        sop.from_lane = static_cast<uint16_t>(source + 1);
        sop.op = op;
        schedule_delivery(r, sop);
    }
}

void NetworkSim::broadcast(int source, const Operation& op) {
    if (m_offline[source]) {
        // A disconnected replica sends nothing until it reconnects.
        m_pending_outbound[source].push_back(op);
        return;
    }
    schedule_to_peers(source, op);
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
    // What this replica missed is already in its inbox; the next tick delivers
    // it now that the replica is online again.
    //
    // Release what this replica produced while it was off the network.
    auto outbound = std::move(m_pending_outbound[replica]);
    m_pending_outbound[replica].clear();
    for (const auto& op : outbound)
        schedule_to_peers(replica, op);
}

void NetworkSim::block_link(int from, int to) {
    m_links[from][to] = true;
}

void NetworkSim::unblock_link(int from, int to) {
    m_links[from][to] = false;
}

void NetworkSim::tick(uint64_t ms) {
    m_clock += ms;
    for (int r = 0; r < static_cast<int>(m_replicas.size()); ++r) {
        if (m_offline[r]) continue;
        std::vector<Operation> batch;
        // Collect due operations, leaving anything behind a blocked link queued.
        std::vector<ScheduledOp> held;
        while (!m_queues[r].empty() && m_queues[r].top().deliver_at_ms <= m_clock) {
            ScheduledOp sop = m_queues[r].top();
            m_queues[r].pop();
            const int from = static_cast<int>(sop.from_lane) - 1;
            if (from < 0 || from >= static_cast<int>(m_links.size())) continue;
            if (m_links[from][r]) {
                held.push_back(sop);
                continue;
            }
            m_delivery_log.emplace_back(from, r);
            batch.push_back(sop.op);
        }
        for (const auto& sop : held) m_queues[r].push(sop);
        if (!batch.empty())
            m_replicas[r].apply_ops(batch);
    }
}

void NetworkSim::drain() {
    // A drain is the end-of-test flush: it heals every partition, because a
    // scenario asserts convergence after healing.
    for (auto& links : m_links) std::fill(links.begin(), links.end(), false);
    for (size_t r = 0; r < m_replicas.size(); ++r)
        if (m_offline[r]) reconnect(static_cast<int>(r));
    // Move the clock forward, never backward: operations scheduled after an
    // earlier drain already carry a delivery time beyond the old drain's mark,
    // and rewinding the clock would strand them forever.
    if (m_clock < UINT64_MAX / 2) m_clock = UINT64_MAX / 2;
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
        if (buf.visible_length() != static_cast<uint32_t>(text.size())) {
            QFAIL(qPrintable(QString("INV-1 at %1 r%2").arg(context).arg(r)));
        }

        // INV-2 + INV-8: fragment byte sums match rope lengths
        uint32_t vis = 0, del = 0;
        for (const auto& f : frags) {
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

bool NetworkSim::is_online(int replica) const {
    return !m_offline[replica];
}

bool NetworkSim::is_link_blocked(int from, int to) const {
    return m_links[from][to];
}

size_t NetworkSim::queued_inbound(int replica) const {
    return m_queues[replica].size();
}

size_t NetworkSim::queued_outbound(int replica) const {
    return m_pending_outbound[replica].size();
}

}  // namespace CollabText::Crdt
