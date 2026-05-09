#include <QElapsedTimer>
#include <QTest>
#include "crdt/Buffer.h"
#include "crdt/Clock.h"

using namespace CollabText::Crdt;

class TestClock : public QObject {
    Q_OBJECT
private slots:
    void lamport_tick_increments() {
        Lamport clock(1, 1);
        Lamport t1 = clock.tick();
        Lamport t2 = clock.tick();
        QCOMPARE(t1.value, 1u);
        QCOMPARE(t2.value, 2u);
        QCOMPARE(clock.value, 3u);
    }
    void lamport_observe_jumps_ahead() {
        Lamport clock(1, 1);
        clock.observe(Lamport(2, 100));
        QCOMPARE(clock.value, 101u);
        QCOMPARE(clock.replica_id, (uint16_t)1);
    }
    void lamport_observe_does_not_go_backwards() {
        Lamport clock(1, 200);
        clock.observe(Lamport(2, 50));
        QCOMPARE(clock.value, 200u);
    }
    void lamport_ordering_value_first() {
        Lamport a(1, 10), b(2, 20);
        QVERIFY(a < b);
    }
    void lamport_ordering_replica_tiebreak() {
        Lamport a(1, 10), b(2, 10);
        QVERIFY(a < b);
    }
    void global_observe_and_get() {
        Global g;
        g.observe(Lamport(3, 42));
        QCOMPARE(g.get(3), 42u);
        QCOMPARE(g.get(0), 0u);
    }
    void global_observed() {
        Global g;
        g.observe(Lamport(1, 10));
        QVERIFY(g.observed(Lamport(1, 10)));
        QVERIFY(g.observed(Lamport(1, 5)));
        QVERIFY(!g.observed(Lamport(1, 11)));
        QVERIFY(!g.observed(Lamport(2, 1)));
    }
    void global_observed_all() {
        Global a, b;
        a.observe(Lamport(0, 10)); a.observe(Lamport(1, 20));
        b.observe(Lamport(0, 5)); b.observe(Lamport(1, 15));
        QVERIFY(a.observed_all(b));
        QVERIFY(!b.observed_all(a));
    }
    void global_observed_all_shorter_vector_fails() {
        Global a, b;
        a.observe(Lamport(0, 10));
        b.observe(Lamport(0, 10)); b.observe(Lamport(5, 1));
        QVERIFY(!a.observed_all(b));
    }
    void global_join() {
        Global a, b;
        a.observe(Lamport(0, 10)); a.observe(Lamport(1, 5));
        b.observe(Lamport(0, 3)); b.observe(Lamport(1, 20));
        a.join(b);
        QCOMPARE(a.get(0), 10u);
        QCOMPARE(a.get(1), 20u);
    }
    void global_meet() {
        Global a, b;
        a.observe(Lamport(0, 10)); a.observe(Lamport(1, 20));
        b.observe(Lamport(0, 5)); b.observe(Lamport(1, 30));
        a.meet(b);
        QCOMPARE(a.get(0), 5u);
        QCOMPARE(a.get(1), 20u);
    }
    // Regression: SBO heap-promotion with replica_id forcing capacity past
    // UINT16_MAX truncated m_capacity to 0, corrupting the Global. Reproduces
    // for any replica_id ≥ 32768 (so doubled growth from 4 reaches 65536).
    void global_observe_large_replica_id_does_not_corrupt() {
        Global g;
        g.observe(Lamport(47372, 16384));
        QCOMPARE(g.get(47372), 16384u);
        QVERIFY(g.size() >= 47373);
    }
    void global_observe_replica_id_at_uint16_max() {
        Global g;
        g.observe(Lamport(65535, 1));
        QCOMPARE(g.get(65535), 1u);
        QVERIFY(g.size() >= 65536);
    }
    void global_copy_after_large_replica_id() {
        Global a;
        a.observe(Lamport(47372, 16384));
        Global b = a;             // copy ctor through heap path
        QCOMPARE(b.get(47372), 16384u);
        Global c;
        c = a;                    // copy assignment through heap path
        QCOMPARE(c.get(47372), 16384u);
    }
    void global_sparse_layout_keeps_dense_view() {
        // The dense view is what the wire encoder relies on: g.size() returns
        // (max_replica_id + 1) and g[i] returns the value for replica_id == i.
        // Multiple distinct replicas observed on a sparse Global must still
        // round-trip through the dense API.
        Global g;
        g.observe(Lamport(7, 11));
        g.observe(Lamport(3, 22));
        g.observe(Lamport(7, 5));   // older — must not regress
        QCOMPARE(g.size(), size_t(8));
        QCOMPARE(g[0], 0u);
        QCOMPARE(g[3], 22u);
        QCOMPARE(g[7], 11u);
        QCOMPARE(g[6], 0u);
        QCOMPARE(g.pair_count(), size_t(2));
        QCOMPARE(g.pair(0).replica_id, uint16_t(3));
        QCOMPARE(g.pair(0).value, 22u);
        QCOMPARE(g.pair(1).replica_id, uint16_t(7));
        QCOMPARE(g.pair(1).value, 11u);
    }
    // Regression for the O(replica_id) edit-cost bug
    // (docs/handoff/2026-05-09-collabtext-bug-report.md): per-edit cost was
    // linear in the buffer's replica_id (27.8 ms at replica_id=60000 vs
    // 0.14 ms at replica_id=1, on a 400-edit append-only workload). Root cause
    // was a dense `Global` array indexed by replica_id; three Globals lived
    // inside every FragmentSummary, propagated up the SumTree on each edit.
    // After the sparse-Global fix, per-edit cost is independent of replica_id.
    void buffer_apply_local_edit_cost_independent_of_replica_id() {
        // Run the same workload with the smallest and a near-max replica_id.
        // The two should be within a few × of each other; we assert a
        // generous 10× ceiling to keep the test non-flaky on shared CI.
        constexpr int kEdits = 400;
        const std::string chunk(180, 'a');

        auto run = [&](uint16_t replica_id) -> qint64 {
            CollabText::Crdt::Buffer buf(replica_id);
            uint32_t cursor = 0;
            QElapsedTimer t;
            t.start();
            for (int i = 0; i < kEdits; ++i) {
                buf.apply_local_edit({{cursor, cursor}}, {chunk});
                cursor += static_cast<uint32_t>(chunk.size());
            }
            return t.nsecsElapsed();
        };

        // Warm-up: avoid the very first run paying allocator/page-fault costs.
        (void)run(1);

        const qint64 small = run(1);
        const qint64 large = run(60000);

        // Pre-fix: large was ~200× small. Post-fix: large is within ~1× small.
        // 10× covers run-to-run noise comfortably.
        const qint64 ceiling = small * 10 + 50'000'000;  // +50 ms slack floor
        QVERIFY2(large <= ceiling,
                 qPrintable(QString("replica_id=60000 took %1 ns vs %2 ns "
                                    "for replica_id=1 (ceiling %3 ns)")
                                .arg(large)
                                .arg(small)
                                .arg(ceiling)));
    }
};

QTEST_MAIN(TestClock)
#include "tst_clock.moc"
