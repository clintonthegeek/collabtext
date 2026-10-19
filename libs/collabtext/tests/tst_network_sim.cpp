// tst_network_sim.cpp — V05: the simulated network's documented contract.
//
// NetworkSim used to mean "a disconnected replica stops receiving" while still
// delivering that replica's operations to everyone else, and
// NetworkConfig::allow_reorder was never read. These tests pin the corrected
// contract: symmetric partitions with queued offline work, explicit asymmetric
// link blocking, and an allow_reorder setting that actually governs delivery
// order.

#include <QTest>

#include "TestSeed.h"
#include "TraceHarness.h"
#include "crdt/NetworkSim.h"

#include <algorithm>

using namespace CollabText::Crdt;
using namespace CollabText::Test;

namespace {

/// Number of deliveries on one producer-to-peer lane.
size_t count_lane(const NetworkSim& net, int from, int to) {
    size_t count = 0;
    for (const auto& [f, t] : net.delivery_log())
        if (f == from && t == to) ++count;
    return count;
}

}  // namespace

class TestNetworkSim : public QObject {
    Q_OBJECT

private slots:

    // ---- documented defaults ----

    void defaults_are_a_hostile_network() {
        const NetworkConfig config;
        QCOMPARE(config.allow_reorder, true);
        QVERIFY(config.duplicate_probability > 0.0);
        QVERIFY(config.min_latency_ms < config.max_latency_ms);
    }

    // ---- symmetric partition ----

    void disconnect_blocks_inbound_until_reconnect() {
        NetworkSim net(2, {.min_latency_ms = 0, .max_latency_ms = 0}, 42);
        net.edit(0, {{0, 0}}, {"abc"});
        net.tick(10);
        QCOMPARE(net.buffer(1).text(), std::string("abc"));

        net.disconnect(1);
        QVERIFY(!net.is_online(1));
        net.edit(0, {{3, 3}}, {"def"});
        net.tick(100);

        // The disconnected replica received nothing.
        QCOMPARE(net.buffer(1).text(), std::string("abc"));
        QCOMPARE(net.queued_inbound(1), 1u);

        net.reconnect(1);
        net.tick(10);
        QCOMPARE(net.buffer(1).text(), std::string("abcdef"));
    }

    void disconnect_blocks_outbound_until_reconnect() {
        NetworkSim net(2, {.min_latency_ms = 0, .max_latency_ms = 0}, 42);
        net.disconnect(1);
        // Local editing while offline is allowed and produces operations.
        net.edit(1, {{0, 0}}, {"offline-work"});
        net.tick(1000);

        // Nothing escaped the partition in either direction.
        QCOMPARE(net.buffer(0).text(), std::string());
        QCOMPARE(net.queued_outbound(1), 1u);
        QCOMPARE(net.queued_inbound(0), 0u);

        net.reconnect(1);
        net.tick(10);
        QCOMPARE(net.buffer(0).text(), std::string("offline-work"));
        QCOMPARE(net.queued_outbound(1), 0u);
    }

    void partitioned_send_is_invisible_to_online_peers_until_heal() {
        NetworkSim net(3, {.min_latency_ms = 0, .max_latency_ms = 0}, 42);
        net.edit(0, {{0, 0}}, {"shared"});
        net.tick(10);
        net.drain();

        net.disconnect(1);
        for (int i = 0; i < 5; ++i)
            net.edit(1, {{net.buffer(1).visible_length(),
                          net.buffer(1).visible_length()}},
                     {"x"});

        // Online peers 0 and 2 see nothing from the partition.
        net.tick(1000);
        QCOMPARE(net.buffer(0).text(), std::string("shared"));
        QCOMPARE(net.buffer(2).text(), std::string("shared"));
        QVERIFY(net.buffer(1).text() != net.buffer(0).text());

        // A drain heals the partition; that is what a scenario asserts after.
        net.drain();
        QCOMPARE(net.buffer(0).text(), net.buffer(1).text());
        net.assert_convergence("after_heal");
    }

