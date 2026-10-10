#include "crdt/Fragment.h"
#include "crdt/Buffer.h"
#include "crdt/IdList.h"
#include "crdt/OperationPayload.h"
#include "crdt/PositionTraversal.h"
#include "crdt/SequenceOrder.h"
#include "TestActor.h"

#include <QtTest>

using namespace CollabText::Crdt;
using CollabText::Test::actor;

namespace {

bool same(const std::vector<Fragment>& a, const std::vector<Fragment>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].origin != b[i].origin || a[i].link != b[i].link ||
            a[i].byte_length != b[i].byte_length || a[i].length != b[i].length ||
            a[i].deletions != b[i].deletions || a[i].visible != b[i].visible ||
            a[i].text != b[i].text) return false;
    }
    return true;
}

bool same(const std::vector<IdListEntry>& a, const std::vector<IdListEntry>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].origin != b[i].origin || a[i].link != b[i].link ||
            a[i].id != b[i].id || a[i].deletions != b[i].deletions ||
            a[i].visible != b[i].visible) return false;
    }
    return true;
}

std::vector<CollabText::detail::PositionItem> positions(const std::vector<Fragment>& runs) {
    std::vector<CollabText::detail::PositionItem> out;
    for (const auto& run : runs) {
        size_t byte = 0;
        for (uint32_t item = 0; item < run.length; ++item) {
            const auto lead = static_cast<unsigned char>(run.text.at(byte));
            const uint32_t width = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2
                                   : (lead & 0xF0) == 0xE0 ? 3 : 4;
            out.push_back({run.item_at(item), width, run.visible, std::nullopt});
            byte += width;
        }
        Q_ASSERT(byte == run.text.size());
    }
    return out;
}

} // namespace

class SequenceFastPathsTest : public QObject {
    Q_OBJECT
private slots:
    void empty_root_insert_matches_general_placement();
    void fragment_append_matches_general_placement();
    void fragment_split_falls_back_and_matches_general_placement();
    void list_append_matches_general_placement();
    void list_head_insertion_falls_back_and_matches_general_placement();
    void concurrent_hidden_origin_and_anchors_match_general_placement();
    void retired_local_origin_append_matches_general_placement();
};

void SequenceFastPathsTest::empty_root_insert_matches_general_placement() {
    std::vector<Fragment> fast, slow;
    Fragment incoming({actor(1), 1}, Link{}, 1, 1, "x");

    SequenceOrder::reset_integration_counters();
    QVERIFY(SequenceOrder::integrate(fast, incoming));
    const auto fast_counts = SequenceOrder::integration_counters();
    QVERIFY(SequenceOrder::integrate_slow(slow, incoming));

    QVERIFY(same(fast, slow));
    QCOMPARE(fast_counts.append_fast, uint64_t(1));
    QCOMPARE(fast_counts.general, uint64_t(0));
}

void SequenceFastPathsTest::fragment_append_matches_general_placement() {
    const auto a = actor(1), b = actor(2);
    std::vector<Fragment> fast{
        Fragment({a, 1}, Link{}, 2, 2, "ab")};
    auto slow = fast;
    const auto link = SequenceOrder::mint_link(fast, Identity{a, 2});
    Fragment incoming({b, 1}, link, 2, 2, "XY");

    SequenceOrder::reset_integration_counters();
    QVERIFY(SequenceOrder::integrate(fast, incoming));
    const auto fast_counts = SequenceOrder::integration_counters();
    QVERIFY(SequenceOrder::integrate_slow(slow, incoming));
    const auto both_counts = SequenceOrder::integration_counters();

    QVERIFY(same(fast, slow));
    QCOMPARE(fast_counts.append_fast, uint64_t(1));
    QCOMPARE(fast_counts.general, uint64_t(0));
    QCOMPARE(both_counts.general, uint64_t(1));
}

