#include <QTest>
#include "crdt/IdList.h"

using namespace CollabText::Crdt;

class TestIdListGc : public QObject {
    Q_OBJECT
private slots:
    void collect_garbage_removes_old_tombstones() {
        IdList list(1);
        list.insert_after(Anchor::min(), 0xAA);
        list.remove_at(Anchor(1, 1, Bias::Left));
        list.set_max_undo_depth(0);   // discard all undo entries
        list.set_max_undo_depth(1000);
        QCOMPARE(list.tombstone_count(), size_t{1});
        size_t collected = list.collect_garbage();
        QCOMPARE(collected, size_t{1});
        QCOMPARE(list.tombstone_count(), size_t{0});
    }

    void collect_garbage_skips_remote_deletions() {
        IdList alice(1), bob(2);
        auto op_a = alice.insert_after(Anchor::min(), 0xAA);
        bob.apply_ops({op_a});
        auto rm_b = bob.remove_at(Anchor(1, 1, Bias::Left));
        alice.apply_ops({rm_b});
        alice.set_max_undo_depth(0);
        alice.set_max_undo_depth(1000);
        QCOMPARE(alice.collect_garbage(), size_t{0});  // remote deletion — needs watermark
    }

    void compact_with_watermark_reclaims_acknowledged() {
        IdList alice(1), bob(2);
        auto op_a = alice.insert_after(Anchor::min(), 0xAA);
        bob.apply_ops({op_a});
        auto rm_b = bob.remove_at(Anchor(1, 1, Bias::Left));
        alice.apply_ops({rm_b});

        // Watermark = meet of all replicas' versions (everything everyone has seen).
        Global wm = alice.version();
        wm.meet(bob.version());

        alice.set_max_undo_depth(0);
        alice.set_max_undo_depth(1000);
        size_t reclaimed = alice.compact(wm);
        QCOMPARE(reclaimed, size_t{1});
    }

    void gc_protects_entries_in_undo_stack() {
        IdList list(1);
        list.insert_after(Anchor::min(), 0xAA);
        list.remove_at(Anchor(1, 1, Bias::Left));  // deletion id is in undo stack
        QCOMPARE(list.collect_garbage(), size_t{0});  // protected
        list.set_max_undo_depth(0);   // drop undo entry
        list.set_max_undo_depth(1000);
        QCOMPARE(list.collect_garbage(), size_t{1});  // now reclaimable
    }
};

QTEST_GUILESS_MAIN(TestIdListGc)
#include "tst_idlist_gc.moc"
