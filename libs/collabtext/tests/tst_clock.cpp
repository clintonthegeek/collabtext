/// tst_clock.cpp — the engine's identity and version-vector unit tests.
///
/// Migrated in P06a from the 16-bit `replica_id`/32-bit `value` pair to the
/// 128-bit ActorId plus a 64-bit counter. Two things changed shape rather than
/// merely widening:
///
///   * `Global` lost its dense view entirely. `size()` used to mean "max observed
///     replica_id + 1" and `operator[](i)` "the value for replica_id == i", and
///     neither has a meaning when keys are 128-bit opaque values rather than
///     consecutive integers. There is no replacement, because there is nothing
///     to be dense over. `tst_identity_types.cpp` pins the sparse API.
///   * The three replica-id-magnitude regressions are kept as their real
///     invariant — growth past the inline capacity must not corrupt the vector,
///     and copies must work across the heap path — now expressed with actor
///     counts instead of key magnitudes, since that is the only way a wide key
///     can still reach the spill path.

#include <QElapsedTimer>
#include <QTest>

#include "TestActor.h"

#include "crdt/Buffer.h"
#include "crdt/Clock.h"

using namespace CollabText::Crdt;
using CollabText::ActorId;
using CollabText::Test::actor;
using CollabText::Test::actor64;
using CollabText::Test::id;

class TestClock : public QObject {
    Q_OBJECT
private slots:
    void identity_tick_increments() {
        Identity clock = id(1, 1);
        const Identity t1 = clock.tick();
        const Identity t2 = clock.tick();
        // tick() returns the value the clock had before the call and advances
        // it; both halves are load-bearing at the call sites.
        QCOMPARE(t1.counter, quint64(1));
        QCOMPARE(t2.counter, quint64(2));
        QCOMPARE(clock.counter, quint64(3));
        QCOMPARE(t1.actor, actor(1));
    }

    void identity_observe_jumps_ahead() {
        Identity clock = id(1, 1);
        clock.observe(id(2, 100));
        QCOMPARE(clock.counter, quint64(101));
        // The actor half is unchanged: this is the causal clock, so it rises
        // past what it saw without becoming somebody else's identity.
        QCOMPARE(clock.actor, actor(1));
    }

    void identity_observe_does_not_go_backwards() {
        Identity clock = id(1, 200);
        clock.observe(id(2, 50));
        QCOMPARE(clock.counter, quint64(200));
    }

    void identity_ordering_counter_first() {
        QVERIFY(id(1, 10) < id(2, 20));
    }

    void identity_ordering_actor_tiebreak() {
        QVERIFY(id(1, 10) < id(2, 10));
        QVERIFY(!(id(2, 10) < id(1, 10)));
        QCOMPARE(id(1, 10), id(1, 10));
    }

    void identity_min_is_below_every_valid_identity() {
        const Identity least = Identity::min();
        QVERIFY(!least.actor.valid());
        QVERIFY(least < id(1, 0));
        QVERIFY(least < id(255, UINT64_MAX));
    }

    void identity_counter_passes_the_old_32_bit_ceiling() {
        const quint64 past = (quint64{1} << 32) + 7;
        QCOMPARE(id(1, past).counter, past);
        QVERIFY(id(1, past - 1) < id(1, past));
    }

    // ---- version vector ----

    void global_observe_and_get() {
        Global g;
        g.observe(id(3, 42));
        QCOMPARE(g.get(actor(3)), quint64(42));
        QCOMPARE(g.get(actor(0)), quint64(0));
        QCOMPARE(g.get(actor(4)), quint64(0));
    }

    void global_ignores_counter_zero() {
        // Counter 0 is the "unseen" sentinel, not an identity: recording it
        // would make `observed()` vacuously true for records that were never
        // seen, and P05 rejects such an operation as invalid_identity.
        Global g;
        g.observe(id(3, 0));
        QVERIFY(g.empty());
    }

    void global_observed() {
        Global g;
        g.observe(id(1, 10));
        QVERIFY(g.observed(id(1, 10)));
        QVERIFY(g.observed(id(1, 5)));
        QVERIFY(!g.observed(id(1, 11)));
        QVERIFY(!g.observed(id(2, 1)));
    }

    void global_observed_all() {
        Global a, b;
        a.observe(id(0, 10)); a.observe(id(1, 20));
        b.observe(id(0, 5));  b.observe(id(1, 15));
        QVERIFY(a.observed_all(b));
        QVERIFY(!b.observed_all(a));
    }