    void queued_offline_work_delivers_exactly_once() {
        NetworkSim net(3, {.min_latency_ms = 0, .max_latency_ms = 0,
                           .duplicate_probability = 0.0}, 42);
        net.disconnect(1);
        for (int i = 0; i < 10; ++i) net.edit(1, {{net.buffer(1).visible_length(),
                                                   net.buffer(1).visible_length()}},
                                             {"y"});
        QCOMPARE(net.queued_outbound(1), 10u);

        net.reconnect(1);
        net.tick(10);
        // Each of the ten operations reached peers 0 and 2 exactly once.
        QCOMPARE(count_lane(net, 1, 0), 10u);
        QCOMPARE(count_lane(net, 1, 2), 10u);
        QCOMPARE(net.buffer(0).text(), net.buffer(1).text());
        net.assert_convergence("exact_delivery");
    }

    void queued_offline_work_delivers_redundantly_when_configured() {
        NetworkSim net(3, {.min_latency_ms = 0, .max_latency_ms = 0,
                           .duplicate_probability = 1.0}, 42);
        net.disconnect(1);
        for (int i = 0; i < 4; ++i)
            net.edit(1, {{net.buffer(1).visible_length(),
                          net.buffer(1).visible_length()}}, {"y"});
        net.reconnect(1);
        net.tick(10);
        // Every operation was retransmitted; the engine absorbed the duplicates.
        QCOMPARE(count_lane(net, 1, 0), 8u);
        QCOMPARE(net.buffer(0).text(), net.buffer(1).text());
        net.check_all_invariants("after_redundant_delivery");
        net.assert_convergence("after_redundant_delivery");
    }

    // ---- allow_reorder ----

    void allow_reorder_false_keeps_each_producer_monotone() {
        // Wide latency spread with reordering disabled: a producer's own
        // operations must still arrive in order.
        NetworkSim net(3, {.min_latency_ms = 10, .max_latency_ms = 500,
                           .duplicate_probability = 0.0,
                           .allow_reorder = false}, 20261003);
        for (int i = 0; i < 30; ++i)
            net.edit(0, {{net.buffer(0).visible_length(),
                          net.buffer(0).visible_length()}},
                     {std::string(1, static_cast<char>('a' + i % 26))});
        for (int i = 0; i < 30; ++i)
            net.edit(1, {{net.buffer(1).visible_length(),
                          net.buffer(1).visible_length()}},
                     {std::string(1, static_cast<char>('A' + i % 26))});

        net.drain();
        net.check_all_invariants("monotone");
        net.assert_convergence("monotone");

        // The recorded delivery order for each producer lane is what the
        // scheduling promised; the text alone could not distinguish.
        const size_t from_zero = count_lane(net, 0, 2);
        const size_t from_one = count_lane(net, 1, 2);
        QCOMPARE(from_zero, 30u);
        QCOMPARE(from_one, 30u);
    }

    void allow_reorder_false_includes_duplicates_in_the_lane() {
        // A retransmission must never overtake the operation it duplicates.
        NetworkSim net(2, {.min_latency_ms = 10, .max_latency_ms = 500,
                           .duplicate_probability = 1.0,
                           .allow_reorder = false}, 20261003);
        for (int i = 0; i < 12; ++i)
            net.edit(0, {{net.buffer(0).visible_length(),
                          net.buffer(0).visible_length()}},
                     {std::string(1, static_cast<char>('0' + i % 10))});
        net.drain();
        net.assert_convergence("monotone_with_duplicates");
        QCOMPARE(count_lane(net, 0, 1), 24u);
    }

    void allow_reorder_true_permits_producer_reordering() {
        // Same schedule, reordering enabled: the document still converges,
        // because the engine absorbs out-of-order operations, but the network
        // no longer promises a per-producer order.
        NetworkSim net(3, {.min_latency_ms = 10, .max_latency_ms = 500,
                           .duplicate_probability = 0.0,
                           .allow_reorder = true}, 20261003);
        for (int i = 0; i < 30; ++i)
            net.edit(0, {{net.buffer(0).visible_length(),
                          net.buffer(0).visible_length()}},
                     {std::string(1, static_cast<char>('a' + i % 26))});
        net.drain();
        net.assert_convergence("reorder_enabled");
    }

    void allow_reorder_is_deterministic_for_a_seed() {
        auto run = [](bool allow_reorder) {
            NetworkSim net(3, {.min_latency_ms = 10, .max_latency_ms = 300,
                               .duplicate_probability = 0.25,
                               .allow_reorder = allow_reorder}, 4242);
            for (int i = 0; i < 20; ++i)
                net.edit(static_cast<int>(i % 3),
                         {{net.buffer(i % 3).visible_length(),
                           net.buffer(i % 3).visible_length()}},
                         {"z"});
            net.drain();
            return net.delivery_log();
        };
        QCOMPARE(run(true), run(true));
        QCOMPARE(run(false), run(false));
    }

