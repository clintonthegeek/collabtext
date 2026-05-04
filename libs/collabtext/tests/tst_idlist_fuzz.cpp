#include <QTest>
#include <algorithm>
#include <random>
#include "crdt/IdList.h"

using namespace CollabText::Crdt;

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

class TestIdListFuzz : public QObject {
    Q_OBJECT
private slots:

    void invariant_checker_passes_on_valid_list() {
        IdList list(1);
        check_invariants(list, "empty");
        list.insert_after(Anchor::min(), 1);
        check_invariants(list, "one-element");
        list.insert_after(Anchor::min(), 2);
        check_invariants(list, "two-elements");
        list.remove_at(Anchor(1, 1, Bias::Left));
        check_invariants(list, "one-tombstone");
    }

    void two_replica_fuzz_50_ops() {
        std::mt19937 rng(1234);
        IdList alice(1), bob(2);
        std::vector<IdListOperation> alice_outbox, bob_outbox;

        auto choose_anchor = [&](const IdList& list) -> Anchor {
            if (list.size() == 0) return Anchor::min();
            std::uniform_int_distribution<uint32_t> idx(0, list.size() - 1);
            return list.anchor_at_index(idx(rng), Bias::Left);
        };

        for (int step = 0; step < 50; ++step) {
            IdList& side = (step % 2 == 0) ? alice : bob;
            auto& outbox = (step % 2 == 0) ? alice_outbox : bob_outbox;

            std::uniform_int_distribution<int> action(0, 9);
            int a = action(rng);
            if (a < 6 || side.size() == 0) {
                outbox.push_back(side.insert_after(choose_anchor(side), uint64_t(step + 1)));
            } else {
                outbox.push_back(side.remove_at(choose_anchor(side)));
            }
            check_invariants(side, "post-local-op");
        }

        auto all = alice_outbox;
        all.insert(all.end(), bob_outbox.begin(), bob_outbox.end());
        std::shuffle(all.begin(), all.end(), rng);
        alice.apply_ops(all);
        bob.apply_ops(all);
        check_invariants(alice, "post-converge-alice");
        check_invariants(bob, "post-converge-bob");
        QCOMPARE(alice.ids(), bob.ids());
    }

    void three_replica_fuzz_adversarial() {
        for (uint64_t seed : {1234ULL, 5678ULL, 99ULL, 314ULL, 2718ULL})
            run_three_replica_fuzz(seed);
    }

    void two_replica_fuzz_with_undo() {
        std::mt19937 rng(4321);
        IdList alice(1), bob(2);
        std::vector<IdListOperation> alice_outbox, bob_outbox;

        auto choose_anchor = [&](const IdList& list) -> Anchor {
            if (list.size() == 0) return Anchor::min();
            std::uniform_int_distribution<uint32_t> idx(0, list.size() - 1);
            return list.anchor_at_index(idx(rng), Bias::Left);
        };

        for (int step = 0; step < 50; ++step) {
            IdList& side = (step % 2 == 0) ? alice : bob;
            auto& outbox = (step % 2 == 0) ? alice_outbox : bob_outbox;

            std::uniform_int_distribution<int> action(0, 9);
            int a = action(rng);
            if (a < 5 || side.size() == 0) {
                outbox.push_back(side.insert_after(choose_anchor(side), uint64_t(step + 100)));
            } else if (a < 9) {
                outbox.push_back(side.remove_at(choose_anchor(side)));
            } else {
                auto undo_op = side.undo();
                if (undo_op) outbox.push_back(*undo_op);
            }
            check_invariants(side, "post-local-op");
        }

        auto all = alice_outbox;
        all.insert(all.end(), bob_outbox.begin(), bob_outbox.end());
        std::shuffle(all.begin(), all.end(), rng);
        alice.apply_ops(all);
        bob.apply_ops(all);
        check_invariants(alice, "post-converge-alice");
        check_invariants(bob, "post-converge-bob");
        QCOMPARE(alice.ids(), bob.ids());
    }

private:
    static void run_three_replica_fuzz(uint64_t seed) {
        std::mt19937 rng(static_cast<uint32_t>(seed));
        IdList alice(1), bob(2), carol(3);
        IdList* replicas[] = {&alice, &bob, &carol};
        std::vector<std::vector<IdListOperation>> queues(3);

        auto broadcast = [&](const IdListOperation& op, int source) {
            for (int r = 0; r < 3; ++r) {
                if (r == source) continue;
                size_t pos = queues[r].empty() ? 0 : (rng() % (queues[r].size() + 1));
                queues[r].insert(queues[r].begin() + static_cast<ptrdiff_t>(pos), op);
            }
        };

        auto choose_anchor = [&](const IdList& list, std::mt19937& r) -> Anchor {
            if (list.size() == 0) return Anchor::min();
            std::uniform_int_distribution<uint32_t> idx(0, list.size() - 1);
            return list.anchor_at_index(idx(r), Bias::Left);
        };

        for (int i = 0; i < 100; ++i) {
            int action = rng() % 100;
            if (action < 50) {
                int r = rng() % 3;
                if (rng() % 3 != 0 || replicas[r]->size() == 0) {
                    auto op = replicas[r]->insert_after(choose_anchor(*replicas[r], rng), uint64_t(i * 3 + r + 1));
                    broadcast(op, r);
                } else {
                    auto op = replicas[r]->remove_at(choose_anchor(*replicas[r], rng));
                    broadcast(op, r);
                }
            } else if (action < 65) {
                // Undo
                int r = rng() % 3;
                auto op = replicas[r]->undo();
                if (op) broadcast(*op, r);
            } else {
                // Deliver some messages
                int r = rng() % 3;
                if (!queues[r].empty()) {
                    int count = 1 + static_cast<int>(rng() % std::min<size_t>(5, queues[r].size()));
                    std::vector<IdListOperation> batch;
                    for (int c = 0; c < count && !queues[r].empty(); ++c) {
                        batch.push_back(queues[r].front());
                        queues[r].erase(queues[r].begin());
                    }
                    replicas[r]->apply_ops(batch);
                }
            }

            if (i % 20 == 0) {
                for (int r = 0; r < 3; ++r)
                    check_invariants(*replicas[r], qPrintable(QString("replica_%1_step_%2_seed_%3")
                        .arg(r).arg(i).arg(seed)));
            }
        }

        // Drain all queues
        for (int r = 0; r < 3; ++r) {
            if (!queues[r].empty()) {
                replicas[r]->apply_ops(queues[r]);
                queues[r].clear();
            }
        }

        for (int r = 0; r < 3; ++r)
            check_invariants(*replicas[r], qPrintable(QString("final_replica_%1_seed_%2").arg(r).arg(seed)));

        if (alice.ids() != bob.ids() || bob.ids() != carol.ids()) {
            QString msg = QString("CONVERGENCE FAILURE seed=%1").arg(seed);
            QFAIL(qPrintable(msg));
        }
    }
};

QTEST_GUILESS_MAIN(TestIdListFuzz)
#include "tst_idlist_fuzz.moc"
