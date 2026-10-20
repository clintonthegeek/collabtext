#include <QTest>
#include "crdt/SumTree.h"
#include "crdt/TreeNodeId.h"
#include <numeric>
#include <string>

using namespace CollabText::Crdt;

// ============================================================================
// Test item: a simple integer with count/sum summary
// ============================================================================

struct IntItem {
    int value = 0;

    struct Summary {
        int count = 0;
        int sum = 0;
        int max_value = 0;

        static Summary zero() { return {0, 0, 0}; }
        void add_summary(const Summary& other) {
            count += other.count;
            sum += other.sum;
            max_value = std::max(max_value, other.max_value);
        }
        bool operator==(const Summary&) const = default;
    };

    Summary summary() const { return {1, value, value}; }
    bool operator==(const IntItem&) const = default;
};

// Dimension: count of items (for seeking by index)
struct ItemCount {
    int value = 0;

    static ItemCount zero() { return {0}; }
    void add_summary(const IntItem::Summary& s) { value += s.count; }

    auto operator<=>(const ItemCount&) const = default;
    bool operator==(const ItemCount&) const = default;
};

// Dimension: cumulative sum (for seeking by prefix sum)
struct ItemSum {
    int value = 0;

    static ItemSum zero() { return {0}; }
    void add_summary(const IntItem::Summary& s) { value += s.sum; }

    auto operator<=>(const ItemSum&) const = default;
    bool operator==(const ItemSum&) const = default;
};

// Use B=2 for testing to exercise splitting more aggressively
using TestTree = SumTree<IntItem, 2>;

class tst_sumtree : public QObject {
    Q_OBJECT

private slots:
    // ---- Basic construction ----

    void empty_tree() {
        TestTree tree;
        QVERIFY(tree.empty());
        QCOMPARE(tree.summary().count, 0);
        QCOMPARE(tree.summary().sum, 0);
    }

    void tree_node_ids_are_scoped_copyable_and_checked() {
        auto fresh = NodeIdAllocator::create();
        QVERIFY(fresh.has_value());
        auto first_id = fresh->allocate();
        QVERIFY(first_id.has_value());
        QCOMPARE(first_id->tree, fresh->tree_namespace());
        QCOMPARE(first_id->local, uint64_t{1});

        auto fork = *fresh;
        const auto left_id = fresh->allocate();
        const auto right_id = fork.allocate();
        QVERIFY(left_id.has_value());
        QVERIFY(right_id.has_value());
        QCOMPARE(*left_id, *right_id);
        QCOMPARE(left_id->local, uint64_t{2});

        auto other_lineage = NodeIdAllocator::create();
        QVERIFY(other_lineage.has_value());
        auto other_id = other_lineage->allocate();
        QVERIFY(other_id.has_value());
        QVERIFY(other_id->tree != first_id->tree);

        NodeIdAllocator at_limit{TreeNamespace{99}, UINT64_MAX};
        auto final_id = at_limit.allocate();
        QVERIFY(final_id.has_value());
        QCOMPARE(final_id->local, UINT64_MAX);
        QVERIFY(at_limit.exhausted());
        QVERIFY(!at_limit.allocate().has_value());
        NodeIdAllocator invalid_namespace{TreeNamespace{}, 1};
        QVERIFY(!invalid_namespace.allocate().has_value());
    }

    void stable_sumtree_ids_survive_copy_on_write_and_splice() {
        using StableTree = SumTree<IntItem, 2, StableNodeIds>;
        auto policy = StableNodeIds::create();
        QVERIFY(policy.has_value());
        StableTree source(*policy);
        for (int i = 0; i < 40; ++i) source.push_item({i});
        QVERIFY(source.has_valid_node_ids());
        QVERIFY(source.has_unique_node_ids());

        StableTree fork = source;
        fork.insert_item<ItemCount>({7}, {-1});
        QVERIFY(fork.has_valid_node_ids());
        QVERIFY(fork.has_unique_node_ids());
        QVERIFY(source.has_unique_node_ids());
        QCOMPARE(source.summary().count, 40);
        QCOMPARE(fork.summary().count, 41);

        auto replacement_policy = StableNodeIds::create();
        QVERIFY(replacement_policy.has_value());
        StableTree replacement(*replacement_policy);
        replacement.push_item({100});
        replacement.push_item({101});
        const auto spliced = source.splice_range<ItemCount>(
            {10}, {30}, std::move(replacement));
        QVERIFY(spliced.has_value());
        QVERIFY(spliced->has_valid_node_ids());
        QVERIFY(spliced->has_unique_node_ids());
        QCOMPARE(spliced->summary().count, 22);

        size_t visited = 0;
        spliced->for_each_node([&](const auto&, NodeId id,
                                   std::optional<NodeId> parent, uint16_t) {
            QVERIFY(id.valid());
            QVERIFY(id.tree.valid());
            if (visited++ == 0) QVERIFY(!parent.has_value());
            else QVERIFY(parent.has_value());
        });
        QVERIFY(visited > 1);
    }

    void push_single_item() {
        TestTree tree;
        tree.push_item({42});
        QVERIFY(!tree.empty());
        QCOMPARE(tree.summary().count, 1);
        QCOMPARE(tree.summary().sum, 42);
        QCOMPARE(tree.summary().max_value, 42);
    }

