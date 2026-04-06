#include <QTest>
#include "crdt/Buffer.h"
#include "crdt/Serialization.h"

using namespace CollabText::Crdt;

class TestSerialization : public QObject {
    Q_OBJECT

    void verify_roundtrip(const Operation& op) {
        std::string json = encode_operation(op);
        // Must be single-line (no embedded newlines)
        QVERIFY2(json.find('\n') == std::string::npos,
                 "Encoded JSON contains newlines");
        auto decoded = decode_operation(json);
        QVERIFY2(decoded.has_value(), qPrintable(
            QString("Failed to decode: %1").arg(QString::fromStdString(json))));

        // Re-encode and compare (canonical form)
        std::string json2 = encode_operation(*decoded);
        QCOMPARE(json2, json);
    }

private slots:
    void edit_pure_insert() {
        EditOperation op;
        op.timestamp = Lamport(1, 42);
        op.deletion_id = Lamport(1, 41);
        op.version.observe(Lamport(1, 41));
        op.ranges = {{0, 0}};
        op.new_text = {"hello"};

        EditOperation::InsertedFragment frag;
        frag.origin = Lamport(1, 42);
        frag.locator = Locator({1152921504606846976ULL});
        frag.content = "hello";
        frag.length = 5;
        op.inserted_fragments.push_back(frag);

        verify_roundtrip(Operation(op));
    }

    void edit_pure_deletion() {
        EditOperation op;
        op.timestamp = Lamport(2, 100);
        op.deletion_id = Lamport(2, 99);
        op.version.observe(Lamport(1, 50));
        op.version.observe(Lamport(2, 99));
        op.ranges = {{3, 8}};
        op.new_text = {""};

        EditOperation::DeletionRun run;
        run.replica_id = 1;
        run.start_value = 3;
        run.count = 5;
        op.deletion_runs.push_back(run);

        verify_roundtrip(Operation(op));
    }

    void edit_replace_with_split_relocation() {
        EditOperation op;
        op.timestamp = Lamport(1, 50);
        op.deletion_id = Lamport(1, 49);
        op.version.observe(Lamport(1, 49));
        op.ranges = {{3, 3}};
        op.new_text = {"X"};

        EditOperation::InsertedFragment frag;
        frag.origin = Lamport(1, 50);
        frag.locator = Locator({500, 65535});
        frag.content = "X";
        frag.length = 1;
        op.inserted_fragments.push_back(frag);

        EditOperation::SplitRelocation sr;
        sr.fragment_origin = Lamport(1, 4);
        sr.split_offset = 0;
        sr.fragment_length = 3;
        sr.new_locator = Locator({500, 131070});
        op.split_relocations.push_back(sr);

        verify_roundtrip(Operation(op));
    }

    void edit_multi_range() {
        EditOperation op;
        op.timestamp = Lamport(3, 200);
        op.deletion_id = Lamport(3, 199);
        op.version.observe(Lamport(3, 199));
        op.ranges = {{0, 5}, {10, 15}};
        op.new_text = {"AAA", "BBB"};

        EditOperation::DeletionRun run1{1, 0, 5};
        EditOperation::DeletionRun run2{1, 10, 5};
        op.deletion_runs.push_back(run1);
        op.deletion_runs.push_back(run2);

        EditOperation::InsertedFragment f1;
        f1.origin = Lamport(3, 200);
        f1.locator = Locator({100});
        f1.content = "AAA";
        f1.length = 3;
        op.inserted_fragments.push_back(f1);

        EditOperation::InsertedFragment f2;
        f2.origin = Lamport(3, 203);
        f2.locator = Locator({200});
        f2.content = "BBB";
        f2.length = 3;
        op.inserted_fragments.push_back(f2);

        verify_roundtrip(Operation(op));
    }

    void undo_operation() {
        UndoOperation op;
        op.timestamp = Lamport(1, 43);
        op.version.observe(Lamport(1, 42));
        op.counts = {{Lamport(1, 42), 1}};

        verify_roundtrip(Operation(op));
    }

    void undo_multiple_counts() {
        UndoOperation op;
        op.timestamp = Lamport(2, 150);
        op.version.observe(Lamport(1, 100));
        op.version.observe(Lamport(2, 149));
        op.counts = {{Lamport(1, 42), 1}, {Lamport(2, 100), 2}};

        verify_roundtrip(Operation(op));
    }

