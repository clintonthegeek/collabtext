#include <QTest>

#include <collabtext/CrdtEngine.h>
#include <collabtext/IdListEngine.h>

#include "crdt/Buffer.h"
#include "crdt/Diagnostics.h"
#include "TestActor.h"

using namespace CollabText::Crdt;

class TestPerfDiagnostics : public QObject {
    Q_OBJECT
private slots:
    void fragment_snapshot_counts_only_inside_sample() {
        Buffer buffer(CollabText::Test::test_counter(1));
        QVERIFY(buffer.try_apply_local_edit({{0, 0}}, {"abc"}));

        Diagnostics::reset();
        (void)buffer.fragments();
        QCOMPARE(Diagnostics::snapshot().get(Diagnostics::Counter::fragment_snapshots), uint64_t{0});

        Diagnostics::begin_sample();
        const auto fragments = buffer.fragments();
        Diagnostics::end_sample();
        QCOMPARE(fragments.size(), size_t{1});
        const auto counts = Diagnostics::snapshot();
        QCOMPARE(counts.get(Diagnostics::Counter::fragment_snapshots), uint64_t{1});
        QCOMPARE(counts.get(Diagnostics::Counter::fragment_snapshot_items), uint64_t{1});
        QCOMPARE(counts.get(Diagnostics::Counter::fragment_snapshot_text_bytes), uint64_t{3});
        QCOMPARE(counts.get(Diagnostics::Counter::sumtree_items_flattened), uint64_t{1});
    }

    void position_projection_counts_scalars() {
        Buffer buffer(CollabText::Test::test_counter(2));
        QVERIFY(buffer.try_apply_local_edit({{0, 0}}, {"a\xf0\x9f\x99\x82"}));

        Diagnostics::reset();
        Diagnostics::begin_sample();
        const auto items = buffer.position_items();
        Diagnostics::end_sample();
        QVERIFY(items);
        QCOMPARE(items.value().size(), size_t{2});
        const auto counts = Diagnostics::snapshot();
        QCOMPARE(counts.get(Diagnostics::Counter::position_items_materialized), uint64_t{2});
        QCOMPARE(counts.get(Diagnostics::Counter::position_scalars_materialized), uint64_t{2});
    }

    void utf16_range_and_position_traversal_count_work() {
        Buffer buffer(CollabText::Test::test_counter(3));
        QVERIFY(buffer.try_apply_local_edit({{0, 0}}, {"a\xf0\x9f\x99\x82" "b"}));
        Diagnostics::reset();
        Diagnostics::begin_sample();
        const auto range = buffer.utf16_range(3, 1);
        Diagnostics::end_sample();
        QVERIFY(range);
        QCOMPARE(range.value().first, uint32_t{5});
        QCOMPARE(range.value().second, uint32_t{6});
        auto counts = Diagnostics::snapshot();
        QCOMPARE(counts.get(Diagnostics::Counter::utf16_range_calls), uint64_t{1});
        QCOMPARE(counts.get(Diagnostics::Counter::utf16_range_fragments_visited), uint64_t{1});
        QCOMPARE(counts.get(Diagnostics::Counter::utf16_decoder_steps), uint64_t{13});

        auto engine = CollabText::CrdtEngine::create(
            CollabText::Test::test_counter(4),
            {"perf", "body", CollabText::Primitive::text, 1});
        QVERIFY(engine);
        QVERIFY(engine.value()->try_insert(0, "abc"));
        Diagnostics::reset();
        Diagnostics::begin_sample();
        const auto boundary = engine.value()->boundary_at(1, CollabText::Affinity::left);
        QVERIFY(boundary);
        const auto resolved = engine.value()->resolve(boundary.value());
        Diagnostics::end_sample();
        QVERIFY(resolved);
        counts = Diagnostics::snapshot();
        QCOMPARE(counts.get(Diagnostics::Counter::position_items_materialized), uint64_t{6});
        QCOMPARE(counts.get(Diagnostics::Counter::position_scalars_materialized), uint64_t{6});
        QCOMPARE(counts.get(Diagnostics::Counter::position_items_examined), uint64_t{6});

        auto list = CollabText::IdListEngine::create(
            CollabText::Test::test_counter(5),
            {"perf", "outline", CollabText::Primitive::list, 1});
        QVERIFY(list);
        QVERIFY(list.value()->try_insert_after(CollabText::Crdt::Anchor::min(), 11));
        QVERIFY(list.value()->try_insert_after(CollabText::Crdt::Anchor::min(), 12));
        QVERIFY(list.value()->set_max_undo_depth(1));
        Diagnostics::reset();
        Diagnostics::begin_sample();
        const auto inserted = list.value()->try_insert_after(
            CollabText::Crdt::Anchor::min(), 13);
        const auto element = list.value()->element_of(uint64_t{11});
        Diagnostics::end_sample();
        QVERIFY(inserted);
        QVERIFY(element);
        const auto list_counts = Diagnostics::snapshot();
        QVERIFY(list_counts.get(Diagnostics::Counter::idlist_engine_identity_comparisons) > 0);
        QCOMPARE(list_counts.get(Diagnostics::Counter::undo_active_groups_observed), uint64_t{1});
        QCOMPARE(list_counts.get(Diagnostics::Counter::undo_retired_groups_observed), uint64_t{2});
        QVERIFY(list_counts.get(Diagnostics::Counter::undo_active_budget_bytes_observed) > 0);
        QVERIFY(list_counts.get(Diagnostics::Counter::undo_retired_budget_bytes_observed) > 0);
    }
};

QTEST_MAIN(TestPerfDiagnostics)
#include "tst_perf_diagnostics.moc"