    void push_multiple_items() {
        TestTree tree;
        for (int i = 1; i <= 10; ++i)
            tree.push_item({i});

        QCOMPARE(tree.summary().count, 10);
        QCOMPARE(tree.summary().sum, 55); // 1+2+...+10
        QCOMPARE(tree.summary().max_value, 10);
    }

    void push_many_items_triggers_splits() {
        // With B=2, max 4 items per leaf. 100 items forces many splits.
        TestTree tree;
        int expected_sum = 0;
        for (int i = 1; i <= 100; ++i) {
            tree.push_item({i});
            expected_sum += i;
        }

        QCOMPARE(tree.summary().count, 100);
        QCOMPARE(tree.summary().sum, expected_sum);
        QCOMPARE(tree.summary().max_value, 100);
    }

    // ---- Iteration ----

    void for_each_preserves_order() {
        TestTree tree;
        for (int i = 1; i <= 20; ++i)
            tree.push_item({i});

        std::vector<int> values;
        tree.for_each([&](const IntItem& item) { values.push_back(item.value); });

        QCOMPARE(values.size(), 20u);
        for (int i = 0; i < 20; ++i)
            QCOMPARE(values[i], i + 1);
    }

    void items_returns_all() {
        TestTree tree;
        for (int i = 1; i <= 15; ++i)
            tree.push_item({i});

        auto items = tree.items();
        QCOMPARE(items.size(), 15u);
        for (int i = 0; i < 15; ++i)
            QCOMPARE(items[i].value, i + 1);
    }

    void first_and_last() {
        TestTree tree;
        for (int i = 10; i <= 50; ++i)
            tree.push_item({i});

        QCOMPARE(tree.first().value, 10);
        QCOMPARE(tree.last().value, 50);
    }

    // ---- Cursor seek by count (index) ----

    void seek_first_item() {
        TestTree tree;
        for (int i = 1; i <= 10; ++i)
            tree.push_item({i});

        auto cursor = tree.cursor<ItemCount>();
        QVERIFY(cursor.seek({0}, Bias::Left));
        QVERIFY(cursor.item() != nullptr);
        QCOMPARE(cursor.item()->value, 1);
        QCOMPARE(cursor.position().value, 0);
    }

    void seek_middle_item() {
        TestTree tree;
        for (int i = 1; i <= 10; ++i)
            tree.push_item({i});

        auto cursor = tree.cursor<ItemCount>();
        QVERIFY(cursor.seek({5}, Bias::Left));
        QCOMPARE(cursor.item()->value, 6); // 0-indexed: item 5 is value 6
        QCOMPARE(cursor.position().value, 5);
    }

    void seek_last_item() {
        TestTree tree;
        for (int i = 1; i <= 10; ++i)
            tree.push_item({i});

        auto cursor = tree.cursor<ItemCount>();
        QVERIFY(cursor.seek({9}, Bias::Left));
        QCOMPARE(cursor.item()->value, 10);
        QCOMPARE(cursor.position().value, 9);
    }

    void seek_past_end() {
        TestTree tree;
        for (int i = 1; i <= 5; ++i)
            tree.push_item({i});

        auto cursor = tree.cursor<ItemCount>();
        QVERIFY(!cursor.seek({10}, Bias::Left));
        QVERIFY(cursor.at_end());
    }

    void seek_by_sum() {
        TestTree tree;
        // Items: 3, 5, 2, 4 → cumulative: 3, 8, 10, 14
        tree.push_item({3});
        tree.push_item({5});
        tree.push_item({2});
        tree.push_item({4});

        {
            auto cursor = tree.cursor<ItemSum>();
            cursor.seek({0}, Bias::Left);
            QCOMPARE(cursor.item()->value, 3);
            QCOMPARE(cursor.position().value, 0);
        }
        {
            auto cursor = tree.cursor<ItemSum>();
            cursor.seek({3}, Bias::Left);
            QCOMPARE(cursor.item()->value, 5); // cumulative at 3 is end of item 0
            QCOMPARE(cursor.position().value, 3);
        }
        {
            auto cursor = tree.cursor<ItemSum>();
            cursor.seek({7}, Bias::Left);
            QCOMPARE(cursor.item()->value, 5); // 7 is within item 1 (3..8)
            QCOMPARE(cursor.position().value, 3);
        }
        {
            auto cursor = tree.cursor<ItemSum>();
            cursor.seek({8}, Bias::Left);
            QCOMPARE(cursor.item()->value, 2); // 8 is start of item 2
            QCOMPARE(cursor.position().value, 8);
        }
    }

    void seek_bias_right() {
        TestTree tree;
        // Items: 3, 5, 2, 4 → cumulative: 3, 8, 10, 14
        tree.push_item({3});
        tree.push_item({5});
        tree.push_item({2});
        tree.push_item({4});

        {
            // Bias::Right at exact boundary (8) should find item BEFORE boundary
            auto cursor = tree.cursor<ItemSum>();
            cursor.seek({8}, Bias::Right);
            QCOMPARE(cursor.item()->value, 5); // item ending at 8
            QCOMPARE(cursor.position().value, 3);
        }
        {
            // Bias::Right at 3 should find first item (ending at 3)
            auto cursor = tree.cursor<ItemSum>();
            cursor.seek({3}, Bias::Right);
            QCOMPARE(cursor.item()->value, 3);
            QCOMPARE(cursor.position().value, 0);
        }
    }

    // ---- Cursor next ----

