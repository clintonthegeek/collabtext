#include <QTest>
#include "crdt/NetworkSim.h"
#include "crdt/EditStrategy.h"
#include <random>

using namespace CollabText::Crdt;

class TestRealistic : public QObject {
    Q_OBJECT
private slots:

    // ---- Task 4: Smoke Tests ----

    void networksim_basic_convergence() {
        NetworkSim net(3, {}, 42);
        net.edit(0, {{0, 0}}, {"hello"});
        net.tick(300);
        net.edit(1, {{0, 0}}, {"world "});
        net.tick(300);
        net.drain();
        net.check_all_invariants("basic");
        net.assert_convergence("basic");
    }

    void networksim_disconnect_reconnect() {
        NetworkSim net(2, {}, 42);
        net.edit(0, {{0, 0}}, {"abc"});
        net.tick(300);
        net.assert_convergence("before_disconnect");

        net.disconnect(1);
        net.edit(0, {{3, 3}}, {"def"});
        net.tick(300);
        // Replica 1 hasn't received "def"
        QCOMPARE(net.buffer(1).text(), std::string("abc"));

        net.reconnect(1);
        net.tick(300);
        net.drain();
        net.assert_convergence("after_reconnect");
    }

    void networksim_strategies_compile() {
        NetworkSim net(2, {.min_latency_ms = 0, .max_latency_ms = 0}, 42);
        std::mt19937 rng(42);

        RandomStrategy random;
        RealisticStrategy realistic;

        // Seed some text first
        net.edit(0, {{0, 0}}, {"hello world this is a test document"});
        net.tick(1);

        // RandomStrategy produces valid edits
        auto a1 = random.next_edit(net.buffer(0), rng);
        net.edit(0, a1);
        net.tick(1);
        net.check_all_invariants("random_edit");

        // RealisticStrategy produces valid edits
        auto a2 = realistic.next_edit(net.buffer(1), rng);
        net.edit(1, a2);
        net.tick(1);
        net.check_all_invariants("realistic_edit");

        net.drain();
        net.assert_convergence("strategies");
    }

    void networksim_compact_all() {
        NetworkSim net(2, {.min_latency_ms = 0, .max_latency_ms = 0}, 42);
        net.edit(0, {{0, 0}}, {"hello"});
        net.tick(1);
        net.edit(0, {{0, 5}}, {""});  // delete
        net.tick(1);
        net.drain();

        net.buffer(0).set_max_undo_depth(0);
        net.buffer(1).set_max_undo_depth(0);
        size_t removed = net.compact_all();
        QVERIFY(removed > 0);
        net.check_all_invariants("after_compact");
        net.assert_convergence("after_compact");
    }

    // ---- Task 5: Sustained 3-Client Convergence ----

    void sustained_3_client_convergence() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        std::mt19937 rng(seed);

        NetworkSim net(3, {}, seed);
        RandomStrategy strategy;

        for (int i = 0; i < 500; ++i) {  // ~167 per replica on average
            int r = rng() % 3;
            int action = rng() % 100;

            if (action < 80) {
                auto a = strategy.next_edit(net.buffer(r), rng);
                net.edit(r, a);
            } else if (action < 90) {
                net.undo(r);
            } else {
                net.redo(r);
            }

            net.tick(10);  // 10ms between ops — continuous delivery

            if (i % 300 == 0)
                net.check_all_invariants(qPrintable(QString("step_%1").arg(i)));
        }

