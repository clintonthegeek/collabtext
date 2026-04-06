#include <QTest>
#include "crdt/Buffer.h"

using namespace CollabText::Crdt;

class TestEditsSince : public QObject {
    Q_OBJECT

private slots:
    void no_edits_returns_empty() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        Global v = buf.version();
        // No changes since current version
        auto edits = buf.edits_since(v);
        QCOMPARE(edits.size(), size_t(0));
    }

    void simple_remote_insert() {
        Buffer bufA(1), bufB(2);
        auto op1 = bufA.apply_local_edit({{0, 0}}, {"hello"});
        bufB.apply_ops({op1});

        Global before = bufB.version();

        // A inserts " world" at position 5
        auto op2 = bufA.apply_local_edit({{5, 5}}, {" world"});
        bufB.apply_ops({op2});

        auto edits = bufB.edits_since(before);
        QCOMPARE(edits.size(), size_t(1));
        QCOMPARE(edits[0].old_start, uint32_t(5));
        QCOMPARE(edits[0].old_end, uint32_t(5));  // pure insertion
        QCOMPARE(edits[0].new_text, std::string(" world"));
    }

    void simple_remote_delete() {
        Buffer bufA(1), bufB(2);
        auto op1 = bufA.apply_local_edit({{0, 0}}, {"hello world"});
        bufB.apply_ops({op1});

        Global before = bufB.version();

        // A deletes " world" (bytes 5-11)
        auto op2 = bufA.apply_local_edit({{5, 11}}, {""});
        bufB.apply_ops({op2});

        auto edits = bufB.edits_since(before);
        QCOMPARE(edits.size(), size_t(1));
        QCOMPARE(edits[0].old_start, uint32_t(5));
        QCOMPARE(edits[0].old_end, uint32_t(11));
        QCOMPARE(edits[0].new_text, std::string(""));
    }

    void remote_replace() {
        Buffer bufA(1), bufB(2);
        auto op1 = bufA.apply_local_edit({{0, 0}}, {"hello world"});
        bufB.apply_ops({op1});

        Global before = bufB.version();

        // A replaces "world" with "there"
        auto op2 = bufA.apply_local_edit({{6, 11}}, {"there"});
        bufB.apply_ops({op2});

        auto edits = bufB.edits_since(before);
        // Should have deletion of "world" and insertion of "there",
        // possibly merged into one replacement edit
        QVERIFY(!edits.empty());

        // Apply the edits to the old text and verify result
        std::string old_text = "hello world";
        std::string result = old_text;
        // Apply in reverse order for correct offset handling
        for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
            result.replace(it->old_start, it->old_end - it->old_start, it->new_text);
        }
        QCOMPARE(result, std::string("hello there"));
    }

    void insert_at_beginning() {
        Buffer bufA(1), bufB(2);
        auto op1 = bufA.apply_local_edit({{0, 0}}, {"world"});
        bufB.apply_ops({op1});

        Global before = bufB.version();

        auto op2 = bufA.apply_local_edit({{0, 0}}, {"hello "});
        bufB.apply_ops({op2});

        auto edits = bufB.edits_since(before);
        QCOMPARE(edits.size(), size_t(1));
        QCOMPARE(edits[0].old_start, uint32_t(0));
        QCOMPARE(edits[0].old_end, uint32_t(0));
        QCOMPARE(edits[0].new_text, std::string("hello "));
    }

    void multiple_remote_edits() {
        Buffer bufA(1), bufB(2);
        auto op1 = bufA.apply_local_edit({{0, 0}}, {"aaa bbb ccc"});
        bufB.apply_ops({op1});

        Global before = bufB.version();

        // A does two separate edits
        auto op2 = bufA.apply_local_edit({{0, 3}}, {"AAA"});  // replace "aaa" with "AAA"
        auto op3 = bufA.apply_local_edit({{8, 11}}, {"CCC"});  // replace "ccc" with "CCC"
        bufB.apply_ops({op2, op3});

        auto edits = bufB.edits_since(before);
        // Should have two separate edits (non-adjacent)
        QVERIFY(edits.size() >= 1);

        // Verify by applying edits to old text
        std::string old_text = "aaa bbb ccc";
        std::string result = old_text;
        for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
            result.replace(it->old_start, it->old_end - it->old_start, it->new_text);
        }
        QCOMPARE(result, bufA.text());
    }

    void concurrent_edits_produce_correct_edits() {
        Buffer bufA(1), bufB(2);
        auto op0 = bufA.apply_local_edit({{0, 0}}, {"hello world"});
        bufB.apply_ops({op0});

        Global beforeA = bufA.version();
        Global beforeB = bufB.version();

        // Concurrent edits
        auto opA = bufA.apply_local_edit({{5, 5}}, {"!"});
        auto opB = bufB.apply_local_edit({{0, 0}}, {">"});

        // Exchange
        bufA.apply_ops({opB});
        bufB.apply_ops({opA});

        // Both should converge
        QCOMPARE(bufA.text(), bufB.text());

        // edits_since should reconstruct correctly
        auto editsA = bufA.edits_since(beforeA);
        auto editsB = bufB.edits_since(beforeB);

        // Apply editsA to A's old text
        std::string oldA = "hello world";
        std::string resultA = oldA;
        for (auto it = editsA.rbegin(); it != editsA.rend(); ++it) {
            resultA.replace(it->old_start, it->old_end - it->old_start, it->new_text);
        }
        QCOMPARE(resultA, bufA.text());

        // Apply editsB to B's old text
        std::string oldB = "hello world";
        std::string resultB = oldB;
        for (auto it = editsB.rbegin(); it != editsB.rend(); ++it) {
            resultB.replace(it->old_start, it->old_end - it->old_start, it->new_text);
        }
        QCOMPARE(resultB, bufB.text());
    }

    void undo_produces_edits() {
        Buffer bufA(1), bufB(2);
        auto op1 = bufA.apply_local_edit({{0, 0}}, {"hello"});
        bufB.apply_ops({op1});

        auto op2 = bufA.apply_local_edit({{5, 5}}, {" world"});
        bufB.apply_ops({op2});

        Global before = bufB.version();

        auto undo_op = bufA.undo();
        QVERIFY(undo_op.has_value());
        bufB.apply_ops({*undo_op});

        auto edits = bufB.edits_since(before);
        QVERIFY(!edits.empty());

        // Apply to old text
        std::string old_text = "hello world";
        std::string result = old_text;
        for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
            result.replace(it->old_start, it->old_end - it->old_start, it->new_text);
        }
        QCOMPARE(result, std::string("hello"));
    }

    void empty_document_insert() {
        Buffer bufA(1), bufB(2);
        Global before = bufB.version();

        auto op1 = bufA.apply_local_edit({{0, 0}}, {"hello"});
        bufB.apply_ops({op1});

        auto edits = bufB.edits_since(before);
        QCOMPARE(edits.size(), size_t(1));
        QCOMPARE(edits[0].old_start, uint32_t(0));
        QCOMPARE(edits[0].old_end, uint32_t(0));
        QCOMPARE(edits[0].new_text, std::string("hello"));
    }
};

QTEST_MAIN(TestEditsSince)
#include "tst_edits_since.moc"