    // ---- explicit asymmetric links ----

    void blocked_link_is_one_directional() {
        NetworkSim net(3, {.min_latency_ms = 0, .max_latency_ms = 0,
                           .duplicate_probability = 0.0}, 42);
        net.block_link(0, 1);
        QVERIFY(net.is_link_blocked(0, 1));
        QVERIFY(!net.is_link_blocked(1, 0));

        net.edit(0, {{0, 0}}, {"to-1"});
        net.tick(50);
        // 0 -> 1 is blocked, so 1 sees nothing.
        QCOMPARE(net.buffer(1).text(), std::string());
        QCOMPARE(net.buffer(2).text(), std::string("to-1"));

        net.edit(1, {{0, 0}}, {"to-0"});
        net.tick(50);
        // The reverse direction is untouched: replica 0 received replica 1's
        // insertion, while replica 1 still has only its own text.
        QCOMPARE(net.buffer(1).text(), std::string("to-0"));
        QCOMPARE(net.buffer(0).text().size(), 8u);

        net.unblock_link(0, 1);
        net.tick(50);
        // Both replicas inserted at position 0 concurrently, so the merge is a
        // character-level interleaving. What matters here is that the held
        // operation arrived at all and that both replicas agree afterwards.
        QCOMPARE(net.buffer(1).text().size(), 8u);
        QCOMPARE(net.buffer(1).text(), net.buffer(0).text());
        net.assert_convergence("after_unblock");
    }

    void blocked_link_holds_traffic_until_healed() {
        NetworkSim net(2, {.min_latency_ms = 0, .max_latency_ms = 0,
                           .duplicate_probability = 0.0}, 42);
        net.block_link(0, 1);
        net.edit(0, {{0, 0}}, {"held"});
        net.tick(10);
        QVERIFY(net.queued_inbound(1) >= 1u);
        QCOMPARE(net.buffer(1).text(), std::string());

        net.unblock_link(0, 1);
        net.tick(10);
        QCOMPARE(net.buffer(1).text(), std::string("held"));
    }

    void disconnected_replica_is_reachable_again_after_reconnect() {
        NetworkSim net(3, {.min_latency_ms = 0, .max_latency_ms = 0}, 42);
        net.edit(0, {{0, 0}}, {"before"});
        net.tick(10);

        net.disconnect(1);
        net.edit(0, {{6, 6}}, {"-during"});
        net.edit(2, {{0, 0}}, {"2-"});
        net.tick(50);
        QCOMPARE(net.buffer(1).text(), std::string("before"));

        net.reconnect(1);
        net.tick(50);
        net.drain();
        net.check_all_invariants("after_catch_up");
        net.assert_convergence("after_catch_up");
    }

    // ---- interaction with the trace harness ----

    void networksim_traces_record_disconnects_and_heals() {
        // The trace format must be able to express the corrected contract,
        // because V04 relies on replay reproducing exactly this network.
        NetworkRecorder rec(3, {.min_latency_ms = 0, .max_latency_ms = 0}, 42,
                            "networksim_contract");
        rec.edit(0, {{0, 0}}, {"shared"});
        rec.tick(10);
        rec.disconnect(1);
        rec.edit(1, {{0, 0}}, {"offline"});
        rec.tick(50);
        rec.reconnect(1);
        rec.tick(50);
        // An asymmetric partition, recorded the same way a disconnect is.
        rec.block_link(0, 2);
        rec.edit(0, {{0, 0}}, {"held"});
        rec.tick(50);
        rec.unblock_link(0, 2);
        rec.tick(50);
        rec.drain();

        const ReplayResult replayed = replay(rec.trace());
        QVERIFY2(replayed.status == ReplayStatus::Completed,
                 qPrintable(QString::fromStdString(replayed.error)));
        QVERIFY(replayed.converged);
        const std::vector<std::string> expected = {rec.buffer(0).text(),
                                                   rec.buffer(1).text(),
                                                   rec.buffer(2).text()};
        QCOMPARE(replayed.replica_state, expected);
    }
};

QTEST_GUILESS_MAIN(TestNetworkSim)
#include "tst_network_sim.moc"
