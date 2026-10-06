#include "CodecTest.h"
#include <QTemporaryDir>
#include <QTest>

#include "transport/StreamSync.h"
#include "crdt/Buffer.h"
#include "crdt/IdList.h"
#include "crdt/SegmentReader.h"
#include "crdt/SegmentWriter.h"
#include "crdt/Serialization.h"

#include <filesystem>
#include "TestActor.h"

using CollabText::Test::actor;

using namespace CollabText::Crdt;
namespace fs = std::filesystem;

// Each marker covers only a confirmed behavioral assertion, never setup.
// Strict mode reproduces outstanding failures instead of accepting XFAILs.
#define EVALUATION_EXPECT_FAIL(row, reason) \
    do { \
        if (!qEnvironmentVariableIsSet("COLLABTEXT_STRICT_EVALUATION")) \
            QEXPECT_FAIL(row, reason, Continue); \
    } while (false)

static void primitive_rows() {
    QTest::addColumn<bool>("idlist");
    QTest::newRow("buffer") << false;
    QTest::newRow("idlist") << true;
}

class TestEvaluationRegressions : public QObject {
    Q_OBJECT
private slots:
    void ordinary_remote_undo_redo_data() { primitive_rows(); }
    void ordinary_remote_undo_redo() {
        QFETCH(bool, idlist);
        if (idlist) {
            IdList a(CollabText::Test::test_counter(1)), b(CollabText::Test::test_counter(2));
            b.apply_ops({a.insert_after(Anchor::min(), 42)});
            QCOMPARE(b.ids(), std::vector<uint64_t>{42});
            auto undo = a.undo();
            QVERIFY(undo.has_value());
            b.apply_ops({*undo});
            QVERIFY(a.ids().empty());
            QVERIFY(b.ids().empty());
            auto redo = a.redo();
            QVERIFY(redo.has_value());
            QVERIFY(get_idlist_op_identity(*undo) < get_idlist_op_identity(*redo));
            b.apply_ops({*redo});
            QCOMPARE(a.ids(), std::vector<uint64_t>{42});
            QCOMPARE(b.ids(), a.ids());
        } else {
            Buffer a(CollabText::Test::test_counter(1)), b(CollabText::Test::test_counter(2));
            b.apply_ops({a.apply_local_edit({{0, 0}}, {"abc"})});
            QCOMPARE(b.text(), std::string("abc"));
            auto undo = a.undo();
            QVERIFY(undo.has_value());
            b.apply_ops({*undo});
            QCOMPARE(a.text(), std::string());
            QCOMPARE(b.text(), std::string());
            auto redo = a.redo();
            QVERIFY(redo.has_value());
            QVERIFY(get_op_timestamp(*undo) < get_op_timestamp(*redo));
            b.apply_ops({*redo});
            QCOMPARE(a.text(), std::string("abc"));
            QCOMPARE(b.text(), a.text());
        }
    }

    void f1_local_gc_preserves_insertion_redo_data() { primitive_rows(); }
    void f1_local_gc_preserves_insertion_redo() {
        QFETCH(bool, idlist);
        if (idlist) {
            IdList a(CollabText::Test::test_counter(1));
            a.insert_after(Anchor::min(), 42);
            QVERIFY(a.undo().has_value());
            QVERIFY(a.ids().empty());
            a.collect_garbage();
            QVERIFY(a.redo().has_value());
            QCOMPARE(a.ids(), std::vector<uint64_t>{42}); // passing local IdList control
        } else {
            Buffer a(CollabText::Test::test_counter(1));
            a.apply_local_edit({{0, 0}}, {"abc"});
            QVERIFY(a.undo().has_value());
            QCOMPARE(a.text(), std::string());
            a.collect_garbage();
            QVERIFY(a.redo().has_value());
            QCOMPARE(a.text(), std::string("abc"));
        }
    }

