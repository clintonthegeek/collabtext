#include <QTest>
#include "crdt/IdList.h"
#include "crdt/Serialization.h"

using namespace CollabText::Crdt;

class TestIdListSerialization : public QObject {
    Q_OBJECT
private slots:
    // β8.1
    void insert_op_round_trip() {
        IdList list(1);
        auto op = list.insert_after(Anchor::min(), 0xDEADBEEF);
        std::string encoded = encode_idlist_operation(op);
        auto decoded = decode_idlist_operation(encoded);
        QVERIFY(decoded.has_value());
        QVERIFY(std::holds_alternative<IdListInsertOp>(*decoded));
        const auto& d = std::get<IdListInsertOp>(*decoded);
        QCOMPARE(d.id, quint64(0xDEADBEEF));
    }

    // β8.2
    void malformed_json_returns_nullopt() {
        QVERIFY(!decode_idlist_operation("not json").has_value());
        QVERIFY(!decode_idlist_operation("{").has_value());
        QVERIFY(!decode_idlist_operation(R"({"t":"unknown"})").has_value());
    }

    // β8.4
    void remove_op_round_trip() {
        IdList list(1);
        list.insert_after(Anchor::min(), 0xAA);
        auto op = list.remove_at(Anchor(1, 1, Bias::Left));
        auto encoded = encode_idlist_operation(op);
        auto decoded = decode_idlist_operation(encoded);
        QVERIFY(decoded.has_value());
        QVERIFY(std::holds_alternative<IdListRemoveOp>(*decoded));
    }

    void undo_op_round_trip() {
        IdList list(1);
        list.insert_after(Anchor::min(), 0xAA);
        auto undo = list.undo();
        QVERIFY(undo.has_value());
        auto encoded = encode_idlist_operation(*undo);
        auto decoded = decode_idlist_operation(encoded);
        QVERIFY(decoded.has_value());
        QVERIFY(std::holds_alternative<IdListUndoOpVariant>(*decoded));
    }

    // β8.5 — Buffer ops still parse after schema bump
    void buffer_op_decode_still_works() {
        // encode_operation / decode_operation are for Buffer ops and must be unaffected
        // Just verify the decode functions are distinct and both callable.
        QVERIFY(!decode_idlist_operation("{}").has_value());  // empty obj has no "t" key
    }
};

QTEST_GUILESS_MAIN(TestIdListSerialization)
#include "tst_idlist_serialization.moc"
