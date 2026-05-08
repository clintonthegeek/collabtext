/// tst_crdtengine_op_api.cpp — Tests for CrdtEngine::setOnLocalOp / applyRemoteOp
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

using namespace CollabText;
using namespace CollabText::Crdt;

class TestCrdtEngineOpApi : public QObject {
    Q_OBJECT

private slots:

    // ── callback fires on insert ──────────────────────────────────────────────

    void setOnLocalOp_fires_on_insert() {
        CrdtEngine engine(1);
        std::optional<Operation> captured;
        engine.setOnLocalOp([&](const Operation &op) {
            captured = op;
        });

        engine.insert(0, "hello");

        QVERIFY(captured.has_value());
        // The captured op should have a non-zero Lamport counter.
        QVERIFY(op_lamport(*captured).counter() > 0);
    }

    // ── callback fires on remove ──────────────────────────────────────────────

    void setOnLocalOp_fires_on_remove() {
        CrdtEngine engine(1);
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
        CrdtEngine engine(1);
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
        CrdtEngine engine(1);
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

    void applyRemoteOp_converges_insert() {
        CrdtEngine engine1(1);
        CrdtEngine engine2(2);

        std::vector<std::string> wire_ops;
        engine1.setOnLocalOp([&](const Operation &op) {
            wire_ops.push_back(encode_operation(op));
        });

        engine1.insert(0, "hello");

        QVERIFY(!wire_ops.empty());
        for (const auto &wire : wire_ops) {
            auto decoded = decode_operation(wire);
            QVERIFY(decoded.has_value());
            bool ok = engine2.applyRemoteOp(*decoded);
            QVERIFY(ok);
        }

        QCOMPARE(engine1.text(), engine2.text());
        QCOMPARE(engine2.text(), std::string("hello"));
    }

    // ── encode/decode round-trip preserves Lamport identity ──────────────────

    void encode_decode_preserves_lamport() {
        CrdtEngine engine(1);
        std::optional<Operation> captured;
        engine.setOnLocalOp([&](const Operation &op) {
            if (!captured) captured = op;
        });

        engine.insert(0, "abc");
        QVERIFY(captured.has_value());

        std::string wire = encode_operation(*captured);
        auto decoded = decode_operation(wire);
        QVERIFY(decoded.has_value());

        QCOMPARE(op_lamport(*decoded).counter(), op_lamport(*captured).counter());
        QCOMPARE(op_lamport(*decoded).replica_id, op_lamport(*captured).replica_id);
    }

    // ── undo round-trip: undo on engine 1 propagated to engine 2 ─────────────

    void applyRemoteOp_undo_roundtrip() {
        CrdtEngine engine1(1);
        CrdtEngine engine2(2);

        // Collect all ops engine1 emits (insert + undo)
        std::vector<std::string> wire_ops;
        engine1.setOnLocalOp([&](const Operation &op) {
            wire_ops.push_back(encode_operation(op));
        });

        engine1.insert(0, "hello");
        engine1.undo();

        // Apply all ops to engine2 in order
        for (const auto &wire : wire_ops) {
            auto decoded = decode_operation(wire);
            QVERIFY(decoded.has_value());
            bool ok = engine2.applyRemoteOp(*decoded);
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
        CrdtEngine engine1(1);
        CrdtEngine engine2(2);

        std::vector<std::string> ops_from_1;
        std::vector<std::string> ops_from_2;

        engine1.setOnLocalOp([&](const Operation &op) {
            ops_from_1.push_back(encode_operation(op));
        });
        engine2.setOnLocalOp([&](const Operation &op) {
            ops_from_2.push_back(encode_operation(op));
        });

        // Engine 1 inserts "hello"
        engine1.insert(0, "hello");

        // Engine 2 receives engine 1's ops first (no concurrent conflict)
        for (const auto &wire : ops_from_1) {
            auto decoded = decode_operation(wire);
            QVERIFY(decoded.has_value());
            QVERIFY(engine2.applyRemoteOp(*decoded));
        }
        ops_from_1.clear();

        // Now engine 2 appends " world" after "hello" — non-conflicting position
        engine2.insert(5, " world");

        // Engine 1 receives engine 2's ops
        for (const auto &wire : ops_from_2) {
            auto decoded = decode_operation(wire);
            QVERIFY(decoded.has_value());
            QVERIFY(engine1.applyRemoteOp(*decoded));
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
        CrdtEngine engine1(1);
        CrdtEngine engine2(2);

        std::vector<std::string> ops_from_1;
        std::vector<std::string> ops_from_2;

        engine1.setOnLocalOp([&](const Operation &op) {
            ops_from_1.push_back(encode_operation(op));
        });
        engine2.setOnLocalOp([&](const Operation &op) {
            ops_from_2.push_back(encode_operation(op));
        });

        engine1.insert(0, "hello");
        engine2.insert(0, "world");

        for (const auto &wire : ops_from_1) {
            auto decoded = decode_operation(wire);
            QVERIFY(decoded.has_value());
            QVERIFY(engine2.applyRemoteOp(*decoded));
        }
        for (const auto &wire : ops_from_2) {
            auto decoded = decode_operation(wire);
            QVERIFY(decoded.has_value());
            QVERIFY(engine1.applyRemoteOp(*decoded));
        }

        QCOMPARE(engine1.text(), engine2.text());
        QCOMPARE(static_cast<int>(engine1.text().size()), 10);  // all chars preserved
    }

    // ── sequential multi-op convergence ──────────────────────────────────────

    void sequential_multi_op_convergence() {
        CrdtEngine engine1(1);
        CrdtEngine engine2(2);

        std::vector<std::string> wire_ops;
        engine1.setOnLocalOp([&](const Operation &op) {
            wire_ops.push_back(encode_operation(op));
        });

        engine1.insert(0, "hello");
        engine1.insert(5, " world");
        engine1.remove(5, 1); // remove space

        for (const auto &wire : wire_ops) {
            auto decoded = decode_operation(wire);
            QVERIFY(decoded.has_value());
            QVERIFY(engine2.applyRemoteOp(*decoded));
        }

        QCOMPARE(engine1.text(), engine2.text());
        QCOMPARE(engine2.text(), std::string("helloworld"));
    }

    // ── applyRemoteOp returns true for well-formed in-domain ops ─────────────

    void applyRemoteOp_returns_true_for_valid_op() {
        CrdtEngine engine1(1);
        CrdtEngine engine2(2);

        std::optional<Operation> captured;
        engine1.setOnLocalOp([&](const Operation &op) {
            if (!captured) captured = op;
        });

        engine1.insert(0, "test");
        QVERIFY(captured.has_value());

        bool result = engine2.applyRemoteOp(*captured);
        QVERIFY(result);
    }
};

QTEST_GUILESS_MAIN(TestCrdtEngineOpApi)
#include "tst_crdtengine_op_api.moc"
