#include <QTest>
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
};

QTEST_MAIN(TestClock)
#include "tst_clock.moc"