        net.drain();
        net.check_all_invariants("final");
        net.assert_convergence("sustained_3_client");
    }

    // ---- Task 6: Disconnect/Reconnect Tests ----

    void disconnect_reconnect_cycle() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        std::mt19937 rng(seed);

        NetworkSim net(3, {}, seed);
        RandomStrategy strategy;

        // Phase 1: all online, 100 ops
        for (int i = 0; i < 100; ++i) {
            int r = rng() % 3;
            net.edit(r, strategy.next_edit(net.buffer(r), rng));
            net.tick(10);
        }
        net.check_all_invariants("phase1");

        // Phase 2: replica 2 disconnects, 200 ops between 0 and 1
        net.disconnect(2);
        for (int i = 0; i < 200; ++i) {
            int r = rng() % 2;  // only 0 or 1
            net.edit(r, strategy.next_edit(net.buffer(r), rng));
            net.tick(10);
        }
        net.check_all_invariants("phase2");

        // Phase 3: replica 2 reconnects, bulk sync
        net.reconnect(2);
        net.tick(500);  // allow bulk delivery

        // Phase 4: all edit 100 more ops
        for (int i = 0; i < 100; ++i) {
            int r = rng() % 3;
            net.edit(r, strategy.next_edit(net.buffer(r), rng));
            net.tick(10);
        }

        net.drain();
        net.check_all_invariants("final");
        net.assert_convergence("disconnect_reconnect");
    }

    void cascading_disconnects() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        std::mt19937 rng(seed);

        NetworkSim net(5, {}, seed);
        RandomStrategy strategy;

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
                int r = online[rng() % online.size()];
                net.edit(r, strategy.next_edit(net.buffer(r), rng));
                net.tick(10);
            }
            if (phase + 1 < 5)
                net.disconnect(phase + 1);
        }

        net.check_all_invariants("all_disconnected");

        // Reconnect in reverse order, with 50 ops between each
        for (int phase = 3; phase >= 0; --phase) {
            net.reconnect(phase + 1);
            net.tick(500);
            for (int i = 0; i < 50; ++i) {
                int r = rng() % 5;
                net.edit(r, strategy.next_edit(net.buffer(r), rng));
                net.tick(10);
            }
        }

        net.drain();
        net.check_all_invariants("final");
        net.assert_convergence("cascading_disconnects");
    }

    // ---- Task 7: Scale/GC Tests ----

    void long_partition_with_gc() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        std::mt19937 rng(seed);

        NetworkSim net(2, {.min_latency_ms = 0, .max_latency_ms = 0}, seed);
        RandomStrategy strategy;

        // Build a shared 5K document
        while (net.buffer(0).visible_length() < 5000) {
            std::string chunk(100, static_cast<char>('a' + (rng() % 26)));
            net.edit(0, {{net.buffer(0).visible_length(),
                          net.buffer(0).visible_length()}}, {chunk});
        }
        net.drain();
        net.assert_convergence("initial_doc");

        // Partition: both edit independently for 500 ops
        net.disconnect(1);
        for (int i = 0; i < 500; ++i) {
            int r = i % 2;  // alternate, but 1 is offline so its ops aren't delivered
            net.edit(r, strategy.next_edit(net.buffer(r), rng));
        }
        net.check_all_invariants("partitioned");

        // Reconnect and sync
        net.reconnect(1);
        net.drain();
        net.check_all_invariants("after_sync");
        net.assert_convergence("after_sync");

        // GC: compute watermark, compact both
        net.buffer(0).set_max_undo_depth(0);
        net.buffer(1).set_max_undo_depth(0);
        size_t removed = net.compact_all();
        qDebug() << "Tombstones removed by compact:" << removed;

        net.check_all_invariants("after_gc");
        net.assert_convergence("after_gc");
    }

    void ten_client_sustained() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        std::mt19937 rng(seed);

        NetworkSim net(10, {}, seed);
        RandomStrategy strategy;

        for (int i = 0; i < 500; ++i) {
            int r = rng() % 10;

            int action = rng() % 100;
            if (action < 80) {
                net.edit(r, strategy.next_edit(net.buffer(r), rng));
            } else if (action < 90) {
                net.undo(r);
            } else {
                net.redo(r);
            }

            net.tick(5);

            // Random disconnects: 10% chance per 100 ops
            if (i % 100 == 0 && i > 0) {
                int target = rng() % 10;
                if (rng() % 10 == 0) {
                    net.disconnect(target);
                }
                // Reconnect a random offline replica
                for (int j = 0; j < 10; ++j) {
                    // Try reconnecting one that might be offline
                    // (reconnect is a no-op if already online)
                    int candidate = rng() % 10;
                    net.reconnect(candidate);
                    break;
                }
            }

            if (i % 500 == 0)
                net.check_all_invariants(qPrintable(QString("step_%1").arg(i)));
        }

        // Reconnect all before draining
        for (int r = 0; r < 10; ++r)
            net.reconnect(r);
        net.drain();
        net.check_all_invariants("final");
        net.assert_convergence("ten_client_sustained");
    }
};

QTEST_MAIN(TestRealistic)
#include "tst_realistic.moc"
