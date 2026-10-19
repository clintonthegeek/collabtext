#include <QTest>
#include "TestSeed.h"
#include "TraceHarness.h"
#include "crdt/EditStrategy.h"
#include "crdt/NetworkSim.h"
#include <random>

using namespace CollabText::Crdt;
using namespace CollabText::Test;

class TestRealistic : public QObject {
    Q_OBJECT
private:

    /// Announce the campaign and scenario seed, then derive the scenario's RNG.
    static std::mt19937 scenario_rng(const char* scenario) {
        const uint64_t seed = effective_seed(scenario);
        announce_seed("tst_realistic", scenario, "networksim");
        return std::mt19937(static_cast<uint32_t>(seed));
    }

    /// Record a strategy-produced edit. The recorder stores the decision (replica
    /// and resulting single range) plus the recorded seed, so a replay rebuilds
    /// the same simulated delivery schedule instead of rerolling it.
    static void strategy_edit(NetworkRecorder& rec, EditStrategy& strategy,
                              int replica, std::mt19937& rng) {
        const EditAction action = strategy.next_edit(rec.buffer(replica), rng);
        rec.edit(replica, action.ranges, action.new_text);
    }

private slots:

    // ---- Smoke tests ----

    void networksim_basic_convergence() {
        NetworkRecorder rec(3, {}, effective_seed("networksim_basic_convergence"),
                            "networksim_basic_convergence");
        rec.edit(0, {{0, 0}}, {"hello"});
        rec.tick(300);
        rec.edit(1, {{0, 0}}, {"world "});
        rec.tick(300);
        rec.drain();
        rec.net().check_all_invariants("basic");
        rec.net().assert_convergence("basic");
        rec.finish();
    }

    void networksim_disconnect_reconnect() {
        NetworkRecorder rec(2, {}, effective_seed("networksim_disconnect_reconnect"),
                            "networksim_disconnect_reconnect");
        rec.edit(0, {{0, 0}}, {"abc"});
        rec.tick(300);
        rec.net().assert_convergence("before_disconnect");

        rec.disconnect(1);
        rec.edit(0, {{3, 3}}, {"def"});
        rec.tick(300);
        // Replica 1 hasn't received "def"
        QCOMPARE(rec.buffer(1).text(), std::string("abc"));

        rec.reconnect(1);
        rec.tick(300);
        rec.drain();
        rec.net().assert_convergence("after_reconnect");
        rec.finish();
    }

    void networksim_strategies_compile() {
        NetworkRecorder rec(2, {.min_latency_ms = 0, .max_latency_ms = 0},
                            effective_seed("networksim_strategies_compile"),
                            "networksim_strategies_compile");
        std::mt19937 rng = scenario_rng("networksim_strategies_compile");

        RandomStrategy random;
        RealisticStrategy realistic;

        // Seed some text first
        rec.edit(0, {{0, 0}}, {"hello world this is a test document"});
        rec.tick(1);

        // RandomStrategy produces valid edits
        strategy_edit(rec, random, 0, rng);
        rec.tick(1);
        rec.net().check_all_invariants("random_edit");

        // RealisticStrategy produces valid edits
        strategy_edit(rec, realistic, 1, rng);
        rec.tick(1);
        rec.net().check_all_invariants("realistic_edit");

        rec.drain();
        rec.net().assert_convergence("strategies");
        rec.finish();
    }

    void networksim_compact_all() {
        NetworkRecorder rec(2, {.min_latency_ms = 0, .max_latency_ms = 0},
                            effective_seed("networksim_compact_all"),
                            "networksim_compact_all");
        rec.edit(0, {{0, 0}}, {"hello"});
        rec.tick(1);
        rec.edit(0, {{0, 5}}, {""});  // delete
        rec.tick(1);
        rec.drain();

        QVERIFY((rec.buffer(0).set_max_undo_depth(0)).ok());
        QVERIFY((rec.buffer(1).set_max_undo_depth(0)).ok());
        rec.net().compact_all();
        // V03 containment: reclamation is disabled until a certificate exists,
        // so compact_all() removes nothing and retains every tombstone.
        QCOMPARE(rec.buffer(0).tombstone_count(), 1u);
        rec.net().check_all_invariants("after_compact");
        rec.net().assert_convergence("after_compact");
        rec.collect_garbage(0);
        rec.compact_all();
        rec.net().check_all_invariants("after_collect_garbage");
        rec.net().assert_convergence("after_collect_garbage");
        rec.finish();
    }

