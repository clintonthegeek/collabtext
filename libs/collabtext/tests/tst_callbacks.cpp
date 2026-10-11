#include <collabtext/CrdtEngine.h>

#include <QTest>

#include "TestActor.h"

#include <optional>
#include <vector>

using namespace CollabText;

class TestCallbacks : public QObject {
  Q_OBJECT
private slots:
  void remoteBatchNotifiesOnceOnlyWhenVisibleStateChanges() {
    CrdtEngine source(Test::test_counter(141));
    CrdtEngine target(Test::test_counter(142));
    std::vector<Crdt::Operation> operations;
    source.setOnLocalOp([&](const Crdt::Operation &operation) {
      operations.push_back(operation);
    });
    QVERIFY(source.try_insert(0, "a").ok());
    QVERIFY(source.try_insert(1, "b").ok());
    QCOMPARE(operations.size(), size_t(2));

    size_t notifications = 0;
    target.setOnChange([&] { ++notifications; });
    const auto mixed = target.apply_ops({operations[0], operations[1]});
    QCOMPARE(mixed.size(), size_t(2));
    QCOMPARE(mixed[0].kind(), ApplyOutcomeKind::applied);
    QCOMPARE(mixed[1].kind(), ApplyOutcomeKind::applied);
    QCOMPARE(target.text(), std::string("ab"));
    QCOMPARE(notifications, size_t(1));

    const auto no_change = target.apply_ops({operations[0], Crdt::Operation{}});
    QCOMPARE(no_change.size(), size_t(2));
    QCOMPARE(no_change[0].kind(), ApplyOutcomeKind::duplicate);
    QCOMPARE(no_change[1].kind(), ApplyOutcomeKind::rejected);
    QCOMPARE(notifications, size_t(1));
  }

  void callbackMutationIsRejectedAndCallbackSnapshotIsStable() {
    CrdtEngine source(Test::test_counter(143));
    CrdtEngine target(Test::test_counter(144));
    std::vector<Crdt::Operation> operations;
    source.setOnLocalOp([&](const Crdt::Operation &operation) {
      operations.push_back(operation);
    });
    QVERIFY(source.try_insert(0, "ab").ok());
    QVERIFY(source.try_insert(2, "c").ok());

    std::optional<ErrorCode> nested_error;
    size_t notifications = 0;
    target.setOnChange([&] {
      ++notifications;
      auto nested = target.try_replace(0, 0, "invalid-nested-edit");
      QVERIFY(!nested.ok());
      nested_error = nested.error().code();
      target.setOnChange({});
    });
    const auto applied = target.apply_ops({operations[0]});
    QCOMPARE(applied[0].kind(), ApplyOutcomeKind::applied);
    QCOMPARE(nested_error, std::optional(ErrorCode::reentrant_mutation));
    QCOMPARE(target.text(), std::string("ab"));
    QCOMPARE(notifications, size_t(1));

    // Clearing the callback from within itself affects later events, not the
    // callback object currently being invoked.
    const auto later = target.apply_remote(operations[1]);
    QCOMPARE(later.kind(), ApplyOutcomeKind::applied);
    QCOMPARE(target.text(), std::string("abc"));
    QCOMPARE(notifications, size_t(1));

    std::optional<ErrorCode> local_nested_error;
    target.setOnLocalOp([&](const Crdt::Operation &) {
      auto nested = target.try_remove(0, 1);
      QVERIFY(!nested.ok());
      local_nested_error = nested.error().code();
    });
    auto outer = target.try_insert(3, "d");
    QVERIFY(outer.ok());
    QCOMPARE(local_nested_error,
             std::optional(ErrorCode::reentrant_mutation));
    QCOMPARE(target.text(), std::string("abcd"));
    QCOMPARE(to_string(ErrorCode::reentrant_mutation),
             std::string_view("reentrant_mutation"));
  }
};

QTEST_MAIN(TestCallbacks)
#include "tst_callbacks.moc"
