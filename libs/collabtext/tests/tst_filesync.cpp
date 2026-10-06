#include <QTest>
#include <QTemporaryDir>
#include "crdt/FileSync.h"
#include "crdt/Serialization.h"
#include "CodecTest.h"
#include "TestActor.h"

using CollabText::Test::actor;

using namespace CollabText::Crdt;
namespace fs = std::filesystem;

namespace {
size_t count_dir_entries(const fs::path& dir) {
    if (!fs::exists(dir)) return 0;
    size_t n = 0;
    for (auto& e : fs::directory_iterator(dir)) { (void)e; ++n; }
    return n;
}
}

class TestFileSync : public QObject {
    Q_OBJECT
private slots:
    void failed_decode_retains_batch_and_blocks_later_records() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        Buffer source(CollabText::Test::test_counter(1)), target(CollabText::Test::test_counter(2));
        FileSync receiver(target, shared, "receiver");
        receiver.start();
        SegmentWriter writer(shared / "replicas" / "sender" / "log" / "ops", SegmentKind::Ops);
        writer.start();
        const std::string unsupported = "CTBO0002:{}";
        auto first = source.apply_local_edit({{0, 0}}, {"a"});
        writer.append(checked_codec(encode_operation(first)), op_identity(first).counter);
        writer.append(unsupported, 3);
        writer.flush();
        QCOMPARE(receiver.poll(), size_t(0));
        QVERIFY(target.text().empty());
        QVERIFY(receiver.last_error().has_value());
        QCOMPARE(receiver.last_error()->code(), CollabText::ErrorCode::unsupported_version);
        QCOMPARE(receiver.blocked_records().at("sender").size(), size_t(2));
        QCOMPARE(receiver.blocked_records().at("sender")[1], unsupported);
        auto later = source.apply_local_edit({{1, 1}}, {"b"});
        writer.append(checked_codec(encode_operation(later)), op_identity(later).counter);
        writer.flush();
        QCOMPARE(receiver.poll(), size_t(0));
        QVERIFY(target.text().empty());
        const auto cursor = shared / "local" / "receiver" / "read-cursors" / "ops" / "sender.bin";
        QVERIFY(!fs::exists(cursor));
    }
    void encoder_refusal_is_reported_to_local_owner() {
        QTemporaryDir tmp;
        Buffer target(CollabText::Test::test_counter(2));
        FileSync sync(target, tmp.path().toStdString(), "receiver");
        sync.start();
        auto result = sync.push_local_op(Operation{});
        QVERIFY(!result.ok());
        QCOMPARE(result.error().code(), CollabText::ErrorCode::invalid_identity);
        QVERIFY(sync.last_error().has_value());
    }
    void start_creates_directory_structure() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        Buffer buf(CollabText::Test::test_counter(1));
        FileSync sync(buf, shared, "laptop-1");
        sync.start();
        QVERIFY(fs::exists(shared / "replicas" / "laptop-1" / "log" / "ops"));
        QVERIFY(fs::exists(shared / "local" / "laptop-1"));
        QVERIFY(fs::exists(shared / ".stignore"));
    }

    void no_sequences_json_in_synced_root() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        Buffer buf(CollabText::Test::test_counter(1));
        FileSync sync(buf, shared, "r");
        sync.start();
        auto op = buf.apply_local_edit({{0, 0}}, {"hello"});
        QVERIFY(sync.push_local_op(op).ok());
        sync.poll();
        sync.flush();
        QVERIFY(!fs::exists(shared / "replicas" / "r" / "sequences.json"));
        QVERIFY(!fs::exists(shared / "replicas" / "r" / "ops"));
    }

    void local_op_lands_in_open_or_sealed_under_log_ops() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        Buffer buf(CollabText::Test::test_counter(1));
        FileSync sync(buf, shared, "r");
        sync.start();
        auto op = buf.apply_local_edit({{0, 0}}, {"hello"});
        QVERIFY(sync.push_local_op(op).ok());
        sync.poll();
        sync.flush();
        auto log_ops = shared / "replicas" / "r" / "log" / "ops";
        bool any_record = false;
        for (auto& e : fs::directory_iterator(log_ops)) {
            std::string name = e.path().filename().string();
            if ((name.ends_with(".open") || name.ends_with(".seg.zst"))
                && fs::file_size(e.path()) > 0)
                any_record = true;
        }
        QVERIFY(any_record);
    }

    void two_replicas_sync_via_shared_folder() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        Buffer bufA(CollabText::Test::test_counter(1)), bufB(CollabText::Test::test_counter(2));
        FileSync syncA(bufA, shared, "A");
        FileSync syncB(bufB, shared, "B");
        syncA.start(); syncB.start();
        auto opA = bufA.apply_local_edit({{0, 0}}, {"hello"});
        QVERIFY(syncA.push_local_op(opA).ok());
        syncA.poll(); syncA.flush();
        size_t applied = syncB.poll();
        QVERIFY(applied > 0);
        QCOMPARE(bufB.text(), std::string("hello"));
    }

    void bidirectional_sync() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        Buffer bufA(CollabText::Test::test_counter(1)), bufB(CollabText::Test::test_counter(2));
        FileSync syncA(bufA, shared, "A");
        FileSync syncB(bufB, shared, "B");
        syncA.start(); syncB.start();

        auto op1 = bufA.apply_local_edit({{0, 0}}, {"hello"});
        QVERIFY(syncA.push_local_op(op1).ok());
        syncA.poll(); syncA.flush();

        syncB.poll();
        QCOMPARE(bufB.text(), std::string("hello"));

        auto op2 = bufB.apply_local_edit({{5, 5}}, {" world"});
        QVERIFY(syncB.push_local_op(op2).ok());
        syncB.poll(); syncB.flush();

        syncA.poll();
        QCOMPARE(bufA.text(), std::string("hello world"));
    }

    void file_count_budget_for_small_session() {
        QTemporaryDir tmp;
        fs::path shared = tmp.path().toStdString();
        Buffer buf(CollabText::Test::test_counter(1));
        FileSync sync(buf, shared, "r");
        sync.start();
        std::string text = "the quick brown fox";
        for (size_t i = 0; i < text.size(); ++i) {
            auto op = buf.apply_local_edit({{i, i}}, {std::string(1, text[i])});
            QVERIFY(sync.push_local_op(op).ok());
            sync.poll();
        }
        sync.flush();
        size_t total_files = count_dir_entries(
            shared / "replicas" / "r" / "log" / "ops");
        QVERIFY2(total_files <= 2,
            qPrintable(QString("expected ≤ 2 files, got %1").arg(total_files)));
    }
};

QTEST_APPLESS_MAIN(TestFileSync)
#include "tst_filesync.moc"