    void cursor_next_iterates_all() {
        TestTree tree;
        for (int i = 1; i <= 20; ++i)
            tree.push_item({i});

        auto cursor = tree.cursor<ItemCount>();
        cursor.seek({0}, Bias::Left);

        std::vector<int> values;
        while (cursor.item()) {
            values.push_back(cursor.item()->value);
            cursor.next();
        }

        QCOMPARE(values.size(), 20u);
        for (int i = 0; i < 20; ++i)
            QCOMPARE(values[i], i + 1);
    }

    // ---- Cursor slice ----

    void slice_empty_prefix() {
        TestTree tree;
        for (int i = 1; i <= 10; ++i)
            tree.push_item({i});

        auto cursor = tree.cursor<ItemCount>();
        cursor.seek({0}, Bias::Left);
        auto prefix = cursor.slice({0});

        QVERIFY(prefix.empty());
        QCOMPARE(cursor.item()->value, 1);
    }

    void slice_full_tree() {
        TestTree tree;
        for (int i = 1; i <= 10; ++i)
            tree.push_item({i});

        auto cursor = tree.cursor<ItemCount>();
        cursor.seek({0}, Bias::Left);
        auto all = cursor.slice({10});

        QCOMPARE(all.summary().count, 10);
        auto items = all.items();
        for (int i = 0; i < 10; ++i)
            QCOMPARE(items[i].value, i + 1);
        QVERIFY(cursor.at_end());
    }

    void slice_prefix() {
        TestTree tree;
        for (int i = 1; i <= 10; ++i)
            tree.push_item({i});

        auto cursor = tree.cursor<ItemCount>();
        cursor.seek({0}, Bias::Left);
        auto prefix = cursor.slice({5});

        QCOMPARE(prefix.summary().count, 5);
        auto items = prefix.items();
        for (int i = 0; i < 5; ++i)
            QCOMPARE(items[i].value, i + 1);

        // Cursor should be at item 6
        QCOMPARE(cursor.item()->value, 6);
    }

    void slice_then_suffix() {
        TestTree tree;
        for (int i = 1; i <= 10; ++i)
            tree.push_item({i});

        auto cursor = tree.cursor<ItemCount>();
        cursor.seek({0}, Bias::Left);

        auto prefix = cursor.slice({4});
        auto suffix = cursor.suffix();

        QCOMPARE(prefix.summary().count, 4);
        QCOMPARE(suffix.summary().count, 6);

        auto prefix_items = prefix.items();
        for (int i = 0; i < 4; ++i)
            QCOMPARE(prefix_items[i].value, i + 1);

        auto suffix_items = suffix.items();
        for (int i = 0; i < 6; ++i)
            QCOMPARE(suffix_items[i].value, i + 5);
    }

    void slice_by_sum() {
        TestTree tree;
        // Items: 3, 5, 2, 4 → cumulative: 3, 8, 10, 14
        tree.push_item({3});
        tree.push_item({5});
        tree.push_item({2});
        tree.push_item({4});

        auto cursor = tree.cursor<ItemSum>();
        cursor.seek({0}, Bias::Left);

        // Slice up to sum=8 (includes items with sum 3 and 5)
        auto prefix = cursor.slice({8});
        QCOMPARE(prefix.summary().count, 2);
        QCOMPARE(prefix.summary().sum, 8);

        auto suffix = cursor.suffix();
        QCOMPARE(suffix.summary().count, 2);
        QCOMPARE(suffix.summary().sum, 6);
    }

    void slice_preserves_total() {
        // For any split point, prefix + suffix should equal the full tree
        TestTree tree;
        int total_sum = 0;
        for (int i = 1; i <= 50; ++i) {
            tree.push_item({i});
            total_sum += i;
        }

        for (int split = 0; split <= 50; ++split) {
            auto cursor = tree.cursor<ItemCount>();
            cursor.seek({0}, Bias::Left);

            auto prefix = cursor.slice({split});
            auto suffix = cursor.suffix();

            QCOMPARE(prefix.summary().count + suffix.summary().count, 50);
            QCOMPARE(prefix.summary().sum + suffix.summary().sum, total_sum);
        }
    }

    // ---- push_tree ----

    void push_tree_empty_into_empty() {
        TestTree a, b;
        a.push_tree(std::move(b));
        QVERIFY(a.empty());
    }

    void push_tree_into_empty() {
        TestTree a, b;
        for (int i = 1; i <= 5; ++i)
            b.push_item({i});

        a.push_tree(std::move(b));
        QCOMPARE(a.summary().count, 5);
        auto items = a.items();
        for (int i = 0; i < 5; ++i)
            QCOMPARE(items[i].value, i + 1);
    }

    void push_tree_empty_into_nonempty() {
        TestTree a, b;
        for (int i = 1; i <= 5; ++i)
            a.push_item({i});

        a.push_tree(std::move(b));
        QCOMPARE(a.summary().count, 5);
    }

    void push_tree_concatenation() {
        TestTree a, b;
        for (int i = 1; i <= 5; ++i)
            a.push_item({i});
        for (int i = 6; i <= 10; ++i)
            b.push_item({i});

        a.push_tree(std::move(b));
        QCOMPARE(a.summary().count, 10);
        QCOMPARE(a.summary().sum, 55);

        auto items = a.items();
        for (int i = 0; i < 10; ++i)
            QCOMPARE(items[i].value, i + 1);
    }