    void global_observed_all_shorter_vector_fails() {
        Global a, b;
        a.observe(id(0, 10));
        b.observe(id(0, 10)); b.observe(id(5, 1));
        QVERIFY(!a.observed_all(b));
    }

    void global_join() {
        Global a, b;
        a.observe(id(0, 10)); a.observe(id(1, 5));
        b.observe(id(0, 3));  b.observe(id(1, 20));
        a.join(b);
        QCOMPARE(a.get(actor(0)), quint64(10));
        QCOMPARE(a.get(actor(1)), quint64(20));
    }

    void global_meet() {
        Global a, b;
        a.observe(id(0, 10)); a.observe(id(1, 20));
        b.observe(id(0, 5));  b.observe(id(1, 30));
        a.meet(b);
        QCOMPARE(a.get(actor(0)), quint64(5));
        QCOMPARE(a.get(actor(1)), quint64(20));
    }

    // P03: meet() is a nonzero-minimum aggregation, which is a different
    // question from coverage intersection. Pin both, because F3 was the two
    // being used interchangeably.
    void global_meet_leaves_absent_components_alone() {
        Global a;
        a.observe(id(0, 10));
        Global b;
        a.meet(b);
        QCOMPARE(a.get(actor(0)), quint64(10));
        QCOMPARE(a.pair_count(), size_t(1));
    }

    void global_coverage_intersection_treats_absence_as_zero() {
        Global a, b;
        a.observe(id(0, 10));
        Global c = a.coverage_intersection(b);
        QCOMPARE(c.get(actor(0)), quint64(0));
        QVERIFY(c.empty());
        QVERIFY(b.coverage_intersection(a).empty());
    }

    void global_coverage_intersection_is_a_union_fold() {
        Global a, b, c;
        a.observe(id(0, 10)); a.observe(id(1, 7));
        b.observe(id(0, 4));  b.observe(id(2, 9));
        c.observe(id(1, 2));  c.observe(id(2, 12));

        Global all = a.coverage_intersection(b).coverage_intersection(c);
        // No actor appears in all three vectors, so the fold is empty.
        QCOMPARE(all.get(actor(0)), quint64(0));
        QCOMPARE(all.get(actor(1)), quint64(0));
        QCOMPARE(all.get(actor(2)), quint64(0));
        QVERIFY(all.empty());

        // With every actor present in every vector, the minima survive.
        Global full_a, full_b;
        full_a.observe(id(0, 10)); full_a.observe(id(1, 7));
        full_b.observe(id(0, 4));  full_b.observe(id(1, 3));
        const Global both = full_a.coverage_intersection(full_b);
        QCOMPARE(both.get(actor(0)), quint64(4));
        QCOMPARE(both.get(actor(1)), quint64(3));
        QCOMPARE(both.pair_count(), size_t(2));
    }

    void global_coverage_intersection_is_commutative_and_idempotent() {
        Global a, b;
        a.observe(id(3, 12)); a.observe(id(9, 2));
        b.observe(id(3, 5));  b.observe(id(7, 8));
        QCOMPARE(a.coverage_intersection(b), b.coverage_intersection(a));
        QCOMPARE(a.coverage_intersection(a), a);
    }

    void global_coverage_intersection_survives_heap_promotion() {
        // The intersection walks both sorted pair lists directly, so it must
        // produce identical results whether the result stayed inline or spilled.
        Global a, b;
        for (uint8_t replica = 1; replica <= 6; ++replica) {
            a.observe(id(replica, 100 - replica));
            b.observe(id(replica, 200 - replica));
        }
        const Global small = a.coverage_intersection(b);
        for (uint8_t replica = 7; replica <= 12; ++replica) {
            a.observe(id(replica, 100 - replica));
            b.observe(id(replica, 200 - replica));
        }
        const Global large = a.coverage_intersection(b);
        QVERIFY(large.pair_count() > small.pair_count());
        QCOMPARE(small.pair_count(), size_t(6));
        QCOMPARE(large.pair_count(), size_t(12));
        for (uint8_t replica = 1; replica <= 6; ++replica) {
            QCOMPARE(large.get(actor(replica)), small.get(actor(replica)));
            QCOMPARE(large.get(actor(replica)), quint64(100 - replica));
        }
        for (uint8_t replica = 7; replica <= 12; ++replica)
            QCOMPARE(large.get(actor(replica)), quint64(100 - replica));
    }

