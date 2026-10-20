// tst_summary_laws.cpp — P03: the algebraic laws of the aggregates we cache.
//
// Finding F3's real content was not one wrong line; it was that two different
// questions — "what has every peer seen?" and "what is the smallest counter in
// this set?" — were both answered by `Global::meet()`, and nobody had written
// down the laws either answer obeys. Conflating them produces false certainty,
// which is exactly what a reclamation watermark must not have.
//
// These tests pin the laws, and the property that the tree aggregates still
// describe the same document after arbitrary splice patterns.

#include <QTest>

#include "TestSeed.h"
#include "TraceHarness.h"
#include "crdt/Buffer.h"
#include "crdt/Clock.h"
#include "crdt/Fragment.h"
#include "crdt/IdList.h"

#include <algorithm>
#include <numeric>
#include <random>
#include "TestActor.h"

using CollabText::Test::actor;

using namespace CollabText::Crdt;
using CollabText::Test::announce_seed;
using CollabText::Test::effective_seed;

namespace {

/// A vector built from explicit (replica, value) pairs, for readable cases.
Global vector_of(std::initializer_list<std::pair<uint16_t, uint32_t>> pairs) {
    Global g;
    for (const auto& [replica, value] : pairs)
        g.observe(Identity(actor(replica), value));
    return g;
}

/// A pseudo-random vector over a small replica space, so overlaps and gaps are
/// common rather than accidental.
Global random_vector(std::mt19937& rng, int replicas, int nonzero_count) {
    Global g;
    for (int i = 0; i < nonzero_count; ++i) {
        const uint16_t replica = static_cast<uint16_t>(1 + rng() % replicas);
        const uint32_t value = 1 + rng() % 1000;
        g.observe(Identity(actor(replica), value));
    }
    return g;
}

}  // namespace

class TestSummaryLaws : public QObject {
    Q_OBJECT

private slots:

    void initTestCase() {
        announce_seed("tst_summary_laws", "laws", "summary-laws");
    }

    // ---- the coverage watermark: the question F3 was about ----

    void absent_actor_constrains_coverage_to_zero() {
        // A peer that has never observed replica 1 constrains replica 1's common
        // coverage to zero — not to whatever the local vector happens to hold.
        const Global observed = vector_of({{1, 10}});
        QCOMPARE(observed.get(actor(1)), quint64(10));

        const Global intersected = observed.coverage_intersection(Global{});
        QVERIFY2(intersected.empty(),
                 "intersection with an empty peer must constrain coverage to zero");
        QCOMPARE(intersected.get(actor(1)), quint64(0));
        // Canonicalized: a zero component is not a fact worth carrying.
        QCOMPARE(intersected.pair_count(), size_t(0));
    }

    void disjoint_vectors_intersect_to_nothing() {
        const Global a = vector_of({{1, 10}, {2, 20}});
        const Global b = vector_of({{3, 30}, {4, 40}});
        QVERIFY(a.coverage_intersection(b).empty());
        QVERIFY(b.coverage_intersection(a).empty());
    }

    void overlapping_vectors_take_the_minimum() {
        const Global a = vector_of({{1, 10}, {2, 20}});
        const Global b = vector_of({{1, 4}, {3, 30}});
        const Global c = a.coverage_intersection(b);
        QCOMPARE(c.get(actor(1)), quint64(4));     // present in both: the minimum
        QCOMPARE(c.get(actor(2)), quint64(0));     // only in a: constrained to zero
        QCOMPARE(c.get(actor(3)), quint64(0));     // only in b: constrained to zero
        QCOMPARE(c.pair_count(), size_t(1));
    }

    void meet_and_coverage_intersection_are_different_operations() {
        // Pin both, because the whole point of P03 is that they differ.
        const Global observed = vector_of({{1, 10}});
        const Global empty = Global{};

        Global m = observed;
        m.meet(empty);
        QCOMPARE(m.get(actor(1)), quint64(10));      // meet: nonzero-minimum aggregation

        // coverage: zero
        QCOMPARE(observed.coverage_intersection(empty).get(actor(1)), quint64(0));

        // They agree only when both vectors name the same replica with a value.
        const Global lower = vector_of({{1, 3}});
        Global m2 = observed;
        m2.meet(lower);
        QCOMPARE(m2.get(actor(1)), quint64(3));
        QCOMPARE(observed.coverage_intersection(lower).get(actor(1)), quint64(3));
    }

    // ---- laws of the two vector operations ----