    void f1_remote_gc_preserves_insertion_redo_data() { primitive_rows(); }
    void f1_remote_gc_preserves_insertion_redo() {
        QFETCH(bool, idlist);
        if (idlist) {
            IdList a(CollabText::Test::test_counter(1)), b(CollabText::Test::test_counter(2));
            b.apply_ops({a.insert_after(Anchor::min(), 42)});
            auto undo = a.undo();
            QVERIFY(undo.has_value());
            b.apply_ops({*undo});
            QVERIFY(b.ids().empty());
            b.collect_garbage();
            auto redo = a.redo();
            QVERIFY(redo.has_value());
            b.apply_ops({*redo});
            QCOMPARE(a.ids(), std::vector<uint64_t>{42});
            QCOMPARE(b.ids(), a.ids());
        } else {
            Buffer a(CollabText::Test::test_counter(1)), b(CollabText::Test::test_counter(2));
            b.apply_ops({a.apply_local_edit({{0, 0}}, {"abc"})});
            auto undo = a.undo();
            QVERIFY(undo.has_value());
            b.apply_ops({*undo});
            QCOMPARE(b.text(), std::string());
            b.collect_garbage();
            auto redo = a.redo();
            QVERIFY(redo.has_value());
            b.apply_ops({*redo});
            QCOMPARE(a.text(), std::string("abc"));
            QCOMPARE(b.text(), a.text());
        }
    }

    void f2_compact_preserves_remote_delete_undo_data() { primitive_rows(); }
    void f2_compact_preserves_remote_delete_undo() {
        QFETCH(bool, idlist);
        if (idlist) {
            IdList a(CollabText::Test::test_counter(1)), b(CollabText::Test::test_counter(2));
            b.apply_ops({a.insert_after(Anchor::min(), 42)});
            a.apply_ops({b.remove_at(b.anchor_at_index(0))});
            QVERIFY(a.ids().empty());
            QVERIFY(b.ids().empty());
            // Remove A's incidental local-insertion pin; B still retains deletion undo.
            a.set_max_undo_depth(0);
            Global watermark = a.version();
            watermark.meet(b.version());
            a.compact(watermark);
            auto undo = b.undo();
            QVERIFY(undo.has_value());
            a.apply_ops({*undo});
            QCOMPARE(b.ids(), std::vector<uint64_t>{42});
            QCOMPARE(a.ids(), b.ids());
        } else {
            Buffer a(CollabText::Test::test_counter(1)), b(CollabText::Test::test_counter(2));
            b.apply_ops({a.apply_local_edit({{0, 0}}, {"abc"})});
            a.apply_ops({b.apply_local_edit({{0, 3}}, {""})});
            QCOMPARE(a.text(), std::string());
            QCOMPARE(b.text(), std::string());
            Global watermark = a.version();
            watermark.meet(b.version());
            a.compact(watermark);
            auto undo = b.undo();
            QVERIFY(undo.has_value());
            a.apply_ops({*undo});
            QCOMPARE(b.text(), std::string("abc"));
            QCOMPARE(a.text(), b.text());
        }
    }

    void f3_absent_actor_constrains_common_coverage() {
        // P03: coverage intersection is now a distinct operation from
        // Global::meet(), which remains a nonzero-minimum aggregation and is
        // still not a watermark. Both behaviours are pinned here so the
        // distinction cannot quietly disappear again.
        Global observed, empty;
        observed.observe(Identity(actor(1), 10));
        QCOMPARE(observed.get(actor(1)), 10u);

        // The watermark: a peer with no history has seen none of it.
        QCOMPARE(observed.coverage_intersection(empty).get(actor(1)), 0u);
        QVERIFY(observed.coverage_intersection(empty).empty());

        // meet() is the other question, and answers differently on purpose.
        Global aggregated = observed;
        aggregated.meet(empty);
        QCOMPARE(aggregated.get(actor(1)), 10u);
    }

