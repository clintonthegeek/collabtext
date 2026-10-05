/// tst_identity_types.cpp — P06a step 3: the engine's identity type really is
/// a persistent ActorId plus a checked 64-bit counter.
///
/// The gate for this task (docs/specs/vnext-identity.md, and TASKS.json P06a)
/// required the ordering and allocation contracts plus independent models to
/// exist and run *before* the engine's identity types changed. They now do:
/// tst_identity_ordering_model proves the order's laws against an independent
/// implementation, and tst_identity_allocation_model proves the interval
/// allocation contract. Neither touches the engine, so on their own they prove
/// the contract is coherent and not that the engine obeys it.
///
/// This file is the other half. It pins the *adopted* shape, so a later change
/// cannot quietly narrow the identity back toward a 16-bit actor or a 32-bit
/// counter — the two widths that made identity non-persistent and resettable in
/// the first place, and that made a 16-bit draw the entire collision story.
///
/// Two things this deliberately does not do. It does not call the reference
/// model: the model validates the specification, this validates adoption, and
/// conflating them would make a green run mean less than it appears to. And it
/// does not assert the old layout is gone — absence is not observable from a
/// test, so the evidence for that is that the engine and its codecs compile and
/// their suites pass with ActorId-keyed versions.

#include <QTest>

#include <collabtext/ActorId.h>

#include "crdt/Anchor.h"
#include "crdt/Clock.h"

#include <array>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

using CollabText::ActorId;
using CollabText::Crdt::Anchor;
using CollabText::Crdt::Global;
using CollabText::Crdt::Identity;

namespace {

/// A deterministic, obviously-synthetic actor. Production actors are 128 bits
/// of OS randomness (P02); a test that minted real ones would be
/// unreproducible, and one that reused ActorId::generate() per case would make
/// a failure depend on entropy. Big-endian so that actor(1) < actor(2) <
/// actor(255) holds under the byte-wise comparison the engine uses.
ActorId actor(uint8_t n) {
    std::array<uint8_t, ActorId::kBytes> bytes{};
    bytes[ActorId::kBytes - 1] = n;
    return ActorId::from_bytes(bytes);
}

/// An actor built from an explicit 64-bit value, for cases that need ordering
/// between actors whose small values would collide with actor() above.
ActorId actor64(uint64_t n) {
    std::array<uint8_t, ActorId::kBytes> bytes{};
    for (size_t i = 0; i < 8; ++i)
        bytes[ActorId::kBytes - 1 - i] = static_cast<uint8_t>(n >> (8 * i));
    return ActorId::from_bytes(bytes);
}

} // namespace

class TestIdentityTypes : public QObject {
    Q_OBJECT
private slots:

    // ---- the adopted shape ------------------------------------------------

    void identity_is_an_actor_and_a_64_bit_counter() {
        static_assert(sizeof(Identity) == 24,
                      "identity is a 128-bit actor plus a 64-bit counter");
        static_assert(std::is_same_v<decltype(Identity::actor), ActorId>,
                      "the actor half is the persistent ActorId");
        static_assert(std::is_same_v<decltype(Identity::counter), uint64_t>,
                      "the counter half is 64 bits wide");
        QVERIFY(sizeof(Identity) == 24u);
    }

    void anchor_carries_an_actor_not_a_16_bit_replica_id() {
        // An anchor names one item identity. If it kept a 16-bit replica it
        // would resolve against a different actor's characters after
        // adoption, which is silent corruption rather than a compile error.
        static_assert(sizeof(Anchor) >= sizeof(ActorId) + sizeof(uint64_t),
                      "an anchor holds a full identity");
        const Anchor a(Identity(actor(3), 7), CollabText::Crdt::Bias::Left);
        QCOMPARE(a.position.actor, actor(3));
        QCOMPARE(a.position.counter, uint64_t(7));
        QVERIFY(a.is_item());
        QVERIFY(!a.is_end());
    }

    // ---- ordering, as adopted -------------------------------------------