void SequenceFastPathsTest::fragment_split_falls_back_and_matches_general_placement() {
    const auto a = actor(1), b = actor(2);
    std::vector<Fragment> fast{
        Fragment({a, 1}, Link{}, 2, 2, "ab")};
    auto slow = fast;
    const auto link = SequenceOrder::mint_link(fast, Identity{a, 1});
    Fragment incoming({b, 1}, link, 4, 1, "🙂");

    SequenceOrder::reset_integration_counters();
    QVERIFY(SequenceOrder::integrate(fast, incoming));
    const auto fast_counts = SequenceOrder::integration_counters();
    QVERIFY(SequenceOrder::integrate_slow(slow, incoming));

    QVERIFY(same(fast, slow));
    QCOMPARE(fast_counts.append_fast, uint64_t(0));
    QCOMPARE(fast_counts.general, uint64_t(1));
    QCOMPARE(fast.size(), size_t(3));
    QCOMPARE(fast[0].text, std::string("a"));
    QCOMPARE(fast[1].text, std::string("🙂"));
    QCOMPARE(fast[2].text, std::string("b"));
}

void SequenceFastPathsTest::list_append_matches_general_placement() {
    const auto a = actor(1), b = actor(2);
    std::vector<IdListEntry> fast{{Identity{a, 1}, Link{}, 11}};
    auto slow = fast;
    const auto link = SequenceOrder::mint_link(fast, Identity{a, 1});
    IdListEntry incoming({b, 1}, link, 22);

    SequenceOrder::reset_integration_counters();
    QVERIFY(SequenceOrder::integrate(fast, incoming));
    const auto fast_counts = SequenceOrder::integration_counters();
    QVERIFY(SequenceOrder::integrate_slow(slow, incoming));

    QVERIFY(same(fast, slow));
    QCOMPARE(fast_counts.append_fast, uint64_t(1));
    QCOMPARE(fast_counts.general, uint64_t(0));
}

void SequenceFastPathsTest::list_head_insertion_falls_back_and_matches_general_placement() {
    const auto a = actor(1), b = actor(2);
    std::vector<IdListEntry> fast{{Identity{a, 1}, Link{}, 11}};
    auto slow = fast;
    const auto link = SequenceOrder::mint_link(fast, std::nullopt);
    IdListEntry incoming({b, 1}, link, 22);

    SequenceOrder::reset_integration_counters();
    QVERIFY(SequenceOrder::integrate(fast, incoming));
    const auto fast_counts = SequenceOrder::integration_counters();
    QVERIFY(SequenceOrder::integrate_slow(slow, incoming));

    QVERIFY(same(fast, slow));
    QCOMPARE(fast_counts.append_fast, uint64_t(0));
    QCOMPARE(fast_counts.general, uint64_t(1));
}

void SequenceFastPathsTest::concurrent_hidden_origin_and_anchors_match_general_placement() {
    const auto a = actor(1), b = actor(2), c = actor(3);
    std::vector<Fragment> fast, slow;
    const Fragment first({a, 1}, Link{}, 5, 2, "a🙂");
    const Fragment concurrent({b, 1}, Link{}, 1, 1, "b");

    SequenceOrder::reset_integration_counters();
    QVERIFY(SequenceOrder::integrate(fast, first));
    QVERIFY(SequenceOrder::integrate(fast, concurrent));
    auto counters = SequenceOrder::integration_counters();
    QCOMPARE(counters.append_fast, uint64_t(1));
    QCOMPARE(counters.general, uint64_t(1));
    QVERIFY(SequenceOrder::integrate_slow(slow, first));
    QVERIFY(SequenceOrder::integrate_slow(slow, concurrent));

    // Undo/deletion visibility changes do not remove the retained origin from
    // the sequence. A subsequent append may still name the final visible run.
    fast.front().visible = false;
    slow.front().visible = false;
    const auto append_link = SequenceOrder::mint_link(fast, Identity{b, 1});
    const Fragment appended({c, 1}, append_link, 4, 1, "🙂");
    QVERIFY(SequenceOrder::integrate(fast, appended));
    counters = SequenceOrder::integration_counters();
    QCOMPARE(counters.append_fast, uint64_t(2));
    QVERIFY(SequenceOrder::integrate_slow(slow, appended));
    QVERIFY(same(fast, slow));

    const auto fast_items = positions(fast);
    const auto slow_items = positions(slow);
    QVERIFY(fast_items.size() == slow_items.size());
    CollabText::Crdt::Global frontier;
    for (const auto& item : fast_items) frontier.observe(item.identity);
    const CollabText::PositionScope scope{"fast-path-test", "text",
                                           CollabText::Primitive::text, 1};
    std::vector<uint32_t> offsets{0};
    for (const auto& item : fast_items)
        if (item.visible) offsets.push_back(offsets.back() + item.width);
    for (const auto offset : offsets) {
        auto fast_token = CollabText::detail::mint_boundary(
            scope, frontier, fast_items, offset, CollabText::Affinity::left);
        auto slow_token = CollabText::detail::mint_boundary(
            scope, frontier, slow_items, offset, CollabText::Affinity::left);
        QVERIFY2(fast_token.ok(), std::string(fast_token.error().message()).c_str());
        QVERIFY2(slow_token.ok(), std::string(slow_token.error().message()).c_str());
        const auto fast_resolved = CollabText::detail::resolve_boundary(fast_token.value(), fast_items);
        const auto slow_resolved = CollabText::detail::resolve_boundary(slow_token.value(), slow_items);
        QVERIFY(fast_resolved.ok());
        QVERIFY(slow_resolved.ok());
        QCOMPARE(fast_resolved.value().byte_offset, slow_resolved.value().byte_offset);
        QCOMPARE(fast_resolved.value().collapsed(), slow_resolved.value().collapsed());
    }
}

