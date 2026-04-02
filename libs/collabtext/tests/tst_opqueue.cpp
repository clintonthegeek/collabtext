#include <QTest>
#include "crdt/Buffer.h"
#include "crdt/OperationQueue.h"
#include <algorithm>
#include <chrono>
#include <random>

using namespace CollabText::Crdt;

class TestOpQueue : public QObject {
    Q_OBJECT
private slots:

    void empty_queue_noop() {
        Buffer buf(1);
        buf.apply_ops({});
        QCOMPARE(buf.text(), std::string(""));
        QCOMPARE(buf.visible_length(), 0u);
    }

    void single_deferred_retries() {
        // Replica 1 inserts "hello", then deletes "ell".
        // Replica 2 receives delete first (deferred), then insert.
        Buffer bufA(1);
        auto op1 = bufA.apply_local_edit({{0, 0}}, {"hello"});
        auto op2 = bufA.apply_local_edit({{1, 4}}, {""});

        Buffer bufB(2);
        // op2 depends on op1 — should be deferred
        bufB.apply_ops({op2});
        QCOMPARE(bufB.text(), std::string(""));

        // Now deliver op1 — op2 should retry and succeed
        bufB.apply_ops({op1});
        QCOMPARE(bufB.text(), std::string("ho"));
    }

    void deferred_replica_tracking() {
        // Replica 1 creates 3 ops in sequence: insert "abc", insert "d" at end,
        // delete "a". Send in reverse order to replica 2.
        Buffer bufA(1);
        auto op1 = bufA.apply_local_edit({{0, 0}}, {"abc"});
        auto op2 = bufA.apply_local_edit({{3, 3}}, {"d"});
        auto op3 = bufA.apply_local_edit({{0, 1}}, {""});

        Buffer bufB(2);
        // Send in reverse: op3, op2, op1
        bufB.apply_ops({op3});
        QCOMPARE(bufB.text(), std::string(""));

        bufB.apply_ops({op2});
        QCOMPARE(bufB.text(), std::string(""));

        // Deliver op1 — all should now apply
        bufB.apply_ops({op1});
        QCOMPARE(bufB.text(), std::string("bcd"));
    }

    void mixed_replicas_partial_delivery() {
        Buffer bufA(1);
        Buffer bufB(2);
        Buffer bufC(3);

        auto opA = bufA.apply_local_edit({{0, 0}}, {"aa"});
        auto opB = bufB.apply_local_edit({{0, 0}}, {"bb"});
        auto opC = bufC.apply_local_edit({{0, 0}}, {"cc"});

        // Cross-deliver all ops
        bufA.apply_ops({opB, opC});
        bufB.apply_ops({opA, opC});
        bufC.apply_ops({opA, opB});

        // All three replicas must converge
        QCOMPARE(bufA.text(), bufB.text());
        QCOMPARE(bufB.text(), bufC.text());

        // All 6 characters must be present (interleaving may split pairs)
        std::string result = bufA.text();
        QCOMPARE(result.size(), size_t(6));
        QCOMPARE(std::count(result.begin(), result.end(), 'a'), std::ptrdiff_t(2));
        QCOMPARE(std::count(result.begin(), result.end(), 'b'), std::ptrdiff_t(2));
        QCOMPARE(std::count(result.begin(), result.end(), 'c'), std::ptrdiff_t(2));
    }

    void stress_reverse_causal_order() {
        Buffer bufA(1);
        std::vector<Operation> ops;
        for (int i = 0; i < 100; ++i) {
            uint32_t pos = bufA.visible_length();
            auto op = bufA.apply_local_edit({{pos, pos}}, {std::string(1, 'a' + (i % 26))});
            ops.push_back(op);
        }

        // Reverse the ops
        std::reverse(ops.begin(), ops.end());

        Buffer bufB(2);
        auto start = std::chrono::steady_clock::now();

        // Deliver one at a time in reverse order
        for (auto& op : ops) {
            bufB.apply_ops({op});
        }

        auto end = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        // Verify correctness
        QCOMPARE(bufB.text(), bufA.text());

        // Sanity timing check
        qDebug() << "100 reverse-order ops applied in" << ms << "ms";
        QVERIFY2(ms < 5000, "Reverse causal order took too long — possible quadratic regression");
    }

    void convergence_still_passes() {
        std::mt19937 rng(42);  // Deterministic seed

        Buffer bufA(1), bufB(2), bufC(3);
        std::vector<Operation> pendingB, pendingC;

        for (int i = 0; i < 30; ++i) {
            uint32_t len = bufA.visible_length();
            uint32_t start = len > 0 ? (rng() % (len + 1)) : 0;
            uint32_t end = start + (len > start ? (rng() % (len - start + 1)) : 0);
            int ins_len = rng() % 4;
            std::string text;
            for (int j = 0; j < ins_len; ++j)
                text += static_cast<char>('a' + (rng() % 26));

            auto op = bufA.apply_local_edit({{start, end}}, {text});
            pendingB.push_back(op);
            pendingC.push_back(op);
        }

        // Deliver to B in random order
        std::shuffle(pendingB.begin(), pendingB.end(), rng);
        bufB.apply_ops(pendingB);

        // Deliver to C in reverse order
        std::reverse(pendingC.begin(), pendingC.end());
        bufC.apply_ops(pendingC);

        // Flush deferred
        for (int i = 0; i < 10; ++i) {
            bufB.apply_ops({});
            bufC.apply_ops({});
        }

        QCOMPARE(bufA.text(), bufB.text());
        QCOMPARE(bufA.text(), bufC.text());
    }
};

QTEST_MAIN(TestOpQueue)
#include "tst_opqueue.moc"