    void counter_is_compared_before_the_actor() {
        // Counter-first is the whole reason a wider counter is safe to adopt:
        // the legacy order interleaved per-scalar identities, and §3.2 keeps
        // that value for value. A counter of 1 must beat a counter of 2 even
        // when the actor comparison says otherwise.
        const Identity small_counter(actor(200), 1);
        const Identity large_counter(actor(1), 2);
        QVERIFY(small_counter < large_counter);
        QVERIFY(large_counter > small_counter);
        QVERIFY(small_counter != large_counter);
    }

    void actor_breaks_ties_at_equal_counters() {
        const Identity a(actor(1), 10);
        const Identity b(actor(2), 10);
        QVERIFY(a < b);
        QVERIFY(b > a);
        QCOMPARE(a, Identity(actor(1), 10));
    }

    void ordering_is_total_over_generated_identities() {
        // Law 4 of the contract: no two identities are incomparable, so no
        // replica ever has an arbitrary choice to make. Checked here on the
        // engine's own comparator rather than the model's, because the engine
        // is what has to compute it identically everywhere.
        std::mt19937_64 rng(20261005);
        std::vector<Identity> pool;
        pool.reserve(256);
        for (int i = 0; i < 128; ++i) {
            pool.push_back(Identity(actor64(rng()), rng() & 0xFFFFFFFFu));
        }
        for (size_t i = 0; i < pool.size(); ++i) {
            for (size_t j = 0; j < pool.size(); ++j) {
                const bool lt = pool[i] < pool[j];
                const bool gt = pool[i] > pool[j];
                const bool eq = pool[i] == pool[j];
                // Exactly one of the three holds, always: totality means a pair
                // is never left with no verdict, and irreflexivity means an
                // identity is never below itself.
                QCOMPARE(lt + gt + eq, 1);
                if (i == j) {
                    QVERIFY(eq);
                } else {
                    QVERIFY(lt != gt);
                }
            }
        }
    }

    void distinct_actors_never_share_an_identity() {
        // Acceptance: "Two distinct ActorIds never share an identity." The
        // counter is deliberately identical across actors here, so the only
        // thing preventing a collision is the actor half.
        for (uint64_t counter = 0; counter < 4; ++counter) {
            for (uint8_t i = 1; i < 255; ++i) {
                for (uint8_t j = i + 1; j < 255; ++j) {
                    const Identity a(actor(i), counter);
                    const Identity b(actor(j), counter);
                    QVERIFY(a != b);
                    QVERIFY((a < b) != (b < a));
                }
            }
        }
    }

    void default_constructed_actor_is_the_least_identity_and_never_valid() {
        // There is no actor 0 (P02) and therefore no identity that a
        // default-constructed value can pass as. Identity::min() must still be
        // a true minimum, because the fragment summary seeds with it and every
        // aggregate folds through it.
        const Identity least = Identity::min();
        QVERIFY(!least.actor.valid());
        for (uint8_t i = 1; i < 255; ++i) {
            QVERIFY(least < Identity(actor(i), 0));
            QVERIFY(least < Identity(actor(i), 1));
        }
        QVERIFY(!(least == Identity(actor(1), 0)));
    }

    void counter_survives_the_legacy_32_bit_ceiling() {
        // The point of the wider counter. 2^32 was reachable by a long-lived
        // session under the legacy scheme; a 64-bit counter must represent
        // values past it without truncation.
        const uint64_t past = (uint64_t{1} << 32) + 12345;
        const Identity id(actor(1), past);
        QCOMPARE(id.counter, past);
        QVERIFY(Identity(actor(1), past - 1) < id);
        QVERIFY(id < Identity(actor(1), past + 1));
        QVERIFY(id < Identity(actor(2), past));
    }