    void text_with_special_characters() {
        EditOperation op;
        op.timestamp = Lamport(1, 1);
        op.deletion_id = Lamport(1, 0);
        op.ranges = {{0, 0}};
        op.new_text = {"line1\nline2\ttab\"quote\\backslash"};

        EditOperation::InsertedFragment frag;
        frag.origin = Lamport(1, 1);
        frag.locator = Locator({500});
        frag.content = "line1\nline2\ttab\"quote\\backslash";
        frag.length = 30;
        op.inserted_fragments.push_back(frag);

        verify_roundtrip(Operation(op));
    }

    void text_with_unicode() {
        EditOperation op;
        op.timestamp = Lamport(1, 1);
        op.deletion_id = Lamport(1, 0);
        op.ranges = {{0, 0}};
        // Japanese + emoji
        std::string text = "\xe3\x81\x93\xe3\x82\x93\xe3\x81\xab\xe3\x81\xa1\xe3\x81\xaf";  // こんにちは
        op.new_text = {text};

        EditOperation::InsertedFragment frag;
        frag.origin = Lamport(1, 1);
        frag.locator = Locator({500});
        frag.content = text;
        frag.length = 5;
        op.inserted_fragments.push_back(frag);

        verify_roundtrip(Operation(op));
    }

    void empty_edit() {
        EditOperation op;
        op.timestamp = Lamport(1, 1);
        op.deletion_id = Lamport(1, 0);
        // No ranges, no text, no fragments, no deletions
        verify_roundtrip(Operation(op));
    }

    void deep_locator() {
        EditOperation op;
        op.timestamp = Lamport(1, 1);
        op.deletion_id = Lamport(1, 0);
        op.ranges = {{0, 0}};
        op.new_text = {"x"};

        EditOperation::InsertedFragment frag;
        frag.origin = Lamport(1, 1);
        frag.locator = Locator({100, 200, 300, 400, 500});
        frag.content = "x";
        frag.length = 1;
        op.inserted_fragments.push_back(frag);

        verify_roundtrip(Operation(op));
    }

    void global_encode_decode() {
        Global g;
        g.observe(Lamport(0, 10));
        g.observe(Lamport(1, 20));
        g.observe(Lamport(3, 5));

        std::string json = encode_global(g);
        auto decoded = decode_global(json);
        QVERIFY(decoded.has_value());

        std::string json2 = encode_global(*decoded);
        QCOMPARE(json2, json);
    }

    void decode_malformed_returns_nullopt() {
        QVERIFY(!decode_operation("").has_value());
        QVERIFY(!decode_operation("not json").has_value());
        QVERIFY(!decode_operation("{}").has_value());
        QVERIFY(!decode_operation("{\"t\":\"x\"}").has_value());
        QVERIFY(!decode_operation("{\"t\":\"e\"").has_value());  // unclosed
    }

    void real_engine_roundtrip() {
        // Create a real operation from the engine and roundtrip it
        Buffer buf(1);
        auto op = buf.apply_local_edit({{0, 0}}, {"hello world"});

        std::string json = encode_operation(op);
        auto decoded = decode_operation(json);
        QVERIFY(decoded.has_value());

        // Apply the decoded op to a fresh buffer — must produce same text
        Buffer buf2(2);
        buf2.apply_ops({*decoded});
        QCOMPARE(buf2.text(), std::string("hello world"));
    }

    void real_engine_edit_delete_undo_roundtrip() {
        Buffer buf(1);
        auto op1 = buf.apply_local_edit({{0, 0}}, {"hello world"});
        auto op2 = buf.apply_local_edit({{5, 11}}, {""});  // delete " world"
        auto op3 = buf.undo();
        QVERIFY(op3.has_value());

        // Roundtrip all three operations
        std::string j1 = encode_operation(op1);
        std::string j2 = encode_operation(op2);
        std::string j3 = encode_operation(*op3);

        auto d1 = decode_operation(j1);
        auto d2 = decode_operation(j2);
        auto d3 = decode_operation(j3);
        QVERIFY(d1.has_value());
        QVERIFY(d2.has_value());
        QVERIFY(d3.has_value());

        // Apply to fresh buffer — must converge
        Buffer buf2(2);
        buf2.apply_ops({*d1, *d2, *d3});
        QCOMPARE(buf2.text(), buf.text());
    }
};

QTEST_MAIN(TestSerialization)
#include "tst_serialization.moc"
