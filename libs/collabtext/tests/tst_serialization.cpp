#include "CodecTest.h"
#include <QTest>
#include "crdt/Buffer.h"
#include "crdt/Serialization.h"
#include "TestActor.h"

using CollabText::Test::actor;

using namespace CollabText::Crdt;

class TestSerialization : public QObject {
    Q_OBJECT

    void verify_roundtrip(const Operation& op) {
        std::string json = checked_codec(encode_operation(op));
        // Must be single-line (no embedded newlines)
        QVERIFY2(json.find('\n') == std::string::npos,
                 "Encoded JSON contains newlines");
        auto decoded = decode_operation(json);
        QVERIFY2(decoded.ok(), qPrintable(
            QString("Failed to decode: %1").arg(QString::fromStdString(json))));

        // Re-encode and compare (canonical form)
        std::string json2 = checked_codec(encode_operation(decoded.value()));
        QCOMPARE(json2, json);
    }

private slots:
    void oversized_owned_value_is_refused_without_mutation() {
        EditOperation edit;
        const uint32_t n = (4u << 20) + 1;
        edit.timestamp = {actor(1), uint64_t(n) + 1};
        edit.deletion_id = edit.timestamp;
        edit.ranges = {{0, 0}};
        edit.new_text = {std::string(n, 'a')};
        edit.inserted_fragments = {{{actor(1), 1}, Link{}, edit.new_text[0], n}};
        Operation value(edit);
        auto encoded = encode_operation(value);
        QVERIFY(!encoded.ok());
        QCOMPARE(encoded.error().code(), CollabText::ErrorCode::malformed);
        QCOMPARE(op_identity(value), edit.timestamp);
    }
    void edit_pure_insert() {
        EditOperation op;
        op.timestamp = Identity(actor(1), 42);
        op.deletion_id = op.timestamp;
        op.version.observe(Identity(actor(1), 36));
        op.ranges = {{0, 0}};
        op.new_text = {"hello"};

        EditOperation::InsertedFragment frag;
        frag.origin = Identity(actor(1), 37);
        frag.link = Link{Identity(actor(1), 36), Side::Right, std::nullopt};
        frag.content = "hello";
        frag.length = 5;
        op.inserted_fragments.push_back(frag);

        verify_roundtrip(Operation(op));
    }

    void edit_pure_deletion() {
        EditOperation op;
        op.timestamp = Identity(actor(2), 100);
        op.deletion_id = op.timestamp;
        op.version.observe(Identity(actor(1), 50));
        op.version.observe(Identity(actor(2), 99));
        op.ranges = {{3, 8}};
        op.new_text = {""};

        EditOperation::DeletionRun run;
        run.first = Identity(actor(1), 3);
        run.count = 5;
        op.deletion_runs.push_back(run);

        verify_roundtrip(Operation(op));
    }

    // A left child: its parent is its right origin, and it carries both.
    void edit_insert_as_left_child() {
        EditOperation op;
        op.timestamp = Identity(actor(1), 50);
        op.deletion_id = op.timestamp;
        op.version.observe(Identity(actor(1), 48));
        op.ranges = {{3, 3}};
        op.new_text = {"X"};

        EditOperation::InsertedFragment frag;
        frag.origin = Identity(actor(1), 49);
        frag.link = Link{Identity(actor(1), 4), Side::Left, Identity(actor(1), 4)};
        frag.content = "X";
        frag.length = 1;
        op.inserted_fragments.push_back(frag);

        verify_roundtrip(Operation(op));
    }

    // The document start is the root: no parent. The end is no right origin.
    void edit_insert_at_document_start() {
        EditOperation op;
        op.timestamp = Identity(actor(1), 3);
        op.deletion_id = op.timestamp;
        op.ranges = {{0, 0}};
        op.new_text = {"hi"};
        EditOperation::InsertedFragment frag;
        frag.origin = Identity(actor(1), 1);
        frag.link = Link{std::nullopt, Side::Right, std::nullopt};
        frag.content = "hi";
        frag.length = 2;
        op.inserted_fragments.push_back(frag);
        verify_roundtrip(Operation(op));
    }

    void edit_multi_range() {
        EditOperation op;
        op.timestamp = Identity(actor(3), 200);
        op.deletion_id = op.timestamp;
        op.version.observe(Identity(actor(3), 193));
        op.ranges = {{0, 5}, {10, 15}};
        op.new_text = {"AAA", "BBB"};

        EditOperation::DeletionRun run1{Identity(actor(1), 1), 5};
        EditOperation::DeletionRun run2{Identity(actor(1), 10), 5};
        op.deletion_runs.push_back(run1);
        op.deletion_runs.push_back(run2);

        EditOperation::InsertedFragment f1;
        f1.origin = Identity(actor(3), 194);
        f1.link = Link{Identity(actor(3), 192), Side::Right, std::nullopt};
        f1.content = "AAA";
        f1.length = 3;
        op.inserted_fragments.push_back(f1);

        EditOperation::InsertedFragment f2;
        f2.origin = Identity(actor(3), 197);
        f2.link = Link{Identity(actor(3), 192), Side::Right, Identity(actor(3), 192)};
        f2.content = "BBB";
        f2.length = 3;
        op.inserted_fragments.push_back(f2);

        verify_roundtrip(Operation(op));
    }

    void undo_operation() {
        UndoOperation op;
        op.timestamp = Identity(actor(1), 43);
        op.version.observe(Identity(actor(1), 42));
        op.counts = {{Identity(actor(1), 42), 1}};

        verify_roundtrip(Operation(op));
    }

