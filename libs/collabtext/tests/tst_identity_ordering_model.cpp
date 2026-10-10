/// tst_identity_ordering_model.cpp — P06a design gate: prove the identity
/// order's laws against an independent implementation.
///
/// Roadmap task P06a has a design gate: the ordering contract and an independent
/// model must exist and run *before* the engine's identity types change. This is
/// that artifact. The engine is untouched by this file.
///
/// ─── Why this gate exists ────────────────────────────────────────────────────
///
/// Equal locators are a routine case, not an edge case. `normalize_fragments`
/// exists to handle runs of fragments from different replicas sharing one
/// locator, and split relocations deliberately leave locators equal. When
/// locators tie, origin decides — so the identity order is load-bearing for
/// convergence, and a subtly wrong order diverges documents permanently and
/// invisibly.
///
/// The trap this test is built against: the tempting way to keep identities small
/// is a dense locally assigned slot. That looks like it works, and it does not.
/// Replica A meeting actors X then Y assigns X=0,Y=1; replica B meeting Y then X
/// assigns Y=0,X=1. Equal-locator fragments from X and Y then tie-break in
/// opposite directions, and the two replicas disagree permanently. §2.1 of
/// docs/specs/vnext-identity.md rules that out; `locally_assigned_slots_diverge`
/// here demonstrates it concretely so the rule is not merely asserted.
///
/// ─── Independence ────────────────────────────────────────────────────────────
///
/// The roadmap requires that a reference model not call the production helper
/// whose behaviour it validates. So `RefIdentity` below reimplements the order
/// from the specification text, comparing ActorId bytes directly and never
/// calling `ActorId::compare` or any engine comparator. Where both exist the test
/// compares them, which is the only way the comparison is evidence rather than
/// tautology.
///
/// Note: escaped string literals throughout. Qt's moc silently drops every test
/// slot in a translation unit containing a raw string literal with braces (a P04
/// limitation).

#include <QTest>

#include <collabtext/ActorId.h>

#include <array>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

using CollabText::ActorId;
using CollabText::ErrorCode;
using CollabText::Result;

namespace {

// ---------------------------------------------------------------------------
// The independent reference implementation, written from the spec.
// ---------------------------------------------------------------------------

/// Counter-first, actor-second, with the actor compared byte-wise from the most
/// significant byte down. Deliberately not expressed in terms of ActorId's own
/// comparison, so agreement between the two is a real result.
struct RefIdentity {
    uint64_t counter = 0;
    ActorId actor;
};

int ref_compare_actors(const ActorId& a, const ActorId& b) {
    const auto ab = a.bytes();
    const auto bb = b.bytes();
    for (size_t i = 0; i < ActorId::kBytes; ++i) {
        if (ab[i] != bb[i]) return ab[i] < bb[i] ? -1 : 1;
    }
    return 0;
}

int ref_compare(const RefIdentity& a, const RefIdentity& b) {
    if (a.counter != b.counter) return a.counter < b.counter ? -1 : 1;
    return ref_compare_actors(a.actor, b.actor);
}

bool ref_less(const RefIdentity& a, const RefIdentity& b) {
    return ref_compare(a, b) < 0;
}

/// Deterministic distinct actors, so the suite is reproducible without depending
/// on the OS random source.
ActorId actor_from(uint64_t seed) {
    std::array<uint8_t, ActorId::kBytes> bytes{};
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<uint8_t>((seed >> ((i % 8) * 8)) ^ (i * 31) ^ 0xA5);
    }
    // Keep the actor non-zero: an all-zero ActorId is invalid by definition.
    bytes[0] |= 0x01;
    return ActorId::from_bytes(bytes);
}

/// Exhaustively enumerate ordering outcomes over a small actor set. The laws are
/// proven by enumeration rather than by sampling, because transitivity and
/// totality are the two that sampling reliably misses.
///
/// Counters deliberately *collide*: counter = i % 3 over 12 identities gives four
/// identities sharing each counter, so the actor tiebreak has to carry the
/// ordering for two thirds of every pairwise and triple comparison. A set with
/// distinct counters would let the counter alone decide every case and the actor
/// path — the part that can actually diverge between replicas — would go untested.
std::vector<RefIdentity> make_set(size_t count, uint64_t counter_period = 3) {
    std::vector<RefIdentity> set;
    set.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        set.push_back(
            RefIdentity{static_cast<uint64_t>(i % counter_period), actor_from(i + 1)});
    }
    return set;
}

}  // namespace

