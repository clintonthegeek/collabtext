#include "CodecTest.h"
/// tst_crdtengine_op_api.cpp — Tests for CrdtEngine::setOnLocalOp / typed apply_remote
///
/// Proves that:
///   - setOnLocalOp fires on every local edit (insert, remove)
///   - setOnLocalOp fires on undo and redo
///   - encode + decode round-trip preserves the op
///   - applyRemoteOp converges two engines
///   - undo on engine 1 + applyRemoteOp on engine 2 keeps both in sync
///   - two-way convergence: both engines insert, both apply each other's ops

#include <collabtext/CrdtEngine.h>

#include <collabtext/Operations.h>
#include <collabtext/Serialization.h>

#include <QTest>
#include <optional>
#include <string>
#include <vector>
#include "TestActor.h"

using CollabText::Test::actor;

using namespace CollabText;
using namespace CollabText::Crdt;

class TestCrdtEngineOpApi : public QObject {
    Q_OBJECT

private slots:

    void apply_outcomes_and_undo_limits_are_typed() {
        CrdtEngine source(CollabText::Test::test_counter(21));
        CrdtEngine target(CollabText::Test::test_counter(22));
        std::vector<Operation> emitted;
        source.setOnLocalOp([&](const Operation &op) { emitted.push_back(op); });
        int changes = 0;
        target.setOnChange([&] { ++changes; });
        source.insert(0, "a");
        source.insert(1, "b");
        QCOMPARE(emitted.size(), size_t(2));

        const auto applied = target.apply_remote(emitted[0]);
        QCOMPARE(applied.kind(), ApplyOutcomeKind::applied);
        QCOMPARE(target.text(), std::string("a"));
        QCOMPARE(changes, 1);
        QCOMPARE(target.apply_remote(emitted[0]).kind(), ApplyOutcomeKind::duplicate);
        QCOMPARE(changes, 1); // a duplicate is not a visible change

        CrdtEngine delayed(CollabText::Test::test_counter(24));
        const auto deferred = delayed.apply_remote(emitted[1]);
        QCOMPARE(deferred.kind(), ApplyOutcomeKind::deferred);
        QVERIFY(deferred.awaiting().has_value());
        QCOMPARE(delayed.text(), std::string());
        QCOMPARE(changes, 1);

        CrdtEngine bounded_deferred(CollabText::Test::test_counter(26));
        QVERIFY(bounded_deferred.set_deferred_operation_budget({1, 0}).ok());
        const auto byte_limited = bounded_deferred.apply_remote(emitted[1]);
        QCOMPARE(byte_limited.kind(), ApplyOutcomeKind::rejected);
        QVERIFY(byte_limited.error().has_value());
        QCOMPARE(byte_limited.error()->code(), ErrorCode::deferred_queue_full);
        QVERIFY(bounded_deferred.set_deferred_operation_budget({0, 1u << 20}).ok());
        const auto count_limited = bounded_deferred.apply_remote(emitted[1]);
        QCOMPARE(count_limited.kind(), ApplyOutcomeKind::rejected);
        QVERIFY(count_limited.error().has_value());
        QCOMPARE(count_limited.error()->code(), ErrorCode::deferred_queue_full);
        QCOMPARE(bounded_deferred.deferred_operation_count(), size_t(0));
        QCOMPARE(bounded_deferred.deferred_operation_bytes(), size_t(0));

        QVERIFY(bounded_deferred.set_deferred_operation_budget({1, 1u << 20}).ok());
        QCOMPARE(bounded_deferred.apply_remote(emitted[1]).kind(), ApplyOutcomeKind::deferred);
        QCOMPARE(bounded_deferred.deferred_operation_count(), size_t(1));
        QVERIFY(bounded_deferred.deferred_operation_bytes() > 0);
        auto would_evict = bounded_deferred.set_deferred_operation_budget({0, 0});
        QVERIFY(!would_evict.ok());
        QCOMPARE(would_evict.error().code(), ErrorCode::limit_exceeded);
        QCOMPARE(bounded_deferred.deferred_operation_budget().max_operations, size_t(1));
        QCOMPARE(bounded_deferred.apply_remote(emitted[0]).kind(), ApplyOutcomeKind::applied);
        QCOMPARE(bounded_deferred.text(), std::string("ab"));
        QCOMPARE(bounded_deferred.deferred_operation_count(), size_t(0));
        QCOMPARE(bounded_deferred.deferred_operation_bytes(), size_t(0));

        const auto rejected = target.apply_remote(Operation{});
        QCOMPARE(rejected.kind(), ApplyOutcomeKind::rejected);
        QVERIFY(rejected.error().has_value());
        QCOMPARE(changes, 1);

        const auto batch = target.apply_ops({emitted[0], Operation{}});
        QCOMPARE(batch.size(), size_t(2));
        QCOMPARE(batch[0].kind(), ApplyOutcomeKind::duplicate);
        QCOMPARE(batch[1].kind(), ApplyOutcomeKind::rejected);
        QCOMPARE(changes, 1);

        CrdtEngine bounded(CollabText::Test::test_counter(23));
        QVERIFY(bounded.set_max_undo_depth(0).ok());
        QCOMPARE(bounded.max_undo_depth(), size_t(0));
        bounded.insert(0, "x");
        auto unavailable = bounded.try_undo();
        QVERIFY(unavailable.ok());
        QVERIFY(!unavailable.value().has_value());

        CrdtEngine history(CollabText::Test::test_counter(25));
        history.insert(0, "x");
        QCOMPARE(history.undo_depth(), size_t(1));
        QVERIFY(history.undo_history_bytes() > 0);
        auto undo = history.try_undo();
        QVERIFY(undo.ok());
        QVERIFY(undo.value().has_value());
        QVERIFY(history.text().empty());
        auto redo = history.try_redo();
        QVERIFY(redo.ok());
        QVERIFY(redo.value().has_value());
        QCOMPARE(history.text(), std::string("x"));
        QVERIFY(history.set_max_undo_bytes(0).ok());
        QCOMPARE(history.max_undo_bytes(), size_t(0));
        QCOMPARE(history.undo_depth(), size_t(0));
    }

