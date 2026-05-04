#include <QTest>
#include "crdt/IdList.h"

using namespace CollabText::Crdt;

class TestIdListConvergence : public QObject {
    Q_OBJECT
private slots:
    void two_replicas_converge_on_disjoint_inserts() {
        IdList alice(1), bob(2);

        auto op_a = alice.insert_after(Anchor::min(), 0xAA);
        auto op_b = bob.insert_after(Anchor::min(), 0xBB);

        alice.apply_ops({op_b});
        bob.apply_ops({op_a});

        QCOMPARE(alice.ids(), bob.ids());
        QCOMPARE(alice.size(), 2u);
    }

    void concurrent_inserts_at_same_position_tiebreak() {
        IdList alice(1), bob(2);
        auto op_a = alice.insert_after(Anchor::min(), 0xAA);
        auto op_b = bob.insert_after(Anchor::min(), 0xBB);

        alice.apply_ops({op_b});
        bob.apply_ops({op_a});

        QCOMPARE(alice.ids(), bob.ids());
        // Lower replica id wins the tiebreak: alice's entry (replica 1) sorts first.
        QCOMPARE(alice.ids(), (std::vector<uint64_t>{0xAA, 0xBB}));
    }

    void out_of_order_delivery_buffers_then_applies() {
        IdList alice(1), bob(2);
        auto op_a1 = alice.insert_after(Anchor::min(), 1);
        Anchor at_a1(1, 1, Bias::Right);
        auto op_a2 = alice.insert_after(at_a1, 2);  // depends on op_a1

        bob.apply_ops({op_a2});     // out of order — should be buffered
        QCOMPARE(bob.size(), 0u);   // not applied yet
        bob.apply_ops({op_a1});     // now both apply
        QCOMPARE(bob.size(), 2u);
        QCOMPARE(bob.ids(), alice.ids());
    }

    void insert_then_remove_from_other_replica() {
        IdList alice(1), bob(2);
        auto op_a = alice.insert_after(Anchor::min(), 0xAA);
        bob.apply_ops({op_a});  // bob now sees 0xAA at origin (1,1)

        Anchor at_aa(1, 1, Bias::Left);
        auto op_b = bob.remove_at(at_aa);
        alice.apply_ops({op_b});

        QCOMPARE(alice.size(), 0u);
        QCOMPARE(bob.size(), 0u);
        QCOMPARE(alice.tombstone_count(), bob.tombstone_count());
    }

    void concurrent_removes_of_same_element() {
        IdList alice(1), bob(2);
        auto op_a = alice.insert_after(Anchor::min(), 0xAA);
        bob.apply_ops({op_a});

        Anchor at_aa(1, 1, Bias::Left);
        auto rm_alice = alice.remove_at(at_aa);
        auto rm_bob   = bob.remove_at(at_aa);

        alice.apply_ops({rm_bob});
        bob.apply_ops({rm_alice});

        QCOMPARE(alice.size(), 0u);
        QCOMPARE(bob.size(), 0u);
        // Both replicas see two deletions on the same entry.
        QCOMPARE(alice.entries().size(), size_t{1});
        QCOMPARE(alice.entries()[0].deletions.size(), size_t{2});
    }

    void insert_after_concurrently_deleted_anchor() {
        IdList alice(1), bob(2);
        auto op_a1 = alice.insert_after(Anchor::min(), 1);
        bob.apply_ops({op_a1});

        // Both have entry "1" at origin (1,1).
        Anchor at_one(1, 1, Bias::Right);
        // Concurrently:
        auto rm_a   = alice.remove_at(at_one);
        auto ins_b  = bob.insert_after(at_one, 2);

        alice.apply_ops({ins_b});
        bob.apply_ops({rm_a});

        QCOMPARE(alice.ids(), bob.ids());
        QCOMPARE(alice.ids(), (std::vector<uint64_t>{2}));  // "1" gone, "2" survives
    }

    void three_replicas_arbitrary_chain() {
        // op_a: alice (replica 1) first op → timestamp (1,1)
        // op_b: bob (replica 2) inserts after op_a; after apply_ops({op_a}),
        //        bob's clock observes (1,1) → bumps to value 2 → op_b timestamp (2,2)
        // op_c: carol (replica 3) inserts after op_b; after apply_ops({op_a,op_b}),
        //        carol's clock observes max of (1,1),(2,2) → bumps to value 3 → op_c timestamp (3,3)
        IdList alice(1), bob(2), carol(3);
        auto op_a = alice.insert_after(Anchor::min(), 1);
        bob.apply_ops({op_a});
        auto op_b = bob.insert_after(Anchor(1, 1, Bias::Right), 2);
        carol.apply_ops({op_a, op_b});
        auto op_c = carol.insert_after(Anchor(2, 2, Bias::Right), 3);

        alice.apply_ops({op_b, op_c});
        bob.apply_ops({op_c});

        QCOMPARE(alice.ids(), bob.ids());
        QCOMPARE(alice.ids(), carol.ids());
        QCOMPARE(alice.ids(), (std::vector<uint64_t>{1, 2, 3}));
    }

    void duplicate_op_delivery_is_noop() {
        IdList alice(1), bob(2);
        auto op = alice.insert_after(Anchor::min(), 0xAA);
        bob.apply_ops({op});
        bob.apply_ops({op});  // delivered twice
        QCOMPARE(bob.size(), 1u);
    }
};

QTEST_GUILESS_MAIN(TestIdListConvergence)
#include "tst_idlist_convergence.moc"
