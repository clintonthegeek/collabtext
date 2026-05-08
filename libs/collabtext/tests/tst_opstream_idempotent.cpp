/// tst_opstream_idempotent.cpp — Idempotent op re-delivery
///
/// Proves that applying the same CRDT operation to a CrdtEngine twice leaves
/// state unchanged (CRDT identity / idempotency guarantee).
///
///   - apply_remote_op_twice_is_idempotent:
///       An encoded+decoded insert op applied twice to engine2 produces "hello"
///       once; the second application is a no-op.
///
///   - undo_op_idempotent:
///       An undo op applied twice after the insert op leaves engine2 with empty
///       text both times.
///
///   - apply_out_of_order_then_reapply_in_order:
///       engine2 receives the undo op before the insert op (causal dependency
///       unmet — Buffer defers internally), then receives the insert op.  Both
///       ops are applied in causal order and converge to the same empty text as
///       engine1 (undo wins).

#include <QTest>
#include <QTemporaryDir>
#include <collabtext/CrdtEngine.h>
#include <collabtext/Operations.h>
#include <collabtext/Serialization.h>
#include <collabtext/StreamSync.h>
#include <collabtext/OpStream.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace CollabText;
using namespace CollabText::Crdt;

class TestOpStreamIdempotent : public QObject {
    Q_OBJECT

private slots:

    // ── apply remote op twice is idempotent ──────────────────────────────────
    //
    // Capture the insert op from engine1, encode and decode it, apply it to
    // engine2 twice.  The second application must be a no-op: engine2.text()
    // remains "hello".

    void apply_remote_op_twice_is_idempotent() {
        CrdtEngine engine1(1);
        CrdtEngine engine2(2);

        std::vector<std::string> wire_ops;
        engine1.setOnLocalOp([&](const Operation& op) {
            wire_ops.push_back(encode_operation(op));
        });

        engine1.insert(0, "hello");
        QVERIFY(!wire_ops.empty());
        QCOMPARE(engine1.text(), std::string("hello"));

        // Apply once
        for (const auto& wire : wire_ops) {
            auto decoded = decode_operation(wire);
            QVERIFY(decoded.has_value());
            engine2.applyRemoteOp(*decoded);
        }
        QCOMPARE(engine2.text(), std::string("hello"));

        // Apply the same ops a second time — must be a no-op
        for (const auto& wire : wire_ops) {
            auto decoded = decode_operation(wire);
            QVERIFY(decoded.has_value());
            engine2.applyRemoteOp(*decoded);
        }
        QCOMPARE(engine2.text(), std::string("hello"));
    }

    // ── undo op idempotent ────────────────────────────────────────────────────
    //
    // engine1 inserts "hello" then undoes. Both ops are applied in order to
    // engine2. Applying the undo op a second time leaves engine2 with empty text.

    void undo_op_idempotent() {
        CrdtEngine engine1(1);
        CrdtEngine engine2(2);

        std::vector<std::string> wire_ops;
        engine1.setOnLocalOp([&](const Operation& op) {
            wire_ops.push_back(encode_operation(op));
        });

        engine1.insert(0, "hello");
        bool did_undo = engine1.undo();
        QVERIFY(did_undo);
        QCOMPARE(engine1.text(), std::string(""));
        QVERIFY(wire_ops.size() >= 2);  // insert op + undo op

        // Apply all ops in order to engine2
        for (const auto& wire : wire_ops) {
            auto decoded = decode_operation(wire);
            QVERIFY(decoded.has_value());
            engine2.applyRemoteOp(*decoded);
        }
        QCOMPARE(engine2.text(), std::string(""));

        // Apply the undo op again — must still be a no-op
        // The undo op is the last op in wire_ops
        const auto& undo_wire = wire_ops.back();
        auto decoded_undo = decode_operation(undo_wire);
        QVERIFY(decoded_undo.has_value());
        engine2.applyRemoteOp(*decoded_undo);
        QCOMPARE(engine2.text(), std::string(""));
    }

    // ── apply out-of-order then in-order ─────────────────────────────────────
    //
    // engine2 receives the undo op before the insert op.  Buffer defers the undo
    // op internally (causal dependency unmet).  When the insert op arrives, Buffer
    // applies both in causal order.  Final state: empty text (undo wins), matching
    // engine1.

    void apply_out_of_order_then_reapply_in_order() {
        CrdtEngine engine1(1);
        CrdtEngine engine2(2);

        std::vector<std::string> wire_ops;
        engine1.setOnLocalOp([&](const Operation& op) {
            wire_ops.push_back(encode_operation(op));
        });

        engine1.insert(0, "hello");
        bool did_undo = engine1.undo();
        QVERIFY(did_undo);
        QCOMPARE(engine1.text(), std::string(""));
        QVERIFY(wire_ops.size() >= 2);

        // Decode all ops
        std::vector<Operation> decoded_ops;
        for (const auto& wire : wire_ops) {
            auto decoded = decode_operation(wire);
            QVERIFY(decoded.has_value());
            decoded_ops.push_back(*decoded);
        }

        // The insert op is at index 0, the undo op is the last one
        const Operation& insert_op = decoded_ops.front();
        const Operation& undo_op = decoded_ops.back();

        // Step 1: apply undo op first (out of order — causal dependency on insert
        // unmet).  Buffer defers it internally.  engine2 text may be empty (no
        // insert applied yet) or may differ from "hello" — it must NOT be "hello".
        engine2.applyRemoteOp(undo_op);
        // We cannot assert exact state here because Buffer defers internally,
        // but we know "hello" has not been inserted yet.
        QVERIFY(engine2.text() != std::string("hello"));

        // Step 2: apply insert op. Buffer now has its causal dependency satisfied,
        // applies insert then the deferred undo — converging to empty.
        engine2.applyRemoteOp(insert_op);

        // Both engines must converge to the same state (empty after undo)
        QCOMPARE(engine2.text(), engine1.text());
    }
};

QTEST_MAIN(TestOpStreamIdempotent)
#include "tst_opstream_idempotent.moc"