    void push_tree_different_heights() {
        // A has many items (tall tree), B has few (short tree)
        TestTree a, b;
        for (int i = 1; i <= 50; ++i)
            a.push_item({i});
        for (int i = 51; i <= 55; ++i)
            b.push_item({i});

        int total = (55 * 56) / 2;
        a.push_tree(std::move(b));
        QCOMPARE(a.summary().count, 55);
        QCOMPARE(a.summary().sum, total);

        auto items = a.items();
        for (int i = 0; i < 55; ++i)
            QCOMPARE(items[i].value, i + 1);
    }

    void push_tree_short_into_tall() {
        TestTree a, b;
        for (int i = 1; i <= 3; ++i)
            a.push_item({i});
        for (int i = 4; i <= 50; ++i)
            b.push_item({i});

        a.push_tree(std::move(b));
        QCOMPARE(a.summary().count, 50);

        auto items = a.items();
        for (int i = 0; i < 50; ++i)
            QCOMPARE(items[i].value, i + 1);
    }

    // ---- Structural sharing ----

    void slice_shares_structure() {
        // After slicing, modifying the original shouldn't affect the slice
        TestTree tree;
        for (int i = 1; i <= 20; ++i)
            tree.push_item({i});

        auto cursor = tree.cursor<ItemCount>();
        cursor.seek({0}, Bias::Left);
        auto prefix = cursor.slice({10});

        // prefix should have items 1-10
        auto items = prefix.items();
        QCOMPARE(items.size(), 10u);
        for (int i = 0; i < 10; ++i)
            QCOMPARE(items[i].value, i + 1);
    }

    // ---- The CRDT pattern: slice + modify + suffix ----

    void crdt_edit_pattern() {
        // Simulate: document has 20 fragments, edit at position 8
        TestTree tree;
        for (int i = 1; i <= 20; ++i)
            tree.push_item({i});

        // Build new tree: prefix + modified item + suffix
        auto cursor = tree.cursor<ItemCount>();
        cursor.seek({0}, Bias::Left);

        TestTree new_tree;
        new_tree.push_tree(cursor.slice({7}));   // items 1-7
        cursor.next();                            // skip item 8
        new_tree.push_item({99});                 // insert modified item
        new_tree.push_tree(cursor.suffix());      // items 9-20

        QCOMPARE(new_tree.summary().count, 20);
        auto items = new_tree.items();
        // 1,2,3,4,5,6,7,99,9,10,...,20
        for (int i = 0; i < 7; ++i)
            QCOMPARE(items[i].value, i + 1);
        QCOMPARE(items[7].value, 99);
        for (int i = 8; i < 20; ++i)
            QCOMPARE(items[i].value, i + 1);
    }

    void crdt_insert_pattern() {
        // Simulate: insert new item at position 5
        TestTree tree;
        for (int i = 1; i <= 10; ++i)
            tree.push_item({i});

        auto cursor = tree.cursor<ItemCount>();
        cursor.seek({0}, Bias::Left);

        TestTree new_tree;
        new_tree.push_tree(cursor.slice({5}));  // items 1-5
        new_tree.push_item({99});                // new item
        new_tree.push_tree(cursor.suffix());     // items 6-10

        QCOMPARE(new_tree.summary().count, 11);
        auto items = new_tree.items();
        for (int i = 0; i < 5; ++i)
            QCOMPARE(items[i].value, i + 1);
        QCOMPARE(items[5].value, 99);
        for (int i = 6; i < 11; ++i)
            QCOMPARE(items[i].value, i);
    }

    void crdt_delete_pattern() {
        // Simulate: delete items 3-5
        TestTree tree;
        for (int i = 1; i <= 10; ++i)
            tree.push_item({i});

        auto cursor = tree.cursor<ItemCount>();
        cursor.seek({0}, Bias::Left);

        TestTree new_tree;
        new_tree.push_tree(cursor.slice({2}));  // items 1-2

        // Skip items 3-5
        cursor.slice({5});

        new_tree.push_tree(cursor.suffix());     // items 6-10

        QCOMPARE(new_tree.summary().count, 7);
        auto items = new_tree.items();
        QCOMPARE(items[0].value, 1);
        QCOMPARE(items[1].value, 2);
        QCOMPARE(items[2].value, 6);
        QCOMPARE(items[3].value, 7);
    }

    // ---- Stress tests ----

    void stress_push_and_verify() {
        TestTree tree;
        int expected_sum = 0;
        for (int i = 0; i < 1000; ++i) {
            tree.push_item({i});
            expected_sum += i;
        }

        QCOMPARE(tree.summary().count, 1000);
        QCOMPARE(tree.summary().sum, expected_sum);

        // Verify order
        auto items = tree.items();
        for (int i = 0; i < 1000; ++i)
            QCOMPARE(items[i].value, i);
    }

    void stress_seek_all_positions() {
        TestTree tree;
        for (int i = 0; i < 100; ++i)
            tree.push_item({i + 1}); // values 1..100

        for (int pos = 0; pos < 100; ++pos) {
            auto cursor = tree.cursor<ItemCount>();
            QVERIFY(cursor.seek({pos}, Bias::Left));
            QCOMPARE(cursor.item()->value, pos + 1);
            QCOMPARE(cursor.position().value, pos);
        }
    }

