/// Public OpStream interface compilation and basic value semantics.
#include <collabtext/OpStream.h>

#include <QTest>

#include <type_traits>

static_assert(std::is_abstract_v<CollabText::OpStream>);
static_assert(std::is_copy_constructible_v<CollabText::DeliveryToken>);

class InterfaceStub final : public CollabText::OpStream {
public:
    CollabText::Result<void> register_stream(const CollabText::StreamScope&) override {
        return CollabText::Error(CollabText::ErrorCode::unknown_stream, "stub");
    }
    CollabText::Result<void> push(const CollabText::OutboundDelivery&,
                                  CollabText::PushDurability) override {
        return CollabText::Error(CollabText::ErrorCode::unknown_stream, "stub");
    }
    CollabText::Result<CollabText::SubscriptionId> subscribe(
        const CollabText::StreamScope&, CollabText::InboundHandler) override {
        return CollabText::Error(CollabText::ErrorCode::unknown_stream, "stub");
    }
    CollabText::Result<CollabText::SubscriptionId> subscribe_receipts(
        CollabText::ReceiptHandler) override {
        return CollabText::Error(CollabText::ErrorCode::unknown_stream, "stub");
    }
    CollabText::Result<void> unsubscribe(CollabText::SubscriptionId) override {
        return CollabText::Result<void>::success();
    }
    CollabText::Result<CollabText::PollReport> poll(
        const CollabText::StreamScope&, CollabText::PollBudget) override {
        return CollabText::Error(CollabText::ErrorCode::unknown_stream, "stub");
    }
    CollabText::Result<std::vector<CollabText::StreamReceipt>> receipts(
        const CollabText::StreamScope&) const override {
        return std::vector<CollabText::StreamReceipt>{};
    }
};

class TestOpStreamInterface : public QObject {
    Q_OBJECT
private slots:
    void interface_is_subclassable_through_base_pointer() {
        InterfaceStub implementation;
        CollabText::OpStream* stream = &implementation;
        QVERIFY(stream != nullptr);
        const CollabText::StreamScope scope{"text", 1};
        QVERIFY(!stream->register_stream(scope).ok());
        QVERIFY(stream->receipts(scope).ok());
    }

    void recovery_points_are_explicit_and_validated() {
        const auto replay = CollabText::RecoveryPoint::replay_from_beginning();
        QVERIFY(replay.valid());
        QCOMPARE(replay.kind(), CollabText::RecoveryPoint::Kind::replay_from_beginning);
        const auto checkpoint = CollabText::RecoveryPoint::checkpoint("checkpoint-7");
        QVERIFY(checkpoint.valid());
        QCOMPARE(checkpoint.checkpoint_id(), std::string_view("checkpoint-7"));
        QVERIFY(!CollabText::RecoveryPoint::checkpoint("").valid());
    }
};

QTEST_GUILESS_MAIN(TestOpStreamInterface)
#include "tst_opstream_interface.moc"