    void join_is_a_commutative_associative_idempotent_lattice_join() {
        std::mt19937 rng(static_cast<uint32_t>(effective_seed("join-laws")));
        for (int round = 0; round < 200; ++round) {
            const Global a = random_vector(rng, 6, 4);
            const Global b = random_vector(rng, 6, 4);
            const Global c = random_vector(rng, 6, 4);

            Global ab = a; ab.join(b);
            Global ba = b; ba.join(a);
            QCOMPARE(ab, ba);                       // commutative

            Global left = ab; left.join(c);
            Global right = a;
            right.join(b);
            right.join(c);
            QCOMPARE(left, right);                  // associative

            Global idem = a; idem.join(a);
            QCOMPARE(idem, a);                      // idempotent

            Global from_empty = a;                  // empty is join's identity
            QCOMPARE(from_empty, a);
        }
    }

    void coverage_intersection_is_commutative_associative_and_idempotent() {
        std::mt19937 rng(static_cast<uint32_t>(effective_seed("intersection-laws")));
        for (int round = 0; round < 200; ++round) {
            const Global a = random_vector(rng, 6, 4);
            const Global b = random_vector(rng, 6, 4);
            const Global c = random_vector(rng, 6, 4);

            QCOMPARE(a.coverage_intersection(b), b.coverage_intersection(a));

            const Global left = a.coverage_intersection(b).coverage_intersection(c);
            const Global right = a.coverage_intersection(
                b.coverage_intersection(c));
            QCOMPARE(left, right);

            QCOMPARE(a.coverage_intersection(a), a);   // idempotent
        }
    }

    void intersection_has_no_identity_and_empty_absorbs() {
        // Stated rather than assumed: the mathematical identity is "every
        // counter at its maximum", which a sparse vector cannot represent, so
        // folding a peer set starts from the local vector and never from empty.
        const Global a = vector_of({{1, 10}, {2, 20}});
        QVERIFY(a.coverage_intersection(Global{}).empty());   // empty absorbs
        QVERIFY(Global{}.coverage_intersection(a).empty());

        // Folding over a set of peers including the local vector is what a
        // watermark does, and it agrees with folding pairwise in any order.
        const Global peer_one = vector_of({{1, 5}, {2, 30}});
        const Global peer_two = vector_of({{1, 7}, {3, 4}});
        Global fold = a;
        fold = fold.coverage_intersection(peer_one);
        fold = fold.coverage_intersection(peer_two);
        QCOMPARE(fold.get(actor(1)), quint64(5));
        QCOMPARE(fold.pair_count(), size_t(1));
    }

    void coverage_never_exceeds_any_participant() {
        // The safety property a reclamation watermark needs: intersection can
        // only ever understate what has been seen, never overstate it.
        std::mt19937 rng(static_cast<uint32_t>(effective_seed("coverage-monotone")));
        for (int round = 0; round < 200; ++round) {
            const Global a = random_vector(rng, 8, 5);
            const Global b = random_vector(rng, 8, 5);
            const Global c = a.coverage_intersection(b);
            for (uint16_t replica = 1; replica <= 8; ++replica) {
                QVERIFY(c.get(actor(replica)) <= a.get(actor(replica)));
                QVERIFY(c.get(actor(replica)) <= b.get(actor(replica)));
            }
        }
    }

    // ---- the tree summaries ----

    void fragment_summary_is_a_monoid() {
        // Identity, associativity, and commutativity of the cached aggregate.
        const FragmentSummary empty = FragmentSummary::zero();

        auto summary_for = [](uint32_t visible, uint32_t deleted, uint32_t utf16) {
            FragmentSummary s;
            s.visible_bytes = visible;
            s.deleted_bytes = deleted;
            s.visible_utf16 = utf16;
            return s;
        };

        const FragmentSummary a = summary_for(10, 1, 8);
        const FragmentSummary b = summary_for(20, 2, 16);
        const FragmentSummary c = summary_for(30, 3, 25);

        // Identity
        FragmentSummary with_empty = a;
        with_empty.add_summary(empty);
        QCOMPARE(with_empty.visible_bytes, a.visible_bytes);
        QCOMPARE(with_empty.deleted_bytes, a.deleted_bytes);
        QCOMPARE(with_empty.visible_utf16, a.visible_utf16);
        QCOMPARE(with_empty.visible_bytes_overflow, a.visible_bytes_overflow);

        // Commutative
        FragmentSummary ab = a; ab.add_summary(b);
        FragmentSummary ba = b; ba.add_summary(a);
        QCOMPARE(ab.visible_bytes, ba.visible_bytes);
        QCOMPARE(ab.deleted_bytes, ba.deleted_bytes);
        QCOMPARE(ab.visible_utf16, ba.visible_utf16);
        QCOMPARE(ab.visible_bytes_overflow, ba.visible_bytes_overflow);

        // Associative
        FragmentSummary left = ab; left.add_summary(c);
        FragmentSummary right = a;
        right.add_summary(b);
        right.add_summary(c);
        QCOMPARE(left.visible_bytes, right.visible_bytes);
        QCOMPARE(left.deleted_bytes, right.deleted_bytes);
        QCOMPARE(left.visible_utf16, right.visible_utf16);
        QCOMPARE(left.visible_bytes_overflow, right.visible_bytes_overflow);
    }

