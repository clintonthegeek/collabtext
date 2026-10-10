#include <QTest>
#include "crdt/IdList.h"
#include "TestActor.h"

using CollabText::Test::actor;

using namespace CollabText::Crdt;

class TestIdListGc : public QObject {
    Q_OBJECT
private slots:
    void collect_garbage_retains_trimmed_tombstones() {
        IdList list(CollabText::Test::test_counter(1));
        list.insert_after(Anchor::min(), 0xAA);
        list.remove_at(Anchor(Identity(actor(1), 1), Bias::Left));
        QVERIFY((list.set_max_undo_depth(0)).ok());  // discard all undo entries
        QVERIFY((list.set_max_undo_depth(1000)).ok());
        QCOMPARE(list.tombstone_count(), size_t{1});
        size_t collected = list.collect_garbage();
        QCOMPARE(collected, size_t{0});
        QCOMPARE(list.tombstone_count(), size_t{1});
    }

    void collect_garbage_skips_remote_deletions() {
        IdList alice(CollabText::Test::test_counter(1)), bob(CollabText::Test::test_counter(2));
        auto op_a = alice.insert_after(Anchor::min(), 0xAA);
        bob.apply_ops({op_a});
        auto rm_b = bob.remove_at(Anchor(Identity(actor(1), 1), Bias::Left));
        alice.apply_ops({rm_b});
        QVERIFY((alice.set_max_undo_depth(0)).ok());
        QVERIFY((alice.set_max_undo_depth(1000)).ok());
        QCOMPARE(alice.collect_garbage(), size_t{0});  // remote deletion remains restorable
    }

    void compact_with_watermark_preserves_remote_undo() {
        IdList alice(CollabText::Test::test_counter(1)), bob(CollabText::Test::test_counter(2));
        auto op_a = alice.insert_after(Anchor::min(), 0xAA);
        bob.apply_ops({op_a});
        auto rm_b = bob.remove_at(Anchor(Identity(actor(1), 1), Bias::Left));
        alice.apply_ops({rm_b});

        // Watermark = meet of all replicas' versions (everything everyone has seen).
        Global wm = alice.version();
        wm.meet(bob.version());

        QVERIFY((alice.set_max_undo_depth(0)).ok());
        QVERIFY((alice.set_max_undo_depth(1000)).ok());
        size_t reclaimed = alice.compact(wm);
        QCOMPARE(reclaimed, size_t{0});
        QCOMPARE(alice.tombstone_count(), size_t{1});
        auto undo = bob.undo();
        QVERIFY(undo.has_value());
        alice.apply_ops({*undo});
        QCOMPARE(alice.ids(), std::vector<uint64_t>{0xAA});
        QCOMPARE(alice.ids(), bob.ids());
    }

    void gc_protects_entries_in_undo_stack() {
        IdList list(CollabText::Test::test_counter(1));
        list.insert_after(Anchor::min(), 0xAA);
        list.remove_at(Anchor(Identity(actor(1), 1), Bias::Left));  // deletion id is in undo stack
        QCOMPARE(list.collect_garbage(), size_t{0});  // protected
        QVERIFY((list.set_max_undo_depth(0)).ok());  // drop undo entry
        QVERIFY((list.set_max_undo_depth(1000)).ok());
        QCOMPARE(list.collect_garbage(), size_t{0});  // local trimming cannot prove global retirement
        QCOMPARE(list.tombstone_count(), size_t{1});
    }

    void gc_protects_undone_insertion_with_no_deletions() {
        // Entry was inserted then undone (no deletion tombstone), only visible=false
        // because its origin is flagged in undo_map. GC must not collect it while
        // the insertion is still in the undo stack (redo would break otherwise).
        IdList list(CollabText::Test::test_counter(1));
        list.insert_after(Anchor::min(), 0xAA);  // origin (1,1), undo entry: inserted_keys={(1,1)}
        list.undo();                              // invisible now, no deletion, but insertion protected
        QCOMPARE(list.size(), size_t{0});
        QCOMPARE(list.tombstone_count(), size_t{1});
        QCOMPARE(list.collect_garbage(), size_t{0});  // must NOT collect — redo still possible

        // Dropping this stack does not prove remote restoration/anchor retirement.
        QVERIFY((list.set_max_undo_depth(0)).ok());
        QVERIFY((list.set_max_undo_depth(1000)).ok());
        QCOMPARE(list.collect_garbage(), size_t{0});
        QCOMPARE(list.tombstone_count(), size_t{1});
    }
};

QTEST_GUILESS_MAIN(TestIdListGc)
#include "tst_idlist_gc.moc"
