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

    // -----------------------------------------------------------------------
    // Opt 2: Cursor-based apply_local_edit tests
    // -----------------------------------------------------------------------

    void local_edit_replace_mid_fragment() {
        // REGRESSION: was producing "heoXX" due to locator ordering bug
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        buf.apply_local_edit({{2, 4}}, {"XX"});
        QCOMPARE(buf.text(), std::string("heXXo"));
        QCOMPARE(buf.visible_length(), 5u);
    }

    void local_edit_insert_at_start_of_fragment() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"abcdef"});
        buf.apply_local_edit({{0, 0}}, {"XX"});
        QCOMPARE(buf.text(), std::string("XXabcdef"));
    }

    void local_edit_insert_mid_fragment() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"abcdef"});
        buf.apply_local_edit({{3, 3}}, {"XX"});
        QCOMPARE(buf.text(), std::string("abcXXdef"));

        auto frags = buf.fragments();
        bool found_xx = false;
        for (auto& f : frags) {
            if (f.byte_length == 2 && f.visible) { found_xx = true; break; }
        }
        QVERIFY(found_xx);
    }

    void local_edit_delete_spanning_fragments() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"ab"});
        buf.apply_local_edit({{2, 2}}, {"cd"});
        buf.apply_local_edit({{4, 4}}, {"ef"});
        QCOMPARE(buf.text(), std::string("abcdef"));

        buf.apply_local_edit({{0, 6}}, {""});
        QCOMPARE(buf.text(), std::string(""));
        QCOMPARE(buf.visible_length(), 0u);
    }

    void local_edit_delete_partial_fragment() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"abcdef"});
        buf.apply_local_edit({{0, 3}}, {""});
        QCOMPARE(buf.text(), std::string("def"));
        QCOMPARE(buf.visible_length(), 3u);
    }

    void local_edit_multi_range_left_to_right() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"abcdefghij"});
        buf.apply_local_edit({{1, 2}, {4, 5}}, {"X", "Y"});
        QCOMPARE(buf.text(), std::string("aXcdYfghij"));
    }

    void local_edit_replace_entire_document() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        buf.apply_local_edit({{0, 5}}, {"world"});
        QCOMPARE(buf.text(), std::string("world"));
        QCOMPARE(buf.visible_length(), 5u);
    }

    // -----------------------------------------------------------------------
    // Opt 3: VersionedFullOffset / remote edit tests
    // -----------------------------------------------------------------------

    void remote_edit_skips_unseen_fragments() {
        // bufA inserts "aaa" first, then bufB inserts "bbb".
        // bufB syncs opA1 before inserting, so clock values don't overlap.
        // After convergence, bufA deletes its own "aaa"; bufB must apply
        // that delete correctly via versioned seek (not by position).
        Buffer bufA(1);
        Buffer bufB(2);

        auto opA1 = bufA.apply_local_edit({{0, 0}}, {"aaa"});
        // bufB syncs opA1 first so its clock advances past replica 1's values
        bufB.apply_ops({opA1});
        auto opB1 = bufB.apply_local_edit({{3, 3}}, {"bbb"});

        bufA.apply_ops({opB1});
        std::string converged = bufA.text();
        QCOMPARE(bufA.text(), bufB.text());

        auto posA = converged.find("aaa");
        QVERIFY(posA != std::string::npos);
        auto opA2 = bufA.apply_local_edit(
            {{static_cast<uint32_t>(posA), static_cast<uint32_t>(posA + 3)}}, {""});

        bufB.apply_ops({opA2});

        QCOMPARE(bufA.text(), bufB.text());
        QCOMPARE(bufA.text().size(), size_t(3));
        QVERIFY(bufA.text().find("bbb") != std::string::npos);
    }

    void remote_edit_version_filtered_offset() {
        Buffer buf1(1), buf2(2), buf3(3);

        auto op1 = buf1.apply_local_edit({{0, 0}}, {"111"});
        auto op2 = buf2.apply_local_edit({{0, 0}}, {"222"});
        auto op3 = buf3.apply_local_edit({{0, 0}}, {"333"});

        buf1.apply_ops({op2});

        std::string text1 = buf1.text();
        QCOMPARE(text1.size(), size_t(6));

        auto opDel = buf1.apply_local_edit({{0, 3}}, {""});

        Buffer bufFinal(4);
        bufFinal.apply_ops({op1, op2, op3, opDel});
        for (int i = 0; i < 5; ++i) bufFinal.apply_ops({});

        buf1.apply_ops({op3});
        QCOMPARE(buf1.text(), bufFinal.text());
    }

    void remote_edit_convergence_with_concurrent_deletes() {
        Buffer bufA(1), bufB(2);

        auto opIns = bufA.apply_local_edit({{0, 0}}, {"abcdefghij"});
        bufB.apply_ops({opIns});
        QCOMPARE(bufB.text(), std::string("abcdefghij"));

        auto opDelA = bufA.apply_local_edit({{2, 5}}, {""});
        auto opDelB = bufB.apply_local_edit({{3, 7}}, {""});

        bufA.apply_ops({opDelB});
        bufB.apply_ops({opDelA});

        QCOMPARE(bufA.text(), bufB.text());
        // Characters at positions 2,3,4,5,6 all deleted → "abhij"
        QCOMPARE(bufA.text(), std::string("abhij"));
    }

    // -----------------------------------------------------------------------
    // Undo/redo with remote operations
    // -----------------------------------------------------------------------

    void undo_after_remote_edit() {
        Buffer bufA(1), bufB(2);
        auto opA = bufA.apply_local_edit({{0, 0}}, {"hello"});
        auto opB = bufB.apply_local_edit({{0, 0}}, {"world"});

        bufA.apply_ops({opB});
        QCOMPARE(bufA.visible_length(), 10u);

        auto undoOp = bufA.undo();
        QVERIFY(undoOp.has_value());
        QCOMPARE(bufA.visible_length(), 5u);
        QCOMPARE(bufA.text(), std::string("world"));
    }

    void redo_after_remote_edit() {
        Buffer bufA(1), bufB(2);
        auto opA = bufA.apply_local_edit({{0, 0}}, {"hello"});
        auto opB = bufB.apply_local_edit({{0, 0}}, {"world"});

        bufA.apply_ops({opB});
        // Capture state before undo; concurrent inserts may interleave chars
        std::string textBeforeUndo = bufA.text();
        QCOMPARE(bufA.visible_length(), 10u);

        bufA.undo();
        QCOMPARE(bufA.text(), std::string("world"));

        bufA.redo();
        // Redo must restore the exact pre-undo state
        QCOMPARE(bufA.visible_length(), 10u);
        QCOMPARE(bufA.text(), textBeforeUndo);
    }

    void remote_undo_broadcast() {
        Buffer bufA(1), bufB(2);
        auto opIns = bufA.apply_local_edit({{0, 0}}, {"hello"});
        bufB.apply_ops({opIns});
        QCOMPARE(bufB.text(), std::string("hello"));

        auto undoOp = bufA.undo();
        QVERIFY(undoOp.has_value());
        bufB.apply_ops({*undoOp});
        QCOMPARE(bufB.text(), std::string(""));
    }

    void undo_delete_with_remote_interleaving() {
        Buffer bufA(1), bufB(2);
        auto op1 = bufA.apply_local_edit({{0, 0}}, {"abcdef"});
        bufB.apply_ops({op1});

        auto opDel = bufA.apply_local_edit({{2, 4}}, {""});
        auto opBins = bufB.apply_local_edit({{3, 3}}, {"X"});

        bufA.apply_ops({opBins});
        bufB.apply_ops({opDel});

        QCOMPARE(bufA.text(), bufB.text());

        auto undoOp = bufA.undo();
        QVERIFY(undoOp.has_value());

        std::string text = bufA.text();
        QVERIFY(text.find('a') != std::string::npos);
        QVERIFY(text.find('b') != std::string::npos);
        QVERIFY(text.find('c') != std::string::npos);
        QVERIFY(text.find('d') != std::string::npos);
        QVERIFY(text.find('e') != std::string::npos);
        QVERIFY(text.find('f') != std::string::npos);
        QVERIFY(text.find('X') != std::string::npos);
    }

    void new_edit_clears_redo_stack() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"abc"});
        buf.apply_local_edit({{3, 3}}, {"def"});
        buf.undo();
        QCOMPARE(buf.text(), std::string("abc"));

        buf.apply_local_edit({{3, 3}}, {"xyz"});
        auto op = buf.redo();
        QVERIFY(!op.has_value());
        QCOMPARE(buf.text(), std::string("abcxyz"));
    }

    // -----------------------------------------------------------------------
    // Split relocation verification
    // -----------------------------------------------------------------------

    void split_relocation_remote_convergence() {
        Buffer bufA(1), bufB(2);
        auto op1 = bufA.apply_local_edit({{0, 0}}, {"abcdef"});
        bufB.apply_ops({op1});

        auto op2 = bufA.apply_local_edit({{3, 3}}, {"X"});
        bufB.apply_ops({op2});

        QCOMPARE(bufA.text(), std::string("abcXdef"));
        QCOMPARE(bufB.text(), std::string("abcXdef"));
    }

    void split_relocation_with_concurrent_insert() {
        Buffer bufA(1), bufB(2);
        auto op1 = bufA.apply_local_edit({{0, 0}}, {"abcdef"});
        bufB.apply_ops({op1});

        auto opA = bufA.apply_local_edit({{3, 3}}, {"X"});
        auto opB = bufB.apply_local_edit({{3, 3}}, {"Y"});

        bufA.apply_ops({opB});
        bufB.apply_ops({opA});

        QCOMPARE(bufA.text(), bufB.text());
        std::string text = bufA.text();
        QCOMPARE(text.size(), size_t(8));
        QVERIFY(text.find('X') != std::string::npos);
        QVERIFY(text.find('Y') != std::string::npos);
    }

    void replace_mid_fragment_remote_convergence() {
        Buffer bufA(1), bufB(2);
        auto op1 = bufA.apply_local_edit({{0, 0}}, {"hello"});
        bufB.apply_ops({op1});

        auto op2 = bufA.apply_local_edit({{2, 4}}, {"XX"});
        bufB.apply_ops({op2});

        QCOMPARE(bufA.text(), std::string("heXXo"));
        QCOMPARE(bufB.text(), std::string("heXXo"));
    }

    // -----------------------------------------------------------------------
    // apply_local_edit boundary cases
    // -----------------------------------------------------------------------

    void empty_range_insert_only() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        buf.apply_local_edit({{2, 2}}, {"X"});
        QCOMPARE(buf.text(), std::string("heXllo"));
    }

    void adjacent_ranges() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"abcdef"});
        buf.apply_local_edit({{1, 3}, {3, 5}}, {"X", "Y"});
        QCOMPARE(buf.text(), std::string("aXYf"));
    }

    void delete_everything_then_insert() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        buf.apply_local_edit({{0, 5}}, {"world"});
        QCOMPARE(buf.text(), std::string("world"));
        QCOMPARE(buf.visible_length(), static_cast<uint32_t>(buf.text().size()));
    }

    void visible_length_always_matches_text() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello world"});
        QCOMPARE(buf.visible_length(), static_cast<uint32_t>(buf.text().size()));

        buf.apply_local_edit({{5, 6}}, {"_"});
        QCOMPARE(buf.visible_length(), static_cast<uint32_t>(buf.text().size()));

        buf.apply_local_edit({{0, 3}}, {""});
        QCOMPARE(buf.visible_length(), static_cast<uint32_t>(buf.text().size()));

        buf.undo();
        QCOMPARE(buf.visible_length(), static_cast<uint32_t>(buf.text().size()));

        buf.redo();
        QCOMPARE(buf.visible_length(), static_cast<uint32_t>(buf.text().size()));
    }

    // -----------------------------------------------------------------------
    // Concurrent undo (Optimization 5)
    // -----------------------------------------------------------------------

    void concurrent_undo_both_hide() {
        // Alice and Bob each insert their own text, then each undo their own edit.
        // A third buffer receives all ops and both undos — text should be hidden.
        Buffer bufA(1), bufB(2), bufC(3);
        auto insA = bufA.apply_local_edit({{0, 0}}, {"hello"});
        auto insB = bufB.apply_local_edit({{0, 0}}, {"world"});

        // Both buffers sync each other's inserts
        bufA.apply_ops({insB});
        bufB.apply_ops({insA});
        bufC.apply_ops({insA, insB});

        // Each buffer undoes its own edit
        auto undoA = bufA.undo();
        auto undoB = bufB.undo();
        QVERIFY(undoA.has_value());
        QVERIFY(undoB.has_value());

        // Cross-apply undos
        bufA.apply_ops({*undoB});
        bufB.apply_ops({*undoA});
        bufC.apply_ops({*undoA, *undoB});

        // All should converge on empty (both undos hide all text)
        QCOMPARE(bufA.text(), std::string(""));
        QCOMPARE(bufB.text(), std::string(""));
        QCOMPARE(bufC.text(), std::string(""));
    }

    void concurrent_undo_then_redo_wins() {
        // A and B each insert text. Both undo. Then A redoes — redo should win
        // for A's text (higher undo count on A's characters means visible).
        Buffer bufA(1), bufB(2), bufC(3);
        auto insA = bufA.apply_local_edit({{0, 0}}, {"hello"});
        auto insB = bufB.apply_local_edit({{0, 0}}, {"world"});

        bufA.apply_ops({insB});
        bufB.apply_ops({insA});
        bufC.apply_ops({insA, insB});

        auto undoA = bufA.undo();
        auto undoB = bufB.undo();
        QVERIFY(undoA.has_value());
        QVERIFY(undoB.has_value());

        bufA.apply_ops({*undoB});
        auto redoA = bufA.redo();
        QVERIFY(redoA.has_value());

        // Apply all ops to C and B
        bufC.apply_ops({*undoA, *undoB, *redoA});
        bufB.apply_ops({*undoA, *redoA});

        // A's redo wins — A's text "hello" is visible; B's text "world" remains
        // hidden (undone but not redone).
        QVERIFY(bufA.text().find("hello") != std::string::npos);
        QVERIFY(bufA.text().find("world") == std::string::npos);
        QCOMPARE(bufA.text(), bufB.text());
        QCOMPARE(bufA.text(), bufC.text());
    }

    void remote_undo_with_counts() {
        // Verify the new counts-based UndoOperation wire format works
        Buffer bufA(1), bufB(2);
        auto ins = bufA.apply_local_edit({{0, 0}}, {"hello"});
        bufB.apply_ops({ins});

        auto undoOp = bufA.undo();
        QVERIFY(undoOp.has_value());

        // Apply undo to B via remote path
        bufB.apply_ops({*undoOp});
        QCOMPARE(bufB.text(), std::string(""));

        // Redo on A, apply to B
        auto redoOp = bufA.redo();
        QVERIFY(redoOp.has_value());
        bufB.apply_ops({*redoOp});
        QCOMPARE(bufB.text(), std::string("hello"));
        QCOMPARE(bufA.text(), bufB.text());
    }

    // -----------------------------------------------------------------------
    // Unified deletion tracking
    // -----------------------------------------------------------------------

    void fragment_with_multiple_deletions() {
        // Two replicas delete the same character. Undo one — still invisible.
        // Undo both — visible.
        Buffer bufA(1), bufB(2), bufC(3);
        auto ins = bufA.apply_local_edit({{0, 0}}, {"hello"});
        bufB.apply_ops({ins});
        bufC.apply_ops({ins});

        // A deletes "h", B deletes "h"
        auto delA = bufA.apply_local_edit({{0, 1}}, {""});
        auto delB = bufB.apply_local_edit({{0, 1}}, {""});

        // C receives both deletes
        bufC.apply_ops({delA, delB});
        QCOMPARE(bufC.text(), std::string("ello"));

        // A undoes its delete — but B's delete still active
        auto undoA = bufA.undo();
        QVERIFY(undoA.has_value());
        bufC.apply_ops({*undoA});
        QCOMPARE(bufC.text(), std::string("ello"));

        // B undoes its delete — now both deletions undone, "h" visible
        auto undoB = bufB.undo();
        QVERIFY(undoB.has_value());
        bufC.apply_ops({*undoB});
        QCOMPARE(bufC.text(), std::string("hello"));
    }

    void deletion_undo_roundtrip() {
        // Delete, undo, redo — verify the parity model works end-to-end
        Buffer bufA(1), bufB(2);
        auto ins = bufA.apply_local_edit({{0, 0}}, {"hello"});
        bufB.apply_ops({ins});

        auto del = bufA.apply_local_edit({{1, 4}}, {""});
        bufB.apply_ops({del});
        QCOMPARE(bufA.text(), std::string("ho"));
        QCOMPARE(bufB.text(), std::string("ho"));

        auto undo = bufA.undo();
        QVERIFY(undo.has_value());
        bufB.apply_ops({*undo});
        QCOMPARE(bufA.text(), std::string("hello"));
        QCOMPARE(bufB.text(), std::string("hello"));

        auto redo = bufA.redo();
        QVERIFY(redo.has_value());
        bufB.apply_ops({*redo});
        QCOMPARE(bufA.text(), std::string("ho"));
        QCOMPARE(bufB.text(), std::string("ho"));
    }

    void concurrent_delete_and_insertion_undo() {
        // B inserts "hello", A receives. A deletes "ell". B undoes insertion.
        // After merge: everything invisible. B redoes → "ho". A undoes delete → "hello".
        Buffer bufA(1), bufB(2);
        auto ins = bufB.apply_local_edit({{0, 0}}, {"hello"});
        bufA.apply_ops({ins});

        auto del = bufA.apply_local_edit({{1, 4}}, {""});
        auto undoB = bufB.undo();
        QVERIFY(undoB.has_value());

        bufA.apply_ops({*undoB});
        bufB.apply_ops({del});

        QCOMPARE(bufA.text(), std::string(""));
        QCOMPARE(bufB.text(), std::string(""));

        auto redoB = bufB.redo();
        QVERIFY(redoB.has_value());
        bufA.apply_ops({*redoB});

        QCOMPARE(bufA.text(), std::string("ho"));
        QCOMPARE(bufB.text(), std::string("ho"));

        auto undoA = bufA.undo();
        QVERIFY(undoA.has_value());
        bufB.apply_ops({*undoA});

        QCOMPARE(bufA.text(), std::string("hello"));
        QCOMPARE(bufB.text(), std::string("hello"));
    }
    // -----------------------------------------------------------------------
    // Regression: sequential inserts at adjacent positions
    // Two inserts by the same replica: first at position N (middle of
    // existing fragment), then at position N+1. The second insert should
    // go between the first inserted char and the next original char.
    // -----------------------------------------------------------------------

    void sequential_insert_at_adjacent_positions() {
        // Single-op variant: Alice creates text in one shot
        Buffer alice(1);
        auto op = alice.apply_local_edit({{0, 0}}, {"A quick brown fox"});

        Buffer bob(2);
        bob.apply_ops({op});
        QCOMPARE(bob.text(), std::string("A quick brown fox"));

        bob.apply_local_edit({{8, 8}}, {"j"});
        QCOMPARE(bob.text(), std::string("A quick jbrown fox"));

        bob.apply_local_edit({{9, 9}}, {"f"});
        QCOMPARE(bob.text(), std::string("A quick jfbrown fox"));
    }

    void sequential_insert_char_by_char_text() {
        // Reproduce the REAL app scenario: Alice types one char at a time
        // (each char is a separate op), synced to Bob, then Bob types
        // in the middle.
        Buffer alice(1);
        Buffer bob(2);

        // Alice types "A quick brown fox" one character at a time
        std::string text = "A quick brown fox";
        for (size_t i = 0; i < text.size(); ++i) {
            auto op = alice.apply_local_edit(
                {{static_cast<uint32_t>(i), static_cast<uint32_t>(i)}},
                {std::string(1, text[i])});
            bob.apply_ops({op});
        }
        QCOMPARE(bob.text(), std::string("A quick brown fox"));

        // Bob inserts 'j' at byte 8 (between "A quick " and "brown")
        bob.apply_local_edit({{8, 8}}, {"j"});
        QCOMPARE(bob.text(), std::string("A quick jbrown fox"));

        // Bob inserts 'f' at byte 9 (between 'j' and 'b')
        bob.apply_local_edit({{9, 9}}, {"f"});
        QCOMPARE(bob.text(), std::string("A quick jfbrown fox"));
    }

    void sequential_insert_single_chars_at_boundary() {
        // Simpler variant: insert multiple single chars sequentially
        Buffer alice(1);
        auto op = alice.apply_local_edit({{0, 0}}, {"hello"});

        Buffer bob(2);
        bob.apply_ops({op});
        QCOMPARE(bob.text(), std::string("hello"));

        // Bob inserts 'X' at byte 2 (between "he" and "llo")
        bob.apply_local_edit({{2, 2}}, {"X"});
        QCOMPARE(bob.text(), std::string("heXllo"));

        // Bob inserts 'Y' at byte 3 (between 'X' and 'l')
        bob.apply_local_edit({{3, 3}}, {"Y"});
        QCOMPARE(bob.text(), std::string("heXYllo"));

        // Bob inserts 'Z' at byte 4 (between 'Y' and 'l')
        bob.apply_local_edit({{4, 4}}, {"Z"});
        QCOMPARE(bob.text(), std::string("heXYZllo"));
    }
};

QTEST_MAIN(TestBuffer)
#include "tst_buffer.moc"
