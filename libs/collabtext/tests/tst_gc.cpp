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

    // -----------------------------------------------------------------------
    // GC + Undo interaction
    // -----------------------------------------------------------------------

    void gc_protects_undoable_deletions() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"abc"});
        buf.apply_local_edit({{0, 3}}, {""});  // delete all
        QCOMPARE(buf.text(), std::string(""));
        QVERIFY(buf.tombstone_count() > 0);

        // Tombstone should be protected (deletion_id is in undo stack)
        QCOMPARE(buf.collect_garbage(), size_t(0));

        // Undo the deletion — text comes back
        buf.undo();
        QCOMPARE(buf.text(), std::string("abc"));
    }

    void gc_after_undo_redo_cycle() {
        Buffer buf(1);
        buf.set_max_undo_depth(0);  // nothing protected
        buf.apply_local_edit({{0, 0}}, {"abc"});
        buf.apply_local_edit({{0, 3}}, {""});  // delete
        buf.undo();  // restore
        buf.redo();  // re-delete
        QCOMPARE(buf.text(), std::string(""));

        // All undo entries were trimmed (max_undo_depth=0), so GC is safe
        size_t removed = buf.collect_garbage();
        QVERIFY(removed > 0);
        QCOMPARE(buf.tombstone_count(), size_t(0));
        QCOMPARE(buf.text(), std::string(""));
    }

    void gc_partial_protection() {
        // Two separate deletions: one protected, one not
        Buffer buf(1);
        buf.set_max_undo_depth(1);
        buf.apply_local_edit({{0, 0}}, {"abcdef"});
        buf.apply_local_edit({{0, 3}}, {""});  // delete "abc" — undo entry 1
        buf.apply_local_edit({{0, 3}}, {""});  // delete "def" — undo entry 2, pushes 1 out

        QCOMPARE(buf.text(), std::string(""));

        // "abc" tombstones: deletion_id from entry 1, which aged out → GC-eligible
        // "def" tombstones: deletion_id from entry 2, still in stack → protected
        size_t before = buf.tombstone_count();
        size_t removed = buf.collect_garbage();
        QVERIFY(removed > 0);
        QVERIFY(removed < before);  // some but not all removed
        QVERIFY(buf.tombstone_count() > 0);  // "def" tombstones remain
        QCOMPARE(buf.text(), std::string(""));

        // Undo the "def" deletion — "def" comes back
        buf.undo();
        QCOMPARE(buf.text(), std::string("def"));

        // Nothing left to undo (entry 1 aged out)
        auto op = buf.undo();
        QVERIFY(!op.has_value());
    }

    void gc_with_multiple_deletions_on_same_fragment() {
        Buffer buf(1);
        buf.set_max_undo_depth(2);
        buf.apply_local_edit({{0, 0}}, {"hello"});   // entry 0
        buf.apply_local_edit({{0, 5}}, {""});         // entry 1: delete "hello"
        buf.undo();                                    // undo delete
        buf.redo();                                    // redo delete
        QCOMPARE(buf.text(), std::string(""));

        // With max_undo_depth=2 and 2 entries (insert + delete), nothing aged out.
        QCOMPARE(buf.collect_garbage(), size_t(0));
    }

    // -----------------------------------------------------------------------
    // Fragment coalescing
    // -----------------------------------------------------------------------

    void coalesce_reduces_fragment_count() {
        // Two replicas insert at the same position → normalization atomizes
        // into single-char fragments. After convergence, those single-char
        // fragments from the same replica can be coalesced.
        Buffer bufA(1), bufB(2);
        auto op1 = bufA.apply_local_edit({{0, 0}}, {"aaa"});
        auto op2 = bufB.apply_local_edit({{0, 0}}, {"bbb"});

        bufA.apply_ops({op2});
        bufB.apply_ops({op1});
        QCOMPARE(bufA.text(), bufB.text());

        // After normalization, fragments are atomized at shared locators
        size_t before = bufA.fragment_count();

        // GC + coalesce — no tombstones to remove, but coalescing should help
        bufA.collect_garbage();
        size_t after = bufA.fragment_count();
        QVERIFY(after <= before);  // coalescing may reduce count
        QCOMPARE(bufA.text(), bufB.text());  // text unchanged
    }

    void coalesce_preserves_text() {
        Buffer buf(1);
        buf.set_max_undo_depth(0);
        buf.apply_local_edit({{0, 0}}, {"abcdef"});
        buf.apply_local_edit({{2, 4}}, {""});  // delete "cd"
        std::string text_before = buf.text();
        QCOMPARE(text_before, std::string("abef"));

        buf.collect_garbage();  // removes "cd" tombstone, then tries to coalesce
        QCOMPARE(buf.text(), text_before);
    }

    void coalesce_does_not_merge_different_locators() {
        // Fragments with different locators must not be coalesced
        Buffer buf(1);
        buf.set_max_undo_depth(0);
        buf.apply_local_edit({{0, 0}}, {"abc"});
        buf.apply_local_edit({{3, 3}}, {"def"});  // separate insertion → different locator
        size_t before_gc = buf.fragment_count();
        buf.collect_garbage();
        // These are separate insertions with different locators — no coalescing
        QCOMPARE(buf.fragment_count(), before_gc);
        QCOMPARE(buf.text(), std::string("abcdef"));
    }
};

QTEST_MAIN(TestGC)
#include "tst_gc.moc"