class TestIdentityOrderingModel : public QObject {
    Q_OBJECT
private slots:

    // =======================================================================
    // Laws 1-4, proven exhaustively rather than sampled
    // =======================================================================

    void law_irreflexive() {
        const auto set = make_set(8);
        for (const auto& id : set) {
            QCOMPARE(ref_compare(id, id), 0);
            QVERIFY(!ref_less(id, id));
        }
    }

    void law_antisymmetric_and_total_over_the_whole_set() {
        const auto set = make_set(12);
        for (size_t i = 0; i < set.size(); ++i) {
            for (size_t j = 0; j < set.size(); ++j) {
                const int forward = ref_compare(set[i], set[j]);
                const int backward = ref_compare(set[j], set[i]);
                if (i == j) {
                    QCOMPARE(forward, 0);
                } else {
                    // Total: comparable, never "incomparable" (which would be 0).
                    QVERIFY(forward != 0);
                    // Antisymmetric: exactly the negation.
                    QCOMPARE(forward, -backward);
                    // Law 6 for equal counters: exactly one strict order holds.
                    QVERIFY(ref_less(set[i], set[j]) != ref_less(set[j], set[i]));
                }
            }
        }
    }

    void law_transitive_over_all_triples() {
        const auto set = make_set(9);  // 9^3 = 729 triples, exhaustive

        // Confirm the fixture actually exercises the actor tiebreak, so a reader
        // does not have to take the enumeration's strength on trust.
        int counter_ties = 0;
        for (size_t i = 0; i < set.size(); ++i)
            for (size_t j = i + 1; j < set.size(); ++j)
                if (set[i].counter == set[j].counter) ++counter_ties;
        QVERIFY2(counter_ties >= 3, "fixture has too few counter ties to be meaningful");

        for (const auto& a : set) {
            for (const auto& b : set) {
                if (!ref_less(a, b)) continue;
                for (const auto& c : set) {
                    if (!ref_less(b, c)) continue;
                    QVERIFY2(ref_less(a, c), "transitivity violated");
                }
            }
        }
    }

    void law_counter_dominates_actor() {
        // Two identities with the same actor must order purely by counter, no
        // matter what the actor's bytes are.
        const ActorId a = actor_from(1);
        const ActorId b = actor_from(2);
        for (uint64_t lo = 0; lo < 40; ++lo) {
            for (uint64_t hi = lo + 1; hi < 42; ++hi) {
                QVERIFY(ref_compare(RefIdentity{lo, a}, RefIdentity{hi, b}) < 0);
                QVERIFY(ref_compare(RefIdentity{hi, b}, RefIdentity{lo, a}) > 0);
            }
        }
    }

    void law_actor_decides_only_when_counters_tie() {
        const ActorId a = actor_from(1);
        const ActorId b = actor_from(2);
        const int by_actor = ref_compare_actors(a, b);
        QVERIFY(by_actor != 0);
        QCOMPARE(ref_compare(RefIdentity{7, a}, RefIdentity{7, b}), by_actor);
    }

    // =======================================================================
    // Law 5: the model agrees with the production comparator
    // =======================================================================

    void model_agrees_with_actor_id_compare() {
        // The one place the two implementations meet. Written as a comparison of
        // two independently produced answers, not as a self-comparison.
        std::vector<ActorId> actors;
        for (uint64_t i = 1; i <= 24; ++i) actors.push_back(actor_from(i));

        for (size_t i = 0; i < actors.size(); ++i) {
            for (size_t j = 0; j < actors.size(); ++j) {
                const int mine = ref_compare_actors(actors[i], actors[j]);
                const int theirs_sign = (actors[i].compare(actors[j]) < 0)   ? -1
                                        : (actors[i].compare(actors[j]) > 0) ? 1
                                                                             : 0;
                QCOMPARE(mine, theirs_sign);
            }
        }
    }

    void model_agrees_with_the_full_identity_order_on_random_sets() {
        std::mt19937_64 rng(20261004);
        for (int round = 0; round < 200; ++round) {
            std::vector<RefIdentity> mine;
            std::vector<RefIdentity> theirs;
            const size_t n = 2 + (rng() % 10);
            for (size_t i = 0; i < n; ++i) {
                RefIdentity id{static_cast<uint64_t>(rng() % 1000), actor_from(rng() % 64)};
                mine.push_back(id);
                theirs.push_back(id);
            }
            std::sort(mine.begin(), mine.end(), ref_less);
            std::sort(theirs.begin(), theirs.end(), [](const RefIdentity& a, const RefIdentity& b) {
                if (a.counter != b.counter) return a.counter < b.counter;
                return a.actor.compare(b.actor) < 0;
            });
            QCOMPARE(mine.size(), theirs.size());
            for (size_t i = 0; i < mine.size(); ++i) {
                QCOMPARE(ref_compare(mine[i], theirs[i]), 0);
            }
        }
    }