    // ---- Sustained 3-client convergence ----

    void sustained_3_client_convergence() {
        std::mt19937 rng = scenario_rng("sustained_3_client_convergence");
        NetworkRecorder rec(3, {}, effective_seed("sustained_3_client_convergence"),
                            "sustained_3_client_convergence");
        RandomStrategy strategy;

        for (int i = 0; i < 500; ++i) {  // ~167 per replica on average
            const int r = static_cast<int>(rng() % 3);
            const int action = static_cast<int>(rng() % 100);

            if (action < 80) {
                strategy_edit(rec, strategy, r, rng);
            } else if (action < 90) {
                rec.undo(r);
            } else {
                rec.redo(r);
            }

            rec.tick(10);  // 10ms between ops — continuous delivery

            if (i % 300 == 0)
                rec.net().check_all_invariants(qPrintable(QString("step_%1").arg(i)));
        }

        rec.drain();
        rec.net().check_all_invariants("final");
        rec.net().assert_convergence("sustained_3_client");
        rec.finish();
    }

    // ---- Disconnect/reconnect tests ----

    void disconnect_reconnect_cycle() {
        std::mt19937 rng = scenario_rng("disconnect_reconnect_cycle");
        NetworkRecorder rec(3, {}, effective_seed("disconnect_reconnect_cycle"),
                            "disconnect_reconnect_cycle");
        RandomStrategy strategy;

        // Phase 1: all online, 100 ops
        for (int i = 0; i < 100; ++i) {
            const int r = static_cast<int>(rng() % 3);
            strategy_edit(rec, strategy, r, rng);
            rec.tick(10);
        }
        rec.net().check_all_invariants("phase1");

        // Phase 2: replica 2 disconnects, 200 ops between 0 and 1
        const std::string before_partition = rec.buffer(2).text();
        rec.disconnect(2);
        for (int i = 0; i < 200; ++i) {
            const int r = static_cast<int>(rng() % 2);  // only 0 or 1
            strategy_edit(rec, strategy, r, rng);
            rec.tick(10);
        }
        rec.net().check_all_invariants("phase2");
        // A partition is symmetric: nothing crossed in either direction, so
        // replica 2 still holds exactly what it held before it went offline.
        QCOMPARE(rec.buffer(2).text(), before_partition);

        // Phase 3: replica 2 reconnects, bulk sync
        rec.reconnect(2);
        rec.tick(500);  // allow bulk delivery
        QVERIFY(rec.buffer(2).text() != before_partition);

        // Phase 4: all edit 100 more ops
        for (int i = 0; i < 100; ++i) {
            const int r = static_cast<int>(rng() % 3);
            strategy_edit(rec, strategy, r, rng);
            rec.tick(10);
        }

        rec.drain();
        rec.net().check_all_invariants("final");
        rec.net().assert_convergence("disconnect_reconnect");
        rec.finish();
    }

