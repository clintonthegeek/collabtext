#include <QTest>

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
};

QTEST_MAIN(TestPerfDiagnostics)
#include "tst_perf_diagnostics.moc"