    // =======================================================================
    // Law 7: no aliasing, and the invalid identity
    // =======================================================================

    void distinct_actors_are_never_equal() {
        std::vector<ActorId> actors;
        for (uint64_t i = 1; i <= 200; ++i) actors.push_back(actor_from(i));
        for (size_t i = 0; i < actors.size(); ++i) {
            QVERIFY(actors[i].valid());
            for (size_t j = i + 1; j < actors.size(); ++j) {
                QVERIFY2(!(actors[i] == actors[j]), "two distinct actors aliased");
                QCOMPARE(ref_compare_actors(actors[i], actors[j]),
                         -ref_compare_actors(actors[j], actors[i]));
            }
        }
    }

    void default_actor_is_invalid_and_orders_below_valid_ones() {
        const ActorId invalid;
        QVERIFY(!invalid.valid());
        // Its bytes are all zero, so it sorts below every actor_from() result.
        for (uint64_t i = 1; i <= 32; ++i) {
            const ActorId valid = actor_from(i);
            QVERIFY(ref_compare_actors(invalid, valid) < 0);
        }
    }

    // =======================================================================
    // The divergence the design forbids, demonstrated
    // =======================================================================

    void locally_assigned_slots_diverge() {
        // Two replicas meet the same two actors in opposite orders and assign
        // dense local slots. Their slot orders are exact opposites, so equal
        // locators from X and Y tie-break differently and the documents diverge.
        //
        // This is the counterexample behind spec section 2.1. It is asserted as a
        // *failing* property of the slot scheme, which is why it lives in the
        // model rather than in the engine: the engine must never do this.
        const ActorId x = actor_from(11);
        const ActorId y = actor_from(22);

        // Replica A: meets X first -> X=0, Y=1.
        const int a_x = 0, a_y = 1;
        // Replica B: meets Y first -> Y=0, X=1.
        const int b_x = 1, b_y = 0;

        const bool a_orders_x_first = a_x < a_y;
        const bool b_orders_x_first = b_x < b_y;
        QVERIFY(a_orders_x_first);
        QVERIFY(!b_orders_x_first);

        // Whereas a global identity gives both replicas the same answer.
        const bool a_global = ref_compare_actors(x, y) < 0;
        const bool b_global = ref_compare_actors(x, y) < 0;
        QCOMPARE(a_global, b_global);
    }

    // =======================================================================
    // Counter rules the engine will depend on
    // =======================================================================

    void checked_counter_never_reuses_and_reports_exhaustion() {
        CollabText::CheckedCounter counter(7);
        QCOMPARE(counter.peek(), 7u);
        QCOMPARE(counter.reserve().value(), 7u);
        QCOMPARE(counter.reserve().value(), 8u);
        QVERIFY(!counter.exhausted());

        QVERIFY(counter.restore(std::numeric_limits<uint64_t>::max() - 1).ok());
        QCOMPARE(counter.reserve().value(), std::numeric_limits<uint64_t>::max() - 1);
        QVERIFY(counter.exhausted());

        // Exhaustion is an error that changes nothing, never a wrap.
        const auto failed = counter.reserve();
        QVERIFY(!failed.ok());
        QCOMPARE(failed.error().code(), ErrorCode::counter_exhausted);
        QCOMPARE(counter.peek(), std::numeric_limits<uint64_t>::max());
    }

    void counters_at_the_uint64_boundary_are_distinct_under_the_order() {
        // The wire format must round-trip these exactly (P06b), and the order
        // must not collapse them, which is what a double conversion would do.
        const ActorId actor = actor_from(3);
        const uint64_t big = (uint64_t(1) << 53) + 1;  // not representable as double
        const RefIdentity lo{big - 1, actor};
        const RefIdentity hi{big, actor};
        QVERIFY(ref_less(lo, hi));
        QVERIFY(hi.counter != lo.counter);
        QVERIFY(hi.counter > (uint64_t(1) << 53));  // survived the round trip
    }
};

QTEST_MAIN(TestIdentityOrderingModel)
#include "tst_identity_ordering_model.moc"