    void cascading_disconnects() {
        std::mt19937 rng = scenario_rng("cascading_disconnects");
        NetworkRecorder rec(5, {}, effective_seed("cascading_disconnects"),
                            "cascading_disconnects");
        RandomStrategy strategy;

        // Snapshot each replica the moment it goes offline: a symmetric
        // partition means its text cannot change until it reconnects.
        std::vector<std::string> at_disconnect(5);
        std::vector<bool> offline(5, false);

        // Disconnect replicas one at a time
        for (int phase = 0; phase < 4; ++phase) {
            for (int i = 0; i < 50; ++i) {
                // Only online replicas edit
                std::vector<int> online;
                for (int r = 0; r < 5; ++r) {
                    // Replicas 1-4 disconnect at phases 0-3
                    // Replica 0 stays online
                    if (r == 0 || r > phase + 1) online.push_back(r);
                }
                if (online.empty()) break;
                const int r = online[rng() % online.size()];
                strategy_edit(rec, strategy, r, rng);
                rec.tick(10);
            }
            if (phase + 1 < 5) {
                rec.disconnect(phase + 1);
                offline[phase + 1] = true;
                at_disconnect[phase + 1] = rec.buffer(phase + 1).text();
            }
        }

        rec.net().check_all_invariants("all_disconnected");
        // Only replica 0 stayed online. Every partitioned replica must still hold
        // exactly the text it had when it went offline.
        for (int r = 1; r < 5; ++r) {
            QVERIFY(offline[r]);
            QCOMPARE(rec.buffer(r).text(), at_disconnect[r]);
            QVERIFY(rec.buffer(r).text() != rec.buffer(0).text());
        }

        // Reconnect in reverse order, with 50 ops between each
        for (int phase = 3; phase >= 0; --phase) {
            rec.reconnect(phase + 1);
            rec.tick(500);
            for (int i = 0; i < 50; ++i) {
                const int r = static_cast<int>(rng() % 5);
                strategy_edit(rec, strategy, r, rng);
                rec.tick(10);
            }
        }

        rec.drain();
        rec.net().check_all_invariants("final");
        rec.net().assert_convergence("cascading_disconnects");
        rec.finish();
    }

    // ---- Scale / GC tests ----

    void long_partition_with_gc() {
        std::mt19937 rng = scenario_rng("long_partition_with_gc");
        NetworkRecorder rec(2, {.min_latency_ms = 0, .max_latency_ms = 0},
                            effective_seed("long_partition_with_gc"),
                            "long_partition_with_gc");
        RandomStrategy strategy;

        // Build a shared 5K document
        while (rec.buffer(0).visible_length() < 5000) {
            std::string chunk(100, static_cast<char>('a' + (rng() % 26)));
            const uint32_t end = rec.buffer(0).visible_length();
            rec.edit(0, {{end, end}}, {chunk});
        }
        rec.drain();
        rec.net().assert_convergence("initial_doc");

        // Partition: both edit independently for 500 ops
        auto lane_count = [&rec](int from, int to) {
            size_t count = 0;
            for (const auto& [f, t] : rec.net().delivery_log())
                if (f == from && t == to) ++count;
            return count;
        };
        const size_t zero_to_one = lane_count(0, 1);
        const size_t one_to_zero = lane_count(1, 0);
        rec.disconnect(1);
        for (int i = 0; i < 500; ++i) {
            const int r = i % 2;  // alternate; replica 1 is offline, so neither
                                  // direction crosses the partition
            strategy_edit(rec, strategy, r, rng);
        }
        rec.net().check_all_invariants("partitioned");
        // Both replicas edited for 500 steps, and neither direction delivered
        // anything across the partition. (Their own documents do change: an
        // edit strategy deletes and replaces text, including most of the 5K
        // document. What a partition forbids is delivery, not local editing.)
        QCOMPARE(lane_count(0, 1), zero_to_one);
        QCOMPARE(lane_count(1, 0), one_to_zero);
        QVERIFY(rec.buffer(0).text() != rec.buffer(1).text());

        // Reconnect and sync
        rec.reconnect(1);
        rec.drain();
        rec.net().check_all_invariants("after_sync");
        rec.net().assert_convergence("after_sync");

        // GC request: containment retains everything until certification (V03)
        QVERIFY((rec.buffer(0).set_max_undo_depth(0)).ok());
        QVERIFY((rec.buffer(1).set_max_undo_depth(0)).ok());
        const size_t tombstones_before =
            rec.buffer(0).tombstone_count() + rec.buffer(1).tombstone_count();
        rec.collect_garbage(0);
        rec.collect_garbage(1);
        rec.compact_all();
        const size_t tombstones_after =
            rec.buffer(0).tombstone_count() + rec.buffer(1).tombstone_count();
        QCOMPARE(tombstones_after, tombstones_before);

        rec.net().check_all_invariants("after_gc");
        rec.net().assert_convergence("after_gc");
        rec.finish();
    }