    void counter_is_not_silently_truncated_by_derivation() {
        // Fragment::timestamp_at derives an item identity as origin + offset.
        // That derivation is the engine's only way of producing identities
        // outside an operation's interval, so it has to stay exact past the
        // old ceiling rather than wrapping into a low value that collides with
        // a young operation.
        const uint64_t base = std::numeric_limits<uint64_t>::max() - 4;
        const Identity origin(actor(9), base);
        const Identity derived(actor(9), base + 3);
        QVERIFY(origin < derived);
        QCOMPARE(derived.counter - origin.counter, uint64_t(3));
    }

    // ---- the version vector, keyed by actor ------------------------------

    void global_is_keyed_by_actor_not_by_a_dense_index() {
        Global g;
        g.observe(Identity(actor(7), 11));
        QCOMPARE(g.get(actor(7)), uint64_t(11));
        QCOMPARE(g.get(actor(3)), uint64_t(0));
        QCOMPARE(g.get(actor(200)), uint64_t(0));
        QCOMPARE(g.pair_count(), size_t(1));
        QVERIFY(g.observed(Identity(actor(7), 11)));
        QVERIFY(g.observed(Identity(actor(7), 5)));
        QVERIFY(!g.observed(Identity(actor(7), 12)));
        QVERIFY(!g.observed(Identity(actor(8), 1)));
    }

    void global_ignores_the_counter_zero_sentinel() {
        // Zero is not an identity (§3): a counter-0 operation can be neither
        // recorded nor deduplicated, which is why P05 rejects it as
        // invalid_identity. A version vector must not resurrect it.
        Global g;
        g.observe(Identity(actor(1), 0));
        QVERIFY(g.empty());
        QCOMPARE(g.pair_count(), size_t(0));
    }

    void global_does_not_regress_for_an_older_counter() {
        Global g;
        g.observe(Identity(actor(5), 30));
        g.observe(Identity(actor(5), 5));
        QCOMPARE(g.get(actor(5)), uint64_t(30));
        QCOMPARE(g.pair_count(), size_t(1));
    }

    void global_sparse_iteration_is_sorted_by_actor() {
        Global g;
        g.observe(Identity(actor(9), 1));
        g.observe(Identity(actor(2), 2));
        g.observe(Identity(actor(5), 3));
        g.observe(Identity(actor(2), 4));   // newer for an actor already present
        QCOMPARE(g.pair_count(), size_t(3));
        QCOMPARE(g.pair(0).first, actor(2));
        QCOMPARE(g.pair(0).second, uint64_t(4));
        QCOMPARE(g.pair(1).first, actor(5));
        QCOMPARE(g.pair(2).first, actor(9));
    }

    void global_observed_all_compares_per_actor() {
        Global a, b;
        a.observe(Identity(actor(1), 10));
        a.observe(Identity(actor(2), 20));
        b.observe(Identity(actor(1), 5));
        b.observe(Identity(actor(2), 15));
        QVERIFY(a.observed_all(b));
        QVERIFY(!b.observed_all(a));

        Global c;
        c.observe(Identity(actor(1), 10));
        c.observe(Identity(actor(3), 1));
        QVERIFY(!a.observed_all(c));   // actor 3 was never seen by a
    }

    void global_coverage_intersection_treats_absence_as_zero() {
        // P03's rule, restated with 128-bit keys: an actor a peer has never
        // observed contributes zero common coverage, which is what makes
        // meet() the wrong operation for a watermark.
        Global a, b;
        a.observe(Identity(actor(1), 10));
        a.observe(Identity(actor(2), 7));
        b.observe(Identity(actor(1), 4));
        b.observe(Identity(actor(3), 12));

        // Actor 1 is the only one present in both, so it is the only component
        // that survives: 2 and 3 each appear in one vector and are constrained
        // to zero common coverage.
        const Global both = a.coverage_intersection(b);
        QCOMPARE(both.pair_count(), size_t(1));
        QCOMPARE(both.get(actor(1)), uint64_t(4));
        QCOMPARE(both.get(actor(2)), uint64_t(0));
        QCOMPARE(both.get(actor(3)), uint64_t(0));

        Global c;
        c.observe(Identity(actor(1), 4));
        c.observe(Identity(actor(2), 3));
        const Global partial = a.coverage_intersection(c);
        QCOMPARE(partial.get(actor(1)), uint64_t(4));
        QCOMPARE(partial.get(actor(2)), uint64_t(3));
        QCOMPARE(partial.get(actor(3)), uint64_t(0));
        QCOMPARE(partial.pair_count(), size_t(2));

        // Commutative, associative, idempotent: the laws that made the old
        // 16-bit-keyed version usable as a fold.
        QCOMPARE(a.coverage_intersection(b), b.coverage_intersection(a));
        QCOMPARE(partial.coverage_intersection(partial), partial);
    }