    // ── callback fires on insert ──────────────────────────────────────────────

    void setOnLocalOp_fires_on_insert() {
        CrdtEngine engine(CollabText::Test::test_counter(1));
        std::optional<Operation> captured;
        engine.setOnLocalOp([&](const Operation &op) {
            captured = op;
        });

        engine.insert(0, "hello");

        QVERIFY(captured.has_value());
        // The captured op should have a non-zero Identity counter.
        QVERIFY(op_identity(*captured).counter > 0);
    }

    // ── callback fires on remove ──────────────────────────────────────────────

    void setOnLocalOp_fires_on_remove() {
        CrdtEngine engine(CollabText::Test::test_counter(1));
        std::optional<Operation> captured;
        engine.setOnLocalOp([&](const Operation &op) {
            captured = op;
        });

        engine.insert(0, "hello");
        captured.reset();

        engine.remove(1, 3); // remove "ell"

        QVERIFY(captured.has_value());
    }

    // ── callback fires on undo ────────────────────────────────────────────────

    void setOnLocalOp_fires_on_undo() {
        CrdtEngine engine(CollabText::Test::test_counter(1));
        engine.insert(0, "hello");

        std::optional<Operation> captured;
        engine.setOnLocalOp([&](const Operation &op) {
            captured = op;
        });

        bool did_undo = engine.undo();
        QVERIFY(did_undo);
        QVERIFY(captured.has_value());
    }

    // ── callback fires on redo ────────────────────────────────────────────────

    void setOnLocalOp_fires_on_redo() {
        CrdtEngine engine(CollabText::Test::test_counter(1));
        engine.insert(0, "hello");
        engine.undo();

        std::optional<Operation> captured;
        engine.setOnLocalOp([&](const Operation &op) {
            captured = op;
        });

        bool did_redo = engine.redo();
        QVERIFY(did_redo);
        QVERIFY(captured.has_value());
    }