    void stress_slice_at_every_position() {
        TestTree tree;
        int total = 0;
        for (int i = 1; i <= 50; ++i) {
            tree.push_item({i});
            total += i;
        }

        for (int split = 0; split <= 50; ++split) {
            auto cursor = tree.cursor<ItemCount>();
            cursor.seek({0}, Bias::Left);

            auto prefix = cursor.slice({split});
            auto suffix = cursor.suffix();

            // Verify counts
            QCOMPARE(prefix.summary().count, split);
            QCOMPARE(suffix.summary().count, 50 - split);

            // Verify order
            auto p_items = prefix.items();
            for (int i = 0; i < split; ++i)
                QCOMPARE(p_items[i].value, i + 1);

            auto s_items = suffix.items();
            for (int i = 0; i < 50 - split; ++i)
                QCOMPARE(s_items[i].value, split + i + 1);
        }
    }

    // ---- Large B value ----

    void large_branching_factor() {
        // Test with B=6 (production value)
        SumTree<IntItem, 6> tree;
        for (int i = 0; i < 500; ++i)
            tree.push_item({i});

        QCOMPARE(tree.summary().count, 500);

        auto cursor = tree.cursor<ItemCount>();
        cursor.seek({0}, Bias::Left);

        auto prefix = cursor.slice({250});
        auto suffix = cursor.suffix();

        QCOMPARE(prefix.summary().count, 250);
        QCOMPARE(suffix.summary().count, 250);

        auto p_items = prefix.items();
        for (int i = 0; i < 250; ++i)
            QCOMPARE(p_items[i].value, i);
    }

    void push_tree_many_small_trees() {
        // Simulate the CRDT pattern of building from many slice results
        TestTree source;
        for (int i = 0; i < 100; ++i)
            source.push_item({i});

        // Build new tree by slicing source into 10 chunks and reassembling
        TestTree result;
        auto cursor = source.cursor<ItemCount>();
        cursor.seek({0}, Bias::Left);

        for (int chunk = 0; chunk < 10; ++chunk) {
            auto slice = cursor.slice({(chunk + 1) * 10});
            result.push_tree(std::move(slice));
        }

        QCOMPARE(result.summary().count, 100);
        QVERIFY(result.has_valid_occupancy());
        QVERIFY(result.has_valid_summaries());
        auto items = result.items();
        for (int i = 0; i < 100; ++i)
            QCOMPARE(items[i].value, i);
    }

    void push_tree_keeps_balanced_occupancy_for_boundary_heights() {
        const std::vector<int> sizes{
            1, 5, 6, 7, 11, 12, 13, 17, 18, 19, 24, 25,
            71, 72, 73, 143, 144, 145, 173, 300};
        for (const int left_size : sizes) {
            for (const int right_size : sizes) {
                TestTree left;
                TestTree right;
                int expected_sum = 0;
                for (int i = 0; i < left_size; ++i) {
                    left.push_item({i + 1});
                    expected_sum += i + 1;
                }
                for (int i = 0; i < right_size; ++i) {
                    right.push_item({left_size + i + 1});
                    expected_sum += left_size + i + 1;
                }
                left.push_tree(std::move(right));

                QVERIFY2(left.has_valid_occupancy(),
                         qPrintable(QString("invalid join shape: %1 + %2")
                                        .arg(left_size).arg(right_size)));
                QVERIFY(left.has_valid_summaries());
                QCOMPARE(left.summary().count, left_size + right_size);
                QCOMPARE(left.summary().sum, expected_sum);
                const auto actual = left.items();
                QCOMPARE(actual.size(), size_t(left_size + right_size));
                for (int i = 0; i < left_size + right_size; ++i)
                    QCOMPARE(actual[size_t(i)].value, i + 1);
            }
        }
    }

    // ---- In-place mutation tests ----

    void for_each_mut_basic() {
        TestTree tree;
        for (int i = 1; i <= 10; ++i)
            tree.push_item({i});

        // Double every value
        tree.for_each_mut([](IntItem& item) {
            item.value *= 2;
        });

        auto items = tree.items();
        QCOMPARE(items.size(), 10u);
        for (int i = 0; i < 10; ++i)
            QCOMPARE(items[i].value, (i + 1) * 2);

        // Verify summaries updated
        QCOMPARE(tree.summary().count, 10);
        QCOMPARE(tree.summary().sum, 110); // 2+4+6+...+20 = 110
        QCOMPARE(tree.summary().max_value, 20);
    }

    void for_each_mut_copy_on_write() {
        // Ensure COW: modifying a shared tree doesn't affect the original
        TestTree tree;
        for (int i = 1; i <= 10; ++i)
            tree.push_item({i});

        // Make a copy (shares structure)
        TestTree copy = tree;

        // Mutate the copy
        copy.for_each_mut([](IntItem& item) {
            item.value = 0;
        });

        // Original should be unchanged
        auto orig_items = tree.items();
        for (int i = 0; i < 10; ++i)
            QCOMPARE(orig_items[i].value, i + 1);
        QCOMPARE(tree.summary().sum, 55);

        // Copy should be all zeros
        QCOMPARE(copy.summary().sum, 0);
    }

    void recompute_all_summaries_basic() {
        TestTree tree;
        for (int i = 1; i <= 10; ++i)
            tree.push_item({i});

        // Recompute without changes should preserve summaries
        tree.recompute_all_summaries();
        QCOMPARE(tree.summary().count, 10);
        QCOMPARE(tree.summary().sum, 55);
        QCOMPARE(tree.summary().max_value, 10);
    }

