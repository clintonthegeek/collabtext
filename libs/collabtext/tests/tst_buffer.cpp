#include <QTest>
#include "crdt/Buffer.h"

using namespace CollabText::Crdt;

class TestBuffer : public QObject {
    Q_OBJECT
private slots:

    // -----------------------------------------------------------------------
    // Basic inserts
    // -----------------------------------------------------------------------

    void insert_at_beginning() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        QCOMPARE(buf.text(), std::string("hello"));
    }

    void insert_at_end() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        buf.apply_local_edit({{5, 5}}, {" world"});
        QCOMPARE(buf.text(), std::string("hello world"));
    }

    void insert_in_middle() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hllo"});
        buf.apply_local_edit({{1, 1}}, {"e"});
        QCOMPARE(buf.text(), std::string("hello"));
    }

    // -----------------------------------------------------------------------
    // Basic deletes
    // -----------------------------------------------------------------------

    void delete_from_beginning() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        buf.apply_local_edit({{0, 1}}, {""});
        QCOMPARE(buf.text(), std::string("ello"));
    }

    void delete_from_end() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        buf.apply_local_edit({{4, 5}}, {""});
        QCOMPARE(buf.text(), std::string("hell"));
    }

    void delete_from_middle() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        buf.apply_local_edit({{1, 4}}, {""});
        QCOMPARE(buf.text(), std::string("ho"));
    }

    // -----------------------------------------------------------------------
    // Replace (delete + insert)
    // -----------------------------------------------------------------------

    void replace() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        buf.apply_local_edit({{0, 5}}, {"world"});
        QCOMPARE(buf.text(), std::string("world"));
    }

    // -----------------------------------------------------------------------
    // Undo / Redo
    // -----------------------------------------------------------------------

    void undo_reverses_last_edit() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        buf.apply_local_edit({{5, 5}}, {" world"});
        QCOMPARE(buf.text(), std::string("hello world"));

        auto op = buf.undo();
        QVERIFY(op.has_value());
        QCOMPARE(buf.text(), std::string("hello"));
    }

    void redo_restores() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        buf.apply_local_edit({{5, 5}}, {" world"});

        buf.undo();
        QCOMPARE(buf.text(), std::string("hello"));

        auto op = buf.redo();
        QVERIFY(op.has_value());
        QCOMPARE(buf.text(), std::string("hello world"));
    }

    void undo_insert_then_redo() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"abc"});
        QCOMPARE(buf.text(), std::string("abc"));

        buf.undo();
        QCOMPARE(buf.text(), std::string(""));

        buf.redo();
        QCOMPARE(buf.text(), std::string("abc"));
    }

    void undo_delete_restores_text() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        buf.apply_local_edit({{0, 5}}, {""});
        QCOMPARE(buf.text(), std::string(""));

        buf.undo();
        QCOMPARE(buf.text(), std::string("hello"));
    }

    void undo_nothing_returns_nullopt() {
        Buffer buf(1);
        auto op = buf.undo();
        QVERIFY(!op.has_value());
    }

    void redo_nothing_returns_nullopt() {
        Buffer buf(1);
        auto op = buf.redo();
        QVERIFY(!op.has_value());
    }

    // -----------------------------------------------------------------------
    // Remote edit
    // -----------------------------------------------------------------------

    void remote_edit_inserts() {
        // Buffer A inserts "hello"
        Buffer bufA(1);
        auto op = bufA.apply_local_edit({{0, 0}}, {"hello"});

        // Buffer B applies the remote operation
        Buffer bufB(2);
        bufB.apply_ops({op});

        QCOMPARE(bufB.text(), std::string("hello"));
    }

    void remote_edit_delete() {
        // Buffer A inserts "hello" then deletes "ell"
        Buffer bufA(1);
        auto op1 = bufA.apply_local_edit({{0, 0}}, {"hello"});
        auto op2 = bufA.apply_local_edit({{1, 4}}, {""});

        // Buffer B applies both
        Buffer bufB(2);
        bufB.apply_ops({op1, op2});

        QCOMPARE(bufB.text(), std::string("ho"));
    }

    // -----------------------------------------------------------------------
    // Concurrent inserts
    // -----------------------------------------------------------------------

    void concurrent_inserts_both_survive() {
        // Both buffers start empty. A inserts "a", B inserts "b".
        Buffer bufA(1);
        Buffer bufB(2);

        auto opA = bufA.apply_local_edit({{0, 0}}, {"a"});
        auto opB = bufB.apply_local_edit({{0, 0}}, {"b"});

        bufA.apply_ops({opB});
        bufB.apply_ops({opA});

        // Both characters must survive
        std::string textA = bufA.text();
        std::string textB = bufB.text();
        QCOMPARE(textA.size(), size_t(2));
        QCOMPARE(textB.size(), size_t(2));
        QVERIFY(textA.find('a') != std::string::npos);
        QVERIFY(textA.find('b') != std::string::npos);
    }

    void concurrent_inserts_deterministic() {
        // Both buffers must agree on the order
        Buffer bufA(1);
        Buffer bufB(2);

        auto opA = bufA.apply_local_edit({{0, 0}}, {"a"});
        auto opB = bufB.apply_local_edit({{0, 0}}, {"b"});

        bufA.apply_ops({opB});
        bufB.apply_ops({opA});

        QCOMPARE(bufA.text(), bufB.text());
    }

    // -----------------------------------------------------------------------
    // Causal ordering
    // -----------------------------------------------------------------------

    void causal_ordering_defers() {
        // A inserts "hello", then A deletes "ell".
        // B receives the delete BEFORE the insert.
        // The delete should be deferred until the insert arrives.
        Buffer bufA(1);
        auto op1 = bufA.apply_local_edit({{0, 0}}, {"hello"});
        auto op2 = bufA.apply_local_edit({{1, 4}}, {""});

        Buffer bufB(2);
        // Apply op2 first (out of order)
        bufB.apply_ops({op2});
        // op2 should be deferred — text should still be empty
        QCOMPARE(bufB.text(), std::string(""));

        // Now apply op1
        bufB.apply_ops({op1});
        // op1 applies, then deferred op2 should also apply
        QCOMPARE(bufB.text(), std::string("ho"));
    }

    // -----------------------------------------------------------------------
    // Idempotence
    // -----------------------------------------------------------------------

    void duplicate_ops_idempotent() {
        Buffer bufA(1);
        auto op = bufA.apply_local_edit({{0, 0}}, {"hello"});

        Buffer bufB(2);
        bufB.apply_ops({op});
        bufB.apply_ops({op});  // duplicate

        QCOMPARE(bufB.text(), std::string("hello"));
    }

    // -----------------------------------------------------------------------
    // Additional edge cases
    // -----------------------------------------------------------------------

    void visible_length_tracks_correctly() {
        Buffer buf(1);
        QCOMPARE(buf.visible_length(), 0u);

        buf.apply_local_edit({{0, 0}}, {"hello"});
        QCOMPARE(buf.visible_length(), 5u);

        buf.apply_local_edit({{0, 1}}, {""});
        QCOMPARE(buf.visible_length(), 4u);
    }

    void version_advances() {
        Buffer buf(1);
        auto v0 = buf.version();
        buf.apply_local_edit({{0, 0}}, {"hi"});
        auto v1 = buf.version();

        // Version should have advanced for replica 1
        QVERIFY(v1.get(1) > v0.get(1));
    }

    void replica_id_returned() {
        Buffer buf(42);
        QCOMPARE(buf.replica_id(), uint16_t(42));
    }

    void multiple_ranges_in_single_edit() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"abcdef"});
        // Delete 'b' and 'd' in a single multi-range edit (ranges from right to left
        // to test proper ordering)
        buf.apply_local_edit({{1, 2}, {3, 4}}, {"", ""});
        QCOMPARE(buf.text(), std::string("acef"));
    }
};

QTEST_MAIN(TestBuffer)
#include "tst_buffer.moc"