    // Regression: SBO heap-promotion truncated m_capacity to 0 and corrupted
    // the Global. The original trigger was a replica_id large enough that
    // doubled growth from the inline capacity reached the 16-bit ceiling. A
    // 128-bit key cannot overflow a uint32 capacity that way, so the invariant
    // is now stated as what it actually is: growth past the inline capacity
    // must allocate correctly however many actors are observed, and copies must
    // survive the heap path.
    void global_growth_past_inline_capacity_does_not_corrupt() {
        Global g;
        constexpr quint64 kObservations = 5000;
        for (quint64 i = 1; i <= kObservations; ++i) {
            // 250 distinct actors, cycled. Actor 0 is deliberately never used:
            // it is the invalid id, and mixing it in would make this test about
            // validation rather than about growth.
            g.observe(id(static_cast<uint8_t>(1 + (i % 250)), i));
        }
        QCOMPARE(g.pair_count(), size_t(250));
        for (uint8_t who = 1; who <= 250; ++who) {
            QVERIFY(g.get(actor(who)) > 0);
        }
        // i == 5000 lands on actor 1, so actor 250's last observation was 4999.
        QCOMPARE(g.get(actor(250)), kObservations - 1);
        QCOMPARE(g.get(actor(1)), kObservations);
    }

    void global_copy_after_heap_promotion() {
        Global a;
        for (uint8_t i = 1; i <= 40; ++i) a.observe(id(i, i * 3));
        Global b = a;             // copy ctor through heap path
        QCOMPARE(b.pair_count(), size_t(40));
        QCOMPARE(b.get(actor(40)), quint64(120));
        Global c;
        c = a;                    // copy assignment through heap path
        QCOMPARE(c.pair_count(), size_t(40));
        QCOMPARE(c.get(actor(17)), quint64(51));
        Global moved = std::move(c);
        QCOMPARE(moved.get(actor(1)), quint64(3));
    }

    void global_sparse_iteration_is_sorted_by_actor() {
        // Iteration order is the actor order, so the wire encoder's output is a
        // function of the vector's content rather than of insertion order.
        Global g;
        g.observe(id(7, 11));
        g.observe(id(3, 22));
        g.observe(id(7, 5));   // older — must not regress
        QCOMPARE(g.get(actor(3)), quint64(22));
        QCOMPARE(g.get(actor(7)), quint64(11));
        QCOMPARE(g.pair_count(), size_t(2));
        QCOMPARE(g.pair(0).first, actor(3));
        QCOMPARE(g.pair(0).second, quint64(22));
        QCOMPARE(g.pair(1).first, actor(7));
        QCOMPARE(g.pair(1).second, quint64(11));
    }

    // Regression for the O(replica_id) edit-cost bug
    // (docs/handoff/2026-05-09-collabtext-bug-report.md): per-edit cost was
    // linear in the buffer's replica_id (27.8 ms at replica_id=60000 vs
    // 0.14 ms at replica_id=1, on a 400-edit append-only workload). Root cause
    // was a dense `Global` array indexed by replica_id; three Globals lived
    // inside every FragmentSummary, propagated up the SumTree on each edit.
    // After the sparse-Global fix, per-edit cost is independent of the key, and
    // the widened identity makes the question sharper rather than moot: a 24-byte
    // identity in every summary is exactly the cost this guards against
    // multiplying per edit.
    void buffer_apply_local_edit_cost_independent_of_actor() {
        constexpr int kEdits = 400;
        const std::string chunk(180, 'a');

        auto run = [&](const ActorId &who) -> qint64 {
            CollabText::Crdt::Buffer buf(who);
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
        (void)run(actor(1));

        const qint64 small = run(actor(1));
        // A large actor value, not a large actor count: this is the case the
        // old dense layout was accidentally linear in.
        const qint64 large = run(actor64(UINT64_MAX - 1));

        const qint64 ceiling = small * 10 + 50'000'000;  // +50 ms slack floor
        QVERIFY2(large <= ceiling,
                 qPrintable(QString("the large actor took %1 ns vs %2 ns "
                                    "for actor(1) (ceiling %3 ns)")
                                .arg(large)
                                .arg(small)
                                .arg(ceiling)));
    }
};

QTEST_MAIN(TestClock)
#include "tst_clock.moc"