    void edit_item_basic() {
        TestTree tree;
        for (int i = 1; i <= 10; ++i)
            tree.push_item({i});

        // Edit item at index 4 (value=5), change to 99
        bool found = tree.edit_item(ItemCount{4}, [](IntItem& item) {
            item.value = 99;
        });
        QVERIFY(found);

        auto items = tree.items();
        QCOMPARE(items[4].value, 99);
        // Others unchanged
        QCOMPARE(items[0].value, 1);
        QCOMPARE(items[9].value, 10);

        // Verify summaries: 1+2+3+4+99+6+7+8+9+10 = 149
        QCOMPARE(tree.summary().count, 10);
        QCOMPARE(tree.summary().sum, 149);
        QCOMPARE(tree.summary().max_value, 99);
    }

    void edit_item_not_found() {
        TestTree tree;
        for (int i = 1; i <= 5; ++i)
            tree.push_item({i});

        // Seek past end
        bool found = tree.edit_item(ItemCount{10}, [](IntItem& item) {
            item.value = 99;
        });
        QVERIFY(!found);

        // Tree unchanged
        QCOMPARE(tree.summary().sum, 15);
    }

    void edit_item_by_sum() {
        TestTree tree;
        // Items: 3, 5, 2, 4 -> cumulative: 3, 8, 10, 14
        tree.push_item({3});
        tree.push_item({5});
        tree.push_item({2});
        tree.push_item({4});

        // Edit item at cumulative sum 3 (Left bias = first item with end > 3 = item[1]=5)
        bool found = tree.edit_item(ItemSum{3}, [](IntItem& item) {
            item.value = 50;
        });
        QVERIFY(found);

        auto items = tree.items();
        QCOMPARE(items[0].value, 3);
        QCOMPARE(items[1].value, 50);
        QCOMPARE(items[2].value, 2);
        QCOMPARE(items[3].value, 4);
        QCOMPARE(tree.summary().sum, 59); // 3+50+2+4
    }

    void insert_item_basic() {
        TestTree tree;
        for (int i = 1; i <= 5; ++i)
            tree.push_item({i * 10}); // 10, 20, 30, 40, 50

        // Insert 25 at index 2 (before item with value 30)
        tree.insert_item(ItemCount{2}, IntItem{25});

        auto items = tree.items();
        QCOMPARE(items.size(), 6u);
        QCOMPARE(items[0].value, 10);
        QCOMPARE(items[1].value, 20);
        QCOMPARE(items[2].value, 25);
        QCOMPARE(items[3].value, 30);
        QCOMPARE(items[4].value, 40);
        QCOMPARE(items[5].value, 50);

        QCOMPARE(tree.summary().count, 6);
        QCOMPARE(tree.summary().sum, 175); // 10+20+25+30+40+50
    }

    void insert_item_at_beginning() {
        TestTree tree;
        for (int i = 1; i <= 5; ++i)
            tree.push_item({i * 10});

        // Insert at index 0 (before everything)
        tree.insert_item(ItemCount{0}, IntItem{5});

        auto items = tree.items();
        QCOMPARE(items.size(), 6u);
        QCOMPARE(items[0].value, 5);
        QCOMPARE(items[1].value, 10);
    }

    void insert_item_at_end() {
        TestTree tree;
        for (int i = 1; i <= 5; ++i)
            tree.push_item({i * 10});

        // Insert past end (appends)
        tree.insert_item(ItemCount{5}, IntItem{60});

        auto items = tree.items();
        QCOMPARE(items.size(), 6u);
        QCOMPARE(items[5].value, 60);
    }

    void insert_item_causes_splits() {
        // With B=2 (MaxChildren=4), inserting 20+ items via insert_item
        // should cause multiple splits
        TestTree tree;

        // Insert in reverse order to stress splitting
        for (int i = 20; i >= 1; --i) {
            tree.insert_item(ItemCount{0}, IntItem{i});
        }

        auto items = tree.items();
        QCOMPARE(items.size(), 20u);
        for (int i = 0; i < 20; ++i)
            QCOMPARE(items[i].value, i + 1);

        int expected_sum = 20 * 21 / 2; // 210
        QCOMPARE(tree.summary().count, 20);
        QCOMPARE(tree.summary().sum, expected_sum);
        QCOMPARE(tree.summary().max_value, 20);
    }

    void insert_item_into_empty_tree() {
        TestTree tree;
        tree.insert_item(ItemCount{0}, IntItem{42});

        QCOMPARE(tree.summary().count, 1);
        QCOMPARE(tree.summary().sum, 42);
        auto items = tree.items();
        QCOMPARE(items[0].value, 42);
    }

    void remove_item_basic() {
        TestTree tree;
        for (int i = 1; i <= 10; ++i)
            tree.push_item({i});

        // Remove item at index 4 (value=5)
        bool found = tree.remove_item<ItemCount>(ItemCount{4});
        QVERIFY(found);

        auto items = tree.items();
        QCOMPARE(items.size(), 9u);
        // 1,2,3,4,6,7,8,9,10
        QCOMPARE(items[0].value, 1);
        QCOMPARE(items[3].value, 4);
        QCOMPARE(items[4].value, 6);
        QCOMPARE(items[8].value, 10);

        QCOMPARE(tree.summary().count, 9);
        QCOMPARE(tree.summary().sum, 50); // 55 - 5
    }

    void remove_item_first() {
        TestTree tree;
        for (int i = 1; i <= 5; ++i)
            tree.push_item({i});

        bool found = tree.remove_item<ItemCount>(ItemCount{0});
        QVERIFY(found);

        auto items = tree.items();
        QCOMPARE(items.size(), 4u);
        QCOMPARE(items[0].value, 2);
    }

