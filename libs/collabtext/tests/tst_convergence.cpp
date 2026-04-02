#include <QTest>
#include "crdt/Buffer.h"
#include <random>
#include <iostream>

using namespace CollabText::Crdt;

class TestConvergence : public QObject {
    Q_OBJECT

private:
    // Helper: generate a random edit on a buffer
    Operation random_edit(Buffer &buf, std::mt19937 &rng) {
        std::string current = buf.text();
        uint32_t len = buf.visible_length();

        // Random start position (0 to len)
        uint32_t start = len > 0 ? (rng() % (len + 1)) : 0;
        // Random end position (start to len)
        uint32_t end = start + (len > start ? (rng() % (len - start + 1)) : 0);

        // Random new text (0-5 ASCII chars)
        int insert_len = rng() % 6;
        std::string new_text;
        for (int i = 0; i < insert_len; ++i)
            new_text += static_cast<char>('a' + (rng() % 26));

        return buf.apply_local_edit({{start, end}}, {new_text});
    }

    // Core convergence test runner
    void run_convergence(int num_replicas, int num_ops, uint64_t seed) {
        std::mt19937 rng(seed);

        // Create independent replicas
        std::vector<Buffer> replicas;
        for (int i = 0; i < num_replicas; ++i)
            replicas.emplace_back(static_cast<uint16_t>(i + 1)); // replica IDs start at 1

        // Pending operations: (operation, source_replica_id)
        struct PendingOp { Operation op; uint16_t source; };
        std::vector<PendingOp> pending;

        for (int i = 0; i < num_ops; ++i) {
            int action = rng() % 100;

            if (action < 50) {
                // Random edit on random replica
                int r = rng() % num_replicas;
                auto op = random_edit(replicas[r], rng);

                // Broadcast with random duplication (1-3 copies)
                // and random insertion position (out-of-order)
                int copies = 1 + (rng() % 3);
                for (int c = 0; c < copies; ++c) {
                    size_t pos = pending.empty() ? 0 : (rng() % (pending.size() + 1));
                    pending.insert(pending.begin() + static_cast<ptrdiff_t>(pos),
                        PendingOp{op, replicas[r].replica_id()});
                }
            } else if (action < 70) {
                // Random undo/redo
                int r = rng() % num_replicas;
                std::optional<Operation> op;
                if (rng() % 2 == 0)
                    op = replicas[r].undo();
                else
                    op = replicas[r].redo();
                if (op) {
                    int copies = 1 + (rng() % 2);
                    for (int c = 0; c < copies; ++c) {
                        size_t pos = pending.empty() ? 0 : (rng() % (pending.size() + 1));
                        pending.insert(pending.begin() + static_cast<ptrdiff_t>(pos),
                            PendingOp{*op, replicas[r].replica_id()});
                    }
                }
            } else {
                // Deliver some pending ops to a random replica
                if (!pending.empty()) {
                    int r = rng() % num_replicas;
                    int count = 1 + (rng() % std::min<int>(5, static_cast<int>(pending.size())));
                    std::vector<Operation> batch;
                    for (int c = 0; c < count && !pending.empty(); ++c) {
                        if (pending.front().source != replicas[r].replica_id())
                            batch.push_back(pending.front().op);
                        pending.erase(pending.begin());
                    }
                    if (!batch.empty())
                        replicas[r].apply_ops(batch);
                }
            }
        }

        // Drain ALL pending to ALL replicas
        for (const auto &p : pending) {
            for (auto &replica : replicas) {
                if (replica.replica_id() != p.source)
                    replica.apply_ops({p.op});
            }
        }

        // Multiple flush passes for deferred ops
        for (int pass = 0; pass < 10; ++pass) {
            for (auto &replica : replicas)
                replica.apply_ops({});
        }

        // Assert convergence
        std::string expected = replicas[0].text();
        for (int i = 1; i < num_replicas; ++i) {
            if (replicas[i].text() != expected) {
                std::cerr << "CONVERGENCE FAILURE with seed " << seed << "\n";
                std::cerr << "Replica 0 (" << replicas[0].replica_id() << "): \""
                          << expected << "\"\n";
                std::cerr << "Replica " << i << " (" << replicas[i].replica_id()
                          << "): \"" << replicas[i].text() << "\"\n";
            }
            QCOMPARE(replicas[i].text(), expected);
        }
    }

private slots:
    void two_replicas_100_ops() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        run_convergence(2, 100, seed);
    }

    void three_replicas_100_ops() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        run_convergence(3, 100, seed);
    }

    void five_replicas_100_ops() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        run_convergence(5, 100, seed);
    }

    void stress_two_replicas_500_ops() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        run_convergence(2, 500, seed);
    }

    void deterministic_reproducible() {
        // Same seed twice = same result, no crash
        run_convergence(3, 50, 12345);
        run_convergence(3, 50, 12345);
    }
};

QTEST_MAIN(TestConvergence)
#include "tst_convergence.moc"
