#include <QTest>
#include <algorithm>
#include <random>
#include "TestSeed.h"
#include "TraceHarness.h"
#include "crdt/IdList.h"
#include "TestActor.h"

using CollabText::Test::actor;

using namespace CollabText::Crdt;
using namespace CollabText::Test;

// ── Invariant checker ──────────────────────────────────────────────────────

static void check_invariants(const IdList& list, const char* context) {
    auto entries = list.entries();

    // INV-1: size() == count of visible entries
    size_t visible_count = 0;
    for (const auto& e : entries) if (e.visible) ++visible_count;
    if (list.size() != visible_count) {
        QFAIL(qPrintable(QString("INV-1 violated at %1: size()=%2 but visible count=%3")
            .arg(context).arg(list.size()).arg(visible_count)));
    }

    // INV-2: entries() count matches entry_count() (tree bookkeeping consistent)
    if (entries.size() != list.entry_count()) {
        QFAIL(qPrintable(QString("INV-2 violated at %1: entries()=%2 but entry_count()=%3")
            .arg(context).arg(entries.size()).arg(list.entry_count())));
    }

    // INV-3: entries sorted strictly by (locator, origin)
    for (size_t i = 1; i < entries.size(); ++i) {
        const auto& a = entries[i-1];
        const auto& b = entries[i];
        bool ordered = (a.locator < b.locator) ||
                       (a.locator == b.locator && a.origin < b.origin);
        if (!ordered) {
            QFAIL(qPrintable(QString("INV-3 violated at %1: entries[%2] >= entries[%3]")
                .arg(context).arg(i-1).arg(i)));
        }
    }

    // INV-4: no two visible entries share origin (replica_id, char_value)
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].visible) continue;
        for (size_t j = i + 1; j < entries.size(); ++j) {
            if (!entries[j].visible) continue;
            if (entries[i].origin == entries[j].origin) {
                QFAIL(qPrintable(QString("INV-4 violated at %1: duplicate origin at [%2] and [%3]")
                    .arg(context).arg(i).arg(j)));
            }
        }
    }

    // INV-5: visible entries with deletions must have all deletions undone
    // (In practice, a visible entry with a non-undone deletion would fail compute_visible)
    // This invariant is a consequence of set_entries calling compute_visible.

    // INV-6: version() observes every entry's origin
    const Global& ver = list.version();
    for (const auto& e : entries) {
        if (!ver.observed(e.origin)) {
            QFAIL(qPrintable(QString("INV-6 violated at %1: version does not observe entry origin")
                .arg(context)));
        }
    }
}

/// Announce the campaign and scenario seed, then derive the scenario's RNG.
static std::mt19937 scenario_rng(const char* scenario) {
    const uint64_t seed = effective_seed(scenario);
    announce_seed("tst_idlist_fuzz", scenario, "idlist");
    return std::mt19937(static_cast<uint32_t>(seed));
}

/// Assert all replicas agree, keeping the trace when artifact output is
/// configured.
static void assert_converged(IdListRecorder& rec, const char* context) {
    for (int r = 1; r < rec.replicas(); ++r) {
        if (rec.list(r).ids() != rec.list(0).ids()) {
            const std::string artifact = rec.save("failed");
            QFAIL(qPrintable(QString("CONVERGENCE FAILURE at %1: replica %2 differs "
                                     "(seed %3)%4")
                                 .arg(context).arg(r).arg(effective_seed(context))
                                 .arg(artifact.empty()
                                          ? std::string()
                                          : (" artifact=" + artifact))));
        }
    }
}

class TestIdListFuzz : public QObject {
    Q_OBJECT
private slots:

    void invariant_checker_passes_on_valid_list() {
        IdList list(CollabText::Test::test_counter(1));
        check_invariants(list, "empty");
        list.insert_after(Anchor::min(), 1);
        check_invariants(list, "one-element");
        list.insert_after(Anchor::min(), 2);
        check_invariants(list, "two-elements");
        list.remove_at(Anchor(Identity(actor(1), 1), Bias::Left));
        check_invariants(list, "one-tombstone");
    }

    void two_replica_fuzz_50_ops() {
        std::mt19937 rng = scenario_rng("two_replica_fuzz_50_ops");
        IdListRecorder rec(2, effective_seed("two_replica_fuzz_50_ops"),
                           "two_replica_fuzz_50_ops");
        std::vector<int> produced;

        for (int step = 0; step < 50; ++step) {
            const int side = step % 2;
            const uint32_t size = rec.list(side).size();
            const uint32_t index = size
                ? static_cast<uint32_t>(rng() % (size + 1)) : 0u;

            std::uniform_int_distribution<int> action(0, 9);
            const int a = action(rng);
            const int op = (a < 6 || size == 0)
                ? rec.insert_after(side, index, static_cast<uint64_t>(step + 1))
                : rec.remove_at(side, index);
            if (op < 0) continue;
            produced.push_back(op);
            check_invariants(rec.list(side), "post-local-op");
        }

        std::shuffle(produced.begin(), produced.end(), rng);
        for (int r = 0; r < 2; ++r)
            for (const int op : produced) rec.deliver(r, op);
        rec.settle();

        check_invariants(rec.list(0), "post-converge-alice");
        check_invariants(rec.list(1), "post-converge-bob");
        assert_converged(rec, "two_replica_fuzz_50_ops");
        rec.finish();
    }