    void undo_multiple_counts() {
        UndoOperation op;
        op.timestamp = Identity(actor(2), 150);
        op.version.observe(Identity(actor(1), 100));
        op.version.observe(Identity(actor(2), 149));
        op.counts = {{Identity(actor(1), 42), 1}, {Identity(actor(2), 100), 2}};

        verify_roundtrip(Operation(op));
    }

    void text_with_special_characters() {
        EditOperation op;
        op.timestamp = Identity(actor(1), 32);
        op.deletion_id = op.timestamp;
        op.ranges = {{0, 0}};
        op.new_text = {"line1\nline2\ttab\"quote\\backslash"};

        EditOperation::InsertedFragment frag;
        frag.origin = Identity(actor(1), 1);
        frag.link = Link{};
        frag.content = "line1\nline2\ttab\"quote\\backslash";
        frag.length = 31;
        op.inserted_fragments.push_back(frag);

        verify_roundtrip(Operation(op));
    }

    void text_with_unicode() {
        EditOperation op;
        op.timestamp = Identity(actor(1), 6);
        op.deletion_id = op.timestamp;
        op.ranges = {{0, 0}};
        // Japanese + emoji
        std::string text = "\xe3\x81\x93\xe3\x82\x93\xe3\x81\xab\xe3\x81\xa1\xe3\x81\xaf";  // こんにちは
        op.new_text = {text};

        EditOperation::InsertedFragment frag;
        frag.origin = Identity(actor(1), 1);
        frag.link = Link{};
        frag.content = text;
        frag.length = 5;
        op.inserted_fragments.push_back(frag);

        verify_roundtrip(Operation(op));
    }

    void empty_edit() {
        EditOperation op;
        op.timestamp = Identity(actor(1), 1);
        op.deletion_id = op.timestamp;
        // No ranges, no text, no fragments, no deletions
        verify_roundtrip(Operation(op));
    }

    void link_to_a_distant_parent_and_origin() {
        EditOperation op;
        op.timestamp = Identity(actor(1), 2);
        op.deletion_id = op.timestamp;
        op.ranges = {{0, 0}};
        op.new_text = {"x"};

        EditOperation::InsertedFragment frag;
        frag.origin = Identity(actor(1), 1);
        op.version.observe(Identity(actor(2), 0xFFFFFFFFFFFFFFF0ULL));
        op.version.observe(Identity(actor(3), 12345));
        frag.link = Link{Identity(actor(2), 0xFFFFFFFFFFFFFFF0ULL), Side::Right, Identity(actor(3), 12345)};
        frag.content = "x";
        frag.length = 1;
        op.inserted_fragments.push_back(frag);

        verify_roundtrip(Operation(op));
    }

    void global_encode_decode() {
        Global g;
        g.observe(Identity(actor(1), 10));
        g.observe(Identity(actor(2), 20));
        g.observe(Identity(actor(4), 5));

        std::string json = checked_codec(encode_global(g));
        auto decoded = decode_global(json);
        QVERIFY(decoded.ok());

        std::string json2 = checked_codec(encode_global(decoded.value()));
        QCOMPARE(json2, json);
        QCOMPARE(decoded.value().get(actor(4)), quint64(5));
    }

    void global_with_an_all_zero_actor_key_is_rejected() {
        // There is no actor 0 (P02). A record whose version vector names one is
        // malformed, and silently dropping the component would understate
        // coverage — which is exactly what a watermark and a GC certificate are
        // computed from.
        const std::string zero = "{\"00000000000000000000000000000000\":10}";
        QVERIFY(!decode_global(zero).ok());
    }

    void decode_malformed_returns_nullopt() {
        QVERIFY(!decode_operation("").ok());
        QVERIFY(!decode_operation("not json").ok());
        QVERIFY(!decode_operation("{}").ok());
        QVERIFY(!decode_operation("{\"t\":\"x\"}").ok());
        QVERIFY(!decode_operation("{\"t\":\"e\"").ok());  // unclosed
    }

    void real_engine_roundtrip() {
        // Create a real operation from the engine and roundtrip it
        Buffer buf(CollabText::Test::test_counter(1));
        auto op = buf.apply_local_edit({{0, 0}}, {"hello world"});

        std::string json = checked_codec(encode_operation(op));
        auto decoded = decode_operation(json);
        QVERIFY(decoded.ok());

        // Apply the decoded op to a fresh buffer — must produce same text
        Buffer buf2(CollabText::Test::test_counter(2));
        buf2.apply_ops({decoded.value()});
        QCOMPARE(buf2.text(), std::string("hello world"));
    }

    void real_engine_edit_delete_undo_roundtrip() {
        Buffer buf(CollabText::Test::test_counter(1));
        auto op1 = buf.apply_local_edit({{0, 0}}, {"hello world"});
        auto op2 = buf.apply_local_edit({{5, 11}}, {""});  // delete " world"
        auto op3 = buf.undo();
        QVERIFY(op3.has_value());

        // Roundtrip all three operations
        std::string j1 = checked_codec(encode_operation(op1));
        std::string j2 = checked_codec(encode_operation(op2));
        std::string j3 = checked_codec(encode_operation(*op3));

        auto d1 = decode_operation(j1);
        auto d2 = decode_operation(j2);
        auto d3 = decode_operation(j3);
        QVERIFY(d1.ok());
        QVERIFY(d2.ok());
        QVERIFY(d3.ok());

        // Apply to fresh buffer — must converge
        Buffer buf2(CollabText::Test::test_counter(2));
        buf2.apply_ops({d1.value(), d2.value(), d3.value()});
        QCOMPARE(buf2.text(), buf.text());
    }
};

QTEST_MAIN(TestSerialization)
#include "tst_serialization.moc"