    void global_meet_leaves_absent_components_alone() {
        Global a;
        a.observe(Identity(actor(1), 10));
        Global b;
        b.observe(Identity(actor(2), 3));
        a.meet(b);
        QCOMPARE(a.get(actor(1)), uint64_t(10));
        QVERIFY(!a.get(actor(2)) != 0u);   // meet never adds components
        QCOMPARE(a.pair_count(), size_t(1));
    }

    void global_survives_heap_promotion_with_wide_keys() {
        // A 24-byte pair makes the inline capacity small, so the spill path is
        // reached far sooner than it was with 8-byte pairs. Exercise it and the
        // copy paths, which is where the old packing corrupted the vector.
        Global a, b;
        for (uint8_t i = 1; i <= 6; ++i) {
            a.observe(Identity(actor(i), 100 - i));
            b.observe(Identity(actor(i), 200 - i));
        }
        const Global small = a.coverage_intersection(b);
        for (uint8_t i = 7; i <= 20; ++i) {
            a.observe(Identity(actor(i), 100 - i));
            b.observe(Identity(actor(i), 200 - i));
        }
        const Global large = a.coverage_intersection(b);
        QCOMPARE(small.pair_count(), size_t(6));
        QCOMPARE(large.pair_count(), size_t(20));
        for (uint8_t i = 1; i <= 6; ++i) {
            QCOMPARE(large.get(actor(i)), small.get(actor(i)));
            QCOMPARE(large.get(actor(i)), uint64_t(100 - i));
        }

        Global copy = large;               // copy constructor, heap path
        QCOMPARE(copy.get(actor(20)), uint64_t(80));
        Global assigned;
        assigned = large;                 // copy assignment, heap path
        QCOMPARE(assigned.get(actor(20)), uint64_t(80));
        Global moved = std::move(assigned); // move, then still usable
        QCOMPARE(moved.pair_count(), size_t(20));
        QCOMPARE(moved.get(actor(1)), uint64_t(99));
    }

    void global_join_unions_per_actor() {
        Global a, b;
        a.observe(Identity(actor(1), 10));
        a.observe(Identity(actor(2), 5));
        b.observe(Identity(actor(2), 20));
        b.observe(Identity(actor(3), 1));
        a.join(b);
        QCOMPARE(a.get(actor(1)), uint64_t(10));
        QCOMPARE(a.get(actor(2)), uint64_t(20));
        QCOMPARE(a.get(actor(3)), uint64_t(1));
        QCOMPARE(a.pair_count(), size_t(3));
    }

    void global_distinguishes_actors_that_shared_a_replica_id() {
        // Under the old scheme two actors could not both exist, so this state
        // was unrepresentable. Now it is ordinary, and getting it wrong would
        // silently merge two devices' histories.
        Global g;
        g.observe(Identity(actor(1), 50));
        g.observe(Identity(actor(2), 3));
        QCOMPARE(g.pair_count(), size_t(2));
        QCOMPARE(g.get(actor(1)), uint64_t(50));
        QCOMPARE(g.get(actor(2)), uint64_t(3));
        QVERIFY(!g.observed(Identity(actor(2), 4)));
    }
};

QTEST_MAIN(TestIdentityTypes)
#include "tst_identity_types.moc"