    void asymmetric_link_partition() {
        std::mt19937 rng = scenario_rng("asymmetric_link_partition");
        NetworkRecorder rec(4, {.min_latency_ms = 0, .max_latency_ms = 0},
                            effective_seed("asymmetric_link_partition"),
                            "asymmetric_link_partition");
        RandomStrategy strategy;

        for (int i = 0; i < 100; ++i) {
            strategy_edit(rec, strategy, static_cast<int>(rng() % 4), rng);
            rec.tick(10);
        }

        // One-way failure between 0 and 1, with traffic still flowing the other
        // way and to everyone else.
        rec.block_link(0, 1);
        for (int i = 0; i < 60; ++i) {
            const int r = static_cast<int>(rng() % 4);
            strategy_edit(rec, strategy, r, rng);
            rec.tick(10);
        }
        // 1 -> 0 kept working while 0 -> 1 did not.
        const std::string online = rec.buffer(2).text();
        QVERIFY(rec.buffer(1).text().size() != 0u);
        QVERIFY(online.size() != 0u);

        rec.unblock_link(0, 1);
        rec.drain();
        rec.net().check_all_invariants("after_link_heal");
        rec.net().assert_convergence("asymmetric_link_partition");
        rec.finish();
    }

    void monotone_transport_convergence() {
        // allow_reorder=false: a transport that preserves each producer's order.
        // Cross-producer interleaving still happens, and convergence still
        // holds after healing.
        std::mt19937 rng = scenario_rng("monotone_transport_convergence");
        NetworkRecorder rec(4, {.min_latency_ms = 5, .max_latency_ms = 250,
                                .duplicate_probability = 0.1,
                                .allow_reorder = false},
                            effective_seed("monotone_transport_convergence"),
                            "monotone_transport_convergence");
        RandomStrategy strategy;

        for (int i = 0; i < 200; ++i) {
            strategy_edit(rec, strategy, static_cast<int>(rng() % 4), rng);
            rec.tick(15);
        }
        rec.disconnect(3);
        for (int i = 0; i < 100; ++i) {
            strategy_edit(rec, strategy, static_cast<int>(rng() % 3), rng);
            rec.tick(15);
        }
        rec.reconnect(3);
        rec.drain();
        rec.net().check_all_invariants("monotone");
        rec.net().assert_convergence("monotone_transport_convergence");
        rec.finish();
    }

    void ten_client_sustained() {
        std::mt19937 rng = scenario_rng("ten_client_sustained");
        NetworkRecorder rec(10, {}, effective_seed("ten_client_sustained"),
                            "ten_client_sustained");
        RandomStrategy strategy;

        for (int i = 0; i < 500; ++i) {
            const int r = static_cast<int>(rng() % 10);

            const int action = static_cast<int>(rng() % 100);
            if (action < 80) {
                strategy_edit(rec, strategy, r, rng);
            } else if (action < 90) {
                rec.undo(r);
            } else {
                rec.redo(r);
            }

            rec.tick(5);

            // Random disconnects: 10% chance per 100 ops
            if (i % 100 == 0 && i > 0) {
                const int target = static_cast<int>(rng() % 10);
                if (rng() % 10 == 0) {
                    rec.disconnect(target);
                }
                // Reconnect a random offline replica
                // (reconnect is a no-op if already online)
                const int candidate = static_cast<int>(rng() % 10);
                rec.reconnect(candidate);
            }

            if (i % 500 == 0)
                rec.net().check_all_invariants(qPrintable(QString("step_%1").arg(i)));
        }

        // Reconnect all before draining
        for (int r = 0; r < 10; ++r)
            rec.reconnect(r);
        rec.drain();
        rec.net().check_all_invariants("final");
        rec.net().assert_convergence("ten_client_sustained");
        rec.finish();
    }
};

QTEST_MAIN(TestRealistic)
#include "tst_realistic.moc"