void SequenceFastPathsTest::retired_local_origin_append_matches_general_placement() {
    Buffer author(CollabText::Test::test_counter(1));
    QVERIFY(author.set_max_undo_depth(0).ok());
    SequenceOrder::reset_integration_counters();

    const Operation first = author.apply_local_edit({{0, 0}}, {"a"});
    const auto& first_edit = std::get<EditOperation>(detail::OperationAccess::payload(first));
    QVERIFY(first_edit.inserted_fragments.size() == 1);
    const auto& first_run = first_edit.inserted_fragments.front();
    std::vector<Fragment> slow;
    QVERIFY(SequenceOrder::integrate_slow(
        slow, Fragment(first_run.origin, first_run.link,
                       static_cast<uint32_t>(first_run.content.size()),
                       first_run.length, first_run.content)));

    // The zero-depth policy retires the local undo entry immediately. Its
    // inserted identity remains in the document and can be the append parent.
    const Operation second = author.apply_local_edit({{1, 1}}, {"🙂"});
    const auto& second_edit = std::get<EditOperation>(detail::OperationAccess::payload(second));
    QVERIFY(second_edit.inserted_fragments.size() == 1);
    const auto& second_run = second_edit.inserted_fragments.front();
    QVERIFY(SequenceOrder::integrate_slow(
        slow, Fragment(second_run.origin, second_run.link,
                       static_cast<uint32_t>(second_run.content.size()),
                       second_run.length, second_run.content)));

    QVERIFY(same(author.fragments(), slow));
    QCOMPARE(author.text(), std::string("a🙂"));
    const auto counts = SequenceOrder::integration_counters();
    QCOMPARE(counts.append_fast, uint64_t(2));
    QCOMPARE(counts.general, uint64_t(2)); // the two forced-reference inserts above

    const auto fast_items = positions(author.fragments());
    const auto slow_items = positions(slow);
    for (const auto offset : {uint32_t(0), uint32_t(1), uint32_t(5)}) {
        auto fast_token = CollabText::detail::mint_boundary(
            {"retired-origin-test", "text", CollabText::Primitive::text, 1},
            author.version(), fast_items, offset, CollabText::Affinity::right);
        auto slow_token = CollabText::detail::mint_boundary(
            {"retired-origin-test", "text", CollabText::Primitive::text, 1},
            author.version(), slow_items, offset, CollabText::Affinity::right);
        QVERIFY(fast_token.ok());
        QVERIFY(slow_token.ok());
        const auto fast_resolved = CollabText::detail::resolve_boundary(fast_token.value(), fast_items);
        const auto slow_resolved = CollabText::detail::resolve_boundary(slow_token.value(), slow_items);
        QVERIFY(fast_resolved.ok());
        QVERIFY(slow_resolved.ok());
        QCOMPARE(fast_resolved.value().byte_offset, slow_resolved.value().byte_offset);
        QCOMPARE(fast_resolved.value().collapsed(), slow_resolved.value().collapsed());
    }
}

QTEST_GUILESS_MAIN(SequenceFastPathsTest)
#include "tst_sequence_fast_paths.moc"
