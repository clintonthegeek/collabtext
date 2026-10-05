// Installed-header-only behavior checks. No concrete operation payload access.
#include <collabtext/Serialization.h>
#include <collabtext/Operations.h>
#include <collabtext/IdListOperations.h>
#include <collabtext/CrdtEngine.h>
#include <collabtext/IdListEngine.h>
#include <QTest>
#include "TestActor.h"
#include <type_traits>
#include <variant>

using CollabText::Test::actor;
using namespace CollabText::Crdt;

template<class T> concept HasTimestamp = requires(T value) { value.timestamp; };
template<class T> concept HasPayload = requires(T value) { value.ranges; };
static_assert(!HasTimestamp<Operation> && !HasPayload<Operation>);
static_assert(!HasTimestamp<IdListOperation>);
static_assert(!std::is_aggregate_v<Operation>);
static_assert(!std::is_aggregate_v<IdListOperation>);
static_assert(!std::is_convertible_v<Operation, IdListOperation>);

class TestSerializationPublic : public QObject {
    Q_OBJECT
private:
    static Operation text_op(uint16_t actor_number, const std::string& text, bool undo = false) {
        CollabText::CrdtEngine engine(CollabText::Test::test_counter(actor_number));
        Operation captured;
        engine.setOnLocalOp([&](const Operation& op) { captured = op; });
        engine.insert(0, text);
        if (undo) engine.undo();
        return captured;  // survives callback and engine destruction
    }
private slots:
    void identity_carries_an_actor_and_a_counter() {
        Identity id(actor(3), 42);
        QCOMPARE(id.counter, uint64_t(42));
        QCOMPARE(id.actor, actor(3));
    }
    void op_lamport_for_edit_operation() {
        const auto op = text_op(1, "hello");
        QCOMPARE(op_identity(op), Identity(actor(1), 6));
    }
    void op_lamport_for_undo_operation() {
        const auto op = text_op(2, "hello", true);
        QCOMPARE(op_identity(op), Identity(actor(2), 7));
    }
    void idlist_op_lamport_for_insert() {
        CollabText::IdListEngine engine(CollabText::Test::test_counter(5));
        auto op = engine.try_insert_after(Anchor::min(), 99);
        QVERIFY(op.ok());
        QCOMPARE(idlist_op_identity(op.value()), Identity(actor(5), 1));
    }
    void edit_operation_roundtrip() {
        const auto op = text_op(1, "hello");
        const auto wire = encode_operation(op);
        auto decoded = decode_operation(wire);
        QVERIFY(decoded.has_value());
        QCOMPARE(encode_operation(*decoded), wire);
        QCOMPARE(op_identity(*decoded), op_identity(op));
        CollabText::CrdtEngine receiver(CollabText::Test::test_counter(9));
        QVERIFY(receiver.applyRemoteOp(*decoded));
        QCOMPARE(receiver.text(), std::string("hello"));
    }
    void undo_operation_roundtrip() {
        CollabText::CrdtEngine sender(CollabText::Test::test_counter(1));
        CollabText::CrdtEngine receiver(CollabText::Test::test_counter(9));
        Operation captured;
        sender.setOnLocalOp([&](const Operation& op) { captured = op; });
        sender.insert(0, "hello");
        QVERIFY(receiver.applyRemoteOp(captured));
        QVERIFY(sender.undo());
        const auto wire = encode_operation(captured);
        auto decoded = decode_operation(wire);
        QVERIFY(decoded.has_value());
        QCOMPARE(encode_operation(*decoded), wire);
        QCOMPARE(op_identity(*decoded), op_identity(captured));
        QVERIFY(receiver.applyRemoteOp(*decoded));
        QVERIFY(receiver.text().empty());
    }
    void idlist_insert_roundtrip() {
        CollabText::IdListEngine sender(CollabText::Test::test_counter(1));
        CollabText::IdListEngine receiver(CollabText::Test::test_counter(9));
        const uint64_t value = 0xDEADBEEFCAFE0001ULL;
        auto op = sender.try_insert_after(Anchor::min(), value);
        QVERIFY(op.ok());
        const auto wire = encode_idlist_operation(op.value());
        auto decoded = decode_idlist_operation(wire);
        QVERIFY(decoded.has_value());
        QCOMPARE(encode_idlist_operation(*decoded), wire);
        QCOMPARE(idlist_op_identity(*decoded), idlist_op_identity(op.value()));
        QCOMPARE(receiver.apply_ops({*decoded})[0].kind(), CollabText::ApplyOutcomeKind::applied);
        QCOMPARE(receiver.ids(), std::vector<uint64_t>{value});
    }
    void idlist_remove_roundtrip() {
        CollabText::IdListEngine sender(CollabText::Test::test_counter(1));
        CollabText::IdListEngine receiver(CollabText::Test::test_counter(9));
        auto inserted = sender.try_insert_after(Anchor::min(), 42);
        QVERIFY(inserted.ok());
        QCOMPARE(receiver.apply_ops({inserted.value()})[0].kind(), CollabText::ApplyOutcomeKind::applied);
        auto anchor = sender.try_anchor_of(42);
        QVERIFY(anchor.ok());
        auto removed = sender.try_remove_at(anchor.value());
        QVERIFY(removed.ok());
        const auto wire = encode_idlist_operation(removed.value());
        auto decoded = decode_idlist_operation(wire);
        QVERIFY(decoded.has_value());
        QCOMPARE(encode_idlist_operation(*decoded), wire);
        QCOMPARE(receiver.apply_ops({*decoded})[0].kind(), CollabText::ApplyOutcomeKind::applied);
        QVERIFY(receiver.ids().empty());
    }
    void idlist_undo_roundtrip() {
        CollabText::IdListEngine sender(CollabText::Test::test_counter(1));
        CollabText::IdListEngine receiver(CollabText::Test::test_counter(9));
        auto inserted = sender.try_insert_after(Anchor::min(), 42);
        QVERIFY(inserted.ok());
        QCOMPARE(receiver.apply_ops({inserted.value()})[0].kind(), CollabText::ApplyOutcomeKind::applied);
        auto undone = sender.undo();
        QVERIFY(undone.has_value());
        const auto wire = encode_idlist_operation(*undone);
        auto decoded = decode_idlist_operation(wire);
        QVERIFY(decoded.has_value());
        QCOMPARE(encode_idlist_operation(*decoded), wire);
        QCOMPARE(receiver.apply_ops({*decoded})[0].kind(), CollabText::ApplyOutcomeKind::applied);
        QVERIFY(receiver.ids().empty());
    }
    void decode_operation_nullopt_on_invalid() {
        for (auto bytes : {"", "not json", "{}", "{\"t\":\"x\"}"})
            QVERIFY(!decode_operation(bytes).has_value());
    }
    void decode_idlist_operation_nullopt_on_invalid() {
        for (auto bytes : {"", "not json", "{}", "{\"t\":\"unknown\"}"})
            QVERIFY(!decode_idlist_operation(bytes).has_value());
    }
};
QTEST_GUILESS_MAIN(TestSerializationPublic)
#include "tst_serialization_public.moc"

template<class T> concept VariantPayload = requires { std::variant_size<T>::value; };
static_assert(!VariantPayload<CollabText::Crdt::Operation>);
static_assert(!VariantPayload<CollabText::Crdt::IdListOperation>);