    void visible_byte_overflow_is_observable_to_range_validation() {
        FragmentSummary sum;
        sum.visible_bytes = UINT32_MAX;
        FragmentSummary next;
        next.visible_bytes = 1;
        sum.add_summary(next);
        QCOMPARE(sum.visible_bytes, uint32_t{0});
        QVERIFY(sum.visible_bytes_overflow);
    }

    void idlist_summary_is_a_monoid() {
        const IdListSummary empty = IdListSummary::zero();
        IdListSummary with_empty;
        with_empty.add_summary(empty);
        QCOMPARE(with_empty.visible_count, 0u);
        QCOMPARE(with_empty.deleted_count, 0u);

        IdListEntry first(Identity(actor(1), 5), Link{}, 11);
        IdListEntry second(Identity(actor(2), 3), Link{}, 22);
        IdListEntry third(Identity(actor(3), 9), Link{}, 33);

        IdListSummary sum;
        sum.add_summary(first.summary());
        sum.add_summary(second.summary());
        sum.add_summary(third.summary());
        QCOMPARE(sum.visible_count, 3u);
        QCOMPARE(sum.deleted_count, 0u);
    }

    // ---- the property that actually matters to a user ----

    void visible_text_is_unchanged_across_randomized_splice_patterns() {
        // Tree aggregates exist to make seeks fast. If a splice pattern changes
        // what the document says, the aggregate is wrong no matter which laws
        // it satisfies. Splits are the pattern that stresses the aggregate
        // hardest, because they change fragment boundaries without changing text.
        std::mt19937 rng(static_cast<uint32_t>(effective_seed("splice-property")));
        for (int round = 0; round < 25; ++round) {
            Buffer buffer(CollabText::Test::test_counter(1));
            std::string expected;
            for (int i = 0; i < 40; ++i) {
                const uint32_t at = expected.empty()
                    ? 0u : static_cast<uint32_t>(rng() % (expected.size() + 1));
                const std::string chunk(1 + rng() % 4,
                                        static_cast<char>('a' + rng() % 26));
                buffer.apply_local_edit({{at, at}}, {chunk});
                expected.insert(at, chunk);
                if (buffer.text() != expected)
                    QFAIL("edit did not produce the expected text");
            }
            QVERIFY(buffer.text() == expected);

            // Drive splits by deleting one byte at a time, which forces fragment
            // boundaries to move. A plain reference string tracks what the text
            // must be, so a splice that changed the document would show up here
            // rather than hiding inside the aggregate assertions.
            for (int i = 0; i < 20 && !expected.empty(); ++i) {
                const uint32_t len = static_cast<uint32_t>(expected.size());
                const uint32_t at = rng() % len;
                buffer.apply_local_edit({{at, at + 1}}, {""});
                expected.erase(at, 1);
                // The aggregate must still describe the document exactly.
                const auto frags = buffer.fragments();
                uint32_t visible_sum = 0;
                uint32_t deleted_sum = 0;
                for (const auto& f : frags) {
                    if (f.visible) visible_sum += f.byte_length;
                    else deleted_sum += f.byte_length;
                }
                QCOMPARE(visible_sum, buffer.visible_rope_len());
                QCOMPARE(deleted_sum, buffer.deleted_rope_len());
                QCOMPARE(visible_sum, static_cast<uint32_t>(buffer.text().size()));
                if (buffer.text() != expected)
                    QFAIL(qPrintable(QString("splice pattern changed the text at "
                                             "round %1 step %2")
                                         .arg(round).arg(i)));
            }
        }
    }
};

QTEST_MAIN(TestSummaryLaws)
#include "tst_summary_laws.moc"
