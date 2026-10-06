#include "CodecTest.h"
#include <QTest>
#include "crdt/Buffer.h"
#include "crdt/IdList.h"
#include "crdt/OperationPayload.h"
#include "crdt/Serialization.h"
#include "TestActor.h"
#include <type_traits>

using namespace CollabText::Crdt;
using namespace CollabText;

static_assert(std::is_same_v<decltype(operation_get<EditOperation>(std::declval<Operation&>())),
                             const EditOperation&>);
static_assert(std::is_same_v<decltype(operation_get<IdListInsertOp>(std::declval<IdListOperation&>())),
                             const IdListInsertOp&>);

class TestOperationValues : public QObject {
    Q_OBJECT
private slots:
    void text_builder_is_frozen_before_values_are_shared() {
        Buffer source(Test::test_counter(1));
        auto original = source.apply_local_edit({{0, 0}}, {"hello"});
        auto builder = operation_get<EditOperation>(original);
        Operation frozen{builder};
        Operation peer_copy = frozen;
        const auto wire = checked_codec(encode_operation(frozen));
        builder.inserted_fragments[0].content = "BAD";
        builder.timestamp.counter = 999;
        QCOMPARE(checked_codec(encode_operation(frozen)), wire);
        QCOMPARE(checked_codec(encode_operation(peer_copy)), wire);
        Buffer receiver(Test::test_counter(2));
        QCOMPARE(receiver.apply_ops({peer_copy})[0].kind(), ApplyOutcomeKind::applied);
        QCOMPARE(receiver.text(), std::string("hello"));
    }
    void list_builder_is_frozen_before_values_are_shared() {
        IdList source(Test::test_counter(1));
        auto original = source.insert_after(Anchor::min(), 42);
        auto builder = operation_get<IdListInsertOp>(original);
        IdListOperation frozen{builder};
        IdListOperation peer_copy = frozen;
        const auto wire = checked_codec(encode_idlist_operation(frozen));
        builder.id = 777;
        builder.timestamp.counter = 999;
        QCOMPARE(checked_codec(encode_idlist_operation(frozen)), wire);
        QCOMPARE(checked_codec(encode_idlist_operation(peer_copy)), wire);
        IdList receiver(Test::test_counter(2));
        QCOMPARE(receiver.apply_ops({peer_copy})[0].kind(), ApplyOutcomeKind::applied);
        QCOMPARE(receiver.ids(), std::vector<uint64_t>{42});
    }
    void retained_callbacks_survive_engine_and_original_destruction() {
        std::vector<Operation> retained;
        {
            Buffer source(Test::test_counter(1));
            auto edit = source.apply_local_edit({{0, 0}}, {"hello"});
            retained.push_back(edit);
            auto undo = source.undo();
            QVERIFY(undo.has_value());
            retained.push_back(*undo);
        }
        Buffer receiver(Test::test_counter(2));
        auto outcomes = receiver.apply_ops(retained);
        QCOMPARE(outcomes[0].kind(), ApplyOutcomeKind::applied);
        QCOMPARE(outcomes[1].kind(), ApplyOutcomeKind::applied);
        QVERIFY(receiver.text().empty());
    }
    void empty_and_moved_from_values_reject_without_mutation() {
        Buffer source(Test::test_counter(1)), receiver(Test::test_counter(2));
        Operation original = source.apply_local_edit({{0, 0}}, {"a"});
        Operation moved = std::move(original);
        QCOMPARE(receiver.apply_ops({Operation{}, original})[0].kind(), ApplyOutcomeKind::rejected);
        QVERIFY(receiver.text().empty());
        QCOMPARE(receiver.version().pair_count(), size_t(0));
        QCOMPARE(receiver.apply_ops({moved})[0].kind(), ApplyOutcomeKind::applied);
        IdList list_source(Test::test_counter(1)), list_receiver(Test::test_counter(2));
        auto list_original = list_source.insert_after(Anchor::min(), 42);
        auto list_moved = std::move(list_original);
        const auto outcomes = list_receiver.apply_ops({IdListOperation{}, list_original});
        QCOMPARE(outcomes[0].kind(), ApplyOutcomeKind::rejected);
        QCOMPARE(outcomes[1].kind(), ApplyOutcomeKind::rejected);
        QVERIFY(list_receiver.ids().empty());
        QCOMPARE(list_receiver.apply_ops({list_moved})[0].kind(), ApplyOutcomeKind::applied);
    }
    void decoded_values_survive_input_and_decoder_storage() {
        Operation retained;
        {
            Buffer source(Test::test_counter(1));
            auto op = source.apply_local_edit({{0, 0}}, {"\xce\xb1\xf0\x9f\x98\x80"});
            auto wire = checked_codec(encode_operation(op));
            auto decoded = decode_operation(wire);
            QVERIFY(decoded.ok());
            retained = decoded.value();
            wire.assign(wire.size(), '!');
        }
        Buffer receiver(Test::test_counter(2));
        QCOMPARE(receiver.apply_ops({retained})[0].kind(), ApplyOutcomeKind::applied);
        QCOMPARE(receiver.text(), std::string("\xce\xb1\xf0\x9f\x98\x80"));
    }
    void sizes_are_recorded_without_timing_measurements() {
        qInfo("Identity=%zu FragmentSummary=%zu Operation=%zu IdListOperation=%zu", sizeof(Identity),
              sizeof(FragmentSummary), sizeof(Operation), sizeof(IdListOperation));
        QVERIFY(sizeof(Operation) == sizeof(std::shared_ptr<const detail::OperationPayload>));
        QVERIFY(sizeof(IdListOperation) == sizeof(std::shared_ptr<const detail::IdListOperationPayload>));
    }
};
QTEST_GUILESS_MAIN(TestOperationValues)
#include "tst_operation_values.moc"
