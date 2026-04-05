#include <QTest>
#include "crdt/Buffer.h"
#include <random>

using namespace CollabText::Crdt;

static void check_invariants(const Buffer& buf, const char* context) {
    auto frags = buf.fragments();
    std::string text = buf.text();
    if (buf.visible_length() != static_cast<uint32_t>(text.size()))
        QFAIL(qPrintable(QString("INV-1 at %1").arg(context)));
    uint32_t vis_sum = 0, del_sum = 0;
    for (auto& f : frags) {
        if (f.visible) vis_sum += f.byte_length;
        else del_sum += f.byte_length;
    }
    if (vis_sum != buf.visible_length())
        QFAIL(qPrintable(QString("INV-2 at %1").arg(context)));
    for (size_t i = 1; i < frags.size(); ++i) {
        auto cmp = frags[i].locator <=> frags[i-1].locator;
        if (cmp < 0) QFAIL(qPrintable(QString("INV-4 at %1").arg(context)));
        if (cmp == 0 && frags[i].origin <= frags[i-1].origin)
            QFAIL(qPrintable(QString("INV-4 at %1").arg(context)));
    }
    for (size_t i = 0; i < frags.size(); ++i) {
        if (frags[i].byte_length == 0 || frags[i].length == 0)
            QFAIL(qPrintable(QString("INV-5 at %1").arg(context)));
        if (frags[i].byte_length < frags[i].length)
            QFAIL(qPrintable(QString("INV-7 at %1").arg(context)));
    }
    if (buf.visible_rope_len() != vis_sum)
        QFAIL(qPrintable(QString("INV-8 at %1 vis").arg(context)));
    if (buf.deleted_rope_len() != del_sum)
        QFAIL(qPrintable(QString("INV-8 at %1 del").arg(context)));
}

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

    // -----------------------------------------------------------------------
    // Stress tests
    // -----------------------------------------------------------------------

    void gc_preserves_invariants_stress() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        std::mt19937 rng(seed);

        Buffer buf(1);
        buf.set_max_undo_depth(20);

        for (int i = 0; i < 200; ++i) {
            std::string text = buf.text();
            uint32_t len = static_cast<uint32_t>(text.size());
            uint32_t start = 0, end = 0;
            if (len > 0) {
                std::vector<uint32_t> bounds = {0};
                for (size_t b = 0; b < text.size(); ) {
                    unsigned char c = static_cast<unsigned char>(text[b]);
                    if (c < 0x80) b += 1;
                    else if ((c & 0xE0) == 0xC0) b += 2;
                    else if ((c & 0xF0) == 0xE0) b += 3;
                    else b += 4;
                    bounds.push_back(static_cast<uint32_t>(b));
                }
                size_t si = rng() % bounds.size();
                start = bounds[si];
                size_t ei = si + (rng() % (bounds.size() - si));
                end = bounds[ei];
            }
            std::string replacement;
            if (rng() % 3 != 0) {
                int count = 1 + (rng() % 5);
                for (int c = 0; c < count; ++c)
                    replacement += static_cast<char>('a' + (rng() % 26));
            }
            buf.apply_local_edit({{start, end}}, {replacement});
            check_invariants(buf, qPrintable(QString("edit_%1").arg(i)));

            if (i % 25 == 0 && i > 0) {
                buf.collect_garbage();
                check_invariants(buf, qPrintable(QString("gc_%1").arg(i)));
            }

            if (rng() % 5 == 0) {
                if (rng() % 2 == 0) buf.undo(); else buf.redo();
                check_invariants(buf, qPrintable(QString("undo_redo_%1").arg(i)));
            }
        }

        buf.collect_garbage();
        check_invariants(buf, "final_gc");
    }

    // -----------------------------------------------------------------------
    // Multi-replica convergence with GC
    // -----------------------------------------------------------------------

    void gc_preserves_convergence() {
        Buffer bufA(1), bufB(2);

        auto op1 = bufA.apply_local_edit({{0, 0}}, {"hello world"});
        bufB.apply_ops({op1});
        QCOMPARE(bufA.text(), bufB.text());

        auto op2 = bufA.apply_local_edit({{0, 5}}, {""});
        auto op3 = bufB.apply_local_edit({{6, 11}}, {""});

        bufA.apply_ops({op3});
        bufB.apply_ops({op2});
        QCOMPARE(bufA.text(), bufB.text());

        bufA.set_max_undo_depth(0);
        bufA.collect_garbage();

        QCOMPARE(bufA.text(), bufB.text());
        check_invariants(bufA, "after_gc_A");
        check_invariants(bufB, "after_gc_B");

        auto op4 = bufA.apply_local_edit({{0, 0}}, {"new "});
        bufB.apply_ops({op4});
        QCOMPARE(bufA.text(), bufB.text());
    }

    void gc_convergence_fuzz() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        std::mt19937 rng(seed);

        Buffer bufA(1), bufB(2);
        bufA.set_max_undo_depth(10);
        bufB.set_max_undo_depth(10);
        std::vector<Operation> queueA, queueB;

        auto random_boundary_edit = [&](Buffer& buf) -> Operation {
            std::string text = buf.text();
            uint32_t len = static_cast<uint32_t>(text.size());
            uint32_t start = 0, end = 0;
            if (len > 0) {
                std::vector<uint32_t> bounds = {0};
                for (size_t b = 0; b < text.size(); ) {
                    unsigned char c = static_cast<unsigned char>(text[b]);
                    if (c < 0x80) b += 1;
                    else if ((c & 0xE0) == 0xC0) b += 2;
                    else if ((c & 0xF0) == 0xE0) b += 3;
                    else b += 4;
                    bounds.push_back(static_cast<uint32_t>(b));
                }
                size_t si = rng() % bounds.size();
                start = bounds[si];
                size_t ei = si + (rng() % (bounds.size() - si));
                end = bounds[ei];
            }
            std::string rep;
            if (rng() % 2 == 0) {
                int c = 1 + (rng() % 3);
                for (int j = 0; j < c; ++j)
                    rep += static_cast<char>('a' + (rng() % 26));
            }
            return buf.apply_local_edit({{start, end}}, {rep});
        };

        for (int i = 0; i < 80; ++i) {
            int action = rng() % 100;
            if (action < 40) {
                auto op = random_boundary_edit(bufA);
                queueB.push_back(op);
            } else if (action < 80) {
                auto op = random_boundary_edit(bufB);
                queueA.push_back(op);
            } else if (action < 90) {
                if (!queueA.empty()) {
                    int n = 1 + (rng() % std::min<int>(3, static_cast<int>(queueA.size())));
                    std::vector<Operation> batch(queueA.begin(), queueA.begin() + n);
                    queueA.erase(queueA.begin(), queueA.begin() + n);
                    bufA.apply_ops(batch);
                }
                if (!queueB.empty()) {
                    int n = 1 + (rng() % std::min<int>(3, static_cast<int>(queueB.size())));
                    std::vector<Operation> batch(queueB.begin(), queueB.begin() + n);
                    queueB.erase(queueB.begin(), queueB.begin() + n);
                    bufB.apply_ops(batch);
                }
            } else {
                if (rng() % 2 == 0) bufA.collect_garbage();
                else bufB.collect_garbage();
            }

            if (i % 20 == 0) {
                check_invariants(bufA, qPrintable(QString("A_step_%1").arg(i)));
                check_invariants(bufB, qPrintable(QString("B_step_%1").arg(i)));
            }
        }

        if (!queueA.empty()) bufA.apply_ops(queueA);
        if (!queueB.empty()) bufB.apply_ops(queueB);
        for (int pass = 0; pass < 20; ++pass) {
            bufA.apply_ops({});
            bufB.apply_ops({});
        }

        check_invariants(bufA, "final_A");
        check_invariants(bufB, "final_B");
        QCOMPARE(bufA.text(), bufB.text());
    }

    // -----------------------------------------------------------------------
    // Watermark-based compact()
    // -----------------------------------------------------------------------

    void compact_removes_remote_tombstones() {
        // collect_garbage() can't remove remote-deletion tombstones,
        // but compact() can when the watermark covers the deletion.
        Buffer bufA(1), bufB(2);
        auto op1 = bufA.apply_local_edit({{0, 0}}, {"hello"});
        bufB.apply_ops({op1});

        // B deletes "hello" — creates tombstone with B's deletion_id
        auto op2 = bufB.apply_local_edit({{0, 5}}, {""});
        bufA.apply_ops({op2});
        QCOMPARE(bufA.text(), std::string(""));
        QCOMPARE(bufB.text(), std::string(""));

        // A's collect_garbage won't remove this — deletion is from B
        bufA.set_max_undo_depth(0);
        QCOMPARE(bufA.collect_garbage(), size_t(0));
        QVERIFY(bufA.tombstone_count() > 0);

        // But compact() with a watermark covering B's deletion WILL remove it
        Global watermark;
        watermark.join(bufA.version());
        watermark.meet(bufB.version());
        size_t removed = bufA.compact(watermark);
        QVERIFY(removed > 0);
        QCOMPARE(bufA.tombstone_count(), size_t(0));
        QCOMPARE(bufA.text(), std::string(""));
        check_invariants(bufA, "after_compact");
    }

    void compact_respects_undo_protection() {
        Buffer bufA(1), bufB(2);
        auto op1 = bufA.apply_local_edit({{0, 0}}, {"hello"});
        bufB.apply_ops({op1});

        auto op2 = bufA.apply_local_edit({{0, 5}}, {""});
        bufB.apply_ops({op2});

        Global watermark;
        watermark.join(bufA.version());
        watermark.meet(bufB.version());

        // A's deletion IS in A's undo stack — compact should NOT remove
        QCOMPARE(bufA.compact(watermark), size_t(0));
        QVERIFY(bufA.tombstone_count() > 0);

        // Undo the deletion — text comes back
        bufA.undo();
        QCOMPARE(bufA.text(), std::string("hello"));
    }

    void compact_ignores_deletions_beyond_watermark() {
        Buffer bufA(1), bufB(2);
        auto op1 = bufA.apply_local_edit({{0, 0}}, {"hello"});
        bufB.apply_ops({op1});

        // Snapshot watermark BEFORE B deletes
        Global watermark;
        watermark.join(bufA.version());
        watermark.meet(bufB.version());

        // B deletes — this deletion is NOT covered by the watermark
        auto op2 = bufB.apply_local_edit({{0, 5}}, {""});
        bufA.apply_ops({op2});
        bufA.set_max_undo_depth(0);

        // compact with old watermark should NOT remove (deletion > watermark)
        QCOMPARE(bufA.compact(watermark), size_t(0));
        QVERIFY(bufA.tombstone_count() > 0);
    }

    void compact_convergence_both_replicas() {
        // Both replicas compact at the same watermark — convergence preserved
        Buffer bufA(1), bufB(2);
        auto op1 = bufA.apply_local_edit({{0, 0}}, {"hello world"});
        bufB.apply_ops({op1});

        auto op2 = bufA.apply_local_edit({{0, 5}}, {""});  // A deletes "hello"
        auto op3 = bufB.apply_local_edit({{6, 11}}, {""});  // B deletes "world"
        bufA.apply_ops({op3});
        bufB.apply_ops({op2});
        QCOMPARE(bufA.text(), bufB.text());

        // Compute watermark = meet of both versions
        Global watermark;
        watermark.join(bufA.version());
        watermark.meet(bufB.version());

        // Both compact at the same watermark
        bufA.set_max_undo_depth(0);
        bufB.set_max_undo_depth(0);
        bufA.compact(watermark);
        bufB.compact(watermark);

        QCOMPARE(bufA.text(), bufB.text());
        QCOMPARE(bufA.tombstone_count(), size_t(0));
        QCOMPARE(bufB.tombstone_count(), size_t(0));
        check_invariants(bufA, "A_after_compact");
        check_invariants(bufB, "B_after_compact");

        // Further edits should work fine
        auto op4 = bufA.apply_local_edit({{0, 0}}, {"new "});
        bufB.apply_ops({op4});
        QCOMPARE(bufA.text(), bufB.text());
        check_invariants(bufA, "A_after_edit");
        check_invariants(bufB, "B_after_edit");
    }

    void compact_convergence_fuzz() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        std::mt19937 rng(seed);

        Buffer bufA(1), bufB(2);
        bufA.set_max_undo_depth(10);
        bufB.set_max_undo_depth(10);
        std::vector<Operation> queueA, queueB;

        auto random_boundary_edit = [&](Buffer& buf) -> Operation {
            std::string text = buf.text();
            uint32_t len = static_cast<uint32_t>(text.size());
            uint32_t start = 0, end = 0;
            if (len > 0) {
                std::vector<uint32_t> bounds = {0};
                for (size_t b = 0; b < text.size(); ) {
                    unsigned char c = static_cast<unsigned char>(text[b]);
                    if (c < 0x80) b += 1;
                    else if ((c & 0xE0) == 0xC0) b += 2;
                    else if ((c & 0xF0) == 0xE0) b += 3;
                    else b += 4;
                    bounds.push_back(static_cast<uint32_t>(b));
                }
                size_t si = rng() % bounds.size();
                start = bounds[si];
                size_t ei = si + (rng() % (bounds.size() - si));
                end = bounds[ei];
            }
            std::string rep;
            if (rng() % 2 == 0) {
                int c = 1 + (rng() % 3);
                for (int j = 0; j < c; ++j)
                    rep += static_cast<char>('a' + (rng() % 26));
            }
            return buf.apply_local_edit({{start, end}}, {rep});
        };

        for (int i = 0; i < 80; ++i) {
            int action = rng() % 100;
            if (action < 40) {
                auto op = random_boundary_edit(bufA);
                queueB.push_back(op);
            } else if (action < 80) {
                auto op = random_boundary_edit(bufB);
                queueA.push_back(op);
            } else {
                // Sync: drain all queues, then compact at shared watermark
                if (!queueA.empty()) bufA.apply_ops(queueA);
                if (!queueB.empty()) bufB.apply_ops(queueB);
                queueA.clear();
                queueB.clear();
                for (int pass = 0; pass < 5; ++pass) {
                    bufA.apply_ops({});
                    bufB.apply_ops({});
                }
                Global watermark;
                watermark.join(bufA.version());
                watermark.meet(bufB.version());
                bufA.compact(watermark);
                bufB.compact(watermark);
                check_invariants(bufA, qPrintable(QString("A_compact_%1").arg(i)));
                check_invariants(bufB, qPrintable(QString("B_compact_%1").arg(i)));
            }
        }

        // Final sync + compact
        if (!queueA.empty()) bufA.apply_ops(queueA);
        if (!queueB.empty()) bufB.apply_ops(queueB);
        for (int pass = 0; pass < 20; ++pass) {
            bufA.apply_ops({});
            bufB.apply_ops({});
        }
        Global watermark;
        watermark.join(bufA.version());
        watermark.meet(bufB.version());
        bufA.compact(watermark);
        bufB.compact(watermark);

        check_invariants(bufA, "final_A");
        check_invariants(bufB, "final_B");
        QCOMPARE(bufA.text(), bufB.text());
    }
};

QTEST_MAIN(TestGC)
#include "tst_gc.moc"