    void remove_item_last() {
        TestTree tree;
        for (int i = 1; i <= 5; ++i)
            tree.push_item({i});

        bool found = tree.remove_item<ItemCount>(ItemCount{4});
        QVERIFY(found);

        auto items = tree.items();
        QCOMPARE(items.size(), 4u);
        QCOMPARE(items[3].value, 4);
    }

    void remove_item_not_found() {
        TestTree tree;
        for (int i = 1; i <= 5; ++i)
            tree.push_item({i});

        bool found = tree.remove_item<ItemCount>(ItemCount{10});
        QVERIFY(!found);
        QCOMPARE(tree.summary().count, 5);
    }

    void remove_item_all() {
        TestTree tree;
        for (int i = 1; i <= 10; ++i)
            tree.push_item({i});

        // Remove all items one by one from the front
        for (int i = 0; i < 10; ++i) {
            bool found = tree.remove_item<ItemCount>(ItemCount{0});
            QVERIFY(found);
            QCOMPARE(tree.summary().count, 9 - i);
        }

        QVERIFY(tree.empty());
        QCOMPARE(tree.summary().count, 0);
        QCOMPARE(tree.summary().sum, 0);
    }

    void remove_item_all_from_back() {
        TestTree tree;
        for (int i = 1; i <= 10; ++i)
            tree.push_item({i});

        // Remove from the back each time
        for (int remaining = 10; remaining > 0; --remaining) {
            bool found = tree.remove_item<ItemCount>(ItemCount{remaining - 1});
            QVERIFY(found);
        }

        QVERIFY(tree.empty());
    }

    void mixed_operations() {
        TestTree tree;

        // Build initial tree: 10, 20, 30, 40, 50
        for (int i = 1; i <= 5; ++i)
            tree.push_item({i * 10});

        // Insert 25 at index 2 -> 10, 20, 25, 30, 40, 50
        tree.insert_item(ItemCount{2}, IntItem{25});
        QCOMPARE(tree.summary().count, 6);

        // Edit item at index 3 (value=30) to 35 -> 10, 20, 25, 35, 40, 50
        tree.edit_item(ItemCount{3}, [](IntItem& item) {
            item.value = 35;
        });

        // Remove item at index 0 (value=10) -> 20, 25, 35, 40, 50
        tree.remove_item<ItemCount>(ItemCount{0});

        auto items = tree.items();
        QCOMPARE(items.size(), 5u);
        QCOMPARE(items[0].value, 20);
        QCOMPARE(items[1].value, 25);
        QCOMPARE(items[2].value, 35);
        QCOMPARE(items[3].value, 40);
        QCOMPARE(items[4].value, 50);

        QCOMPARE(tree.summary().count, 5);
        QCOMPARE(tree.summary().sum, 170); // 20+25+35+40+50
        QCOMPARE(tree.summary().max_value, 50);
    }

    void insert_and_remove_stress() {
        // Insert 50 items via insert_item, then remove them all
        TestTree tree;
        for (int i = 0; i < 50; ++i) {
            // Insert at position i (append)
            tree.insert_item(ItemCount{i}, IntItem{i + 1});
        }

        QCOMPARE(tree.summary().count, 50);

        // Verify order
        auto items = tree.items();
        for (int i = 0; i < 50; ++i)
            QCOMPARE(items[i].value, i + 1);

        // Remove every other item from front
        for (int i = 0; i < 25; ++i) {
            tree.remove_item<ItemCount>(ItemCount{0});
        }
        QCOMPARE(tree.summary().count, 25);

        // Remove remaining
        for (int i = 0; i < 25; ++i) {
            tree.remove_item<ItemCount>(ItemCount{0});
        }
        QVERIFY(tree.empty());
    }

    void splice_range_preserves_source_and_sequence() {
        TestTree source;
        int source_sum = 0;
        for (int i = 0; i < 100; ++i) {
            source.push_item({i + 1});
            source_sum += i + 1;
        }
        TestTree replacement;
        replacement.push_item({900});
        replacement.push_item({901});

        auto candidate = source.splice_range<ItemCount>(ItemCount{17},
                                                        ItemCount{83},
                                                        std::move(replacement));
        QVERIFY(candidate.has_value());
        QVERIFY(candidate->has_valid_occupancy());
        QCOMPARE(candidate->summary().count, 36);
        const int removed_sum = (18 + 83) * 66 / 2;
        QCOMPARE(candidate->summary().sum, source_sum - removed_sum + 1801);

        const auto values = candidate->items();
        QCOMPARE(values.size(), size_t{36});
        for (int i = 0; i < 17; ++i) QCOMPARE(values[size_t(i)].value, i + 1);
        QCOMPARE(values[17].value, 900);
        QCOMPARE(values[18].value, 901);
        for (int i = 19; i < 36; ++i)
            QCOMPARE(values[size_t(i)].value, i + 65);

        QCOMPARE(source.summary().count, 100);
        QCOMPARE(source.summary().sum, source_sum);
        const auto original = source.items();
        for (int i = 0; i < 100; ++i)
            QCOMPARE(original[size_t(i)].value, i + 1);
    }

