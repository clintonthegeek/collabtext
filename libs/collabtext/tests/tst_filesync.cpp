#include <QTest>
#include <QTemporaryDir>
#include "crdt/FileSync.h"

using namespace CollabText::Crdt;
namespace fs = std::filesystem;

class TestFileSync : public QObject {
    Q_OBJECT

private slots:
    void start_creates_directory_structure() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        fs::path shared = tmp.path().toStdString();

        Buffer buf(1);
        FileSync sync(buf, shared, "laptop-1");
        sync.start();

        QVERIFY(fs::exists(shared / "replicas" / "laptop-1" / "ops"));
        QVERIFY(fs::exists(shared / "meta"));
        QVERIFY(fs::exists(shared / "local" / "laptop-1"));
        QVERIFY(fs::exists(shared / ".stignore"));
    }

    void local_op_written_to_bucket_file() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();

        Buffer buf(1);
        FileSync sync(buf, shared, "replica-1");
        sync.start();

        auto op = buf.apply_local_edit({{0, 0}}, {"hello"});
        sync.push_local_op(op);
        sync.poll();

        // Check that at least one bucket file exists with content
        auto ops_dir = shared / "replicas" / "replica-1" / "ops";
        bool found_ops = false;
        for (auto& entry : fs::directory_iterator(ops_dir)) {
            if (fs::file_size(entry.path()) > 0) {
                found_ops = true;
                break;
            }
        }
        QVERIFY(found_ops);

        // sequences.json should exist and have entries
        auto seq_path = shared / "replicas" / "replica-1" / "sequences.json";
        QVERIFY(fs::exists(seq_path));
        QVERIFY(fs::file_size(seq_path) > 2);  // More than just "{}"
    }

    void two_replicas_sync_via_shared_folder() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();

        Buffer bufA(1), bufB(2);
        FileSync syncA(bufA, shared, "replica-A");
        FileSync syncB(bufB, shared, "replica-B");
        syncA.start();
        syncB.start();

        // A types "hello"
        auto opA = bufA.apply_local_edit({{0, 0}}, {"hello"});
        syncA.push_local_op(opA);
        syncA.poll();  // flush A's ops to disk

        // B polls and picks up A's ops
        size_t applied = syncB.poll();
        QVERIFY(applied > 0);
        QCOMPARE(bufB.text(), std::string("hello"));
    }

    void bidirectional_sync() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();

        Buffer bufA(1), bufB(2);
        FileSync syncA(bufA, shared, "replica-A");
        FileSync syncB(bufB, shared, "replica-B");
        syncA.start();
        syncB.start();

        // A types "hello"
        auto op1 = bufA.apply_local_edit({{0, 0}}, {"hello"});
        syncA.push_local_op(op1);
        syncA.poll();

        // B picks up, types " world"
        syncB.poll();
        QCOMPARE(bufB.text(), std::string("hello"));

        auto op2 = bufB.apply_local_edit({{5, 5}}, {" world"});
        syncB.push_local_op(op2);
        syncB.poll();

        // A picks up B's edit
        syncA.poll();
        QCOMPARE(bufA.text(), std::string("hello world"));
        QCOMPARE(bufB.text(), std::string("hello world"));
    }

    void concurrent_edits_converge() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();

        Buffer bufA(1), bufB(2);
        FileSync syncA(bufA, shared, "replica-A");
        FileSync syncB(bufB, shared, "replica-B");
        syncA.start();
        syncB.start();

        // Set up initial text on both
        auto op0 = bufA.apply_local_edit({{0, 0}}, {"hello"});
        syncA.push_local_op(op0);
        syncA.poll();
        syncB.poll();

        // Both edit concurrently (before seeing each other's edits)
        auto opA = bufA.apply_local_edit({{5, 5}}, {"!"});
        syncA.push_local_op(opA);

        auto opB = bufB.apply_local_edit({{5, 5}}, {"?"});
        syncB.push_local_op(opB);

        // Flush both
        syncA.poll();
        syncB.poll();

        // Exchange
        syncA.poll();
        syncB.poll();

        // Must converge
        QCOMPARE(bufA.text(), bufB.text());
        // Both characters present
        std::string text = bufA.text();
        QVERIFY(text.find('!') != std::string::npos);
        QVERIFY(text.find('?') != std::string::npos);
    }

    void undo_syncs_across_replicas() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();

        Buffer bufA(1), bufB(2);
        FileSync syncA(bufA, shared, "replica-A");
        FileSync syncB(bufB, shared, "replica-B");
        syncA.start();
        syncB.start();

        // A types "hello"
        auto op1 = bufA.apply_local_edit({{0, 0}}, {"hello"});
        syncA.push_local_op(op1);
        syncA.poll();
        syncB.poll();

        // A undoes
        auto undo_op = bufA.undo();
        QVERIFY(undo_op.has_value());
        syncA.push_local_op(*undo_op);
        syncA.poll();
        QCOMPARE(bufA.text(), std::string(""));

        // B receives undo
        syncB.poll();
        QCOMPARE(bufB.text(), std::string(""));
    }

    void multiple_ops_per_poll() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();

        Buffer bufA(1), bufB(2);
        FileSync syncA(bufA, shared, "replica-A");
        FileSync syncB(bufB, shared, "replica-B");
        syncA.start();
        syncB.start();

        // A makes 10 edits before flushing
        for (int i = 0; i < 10; ++i) {
            std::string ch(1, 'a' + i);
            uint32_t pos = bufA.visible_length();
            auto op = bufA.apply_local_edit({{pos, pos}}, {ch});
            syncA.push_local_op(op);
        }
        syncA.poll();

        // B gets all 10 in one poll
        size_t applied = syncB.poll();
        QCOMPARE(applied, size_t(10));
        QCOMPARE(bufB.text(), bufA.text());
    }

    void poll_is_idempotent() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();

        Buffer bufA(1), bufB(2);
        FileSync syncA(bufA, shared, "replica-A");
        FileSync syncB(bufB, shared, "replica-B");
        syncA.start();
        syncB.start();

        auto op = bufA.apply_local_edit({{0, 0}}, {"hello"});
        syncA.push_local_op(op);
        syncA.poll();

        syncB.poll();
        QCOMPARE(bufB.text(), std::string("hello"));

        // Polling again should not re-apply
        size_t applied = syncB.poll();
        QCOMPARE(applied, size_t(0));
        QCOMPARE(bufB.text(), std::string("hello"));
    }

    void callback_invoked_on_remote_ops() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();

        Buffer bufA(1), bufB(2);
        FileSync syncA(bufA, shared, "replica-A");
        FileSync syncB(bufB, shared, "replica-B");
        syncA.start();
        syncB.start();

        size_t callback_count = 0;
        syncB.set_on_remote_ops([&](size_t n) { callback_count += n; });

        auto op = bufA.apply_local_edit({{0, 0}}, {"hello"});
        syncA.push_local_op(op);
        syncA.poll();
        syncB.poll();

        QCOMPARE(callback_count, size_t(1));
    }

    void three_replicas_converge() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();

        Buffer bufA(1), bufB(2), bufC(3);
        FileSync syncA(bufA, shared, "replica-A");
        FileSync syncB(bufB, shared, "replica-B");
        FileSync syncC(bufC, shared, "replica-C");
        syncA.start(); syncB.start(); syncC.start();

        // A creates initial text
        auto op0 = bufA.apply_local_edit({{0, 0}}, {"base"});
        syncA.push_local_op(op0);
        syncA.poll();
        syncB.poll();
        syncC.poll();

        // All three edit concurrently
        auto opA = bufA.apply_local_edit({{4, 4}}, {"A"});
        syncA.push_local_op(opA);

        auto opB = bufB.apply_local_edit({{4, 4}}, {"B"});
        syncB.push_local_op(opB);

        auto opC = bufC.apply_local_edit({{4, 4}}, {"C"});
        syncC.push_local_op(opC);

        // Flush all
        syncA.poll(); syncB.poll(); syncC.poll();

        // Multiple rounds of polling until convergence
        for (int i = 0; i < 5; ++i) {
            syncA.poll(); syncB.poll(); syncC.poll();
        }

        QCOMPARE(bufA.text(), bufB.text());
        QCOMPARE(bufB.text(), bufC.text());

        std::string text = bufA.text();
        QVERIFY(text.find('A') != std::string::npos);
        QVERIFY(text.find('B') != std::string::npos);
        QVERIFY(text.find('C') != std::string::npos);
    }
};

QTEST_MAIN(TestFileSync)
#include "tst_filesync.moc"