    void f4_sender_restart_delivers_distinct_operation() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const fs::path root(tmp.path().toStdString());
        Buffer a(CollabText::Test::test_counter(1)), b(CollabText::Test::test_counter(2));
        StreamSync receiver(root, "B", 2);
        receiver.register_stream("doc", StreamSync::StreamType::AppendOnly);
        receiver.start();
        bool decoded = true;
        receiver.set_on_inbound([&](const auto&, auto, const auto& payload) {
            auto operation = decode_operation(payload);
            if (operation) b.apply_ops({operation.value()});
            else decoded = false;
        });
        auto first = a.apply_local_edit({{0, 0}}, {"a"});
        {
            StreamSync sender(root, "A", 1);
            sender.register_stream("doc", StreamSync::StreamType::AppendOnly);
            sender.start();
            sender.push("doc", checked_codec(encode_operation(first)));
            sender.flush();
            receiver.poll();
        }
        QVERIFY(decoded);
        QCOMPARE(b.text(), std::string("a"));
        auto second = a.apply_local_edit({{1, 1}}, {"b"});
        QVERIFY(get_op_timestamp(first) < get_op_timestamp(second));
        {
            StreamSync sender(root, "A", 1);
            sender.register_stream("doc", StreamSync::StreamType::AppendOnly);
            sender.start();
            sender.push("doc", checked_codec(encode_operation(second)));
            sender.flush();
            receiver.poll();
        }
        QVERIFY(decoded);
        QCOMPARE(a.text(), std::string("ab"));
        EVALUATION_EXPECT_FAIL("", "F4 / P02,L04: restarted sender reuses an envelope ID and suppresses a fresh operation");
        QCOMPARE(b.text(), a.text());
    }

    void f5_receiver_restart_recovers_consumed_state() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const fs::path root(tmp.path().toStdString());
        Buffer a(CollabText::Test::test_counter(1));
        StreamSync sender(root, "A", 1);
        sender.register_stream("doc", StreamSync::StreamType::AppendOnly);
        sender.start();
        sender.push("doc", checked_codec(encode_operation(a.apply_local_edit({{0, 0}}, {"abc"}))));
        sender.flush();
        for (int session = 0; session != 2; ++session) {
            Buffer b(CollabText::Test::test_counter(2));
            StreamSync receiver(root, "B", 2);
            receiver.register_stream("doc", StreamSync::StreamType::AppendOnly);
            receiver.start();
            bool decoded = true;
            receiver.set_on_inbound([&](const auto&, auto, const auto& payload) {
                auto operation = decode_operation(payload);
                if (operation) b.apply_ops({operation.value()});
                else decoded = false;
            });
            receiver.poll();
            QVERIFY(decoded);
            if (session == 1) {
                EVALUATION_EXPECT_FAIL("", "F5 / L03,L05: persisted read progress cannot bootstrap a fresh engine without recovered state");
                QCOMPARE(b.text(), std::string("abc"));
            } else {
                QCOMPARE(b.text(), std::string("abc")); // first delivery control
            }
        }
    }

    void f6_late_segment_is_delivered_in_order() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const fs::path root(tmp.path().toStdString());
        const auto stream = root / "stream";
        {
            SegmentWriter writer(stream, SegmentKind::Ops);
            writer.start(); writer.append("first", 1); writer.close();
        }
        {
            SegmentWriter writer(stream, SegmentKind::Ops);
            writer.start(); writer.append("second", 2); writer.close();
        }
        const auto first = stream / "0000000001.seg.zst";
        QVERIFY(fs::exists(first));
        QVERIFY(fs::exists(stream / "0000000002.seg.zst"));
        const auto held = root / "held.seg.zst";
        fs::rename(first, held);
        SegmentReader reader(stream, root / "cursor");
        reader.start();
        auto delivered = reader.read_new();
        reader.commit();
        fs::rename(held, first);
        auto later = reader.read_new();
        delivered.insert(delivered.end(), later.begin(), later.end());
        EVALUATION_EXPECT_FAIL("", "F6 / L03: committing segment 2 prevents a late segment 1 from ever being read");
        QCOMPARE(delivered, (std::vector<std::string>{"first", "second"}));
    }

    void f7_insertion_obeys_boundary_affinity() {
        Buffer a(CollabText::Test::test_counter(1));
        a.apply_local_edit({{0, 0}}, {"ab"});
        auto left = a.anchor_at(1, Bias::Left);
        auto right = a.anchor_at(1, Bias::Right);
        a.apply_local_edit({{1, 1}}, {"X"});
        QCOMPARE(a.text(), std::string("aXb"));
        QCOMPARE(a.resolve_anchor(right), 2u);
        EVALUATION_EXPECT_FAIL("", "F7 / S04: left affinity stays before insertion at that boundary");
        QCOMPARE(a.resolve_anchor(left), 1u);
    }

    void f8_concurrent_passages_remain_contiguous() {
        Buffer a(CollabText::Test::test_counter(1)), b(CollabText::Test::test_counter(2));
        auto first = a.apply_local_edit({{0, 0}}, {"abc"});
        auto second = b.apply_local_edit({{0, 0}}, {"XYZ"});
        a.apply_ops({second}); b.apply_ops({first});
        QCOMPARE(a.text(), b.text()); // convergence alone already works
        const auto text = a.text();
        EVALUATION_EXPECT_FAIL("", "F8 / S02,S03: concurrent whole passages must not interleave character by character");
        QVERIFY(text == "abcXYZ" || text == "XYZabc");
    }
};

QTEST_GUILESS_MAIN(TestEvaluationRegressions)
#include "tst_evaluation_regressions.moc"