    // ── one-way convergence: engine 1 inserts, engine 2 applies ──────────────

    void apply_remote_converges_insert() {
        CrdtEngine engine1(CollabText::Test::test_counter(1));
        CrdtEngine engine2(CollabText::Test::test_counter(2));

        std::vector<std::string> wire_ops;
        engine1.setOnLocalOp([&](const Operation &op) {
            wire_ops.push_back(checked_codec(encode_operation(op)));
        });

        engine1.insert(0, "hello");

        QVERIFY(!wire_ops.empty());
        for (const auto &wire : wire_ops) {
            auto decoded = decode_operation(wire);
            QVERIFY(decoded.ok());
            bool ok = CollabText::Test::apply_succeeded(engine2.apply_remote(decoded.value()));
            QVERIFY(ok);
        }

        QCOMPARE(engine1.text(), engine2.text());
        QCOMPARE(engine2.text(), std::string("hello"));
    }

    // ── encode/decode round-trip preserves Identity identity ──────────────────

    void encode_decode_preserves_lamport() {
        CrdtEngine engine(CollabText::Test::test_counter(1));
        std::optional<Operation> captured;
        engine.setOnLocalOp([&](const Operation &op) {
            if (!captured) captured = op;
        });

        engine.insert(0, "abc");
        QVERIFY(captured.has_value());

        std::string wire = checked_codec(encode_operation(*captured));
        auto decoded = decode_operation(wire);
        QVERIFY(decoded.ok());

        QCOMPARE(op_identity(decoded.value()).counter, op_identity(*captured).counter);
        QCOMPARE(op_identity(decoded.value()).actor, op_identity(*captured).actor);
    }

    // ── undo round-trip: undo on engine 1 propagated to engine 2 ─────────────

    void apply_remote_undo_roundtrip() {
        CrdtEngine engine1(CollabText::Test::test_counter(1));
        CrdtEngine engine2(CollabText::Test::test_counter(2));

        // Collect all ops engine1 emits (insert + undo)
        std::vector<std::string> wire_ops;
        engine1.setOnLocalOp([&](const Operation &op) {
            wire_ops.push_back(checked_codec(encode_operation(op)));
        });

        engine1.insert(0, "hello");
        engine1.undo();

        // Apply all ops to engine2 in order
        for (const auto &wire : wire_ops) {
            auto decoded = decode_operation(wire);
            QVERIFY(decoded.ok());
            bool ok = CollabText::Test::apply_succeeded(engine2.apply_remote(decoded.value()));
            QVERIFY(ok);
        }

        // Both engines should have empty text after undo propagated
        QCOMPARE(engine1.text(), engine2.text());
    }

    // ── two-way convergence: both engines insert, both apply each other ───────
    //
    // Use non-conflicting positions so both words appear as contiguous substrings.
    // Engine 1 inserts "hello"; engine 2 receives it, then appends " world".
    // This tests round-trip encode/decode through both directions without the
    // interleaving that concurrent same-position inserts produce.

    void two_way_convergence() {
        CrdtEngine engine1(CollabText::Test::test_counter(1));
        CrdtEngine engine2(CollabText::Test::test_counter(2));

        std::vector<std::string> ops_from_1;
        std::vector<std::string> ops_from_2;

        engine1.setOnLocalOp([&](const Operation &op) {
            ops_from_1.push_back(checked_codec(encode_operation(op)));
        });
        engine2.setOnLocalOp([&](const Operation &op) {
            ops_from_2.push_back(checked_codec(encode_operation(op)));
        });

        // Engine 1 inserts "hello"
        engine1.insert(0, "hello");

        // Engine 2 receives engine 1's ops first (no concurrent conflict)
        for (const auto &wire : ops_from_1) {
            auto decoded = decode_operation(wire);
            QVERIFY(decoded.ok());
            QVERIFY(CollabText::Test::apply_succeeded(engine2.apply_remote(decoded.value())));
        }
        ops_from_1.clear();

        // Now engine 2 appends " world" after "hello" — non-conflicting position
        engine2.insert(5, " world");

        // Engine 1 receives engine 2's ops
        for (const auto &wire : ops_from_2) {
            auto decoded = decode_operation(wire);
            QVERIFY(decoded.ok());
            QVERIFY(CollabText::Test::apply_succeeded(engine1.apply_remote(decoded.value())));
        }

        // Both should converge to the same text with both words as substrings
        QCOMPARE(engine1.text(), engine2.text());
        QVERIFY(engine1.text().find("hello") != std::string::npos);
        QVERIFY(engine1.text().find("world") != std::string::npos);
    }

