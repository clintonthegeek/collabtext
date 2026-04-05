#include <QTest>
#include "crdt/Buffer.h"

using namespace CollabText::Crdt;

class TestGC : public QObject {
    Q_OBJECT
private slots:

    void fragment_count_empty() {
        Buffer buf(1);
        QCOMPARE(buf.fragment_count(), size_t(0));
        QCOMPARE(buf.tombstone_count(), size_t(0));
    }

    void fragment_count_after_insert() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        QVERIFY(buf.fragment_count() > 0);
        QCOMPARE(buf.tombstone_count(), size_t(0));
    }

    void tombstone_count_after_delete() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        buf.apply_local_edit({{0, 5}}, {""});  // delete all
        QCOMPARE(buf.text(), std::string(""));
        QVERIFY(buf.tombstone_count() > 0);
    }

    void max_undo_depth_default() {
        Buffer buf(1);
        QCOMPARE(buf.max_undo_depth(), size_t(1000));
    }

    void set_max_undo_depth() {
        Buffer buf(1);
        buf.set_max_undo_depth(50);
        QCOMPARE(buf.max_undo_depth(), size_t(50));
    }

    // -----------------------------------------------------------------------
    // Basic GC
    // -----------------------------------------------------------------------

    void gc_removes_tombstones() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello world"});
        buf.apply_local_edit({{0, 5}}, {""});  // delete "hello"
        QCOMPARE(buf.text(), std::string(" world"));

        size_t before = buf.fragment_count();
        QVERIFY(buf.tombstone_count() > 0);

        // GC should NOT remove tombstones while deletion is in undo stack
        size_t removed = buf.collect_garbage();
        QCOMPARE(removed, size_t(0));
        QCOMPARE(buf.fragment_count(), before);
        QCOMPARE(buf.text(), std::string(" world"));
    }

    void gc_removes_after_undo_stack_clear() {
        Buffer buf(1);
        buf.set_max_undo_depth(1);
        buf.apply_local_edit({{0, 0}}, {"hello world"});  // undo entry 1
        buf.apply_local_edit({{0, 5}}, {""});              // undo entry 2, pushes entry 1 out

        // The tombstones' deletion_id is from the delete edit, which IS in the stack.
        QCOMPARE(buf.text(), std::string(" world"));
        size_t removed = buf.collect_garbage();
        QCOMPARE(removed, size_t(0));

        // Now do another edit to push the delete entry out
        buf.apply_local_edit({{6, 6}}, {"!"});  // undo entry 3, pushes entry 2 out
        QCOMPARE(buf.text(), std::string(" world!"));

        // Now the delete's deletion_id is no longer in the undo stack
        size_t tombstones_before = buf.tombstone_count();
        QVERIFY(tombstones_before > 0);
        removed = buf.collect_garbage();
        QCOMPARE(removed, tombstones_before);
        QCOMPARE(buf.tombstone_count(), size_t(0));
        QCOMPARE(buf.text(), std::string(" world!"));
    }

    void gc_preserves_visible_text() {
        Buffer buf(1);
        buf.set_max_undo_depth(0);  // no undo protection at all
        buf.apply_local_edit({{0, 0}}, {"abcdef"});
        buf.apply_local_edit({{1, 3}}, {""});  // delete "bc"
        buf.apply_local_edit({{1, 3}}, {""});  // delete "ef" (now "ad" visible)
        std::string text_before = buf.text();
        QCOMPARE(text_before, std::string("af"));

        size_t removed = buf.collect_garbage();
        QVERIFY(removed > 0);
        QCOMPARE(buf.text(), text_before);
        QCOMPARE(buf.tombstone_count(), size_t(0));
    }
};

QTEST_MAIN(TestGC)
#include "tst_gc.moc"