    void three_replica_fuzz_adversarial() {
        for (const int campaign : {0, 1, 2, 3, 4})
            run_three_replica_fuzz(campaign);
    }

    void two_replica_fuzz_with_undo() {
        std::mt19937 rng = scenario_rng("two_replica_fuzz_with_undo");
        IdListRecorder rec(2, effective_seed("two_replica_fuzz_with_undo"),
                           "two_replica_fuzz_with_undo");
        std::vector<int> produced;

        for (int step = 0; step < 50; ++step) {
            const int side = step % 2;
            const uint32_t size = rec.list(side).size();
            const uint32_t index = size
                ? static_cast<uint32_t>(rng() % (size + 1)) : 0u;

            std::uniform_int_distribution<int> action(0, 9);
            const int a = action(rng);
            int op = -1;
            if (a < 5 || size == 0)
                op = rec.insert_after(side, index, static_cast<uint64_t>(step + 100));
            else if (a < 9)
                op = rec.remove_at(side, index);
            else
                op = rec.undo(side);
            if (op < 0) continue;
            produced.push_back(op);
            check_invariants(rec.list(side), "post-local-op");
        }

        std::shuffle(produced.begin(), produced.end(), rng);
        for (int r = 0; r < 2; ++r)
            for (const int op : produced) rec.deliver(r, op);
        rec.settle();

        check_invariants(rec.list(0), "post-converge-alice");
        check_invariants(rec.list(1), "post-converge-bob");
        assert_converged(rec, "two_replica_fuzz_with_undo");
        rec.finish();
    }

private:
    /// One adversarial three-replica campaign. `campaign` selects a different
    /// derived seed per pass, so the five passes are five distinct but
    /// individually reproducible runs.
    static void run_three_replica_fuzz(int campaign) {
        const std::string label = "three_replica_fuzz_adversarial_" +
                                  std::to_string(campaign);
        const uint64_t seed = effective_seed(label);
        announce_seed("tst_idlist_fuzz", label.c_str(), "idlist");
        std::mt19937 rng(static_cast<uint32_t>(seed));
        IdListRecorder rec(3, seed, label);
        std::vector<std::vector<int>> pending(3);

        for (int i = 0; i < 100; ++i) {
            const int action = static_cast<int>(rng() % 100);
            if (action < 50) {
                const int r = static_cast<int>(rng() % 3);
                const uint32_t size = rec.list(r).size();
                const uint32_t index = size
                    ? static_cast<uint32_t>(rng() % (size + 1)) : 0u;
                const bool insert = (rng() % 3 != 0) || size == 0;
                const int op = insert
                    ? rec.insert_after(r, index,
                                       static_cast<uint64_t>(i * 3 + r + 1))
                    : rec.remove_at(r, index);
                if (op < 0) continue;
                for (int peer = 0; peer < 3; ++peer) {
                    if (peer == r) continue;
                    const size_t at = pending[peer].empty()
                        ? 0 : (rng() % (pending[peer].size() + 1));
                    pending[peer].insert(pending[peer].begin() +
                                             static_cast<std::ptrdiff_t>(at), op);
                }
            } else if (action < 65) {
                // Undo
                const int r = static_cast<int>(rng() % 3);
                const int op = rec.undo(r);
                if (op < 0) continue;
                for (int peer = 0; peer < 3; ++peer) {
                    if (peer == r) continue;
                    pending[peer].push_back(op);
                }
            } else {
                // Deliver some messages
                const int r = static_cast<int>(rng() % 3);
                if (pending[r].empty()) continue;
                const int count = 1 + static_cast<int>(
                    rng() % std::min<size_t>(5, pending[r].size()));
                for (int c = 0; c < count; ++c) {
                    rec.deliver(r, pending[r].front());
                    pending[r].erase(pending[r].begin());
                }
            }

            if (i % 20 == 0) {
                for (int r = 0; r < 3; ++r)
                    check_invariants(rec.list(r), qPrintable(
                        QString("replica_%1_step_%2_seed_%3")
                            .arg(r).arg(i).arg(seed)));
            }
        }

        // Drain all queues
        for (int r = 0; r < 3; ++r) {
            while (!pending[r].empty()) {
                rec.deliver(r, pending[r].front());
                pending[r].erase(pending[r].begin());
            }
        }
        rec.settle();

        for (int r = 0; r < 3; ++r)
            check_invariants(rec.list(r),
                qPrintable(QString("final_replica_%1_seed_%2").arg(r).arg(seed)));

        assert_converged(rec, label.c_str());
        rec.finish();
    }
};

QTEST_GUILESS_MAIN(TestIdListFuzz)
#include "tst_idlist_fuzz.moc"
