/// tst_serialization_public.cpp — public surface round-trip tests
///
/// Proves the public API is self-sufficient: only angle-bracket includes from
/// <collabtext/> are used. No "crdt/…" relative paths. No Qt internals.
/// If this file compiles and all tests pass, Task 1.1 is done.

#include <collabtext/Serialization.h>
#include <collabtext/Operations.h>
#include <collabtext/IdListOperations.h>

#include <QTest>
#include "TestActor.h"

using CollabText::Test::actor;

using namespace CollabText::Crdt;

class TestSerializationPublic : public QObject {
    Q_OBJECT

private slots:
    // ── Identity: actor + counter fields ───────────────────────────────────────
    //
    // The contract used to expose `Lamport::counter()` and a public
    // `Lamport::replica_id`. Both are gone (P06a): a 16-bit replica id is not a
    // persistent identity. What replaces them is pinned here so a future
    // narrowing has to break this test rather than slip through.

    void identity_carries_an_actor_and_a_counter() {
        Identity id(actor(3), /*counter=*/42);
        QCOMPARE(id.counter, uint64_t(42));
        QCOMPARE(id.actor, actor(3));
    }

    // ── op_identity() free function ────────────────────────────────────────────

    void op_lamport_for_edit_operation() {
        EditOperation e;
        e.timestamp = Identity(actor(1), 10);
        Operation op{e};
        Identity ts = op_identity(op);
        QCOMPARE(ts.counter, uint64_t(10));
        QCOMPARE(ts.actor, actor(1));
    }

    void op_lamport_for_undo_operation() {
        UndoOperation u;
        u.timestamp = Identity(actor(2), 20);
        Operation op{u};
        Identity ts = op_identity(op);
        QCOMPARE(ts.counter, uint64_t(20));
        QCOMPARE(ts.actor, actor(2));
    }

    void idlist_op_lamport_for_insert() {
        IdListInsertOp i;
        i.timestamp = Identity(actor(5), 99);
        IdListOperation op{i};
        Identity ts = idlist_op_identity(op);
        QCOMPARE(ts.counter, uint64_t(99));
        QCOMPARE(ts.actor, actor(5));
    }

    // ── encode_operation / decode_operation round-trips ──────────────────────

    void edit_operation_roundtrip() {
        EditOperation e;
        e.timestamp = Identity(actor(1), 42);
        e.deletion_id = Identity(actor(1), 41);
        e.version.observe(Identity(actor(1), 41));
        e.ranges = {{0, 0}};
        e.new_text = {"hello"};

        EditOperation::InsertedFragment frag;
        frag.origin = Identity(actor(1), 42);
        frag.locator = Locator({1152921504606846976ULL});
        frag.content = "hello";
        frag.length = 5;
        e.inserted_fragments.push_back(frag);

        Operation op{e};
        std::string json = encode_operation(op);
        QVERIFY(!json.empty());

        auto decoded = decode_operation(json);
        QVERIFY(decoded.has_value());

        // Re-encode must match
        std::string json2 = encode_operation(*decoded);
        QCOMPARE(json2, json);

        // Identity preserved
        QCOMPARE(op_identity(*decoded).counter, uint64_t(42));
    }

    void undo_operation_roundtrip() {
        UndoOperation u;
        u.timestamp = Identity(actor(1), 43);
        u.version.observe(Identity(actor(1), 42));
        u.counts = {{Identity(actor(1), 42), 1}};

        Operation op{u};
        std::string json = encode_operation(op);
        auto decoded = decode_operation(json);
        QVERIFY(decoded.has_value());
        QCOMPARE(encode_operation(*decoded), json);
        QCOMPARE(op_identity(*decoded).counter, uint64_t(43));
    }

    // ── encode_idlist_operation / decode_idlist_operation round-trips ─────────

    void idlist_insert_roundtrip() {
        IdListInsertOp ins;
        ins.timestamp = Identity(actor(1), 1);
        ins.version.observe(Identity(actor(1), 0));
        ins.id = 0xDEADBEEFCAFE0001ULL;
        ins.locator = Locator({500ULL, 1000ULL});

        IdListOperation op{ins};
        std::string json = encode_idlist_operation(op);
        auto decoded = decode_idlist_operation(json);
        QVERIFY(decoded.has_value());
        QVERIFY(std::holds_alternative<IdListInsertOp>(*decoded));
        QCOMPARE(std::get<IdListInsertOp>(*decoded).id, ins.id);
        QCOMPARE(encode_idlist_operation(*decoded), json);
        QCOMPARE(idlist_op_identity(*decoded).counter, uint64_t(1));
    }

    void idlist_remove_roundtrip() {
        IdListRemoveOp rem;
        rem.timestamp = Identity(actor(2), 5);
        rem.version.observe(Identity(actor(1), 1));
        rem.target_origin = Identity(actor(1), 1);

        IdListOperation op{rem};
        std::string json = encode_idlist_operation(op);
        auto decoded = decode_idlist_operation(json);
        QVERIFY(decoded.has_value());
        QVERIFY(std::holds_alternative<IdListRemoveOp>(*decoded));
        QCOMPARE(encode_idlist_operation(*decoded), json);
    }

    void idlist_undo_roundtrip() {
        IdListUndoOpVariant undo;
        undo.timestamp = Identity(actor(1), 10);
        undo.version.observe(Identity(actor(1), 9));
        undo.counts = {{Identity(actor(1), 1), 1}};

        IdListOperation op{undo};
        std::string json = encode_idlist_operation(op);
        auto decoded = decode_idlist_operation(json);
        QVERIFY(decoded.has_value());
        QVERIFY(std::holds_alternative<IdListUndoOpVariant>(*decoded));
        QCOMPARE(encode_idlist_operation(*decoded), json);
    }

    // ── decode returns nullopt on bad input ───────────────────────────────────

    void decode_operation_nullopt_on_invalid() {
        QVERIFY(!decode_operation("").has_value());
        QVERIFY(!decode_operation("not json").has_value());
        QVERIFY(!decode_operation("{}").has_value());
        QVERIFY(!decode_operation("{\"t\":\"x\"}").has_value());
    }

    void decode_idlist_operation_nullopt_on_invalid() {
        QVERIFY(!decode_idlist_operation("").has_value());
        QVERIFY(!decode_idlist_operation("not json").has_value());
        QVERIFY(!decode_idlist_operation("{}").has_value());
        QVERIFY(!decode_idlist_operation(R"({"t":"unknown"})").has_value());
    }
};

QTEST_GUILESS_MAIN(TestSerializationPublic)
#include "tst_serialization_public.moc"
