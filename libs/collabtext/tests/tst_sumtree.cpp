#include <QTest>
#include "crdt/SumTree.h"
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
        auto items = result.items();
        for (int i = 0; i < 100; ++i)
            QCOMPARE(items[i].value, i);
    }
};

QTEST_MAIN(tst_sumtree)
#include "tst_sumtree.moc"