    void splice_range_empty_insertion_is_a_shared_noop() {
        TestTree source;
        for (int i = 0; i < 50; ++i) source.push_item({i + 1});
        TestTree empty;

        const auto candidate = source.splice_range<ItemCount>(
            ItemCount{23}, ItemCount{23}, std::move(empty));
        QVERIFY(candidate.has_value());
        QCOMPARE(candidate->summary(), source.summary());
        const auto actual = candidate->items();
        QCOMPARE(actual.size(), size_t{50});
        for (size_t i = 0; i < actual.size(); ++i)
            QCOMPARE(actual[i].value, int(i + 1));
    }

    void splice_range_checks_order_bounds_and_item_alignment() {
        TestTree source;
        for (int i = 1; i <= 5; ++i) source.push_item({i});
        TestTree replacement;
        replacement.push_item({99});

        QVERIFY(!source.splice_range<ItemCount>(ItemCount{4}, ItemCount{2},
                                                replacement));
        QVERIFY(!source.splice_range<ItemCount>(ItemCount{2}, ItemCount{6},
                                                replacement));
        TestTree weighted;
        weighted.push_item({3});
        weighted.push_item({5});
        weighted.push_item({2});
        QVERIFY(!weighted.splice_range<ItemSum>(ItemSum{1}, ItemSum{4},
                                                replacement));
        QCOMPARE(source.items().size(), size_t{5});
        QCOMPARE(source.summary().sum, 15);
    }

    void splice_range_matches_vector_oracle_for_every_small_boundary_pair() {
        constexpr int source_size = 80;
        TestTree source;
        std::vector<int> expected_source;
        for (int i = 0; i < source_size; ++i) {
            source.push_item({i + 1});
            expected_source.push_back(i + 1);
        }

        for (int begin = 0; begin <= source_size; ++begin) {
            for (int end = begin; end <= source_size; ++end) {
                TestTree replacement;
                std::vector<int> expected = expected_source;
                if ((begin + end) % 3 != 0) {
                    replacement.push_item({1001});
                    expected.insert(expected.begin() + begin, 1001);
                }
                if ((begin + end) % 5 == 0) {
                    replacement.push_item({1002});
                    expected.insert(expected.begin() + begin +
                                        ((begin + end) % 3 != 0 ? 1 : 0),
                                    1002);
                }
                expected.erase(expected.begin() + begin +
                                   ((begin + end) % 3 != 0 ? 1 : 0) +
                                   ((begin + end) % 5 == 0 ? 1 : 0),
                               expected.begin() + end +
                                   ((begin + end) % 3 != 0 ? 1 : 0) +
                                   ((begin + end) % 5 == 0 ? 1 : 0));

                auto candidate = source.splice_range<ItemCount>(
                    ItemCount{begin}, ItemCount{end}, std::move(replacement));
                QVERIFY2(candidate.has_value(), "valid item boundary was refused");
                QVERIFY2(candidate->has_valid_occupancy(),
                         qPrintable(QString("splice produced an underfilled or uneven tree: [%1,%2)")
                                        .arg(begin).arg(end)));
                QVERIFY2(candidate->has_valid_summaries(),
                         qPrintable(QString("splice produced a stale subtree summary: [%1,%2)")
                                        .arg(begin).arg(end)));
                const auto actual = candidate->items();
                QVERIFY2(candidate->summary().count == int(expected.size()),
                         qPrintable(QString("splice count mismatch for [%1,%2): got %3 expected %4, actual items %5")
                                        .arg(begin).arg(end)
                                        .arg(candidate->summary().count)
                                        .arg(expected.size()).arg(actual.size())));
                int expected_sum = std::accumulate(expected.begin(), expected.end(), 0);
                QCOMPARE(candidate->summary().sum, expected_sum);
                QCOMPARE(actual.size(), expected.size());
                for (size_t i = 0; i < expected.size(); ++i)
                    QCOMPARE(actual[i].value, expected[i]);
            }
        }
        QCOMPARE(source.summary().count, source_size);
        QCOMPARE(source.summary().sum, source_size * (source_size + 1) / 2);
    }

    void splice_range_preserves_deep_tree_occupancy_and_summaries() {
        constexpr int source_size = 5000;
        TestTree source;
        std::vector<int> original;
        original.reserve(source_size);
        for (int i = 0; i < source_size; ++i) {
            source.push_item({i + 1});
            original.push_back(i + 1);
        }

        for (int sample = 0; sample < 200; ++sample) {
            const int begin = (sample * 7919) % (source_size + 1);
            const int end = begin +
                ((sample * 53) % (source_size - begin + 1));
            const int inserted = sample % 4;
            TestTree replacement;
            std::vector<int> expected(original.begin(), original.begin() + begin);
            for (int i = 0; i < inserted; ++i) {
                const int value = 100001 + sample * 4 + i;
                replacement.push_item({value});
                expected.push_back(value);
            }
            expected.insert(expected.end(), original.begin() + end, original.end());

            auto candidate = source.splice_range<ItemCount>(
                ItemCount{begin}, ItemCount{end}, std::move(replacement));
            QVERIFY(candidate.has_value());
            QVERIFY(candidate->has_valid_occupancy());
            QVERIFY(candidate->has_valid_summaries());
            QCOMPARE(candidate->summary().count, int(expected.size()));
            QCOMPARE(candidate->summary().sum,
                     std::accumulate(expected.begin(), expected.end(), 0));
            const auto actual = candidate->items();
            QCOMPARE(actual.size(), expected.size());
            for (size_t i = 0; i < expected.size(); ++i)
                QCOMPARE(actual[i].value, expected[i]);
        }
    }
};

QTEST_MAIN(tst_sumtree)
#include "tst_sumtree.moc"
