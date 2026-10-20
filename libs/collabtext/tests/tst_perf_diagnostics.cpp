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
    void local_tail_insert_skips_fragment_flatten_and_rebuild() {
        Buffer buffer(CollabText::Test::test_counter(29));
        std::vector<Operation> seed;
        seed.reserve(128);
        for (uint32_t i = 0; i < 128; ++i) {
            auto appended = buffer.try_apply_local_edit({{i, i}}, {"x"});
            QVERIFY(appended);
            seed.push_back(std::move(appended).value());
        }
        QCOMPARE(buffer.fragment_count(), size_t{128});

        Diagnostics::reset();
        Diagnostics::begin_sample();
        auto appended = buffer.try_apply_local_edit({{128, 128}}, {"y"});
        Diagnostics::end_sample();
        QVERIFY(appended);
        QCOMPARE(buffer.text(), std::string(128, 'x') + "y");
        const auto counts = Diagnostics::snapshot();
        QCOMPARE(counts.get(Diagnostics::Counter::fragment_snapshots), uint64_t{0});
        QCOMPARE(counts.get(Diagnostics::Counter::fragment_snapshot_items),
                 uint64_t{0});
        QCOMPARE(counts.get(Diagnostics::Counter::fragment_tree_rebuild_items),
                 uint64_t{0});
        QCOMPARE(counts.get(Diagnostics::Counter::fragment_index_full_rebuilds),
                 uint64_t{0});
        QCOMPARE(counts.get(Diagnostics::Counter::placement_index_builds), uint64_t{0});
        QCOMPARE(counts.get(Diagnostics::Counter::origin_index_rows_inserted),
                 uint64_t{1});
        QCOMPARE(counts.get(Diagnostics::Counter::fragment_index_publications),
                 uint64_t{1});
        QCOMPARE(counts.get(Diagnostics::Counter::sumtree_items_pushed),
                 uint64_t{1});

        Buffer peer(CollabText::Test::test_counter(30));
        const auto seeded = peer.apply_ops(seed);
        QCOMPARE(seeded.size(), seed.size());
        Diagnostics::reset();
        Diagnostics::begin_sample();
        const auto delivered = peer.apply_ops({appended.value()});
        Diagnostics::end_sample();
        QCOMPARE(delivered.size(), size_t{1});
        QCOMPARE(peer.text(), buffer.text());
        const auto remote_counts = Diagnostics::snapshot();
        QCOMPARE(remote_counts.get(Diagnostics::Counter::fragment_snapshots),
                 uint64_t{0});
        QCOMPARE(remote_counts.get(Diagnostics::Counter::fragment_tree_rebuild_items),
                 uint64_t{0});
        QCOMPARE(remote_counts.get(Diagnostics::Counter::placement_index_builds),
                 uint64_t{0});
        QCOMPARE(remote_counts.get(Diagnostics::Counter::fragment_index_publications),
                 uint64_t{1});
        QCOMPARE(remote_counts.get(Diagnostics::Counter::sumtree_items_pushed),
                 uint64_t{1});
        const auto undone = buffer.try_undo();
        QVERIFY(undone);
        QVERIFY(undone.value().has_value());
        QCOMPARE(buffer.text(), std::string(128, 'x'));

        Buffer marker_buffer(CollabText::Test::test_counter(32));
        QCOMPARE(marker_buffer.apply_ops(seed).size(), seed.size());
        Diagnostics::reset();
        Diagnostics::begin_sample();
        auto marker = marker_buffer.try_insert_marker(128);
        Diagnostics::end_sample();
        QVERIFY(marker);
        QCOMPARE(marker_buffer.fragment_count(), size_t{129});
        QVERIFY(marker_buffer.fragments().back().marker);
        const auto marker_counts = Diagnostics::snapshot();
        QCOMPARE(marker_counts.get(Diagnostics::Counter::fragment_snapshots),
                 uint64_t{0});
        QCOMPARE(marker_counts.get(Diagnostics::Counter::fragment_tree_rebuild_items),
                 uint64_t{0});
        QCOMPARE(marker_counts.get(Diagnostics::Counter::placement_index_builds),
                 uint64_t{0});
        QCOMPARE(marker_counts.get(Diagnostics::Counter::fragment_index_publications),
                 uint64_t{1});
        QCOMPARE(marker_counts.get(Diagnostics::Counter::sumtree_items_pushed),
                 uint64_t{1});

        Buffer marker_peer(CollabText::Test::test_counter(31));
        QCOMPARE(marker_peer.apply_ops(seed).size(), seed.size());
        Diagnostics::reset();
        Diagnostics::begin_sample();
        QCOMPARE(marker_peer.apply_ops({marker.value()}).size(), size_t{1});
        Diagnostics::end_sample();
        QCOMPARE(marker_peer.text(), marker_buffer.text());
        QVERIFY(marker_peer.fragments().back().marker);
        const auto remote_marker_counts = Diagnostics::snapshot();
        QCOMPARE(remote_marker_counts.get(Diagnostics::Counter::fragment_snapshots),
                 uint64_t{0});
        QCOMPARE(remote_marker_counts.get(
                     Diagnostics::Counter::fragment_tree_rebuild_items),
                 uint64_t{0});
        QCOMPARE(remote_marker_counts.get(Diagnostics::Counter::placement_index_builds),
                 uint64_t{0});
    }

    void local_exact_run_delete_edits_only_the_tree_path() {
        Buffer buffer(CollabText::Test::test_counter(34));
        std::vector<Operation> seed;
        seed.reserve(128);
        for (uint32_t i = 0; i < 128; ++i) {
            auto appended = buffer.try_apply_local_edit({{i, i}}, {"x"});
            QVERIFY(appended);
            seed.push_back(std::move(appended).value());
        }
        QCOMPARE(buffer.fragment_count(), size_t{128});

        Buffer peer(CollabText::Test::test_counter(35));
        QCOMPARE(peer.apply_ops(seed).size(), seed.size());

        Diagnostics::reset();
        Diagnostics::begin_sample();
        auto deleted = buffer.try_apply_local_edit({{127, 128}}, {""});
        Diagnostics::end_sample();
        QVERIFY(deleted);
        QCOMPARE(buffer.text(), std::string(127, 'x'));
        QCOMPARE(buffer.fragment_count(), size_t{128});

        const auto counts = Diagnostics::snapshot();
        QCOMPARE(counts.get(Diagnostics::Counter::fragment_snapshots), uint64_t{0});
        QCOMPARE(counts.get(Diagnostics::Counter::fragment_tree_rebuild_items),
                 uint64_t{0});
        QCOMPARE(counts.get(Diagnostics::Counter::placement_index_builds),
                 uint64_t{0});
        QCOMPARE(counts.get(Diagnostics::Counter::fragment_index_full_rebuilds),
                 uint64_t{0});
        QCOMPARE(counts.get(Diagnostics::Counter::fragment_index_publications),
                 uint64_t{1});
        QCOMPARE(counts.get(Diagnostics::Counter::deletion_index_rows_inserted),
                 uint64_t{1});
        QVERIFY(counts.get(Diagnostics::Counter::sumtree_items_visited) < uint64_t{20});

        QCOMPARE(peer.apply_ops({deleted.value()}).size(), size_t{1});
        QCOMPARE(peer.text(), buffer.text());
        const auto undone = buffer.try_undo();
        QVERIFY(undone);
        QVERIFY(undone.value().has_value());
        QCOMPARE(buffer.text(), std::string(128, 'x'));

        Buffer range_buffer(CollabText::Test::test_counter(36));
        QCOMPARE(range_buffer.apply_ops(seed).size(), seed.size());
        Buffer range_peer(CollabText::Test::test_counter(37));
        QCOMPARE(range_peer.apply_ops(seed).size(), seed.size());
        Diagnostics::reset();
        Diagnostics::begin_sample();
        auto range_deleted = range_buffer.try_apply_local_edit({{0, 128}}, {""});
        Diagnostics::end_sample();
        QVERIFY(range_deleted);
        QCOMPARE(range_buffer.text(), std::string{});
        const auto range_counts = Diagnostics::snapshot();
        QCOMPARE(range_counts.get(Diagnostics::Counter::fragment_snapshots),
                 uint64_t{0});
        QCOMPARE(range_counts.get(Diagnostics::Counter::fragment_tree_rebuild_items),
                 uint64_t{0});
        QCOMPARE(range_counts.get(Diagnostics::Counter::placement_index_builds),
                 uint64_t{0});
        QCOMPARE(range_counts.get(Diagnostics::Counter::deletion_index_rows_inserted),
                 uint64_t{128});
        QCOMPARE(range_peer.apply_ops({range_deleted.value()}).size(), size_t{1});
        QCOMPARE(range_peer.text(), range_buffer.text());
        const auto range_undo = range_buffer.try_undo();
        QVERIFY(range_undo);
        QVERIFY(range_undo.value().has_value());
        QCOMPARE(range_buffer.text(), std::string(128, 'x'));

        Buffer deletion_sender(CollabText::Test::test_counter(38));
        auto inserted = deletion_sender.try_apply_local_edit({{0, 0}}, {"hello"});
        QVERIFY(inserted);
        Buffer deletion_receiver(CollabText::Test::test_counter(39));
        QCOMPARE(deletion_receiver.apply_ops({inserted.value()}).size(), size_t{1});
        auto whole_run_delete = deletion_sender.try_apply_local_edit({{0, 5}}, {""});
        QVERIFY(whole_run_delete);
        Diagnostics::reset();
        Diagnostics::begin_sample();
        QCOMPARE(deletion_receiver.apply_ops({whole_run_delete.value()}).size(),
                 size_t{1});
        Diagnostics::end_sample();
        QCOMPARE(deletion_receiver.text(), std::string{});
        const auto remote_delete_counts = Diagnostics::snapshot();
        QCOMPARE(remote_delete_counts.get(Diagnostics::Counter::fragment_snapshots),
                 uint64_t{0});
        QCOMPARE(remote_delete_counts.get(
                     Diagnostics::Counter::fragment_tree_rebuild_items),
                 uint64_t{0});
        QCOMPARE(remote_delete_counts.get(Diagnostics::Counter::placement_index_builds),
                 uint64_t{0});
        QCOMPARE(remote_delete_counts.get(
                     Diagnostics::Counter::fragment_index_publications),
                 uint64_t{1});
        QCOMPARE(remote_delete_counts.get(
                     Diagnostics::Counter::deletion_index_rows_inserted),
                 uint64_t{1});
    }

    void fragment_mutation_updates_only_changed_index_rows() {
        Buffer buffer(CollabText::Test::test_counter(20));
        QVERIFY(buffer.try_apply_local_edit({{0, 0}}, {"abcdef"}));

        Diagnostics::reset();
        Diagnostics::begin_sample();
        const auto inserted = buffer.try_apply_local_edit({{3, 3}}, {"X"});
        Diagnostics::end_sample();
        QVERIFY(inserted);
        QCOMPARE(buffer.text(), std::string("abcXdef"));
        const auto counts = Diagnostics::snapshot();
        QCOMPARE(counts.get(Diagnostics::Counter::fragment_index_full_rebuilds),
                 uint64_t{0});
        QCOMPARE(counts.get(Diagnostics::Counter::fragment_index_publications),
                 uint64_t{1});
        QCOMPARE(counts.get(Diagnostics::Counter::origin_index_rows_erased),
                 uint64_t{1});
        QCOMPARE(counts.get(Diagnostics::Counter::origin_index_rows_inserted),
                 uint64_t{3});
        QCOMPARE(counts.get(Diagnostics::Counter::deletion_index_rows_erased),
                 uint64_t{0});
        QCOMPARE(counts.get(Diagnostics::Counter::deletion_index_rows_inserted),
                 uint64_t{0});

        const auto undone = buffer.try_undo();
        QVERIFY(undone);
        QVERIFY(undone.value().has_value());
        QCOMPARE(buffer.text(), std::string("abcdef"));

        Diagnostics::reset();
        Diagnostics::begin_sample();
        const auto deleted = buffer.try_apply_local_edit({{1, 2}}, {""});
        Diagnostics::end_sample();
        QVERIFY(deleted);
        QCOMPARE(buffer.text(), std::string("acdef"));
        const auto deletion_counts = Diagnostics::snapshot();
        QCOMPARE(deletion_counts.get(
                     Diagnostics::Counter::fragment_index_full_rebuilds),
                 uint64_t{0});
        QCOMPARE(deletion_counts.get(
                     Diagnostics::Counter::fragment_index_publications),
                 uint64_t{1});
        QCOMPARE(deletion_counts.get(
                     Diagnostics::Counter::origin_index_rows_erased),
                 uint64_t{1});
        QCOMPARE(deletion_counts.get(
                     Diagnostics::Counter::origin_index_rows_inserted),
                 uint64_t{3});
        QCOMPARE(deletion_counts.get(
                     Diagnostics::Counter::deletion_index_rows_erased),
                 uint64_t{0});
        QCOMPARE(deletion_counts.get(
                     Diagnostics::Counter::deletion_index_rows_inserted),
                 uint64_t{1});
        const auto deletion_undo = buffer.try_undo();
        QVERIFY(deletion_undo);
        QVERIFY(deletion_undo.value().has_value());
        QCOMPARE(buffer.text(), std::string("abcdef"));
    }

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
        QCOMPARE(counts.get(Diagnostics::Counter::utf16_decoder_steps), uint64_t{2});

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
        QCOMPARE(counts.get(Diagnostics::Counter::position_items_materialized), uint64_t{0});
        QCOMPARE(counts.get(Diagnostics::Counter::position_scalars_materialized), uint64_t{0});
        QCOMPARE(counts.get(Diagnostics::Counter::position_items_examined), uint64_t{0});
        QCOMPARE(counts.get(Diagnostics::Counter::fragment_snapshots), uint64_t{0});
        QCOMPARE(counts.get(Diagnostics::Counter::sumtree_items_visited), uint64_t{2});

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

    void utf16_lookup_bounds_local_unicode_scans() {
        Buffer buffer(CollabText::Test::test_counter(6));
        std::string text;
        for (int i = 0; i < 4096; ++i) text += "a\xf0\x9f\x99\x82";
        QVERIFY(buffer.try_apply_local_edit({{0, 0}}, {text}));

        Diagnostics::reset();
        Diagnostics::begin_sample();
        const auto range = buffer.utf16_range(6144, 3);
        Diagnostics::end_sample();
        QVERIFY(range);
        QCOMPARE(range.value().first, uint32_t{10240});
        QCOMPARE(range.value().second, uint32_t{10245});
        const auto counts = Diagnostics::snapshot();
        QCOMPARE(counts.get(Diagnostics::Counter::utf16_range_fragments_visited), uint64_t{2});
        QVERIFY(counts.get(Diagnostics::Counter::utf16_decoder_steps) <= uint64_t{258});
    }

    void utf16_index_and_summary_build_work_is_counted() {
        Buffer ascii(CollabText::Test::test_counter(7));
        const std::string ascii_text(4096, 'a');
        Diagnostics::reset();
        Diagnostics::begin_sample();
        QVERIFY(ascii.try_apply_local_edit({{0, 0}}, {ascii_text}));
        Diagnostics::end_sample();
        const auto ascii_counts = Diagnostics::snapshot();
        qInfo() << "X07b ASCII build counters: summaries="
                << ascii_counts.get(Diagnostics::Counter::fragment_summary_compositions)
                << "index_builds=" << ascii_counts.get(Diagnostics::Counter::utf16_index_builds)
                << "probe_bytes=" << ascii_counts.get(Diagnostics::Counter::utf16_index_ascii_probe_bytes);
        QCOMPARE(ascii_counts.get(Diagnostics::Counter::utf16_index_builds), uint64_t{1});
        QCOMPARE(ascii_counts.get(Diagnostics::Counter::utf16_index_ascii_probe_bytes), uint64_t{4096});
        QVERIFY(ascii_counts.get(Diagnostics::Counter::fragment_summary_compositions) > 0);
        QCOMPARE(ascii_counts.get(Diagnostics::Counter::utf16_checkpoint_entries_built), uint64_t{0});

        Buffer unicode(CollabText::Test::test_counter(8));
        std::string unicode_text;
        for (int i = 0; i < 1024; ++i) unicode_text += "a\xf0\x9f\x99\x82";
        Diagnostics::reset();
        Diagnostics::begin_sample();
        QVERIFY(unicode.try_apply_local_edit({{0, 0}}, {unicode_text}));
        Diagnostics::end_sample();
        const auto unicode_counts = Diagnostics::snapshot();
        qInfo() << "X07b Unicode build counters: summaries="
                << unicode_counts.get(Diagnostics::Counter::fragment_summary_compositions)
                << "index_builds=" << unicode_counts.get(Diagnostics::Counter::utf16_index_builds)
                << "probe_bytes=" << unicode_counts.get(Diagnostics::Counter::utf16_index_ascii_probe_bytes)
                << "decoder_steps=" << unicode_counts.get(Diagnostics::Counter::utf16_decoder_steps)
                << "checkpoint_entries=" << unicode_counts.get(Diagnostics::Counter::utf16_checkpoint_entries_built);
        QCOMPARE(unicode_counts.get(Diagnostics::Counter::utf16_index_builds), uint64_t{1});
        QVERIFY(unicode_counts.get(Diagnostics::Counter::utf16_checkpoint_entries_built) > 0);
    }

    void undo_recomputes_visibility_only_for_indexed_runs() {
        Buffer seed(CollabText::Test::test_counter(21));
        Buffer inserted(CollabText::Test::test_counter(22));
        auto initial = seed.try_apply_local_edit({{0, 0}}, {"abcdef"});
        QVERIFY(initial);
        inserted.apply_ops({initial.value()});
        QVERIFY(inserted.try_apply_local_edit({{3, 3}}, {"X"}));
        QVERIFY(inserted.fragments().size() >= size_t{3});
        QCOMPARE(inserted.text(), std::string("abcXdef"));

        Diagnostics::reset();
        Diagnostics::begin_sample();
        auto inserted_undo = inserted.try_undo();
        Diagnostics::end_sample();
        QVERIFY(inserted_undo);
        QVERIFY(inserted_undo.value().has_value());
        QCOMPARE(inserted.text(), std::string("abcdef"));
        QCOMPARE(Diagnostics::snapshot().get(
                     Diagnostics::Counter::visibility_recomputations), uint64_t{1});
        QCOMPARE(Diagnostics::snapshot().get(
                     Diagnostics::Counter::sumtree_items_visited), uint64_t{2});

        Buffer deleted(CollabText::Test::test_counter(23));
        deleted.apply_ops({initial.value()});
        QVERIFY(deleted.try_apply_local_edit({{2, 4}}, {""}));
        QVERIFY(deleted.fragments().size() >= size_t{3});
        QCOMPARE(deleted.text(), std::string("abef"));
        Diagnostics::reset();
        Diagnostics::begin_sample();
        auto deletion_undo = deleted.try_undo();
        Diagnostics::end_sample();
        QVERIFY(deletion_undo);
        QVERIFY(deletion_undo.value().has_value());
        QCOMPARE(deleted.text(), std::string("abcdef"));
        QCOMPARE(Diagnostics::snapshot().get(
                     Diagnostics::Counter::visibility_recomputations), uint64_t{1});
        QCOMPARE(Diagnostics::snapshot().get(
                     Diagnostics::Counter::sumtree_items_visited), uint64_t{2});
    }

    void indexed_run_edit_visits_far_fewer_items_than_full_scan() {
        constexpr uint64_t run_count = 5000;
        FragmentTree tree;
        OriginIndex origins;
        DeletionIndex deletions;
        std::vector<std::pair<RunId, uint64_t>> run_rows;
        run_rows.reserve(run_count);
        for (uint64_t i = 0; i < run_count; ++i) {
            Fragment fragment;
            fragment.marker = true;
            fragment.length = 1;
            tree.push_item(std::move(fragment));
            const uint64_t counter = i + 1;
            const RunId run = i + 1;
            QVERIFY(origins.insert(CollabText::Test::actor(40), counter,
                                   counter + 1, run));
            QVERIFY(deletions.insert(
                Identity(CollabText::Test::actor(41), counter),
                CollabText::Test::actor(40), counter, counter + 1, run));
            run_rows.emplace_back(run, i);
        }
        RunDirectory directory;
        QVERIFY(directory.rebuild(std::move(run_rows)));

        Diagnostics::reset();
        Diagnostics::begin_sample();
        tree.for_each([](const Fragment&) {});
        Diagnostics::end_sample();
        const auto full_scan_visits = Diagnostics::snapshot().get(
            Diagnostics::Counter::sumtree_items_visited);
        QCOMPARE(full_scan_visits, run_count);

        Diagnostics::reset();
        Diagnostics::begin_sample();
        const Identity target_id(CollabText::Test::actor(40), run_count);
        const auto origin = origins.locate(target_id);
        const auto incidences = deletions.incidences(
            Identity(CollabText::Test::actor(41), run_count));
        QVERIFY(origin.has_value());
        QCOMPARE(incidences.size(), size_t{1});
        QCOMPARE(incidences.front().run, origin->run);
        const auto ordinal = directory.ordinal(origin->run);
        QVERIFY(ordinal.has_value());
        const bool edited = tree.edit_item<RetainedRunOffset>(
            RetainedRunOffset{*ordinal}, [](Fragment& fragment) {
                fragment.visible = false;
            });
        Diagnostics::end_sample();
        QVERIFY(edited);
        const auto indexed_visits = Diagnostics::snapshot().get(
            Diagnostics::Counter::sumtree_items_visited);
        qInfo() << "X04 indexed/full run visits:" << indexed_visits << "/"
                << full_scan_visits;
        qInfo() << "X04 index node payload bytes (origin/deletion/directory):"
                << OriginIndex::node_payload_bytes() << "/"
                << DeletionIndex::node_payload_bytes() << "/"
                << RunDirectory::node_payload_bytes()
                << "total at 5000 runs:"
                << origins.payload_bytes() + deletions.payload_bytes() +
                       directory.payload_bytes()
                << "(excludes shared_ptr control blocks and allocator overhead)";
        QVERIFY(indexed_visits > 0);
        QVERIFY(indexed_visits <= FragmentTree::MaxChildren);
        QVERIFY(indexed_visits * 10 < full_scan_visits);
    }
};

QTEST_MAIN(TestPerfDiagnostics)
#include "tst_perf_diagnostics.moc"