    // ── concurrent convergence: both engines insert at same position ──────────
    //
    // Two concurrent inserts at position 0 produce the same interleaved text on
    // both replicas (CRDT correctness), but the characters are interleaved so we
    // only assert convergence and total length, not substring order.

    void concurrent_insert_convergence() {
        CrdtEngine engine1(CollabText::Test::test_counter(1));
        CrdtEngine engine2(CollabText::Test::test_counter(2));

        std::vector<std::string> ops_from_1;
        std::vector<std::string> ops_from_2;

        engine1.setOnLocalOp([&](const Operation &op) {
            ops_from_1.push_back(checked_codec(encode_operation(op)));
        });
        engine2.setOnLocalOp([&](const Operation &op) {
            ops_from_2.push_back(checked_codec(encode_operation(op)));
        });

        engine1.insert(0, "hello");
        engine2.insert(0, "world");

        for (const auto &wire : ops_from_1) {
            auto decoded = decode_operation(wire);
            QVERIFY(decoded.ok());
            QVERIFY(CollabText::Test::apply_succeeded(engine2.apply_remote(decoded.value())));
        }
        for (const auto &wire : ops_from_2) {
            auto decoded = decode_operation(wire);
            QVERIFY(decoded.ok());
            QVERIFY(CollabText::Test::apply_succeeded(engine1.apply_remote(decoded.value())));
        }

        QCOMPARE(engine1.text(), engine2.text());
        QCOMPARE(static_cast<int>(engine1.text().size()), 10);  // all chars preserved
    }

    // ── sequential multi-op convergence ──────────────────────────────────────

    void sequential_multi_op_convergence() {
        CrdtEngine engine1(CollabText::Test::test_counter(1));
        CrdtEngine engine2(CollabText::Test::test_counter(2));

        std::vector<std::string> wire_ops;
        engine1.setOnLocalOp([&](const Operation &op) {
            wire_ops.push_back(checked_codec(encode_operation(op)));
        });

        engine1.insert(0, "hello");
        engine1.insert(5, " world");
        engine1.remove(5, 1); // remove space

        for (const auto &wire : wire_ops) {
            auto decoded = decode_operation(wire);
            QVERIFY(decoded.ok());
            QVERIFY(CollabText::Test::apply_succeeded(engine2.apply_remote(decoded.value())));
        }

        QCOMPARE(engine1.text(), engine2.text());
        QCOMPARE(engine2.text(), std::string("helloworld"));
    }

    // ── apply_remote returns an applied outcome for a fresh valid op ─────────────

    void apply_remote_returns_applied_for_valid_op() {
        CrdtEngine engine1(CollabText::Test::test_counter(1));
        CrdtEngine engine2(CollabText::Test::test_counter(2));

        std::optional<Operation> captured;
        engine1.setOnLocalOp([&](const Operation &op) {
            if (!captured) captured = op;
        });

        engine1.insert(0, "test");
        QVERIFY(captured.has_value());

        bool result = CollabText::Test::apply_succeeded(engine2.apply_remote(*captured));
        QVERIFY(result);
    }
};

QTEST_GUILESS_MAIN(TestCrdtEngineOpApi)
#include "tst_crdtengine_op_api.moc"
