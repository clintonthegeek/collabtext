# In-Place SumTree Mutations Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add O(log n) in-place mutation operations to SumTree and use them in Buffer's edit paths, eliminating the O(n) extract-mutate-rebuild pattern for common-case edits.

**Architecture:** Add standalone recursive methods to SumTree (`edit_item`, `insert_item`, `remove_item`) that seek to a position, mutate the leaf, and propagate changes up via natural recursion. No stateful MutableCursor class — each operation does seek+mutate+propagate in one call. Buffer gains fast paths that use these for common-case remote edits (O(log n)) with fallback to the existing extract-mutate-rebuild pattern for complex cases (relocations, normalization).

**Tech Stack:** C++20, Qt6 Test, CMake

**Spec:** `docs/superpowers/specs/2026-04-05-inplace-mutations-design.md`

**Build/test commands:**
```bash
cmake -S . -B build-dev -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-dev -j$(nproc)
ctest --test-dir build-dev --output-on-failure -E "tst_realistic|tst_benchmark"
```

---

## File Map

| File | Action | Responsibility |
|------|--------|---------------|
| `libs/collabtext/src/crdt/SumTree.h` | Modify | Add edit_item, insert_item, remove_item, for_each_mut, recompute_all_summaries |
| `libs/collabtext/src/crdt/Buffer.h` | Modify | Change FRAG_TREE_B from 2 to 6 |
| `libs/collabtext/src/crdt/Buffer.cpp` | Modify | Add fast paths for apply_remote_edit, apply_remote_undo, apply_local_edit |
| `libs/collabtext/tests/tst_sumtree.cpp` | Modify | Add tests for new SumTree mutation operations |

---

### Task 1: Increase FRAG_TREE_B from 2 to 6

One-line change with significant impact. All existing tests must pass — the B
factor only affects tree shape, not semantics.

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.h:24`

- [ ] **Step 1: Change the constant**

In `libs/collabtext/src/crdt/Buffer.h`, change:
```cpp
static constexpr std::size_t FRAG_TREE_B = 6;
```

- [ ] **Step 2: Build and run all tests**

```bash
cmake --build build-dev -j$(nproc)
ctest --test-dir build-dev --output-on-failure -E "tst_realistic|tst_benchmark"
```

All 13 fast tests must pass. The only difference is tree structure (wider
nodes, shallower tree).

- [ ] **Step 3: Commit**

```bash
git add libs/collabtext/src/crdt/Buffer.h
git commit -m "perf: increase FRAG_TREE_B from 2 to 6 (MaxChildren 4 → 12)

Wider nodes mean shallower trees: depth for 1500 fragments drops
from ~6 levels to ~3. All tree operations touch fewer nodes."
```

---

### Task 2: SumTree — for_each_mut and recompute_all_summaries

Two simple additions to SumTree that enable in-place item modification.

**Files:**
- Modify: `libs/collabtext/src/crdt/SumTree.h`
- Modify: `libs/collabtext/tests/tst_sumtree.cpp`

- [ ] **Step 1: Add for_each_mut to SumTree**

Add in the public section of SumTree (after for_each, around line 414):

```cpp
    /// Iterate all items mutably. Calls ensure_mutable on each node.
    template<typename F>
    void for_each_mut(F&& fn) {
        if (m_root) for_each_node_mut(ensure_mutable(m_root), fn);
    }
```

Add the private recursive helper (after for_each_node, around line 793):

```cpp
    template<typename F>
    static void for_each_node_mut(NodePtr& node, F& fn) {
        ensure_mutable(node);
        if (node->is_leaf()) {
            auto& lf = node->leaf();
            for (uint16_t i = 0; i < lf.count; ++i) {
                fn(lf.items[i]);
                lf.item_summaries[i] = lf.items[i].summary();
            }
            recompute_summary(*node);
        } else {
            auto& in = node->internal();
            for (uint16_t i = 0; i < in.count; ++i) {
                for_each_node_mut(ensure_mutable(in.children[i]), fn);
                in.child_summaries[i] = in.children[i]->summary;
            }
            recompute_summary(*node);
        }
    }
```

- [ ] **Step 2: Add recompute_all_summaries to SumTree**

Add in the public section:

```cpp
    /// Recompute all summaries bottom-up. Call after modifying items
    /// via for_each_mut when the modification changes summaries.
    void recompute_all_summaries() {
        if (m_root) recompute_all_recursive(ensure_mutable(m_root));
    }
```

Add the private helper:

```cpp
    static void recompute_all_recursive(NodePtr& node) {
        ensure_mutable(node);
        if (node->is_leaf()) {
            auto& lf = node->leaf();
            for (uint16_t i = 0; i < lf.count; ++i) {
                lf.item_summaries[i] = lf.items[i].summary();
            }
        } else {
            auto& in = node->internal();
            for (uint16_t i = 0; i < in.count; ++i) {
                recompute_all_recursive(ensure_mutable(in.children[i]));
                in.child_summaries[i] = in.children[i]->summary;
            }
        }
        recompute_summary(*node);
    }
```

- [ ] **Step 3: Add tests for for_each_mut**

In `tst_sumtree.cpp`, add a test slot:

```cpp
void TestSumTree::for_each_mut_basic()
{
    // Build a tree with known items
    using Tree = SumTree<Fragment, 2>;  // B=2 for aggressive splitting
    Tree tree;
    Buffer buf(1);
    for (int i = 0; i < 20; ++i) {
        Fragment f(Lamport(1, i), Locator::min(), 1, 1, std::string(1, 'a' + (i % 26)));
        f.visible = true;
        tree.push_item(std::move(f));
    }

    QCOMPARE(tree.summary().visible_bytes, 20u);

    // Mutate all items: make them invisible
    tree.for_each_mut([](Fragment& f) {
        f.visible = false;
    });

    // Summaries should reflect the change
    QCOMPARE(tree.summary().visible_bytes, 0u);
    QCOMPARE(tree.summary().deleted_bytes, 20u);

    // Mutate back
    tree.for_each_mut([](Fragment& f) {
        f.visible = true;
    });
    QCOMPARE(tree.summary().visible_bytes, 20u);
}
```

- [ ] **Step 4: Build and test**

```bash
cmake --build build-dev --target tst_sumtree -j$(nproc)
./build-dev/libs/collabtext/tst_sumtree for_each_mut_basic -v2
```

- [ ] **Step 5: Commit**

```bash
git add libs/collabtext/src/crdt/SumTree.h libs/collabtext/tests/tst_sumtree.cpp
git commit -m "feat(SumTree): add for_each_mut and recompute_all_summaries

for_each_mut iterates all items mutably, updating item summaries
and recomputing node summaries bottom-up.

recompute_all_summaries does a full bottom-up recomputation."
```

---

### Task 3: SumTree — edit_item (in-place modification)

The simplest mutation: seek to an item by dimension, modify it in place,
propagate summary changes up to root. O(log n).

**Files:**
- Modify: `libs/collabtext/src/crdt/SumTree.h`
- Modify: `libs/collabtext/tests/tst_sumtree.cpp`

- [ ] **Step 1: Add edit_item to SumTree**

Add in the public section:

```cpp
    /// Find the item at `target` in dimension D, call fn(item), then
    /// propagate summary changes to root. O(log n).
    /// Returns true if an item was found and edited, false if past end.
    template<typename D, typename F>
        requires DimensionOf<D, Summary>
    bool edit_item(const D& target, F&& fn, Bias bias = Bias::Left) {
        if (!m_root) return false;
        return edit_item_recursive<D>(ensure_mutable(m_root), target, fn, bias,
                                       D::zero());
    }
```

Add the private recursive helper:

```cpp
    template<typename D, typename F>
    bool edit_item_recursive(NodePtr& node, const D& target, F& fn,
                              Bias bias, D position) {
        ensure_mutable(node);

        if (node->is_leaf()) {
            auto& lf = node->leaf();
            for (uint16_t i = 0; i < lf.count; ++i) {
                D item_end = position;
                item_end.add_summary(lf.item_summaries[i]);
                bool past = (bias == Bias::Left)
                    ? (target < item_end) : !(item_end < target);
                if (past) {
                    fn(lf.items[i]);
                    lf.item_summaries[i] = lf.items[i].summary();
                    recompute_summary(*node);
                    return true;
                }
                position = item_end;
            }
            return false;
        }

        auto& in = node->internal();
        for (uint16_t i = 0; i < in.count; ++i) {
            D child_end = position;
            child_end.add_summary(in.child_summaries[i]);
            bool past = (bias == Bias::Left)
                ? (target < child_end) : !(child_end < target);
            if (past) {
                bool found = edit_item_recursive<D>(
                    ensure_mutable(in.children[i]), target, fn, bias, position);
                if (found) {
                    in.child_summaries[i] = in.children[i]->summary;
                    recompute_summary(*node);
                }
                return found;
            }
            position = child_end;
        }
        return false;
    }
```

- [ ] **Step 2: Add test for edit_item**

```cpp
void TestSumTree::edit_item_basic()
{
    using Tree = SumTree<Fragment, 2>;
    Tree tree;
    // Build tree: 10 visible fragments, each 1 byte
    for (int i = 0; i < 10; ++i) {
        Fragment f(Lamport(1, i), Locator::min(), 1, 1, std::string(1, 'a' + i));
        f.visible = true;
        tree.push_item(std::move(f));
    }
    QCOMPARE(tree.summary().visible_bytes, 10u);

    // Edit the 5th fragment (VisibleOffset 4): make it invisible
    bool found = tree.edit_item<VisibleOffset>({4}, [](Fragment& f) {
        f.visible = false;
    });
    QVERIFY(found);
    QCOMPARE(tree.summary().visible_bytes, 9u);
    QCOMPARE(tree.summary().deleted_bytes, 1u);

    // Edit by FragmentOrderDim: find fragment with origin (1,7)
    found = tree.edit_item<FragmentOrderDim>(
        {Locator::min(), Lamport(1, 7)},
        [](Fragment& f) { f.visible = false; });
    QVERIFY(found);
    QCOMPARE(tree.summary().visible_bytes, 8u);
}
```

- [ ] **Step 3: Build and test**

```bash
cmake --build build-dev --target tst_sumtree -j$(nproc)
./build-dev/libs/collabtext/tst_sumtree edit_item_basic -v2
```

- [ ] **Step 4: Commit**

```bash
git add libs/collabtext/src/crdt/SumTree.h libs/collabtext/tests/tst_sumtree.cpp
git commit -m "feat(SumTree): add edit_item — O(log n) in-place modification"
```

---

### Task 4: SumTree — insert_item (B-tree insertion)

Insert an item at a position determined by a dimension seek. Handles leaf
splitting and propagates overflow up to root. O(log n).

**Files:**
- Modify: `libs/collabtext/src/crdt/SumTree.h`
- Modify: `libs/collabtext/tests/tst_sumtree.cpp`

- [ ] **Step 1: Add insert_item to SumTree**

Add in the public section:

```cpp
    /// Insert an item at the position determined by seeking dimension D.
    /// The item is inserted before the first item where D >= target.
    /// If no such item exists, inserts at the end.
    /// Handles leaf splitting and tree growth. O(log n).
    template<typename D>
        requires DimensionOf<D, Summary>
    void insert_item(const D& target, Item item, Bias bias = Bias::Left) {
        Summary item_sum = item.summary();
        if (!m_root) {
            m_root = make_leaf();
        }
        auto overflow = insert_item_recursive<D>(
            ensure_mutable(m_root), target, std::move(item), item_sum,
            bias, D::zero());
        if (overflow) {
            auto new_root = make_internal(m_root->height + 1);
            auto& in = new_root->internal();
            in.children[0] = std::move(m_root);
            in.child_summaries[0] = in.children[0]->summary;
            in.children[1] = std::move(overflow);
            in.child_summaries[1] = in.children[1]->summary;
            in.count = 2;
            recompute_summary(*new_root);
            m_root = std::move(new_root);
        }
    }
```

Add the private recursive helper:

```cpp
    template<typename D>
    NodePtr insert_item_recursive(NodePtr& node, const D& target,
                                   Item item, const Summary& item_sum,
                                   Bias bias, D position) {
        ensure_mutable(node);

        if (node->is_leaf()) {
            auto& lf = node->leaf();
            // Find insertion index
            uint16_t insert_idx = lf.count; // default: end
            for (uint16_t i = 0; i < lf.count; ++i) {
                D item_end = position;
                item_end.add_summary(lf.item_summaries[i]);
                bool past = (bias == Bias::Left)
                    ? (target < item_end) : !(item_end < target);
                if (past) {
                    insert_idx = i;
                    break;
                }
                position = item_end;
            }

            if (lf.count < MaxChildren) {
                // Room — shift right and insert
                for (uint16_t i = lf.count; i > insert_idx; --i) {
                    lf.items[i] = std::move(lf.items[i - 1]);
                    lf.item_summaries[i] = lf.item_summaries[i - 1];
                }
                lf.items[insert_idx] = std::move(item);
                lf.item_summaries[insert_idx] = item_sum;
                lf.count++;
                recompute_summary(*node);
                return nullptr;
            }

            // Leaf full — split and insert into appropriate half
            return split_leaf_and_insert(node, insert_idx, std::move(item), item_sum);
        }

        // Internal node — find which child to descend into
        auto& in = node->internal();
        uint16_t child_idx = in.count - 1; // default: last child
        for (uint16_t i = 0; i < in.count; ++i) {
            D child_end = position;
            child_end.add_summary(in.child_summaries[i]);
            bool past = (bias == Bias::Left)
                ? (target < child_end) : !(child_end < target);
            if (past) {
                child_idx = i;
                break;
            }
            position = child_end;
        }

        auto overflow = insert_item_recursive<D>(
            ensure_mutable(in.children[child_idx]),
            target, std::move(item), item_sum, bias, position);

        in.child_summaries[child_idx] = in.children[child_idx]->summary;
        recompute_summary(*node);

        if (!overflow) return nullptr;

        // Accommodate overflow
        uint16_t insert_after = child_idx + 1;
        if (in.count < MaxChildren) {
            for (uint16_t i = in.count; i > insert_after; --i) {
                in.children[i] = std::move(in.children[i - 1]);
                in.child_summaries[i] = in.child_summaries[i - 1];
            }
            in.children[insert_after] = std::move(overflow);
            in.child_summaries[insert_after] = in.children[insert_after]->summary;
            in.count++;
            recompute_summary(*node);
            return nullptr;
        }

        return split_internal_and_insert_at(node, insert_after, std::move(overflow));
    }

    /// Split a full leaf and insert item at the given index.
    static NodePtr split_leaf_and_insert(NodePtr& node, uint16_t insert_idx,
                                          Item item, const Summary& item_sum) {
        auto& lf = node->leaf();
        assert(lf.count == MaxChildren);

        // Collect all items + the new one into a temp buffer
        uint16_t total = MaxChildren + 1;
        uint16_t mid = total / 2;

        // Build right half
        auto right = make_leaf();
        auto& rlf = right->leaf();

        // Conceptually: items[0..insert_idx) + item + items[insert_idx..MaxChildren)
        // Split at mid. Left keeps [0..mid), right gets [mid..total).
        uint16_t src = 0, dst_left = 0, dst_right = 0;
        bool inserted = false;

        for (uint16_t logical = 0; logical < total; ++logical) {
            Item* cur_item;
            Summary cur_sum;
            if (logical == insert_idx && !inserted) {
                cur_item = &item;
                cur_sum = item_sum;
                inserted = true;
            } else {
                cur_item = &lf.items[src];
                cur_sum = lf.item_summaries[src];
                src++;
            }

            if (logical < mid) {
                if (logical != insert_idx || inserted) {
                    // Item is already in lf or needs to be placed in lf
                }
                // Write to left (in-place reuse of node)
                lf.items[dst_left] = std::move(*cur_item);
                lf.item_summaries[dst_left] = cur_sum;
                dst_left++;
            } else {
                rlf.items[dst_right] = std::move(*cur_item);
                rlf.item_summaries[dst_right] = cur_sum;
                dst_right++;
            }
        }

        lf.count = dst_left;
        rlf.count = dst_right;
        recompute_summary(*node);
        recompute_summary(*right);
        return right;
    }

    /// Split a full internal node, inserting overflow at the given index.
    static NodePtr split_internal_and_insert_at(NodePtr& node, uint16_t insert_idx,
                                                  NodePtr overflow) {
        auto& in = node->internal();
        assert(in.count == MaxChildren);

        uint16_t total = MaxChildren + 1;
        uint16_t mid = total / 2;

        auto right = make_internal(node->height);
        auto& rin = right->internal();

        // Collect children: in.children[0..insert_idx) + overflow + in.children[insert_idx..MaxChildren)
        // Split at mid.
        std::array<NodePtr, MaxChildren + 1> temp_children{};
        std::array<Summary, MaxChildren + 1> temp_sums{};
        uint16_t src = 0;
        for (uint16_t logical = 0; logical < total; ++logical) {
            if (logical == insert_idx) {
                temp_children[logical] = std::move(overflow);
                temp_sums[logical] = temp_children[logical]->summary;
            } else {
                temp_children[logical] = std::move(in.children[src]);
                temp_sums[logical] = in.child_summaries[src];
                src++;
            }
        }

        // Left keeps [0..mid)
        for (uint16_t i = 0; i < mid; ++i) {
            in.children[i] = std::move(temp_children[i]);
            in.child_summaries[i] = temp_sums[i];
        }
        // Clear leftover slots
        for (uint16_t i = mid; i < MaxChildren; ++i) {
            in.children[i] = nullptr;
        }
        in.count = mid;

        // Right gets [mid..total)
        for (uint16_t i = mid; i < total; ++i) {
            rin.children[rin.count] = std::move(temp_children[i]);
            rin.child_summaries[rin.count] = temp_sums[i];
            rin.count++;
        }

        recompute_summary(*node);
        recompute_summary(*right);
        return right;
    }
```

- [ ] **Step 2: Add tests for insert_item**

```cpp
void TestSumTree::insert_item_basic()
{
    using Tree = SumTree<Fragment, 2>;  // B=2 for aggressive splitting
    Tree tree;

    // Build: fragments at origins 0,1,2,3,4 with locator min
    for (int i = 0; i < 5; ++i) {
        Fragment f(Lamport(1, i * 2), Locator::min(), 1, 1,
                   std::string(1, 'a' + i));
        f.visible = true;
        tree.push_item(std::move(f));
    }
    QCOMPARE(tree.summary().visible_bytes, 5u);

    // Insert at origin 3 (between existing 2 and 4)
    Fragment ins(Lamport(1, 3), Locator::min(), 1, 1, "X");
    ins.visible = true;
    tree.insert_item<FragmentOrderDim>(
        {Locator::min(), Lamport(1, 3)}, std::move(ins));

    // Verify: 6 items, all visible
    QCOMPARE(tree.summary().visible_bytes, 6u);

    // Verify order: origins should be 0,2,3,4,6,8
    std::vector<uint32_t> origins;
    tree.for_each([&](const Fragment& f) { origins.push_back(f.origin.value); });
    QCOMPARE(origins.size(), 6u);
    QCOMPARE(origins[0], 0u);
    QCOMPARE(origins[1], 2u);
    QCOMPARE(origins[2], 3u);  // inserted
    QCOMPARE(origins[3], 4u);
    QCOMPARE(origins[4], 6u);
    QCOMPARE(origins[5], 8u);
}

void TestSumTree::insert_item_causes_splits()
{
    using Tree = SumTree<Fragment, 2>;  // MaxChildren=4
    Tree tree;

    // Insert 20 items via insert_item (not push_item) to test splitting
    for (int i = 0; i < 20; ++i) {
        Fragment f(Lamport(1, i), Locator::min(), 1, 1,
                   std::string(1, 'a' + (i % 26)));
        f.visible = true;
        tree.insert_item<FragmentOrderDim>(
            {Locator::min(), Lamport(1, i)}, std::move(f));
    }

    QCOMPARE(tree.summary().visible_bytes, 20u);

    // Verify all items present and ordered
    uint32_t prev = 0;
    int count = 0;
    tree.for_each([&](const Fragment& f) {
        if (count > 0) QVERIFY(f.origin.value >= prev);
        prev = f.origin.value;
        count++;
    });
    QCOMPARE(count, 20);
}
```

- [ ] **Step 3: Build and test**

```bash
cmake --build build-dev --target tst_sumtree -j$(nproc)
./build-dev/libs/collabtext/tst_sumtree insert_item_basic -v2
./build-dev/libs/collabtext/tst_sumtree insert_item_causes_splits -v2
```

- [ ] **Step 4: Commit**

```bash
git add libs/collabtext/src/crdt/SumTree.h libs/collabtext/tests/tst_sumtree.cpp
git commit -m "feat(SumTree): add insert_item — O(log n) positional insertion

B-tree insertion with leaf splitting and overflow propagation.
Inserts before the first item where dimension >= target."
```

---

### Task 5: SumTree — remove_item (B-tree removal)

Remove an item at a position determined by dimension seek. Handles leaf
underflow by merging with siblings. O(log n).

**Files:**
- Modify: `libs/collabtext/src/crdt/SumTree.h`
- Modify: `libs/collabtext/tests/tst_sumtree.cpp`

- [ ] **Step 1: Add remove_item to SumTree**

Add in the public section:

```cpp
    /// Remove the item at `target` in dimension D.
    /// Returns true if an item was found and removed.
    /// Handles leaf underflow and tree height reduction. O(log n).
    template<typename D>
        requires DimensionOf<D, Summary>
    bool remove_item(const D& target, Bias bias = Bias::Left) {
        if (!m_root) return false;
        bool found = remove_item_recursive<D>(
            ensure_mutable(m_root), target, bias, D::zero());
        if (found && m_root && !m_root->is_leaf()) {
            auto& in = m_root->internal();
            if (in.count == 1) {
                m_root = std::move(in.children[0]);
            } else if (in.count == 0) {
                m_root = nullptr;
            }
        }
        if (found && m_root && m_root->is_leaf() && m_root->leaf().count == 0) {
            m_root = nullptr;
        }
        return found;
    }
```

Add the private recursive helper:

```cpp
    template<typename D>
    bool remove_item_recursive(NodePtr& node, const D& target,
                                Bias bias, D position) {
        ensure_mutable(node);

        if (node->is_leaf()) {
            auto& lf = node->leaf();
            for (uint16_t i = 0; i < lf.count; ++i) {
                D item_end = position;
                item_end.add_summary(lf.item_summaries[i]);
                bool past = (bias == Bias::Left)
                    ? (target < item_end) : !(item_end < target);
                if (past) {
                    // Remove item at index i: shift left
                    for (uint16_t j = i; j + 1 < lf.count; ++j) {
                        lf.items[j] = std::move(lf.items[j + 1]);
                        lf.item_summaries[j] = lf.item_summaries[j + 1];
                    }
                    lf.count--;
                    recompute_summary(*node);
                    return true;
                }
                position = item_end;
            }
            return false;
        }

        auto& in = node->internal();
        for (uint16_t i = 0; i < in.count; ++i) {
            D child_end = position;
            child_end.add_summary(in.child_summaries[i]);
            bool past = (bias == Bias::Left)
                ? (target < child_end) : !(child_end < target);
            if (past) {
                bool found = remove_item_recursive<D>(
                    ensure_mutable(in.children[i]), target, bias, position);
                if (!found) return false;

                in.child_summaries[i] = in.children[i]->summary;

                // Check for underflow (leaf or internal with 0 items/children)
                bool underflow = false;
                if (in.children[i]->is_leaf()) {
                    underflow = in.children[i]->leaf().count == 0;
                } else {
                    underflow = in.children[i]->internal().count == 0;
                }

                if (underflow) {
                    // Remove the empty child
                    for (uint16_t j = i; j + 1 < in.count; ++j) {
                        in.children[j] = std::move(in.children[j + 1]);
                        in.child_summaries[j] = in.child_summaries[j + 1];
                    }
                    in.children[in.count - 1] = nullptr;
                    in.count--;
                }

                recompute_summary(*node);
                return true;
            }
            position = child_end;
        }
        return false;
    }
```

Note: This uses a simplified underflow strategy — when a child becomes
empty, it's simply removed. No sibling borrowing or proactive merging. This
is correct (the tree remains valid) and sufficient because remove_item is
only used for individual fragment removal, not bulk operations. The tree may
become slightly unbalanced but within acceptable bounds.

- [ ] **Step 2: Add tests for remove_item**

```cpp
void TestSumTree::remove_item_basic()
{
    using Tree = SumTree<Fragment, 2>;
    Tree tree;

    for (int i = 0; i < 10; ++i) {
        Fragment f(Lamport(1, i), Locator::min(), 1, 1,
                   std::string(1, 'a' + i));
        f.visible = true;
        tree.push_item(std::move(f));
    }
    QCOMPARE(tree.summary().visible_bytes, 10u);

    // Remove item at origin (1,5) via FragmentOrderDim
    bool found = tree.remove_item<FragmentOrderDim>(
        {Locator::min(), Lamport(1, 5)});
    QVERIFY(found);
    QCOMPARE(tree.summary().visible_bytes, 9u);

    // Verify item gone
    bool has_5 = false;
    tree.for_each([&](const Fragment& f) {
        if (f.origin.value == 5) has_5 = true;
    });
    QVERIFY(!has_5);

    // Remove all items one by one
    for (int i = 0; i < 10; ++i) {
        if (i == 5) continue; // already removed
        found = tree.remove_item<FragmentOrderDim>(
            {Locator::min(), Lamport(1, i)});
        QVERIFY(found);
    }
    QVERIFY(tree.empty());
}

void TestSumTree::remove_item_not_found()
{
    using Tree = SumTree<Fragment, 2>;
    Tree tree;

    Fragment f(Lamport(1, 0), Locator::min(), 1, 1, "a");
    f.visible = true;
    tree.push_item(std::move(f));

    // Try to remove past end
    bool found = tree.remove_item<VisibleOffset>({100});
    QVERIFY(!found);
    QCOMPARE(tree.summary().visible_bytes, 1u);
}
```

- [ ] **Step 3: Build and test**

```bash
cmake --build build-dev --target tst_sumtree -j$(nproc)
./build-dev/libs/collabtext/tst_sumtree remove_item_basic -v2
./build-dev/libs/collabtext/tst_sumtree remove_item_not_found -v2
```

- [ ] **Step 4: Commit**

```bash
git add libs/collabtext/src/crdt/SumTree.h libs/collabtext/tests/tst_sumtree.cpp
git commit -m "feat(SumTree): add remove_item — O(log n) positional removal

Simplified underflow handling: empty children are removed.
No sibling borrowing (sufficient for single-item removal)."
```

---

### Task 6: Buffer — apply_remote_undo fast path

Use for_each_mut to recompute visibility in-place instead of
get_fragments/set_fragments.

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.cpp`

- [ ] **Step 1: Rewrite apply_remote_undo**

Replace the current apply_remote_undo (and the undo/redo methods that call
get_fragments/set_fragments). The key change is replacing:

```cpp
auto frags = get_fragments();
set_fragments(std::move(frags));
```

with:

```cpp
m_fragment_tree.for_each_mut([this](Fragment& f) {
    f.visible = f.compute_visible(m_undo_map);
});
```

Full replacement for apply_remote_undo:

```cpp
bool Buffer::apply_remote_undo(const UndoOperation &op) {
    if (m_version.observed(op.timestamp))
        return true;

    if (!m_version.observed_all(op.version))
        return false;

    for (auto &[edit_id, count] : op.counts) {
        m_undo_map.insert(UndoMapEntry{{edit_id, op.timestamp}, count});
    }

    m_clock.observe(op.timestamp);
    m_version.observe(op.timestamp);
    m_version.join(op.version);

    // Recompute visibility in-place (no vector copy, no tree rebuild)
    m_fragment_tree.for_each_mut([this](Fragment& f) {
        f.visible = f.compute_visible(m_undo_map);
    });

    return true;
}
```

Apply the same pattern to local undo() and redo():

```cpp
std::optional<Operation> Buffer::undo() {
    if (m_undo_cursor == 0)
        return std::nullopt;

    m_undo_cursor--;
    auto &entry = m_undo_stack[m_undo_cursor];

    UndoOperation op;
    op.version = m_version;
    op.timestamp = m_clock.tick();

    for (auto &key : entry.inserted_keys) {
        Lamport edit_id(key.replica_id, key.lamport_value);
        uint32_t current = m_undo_map.undo_count(edit_id);
        m_undo_map.insert(UndoMapEntry{{edit_id, op.timestamp}, current + 1});
        op.counts.push_back({edit_id, current + 1});
    }

    if (entry.had_deletions) {
        uint32_t current = m_undo_map.undo_count(entry.deletion_id);
        m_undo_map.insert(
            UndoMapEntry{{entry.deletion_id, op.timestamp}, current + 1});
        op.counts.push_back({entry.deletion_id, current + 1});
    }

    // In-place visibility recompute
    m_fragment_tree.for_each_mut([this](Fragment& f) {
        f.visible = f.compute_visible(m_undo_map);
    });

    m_version.observe(op.timestamp);
    return op;
}
```

Same for redo() — same pattern.

- [ ] **Step 2: Build and test**

```bash
cmake --build build-dev -j$(nproc)
ctest --test-dir build-dev --output-on-failure -E "tst_realistic|tst_benchmark"
```

All 13 fast tests must pass.

- [ ] **Step 3: Commit**

```bash
git add libs/collabtext/src/crdt/Buffer.cpp
git commit -m "perf: undo/redo use for_each_mut instead of get/set_fragments

Recomputes visibility in-place. Avoids copying all fragments to
a vector and rebuilding the tree."
```

---

### Task 7: Buffer — apply_remote_edit fast path

The biggest performance win. For common-case remote edits (no
split_relocations), use in-place SumTree operations instead of
get_fragments/set_fragments.

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.cpp`

- [ ] **Step 1: Add apply_remote_edit_fast method**

Add a private method that handles the common case using in-place operations:

```cpp
bool Buffer::apply_remote_edit_fast(const EditOperation &op) {
    // Fast path: no split relocations and no normalization needed.
    // Use in-place SumTree operations.

    // Apply deletion runs via edit_item
    for (auto& run : op.deletion_runs) {
        uint32_t remaining = run.count;
        uint32_t next_val = run.start_value;

        while (remaining > 0) {
            // Seek to the fragment containing (replica_id, next_val)
            FragmentOrderDim target{Locator::min(), Lamport(run.replica_id, next_val)};

            // We need to find by origin, not by (locator, origin).
            // Since fragments are ordered by (locator, origin) but we're
            // searching by origin only, we must fall back to linear scan
            // for deletions. Use the full path instead.
            //
            // OPTIMIZATION: For deletions, fall back to the existing
            // get_fragments path. The fast path focuses on the insertion
            // case which is more common and benefits most.
            break;
        }
    }

    // If there are deletion runs, fall back to full path
    if (!op.deletion_runs.empty()) return false;

    // Apply insertions via insert_item
    for (auto &ins : op.inserted_fragments) {
        Fragment frag(ins.origin, ins.locator,
                      static_cast<uint32_t>(ins.content.size()), ins.length,
                      ins.content);
        frag.visible = true;  // New insertions are visible

        // Insert at the correct (locator, origin) position
        FragmentOrderDim target{ins.locator, ins.origin};
        m_fragment_tree.insert_item<FragmentOrderDim>(target, std::move(frag));
    }

    // Update clock and version
    m_clock.observe(op.timestamp);
    m_version.observe(op.timestamp);
    m_clock.observe(op.deletion_id);
    m_version.observe(op.deletion_id);

    for (auto &ins : op.inserted_fragments) {
        Lamport last_ts(ins.origin.replica_id, ins.origin.value + ins.length - 1);
        m_clock.observe(last_ts);
        m_version.observe(last_ts);
    }

    m_version.join(op.version);

    // Check if normalization is needed: if any inserted fragment shares a
    // locator with a fragment from a different replica AND has length > 1
    // This is handled below — if normalization is needed, we fall back.
    for (auto &ins : op.inserted_fragments) {
        if (ins.length > 1) {
            // Check if there are other replicas at this locator
            bool needs_normalize = false;
            m_fragment_tree.for_each([&](const Fragment& f) {
                if (needs_normalize) return;
                if (f.locator == ins.locator &&
                    f.origin.replica_id != ins.origin.replica_id) {
                    needs_normalize = true;
                }
            });
            if (needs_normalize) {
                // Remove the just-inserted fragments and fall back
                // Actually — it's simpler to detect this BEFORE inserting.
                // We'll restructure the method to check first.
                // For now, return false to trigger fallback.
                // The caller will redo the operation via the full path.
                //
                // This means the fragments we just inserted are wrong!
                // We need to undo them. The simplest approach: don't use
                // the fast path if ANY insertion has length > 1.
                // Single-character insertions never need normalization.
            }
        }
    }

    return true;
}
```

**Actually, let me simplify the fast path conditions.** The fast path is used
when ALL of these hold:
1. No split_relocations
2. No deletion runs (deletions require origin-based search, which is O(n) on
   a locator-ordered tree)
3. All insertions are single-character (no normalization needed)

This covers the most common case: a remote replica typed a character. Multi-
character pastes and deletions fall back to the full path.

Revised implementation:

```cpp
bool Buffer::apply_remote_edit_fast(const EditOperation &op) {
    // Fast path conditions:
    // - No split relocations (would need normalization)
    // - No deletion runs (would need origin-based search)
    // - All insertions are single-character (no normalization risk)
    if (!op.split_relocations.empty()) return false;
    if (!op.deletion_runs.empty()) return false;
    for (auto &ins : op.inserted_fragments) {
        if (ins.length != 1) return false;
    }

    // Apply insertions via insert_item
    for (auto &ins : op.inserted_fragments) {
        Fragment frag(ins.origin, ins.locator,
                      static_cast<uint32_t>(ins.content.size()), ins.length,
                      ins.content);
        frag.visible = true;

        FragmentOrderDim target{ins.locator, ins.origin};
        m_fragment_tree.insert_item<FragmentOrderDim>(target, std::move(frag));
    }

    // Update clock and version
    m_clock.observe(op.timestamp);
    m_version.observe(op.timestamp);
    m_clock.observe(op.deletion_id);
    m_version.observe(op.deletion_id);

    for (auto &ins : op.inserted_fragments) {
        Lamport last_ts(ins.origin.replica_id, ins.origin.value + ins.length - 1);
        m_clock.observe(last_ts);
        m_version.observe(last_ts);
    }

    m_version.join(op.version);
    return true;
}
```

- [ ] **Step 2: Wire fast path into apply_remote_edit**

At the top of `apply_remote_edit`, before `get_fragments()`:

```cpp
bool Buffer::apply_remote_edit(const EditOperation &op) {
    if (m_version.observed(op.timestamp))
        return true;

    if (!m_version.observed_all(op.version))
        return false;

    // Try fast path first
    if (apply_remote_edit_fast(op))
        return true;

    // Fall back to full path
    auto frags = get_fragments();
    // ... rest of existing code unchanged ...
}
```

Add the declaration to Buffer.h:

```cpp
    /// Fast path for common remote edits (insertion only, no deletions/relocations).
    bool apply_remote_edit_fast(const EditOperation &op);
```

- [ ] **Step 3: Build and test**

```bash
cmake --build build-dev -j$(nproc)
ctest --test-dir build-dev --output-on-failure -E "tst_realistic|tst_benchmark"
```

- [ ] **Step 4: Commit**

```bash
git add libs/collabtext/src/crdt/Buffer.h libs/collabtext/src/crdt/Buffer.cpp
git commit -m "perf: O(log n) fast path for common remote edits

Single-character insertions with no deletions or relocations
use SumTree::insert_item directly. Falls back to full O(n) path
for complex edits."
```

---

### Task 8: Buffer — apply_local_edit skip-rebuild

When no deferred relocations are needed, the cursor-built tree is already
correct. Skip the extract-sort-rebuild phase.

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.cpp`

- [ ] **Step 1: Add early return when no relocations**

In `apply_local_edit`, after the suffix copy and before the
`auto frags = new_tree.items()` line, add:

```cpp
    // ---- Fast path: if no deferred relocations, tree is already ordered ----
    if (deferred_relocs.empty()) {
        // Recompute visibility for all fragments in the new tree
        new_tree.for_each_mut([this](Fragment& f) {
            f.visible = f.compute_visible(m_undo_map);
        });
        m_fragment_tree = std::move(new_tree);

        // Build deletion runs from deleted timestamps
        // (same as existing code below)
        goto build_deletion_runs;
    }

    // ---- Full path: extract, relocate, sort, normalize, rebuild ----
    {
        auto frags = new_tree.items();
        // ... existing relocation/sort/normalize/set_fragments code ...
    }

    build_deletion_runs:
    // ... existing deletion_runs code ...
```

Actually, `goto` is ugly. Better: extract the deletion_runs code into a
helper, or restructure with an if/else:

```cpp
    // ---- Apply deferred relocations, sort, normalize, rebuild ----
    if (deferred_relocs.empty()) {
        // Fast path: no relocations needed, tree is already ordered
        new_tree.for_each_mut([this](Fragment& f) {
            f.visible = f.compute_visible(m_undo_map);
        });
        m_fragment_tree = std::move(new_tree);
    } else {
        // Full path: extract, relocate, sort, normalize, rebuild
        auto frags = new_tree.items();

        // ... existing deferred relocation code (lines 1086-1107) ...
        // ... sort (lines 1110-1113) ...
        // ... normalize_fragments(frags) ...
        // ... set_fragments(std::move(frags)) ...
    }

    // Build deletion runs (shared by both paths)
    // ... existing deletion_runs code (lines 1121-1137) ...
```

- [ ] **Step 2: Build and test**

```bash
cmake --build build-dev -j$(nproc)
ctest --test-dir build-dev --output-on-failure -E "tst_realistic|tst_benchmark"
```

- [ ] **Step 3: Commit**

```bash
git add libs/collabtext/src/crdt/Buffer.cpp
git commit -m "perf: skip extract-sort-rebuild in apply_local_edit when no relocations

When no deferred relocations are needed (common case), the cursor-
built tree is already correctly ordered. Use for_each_mut to set
visibility and use the tree directly."
```

---

### Task 9: Full regression and benchmark validation

Run the full test suite, stability checks, and benchmarks. Compare to
pre-refactor baselines.

**Files:**
- None (read-only validation)

- [ ] **Step 1: Reconfigure cmake (picks up FRAG_TREE_B change)**

```bash
cmake -S . -B build-dev -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-dev -j$(nproc)
```

- [ ] **Step 2: Run all fast tests**

```bash
ctest --test-dir build-dev --output-on-failure -E "tst_realistic|tst_benchmark"
```

All 13 tests must pass.

- [ ] **Step 3: Run realistic tests**

```bash
./build-dev/libs/collabtext/tst_realistic -v2
```

All 11 tests must pass.

- [ ] **Step 4: Stability checks**

```bash
for i in $(seq 1 5); do
    ./build-dev/libs/collabtext/tst_fuzz -v2 2>&1 | grep "Totals:"
done
```

5/5 runs must pass (16/16 each).

- [ ] **Step 5: Run benchmarks**

```bash
./build-dev/libs/collabtext/tst_benchmark single_replica_throughput -v2
./build-dev/libs/collabtext/tst_benchmark single_replica_large_doc -v2
./build-dev/libs/collabtext/tst_benchmark realistic_3_client_throughput -v2
./build-dev/libs/collabtext/tst_benchmark reconnect_sync_cost -v2
./build-dev/libs/collabtext/tst_benchmark gc_under_sustained_editing -v2
```

Pre-refactor baselines (Phase 1):
- 1K: 192 ops/sec
- 10K: 117 ops/sec
- 100K: 30 ops/sec
- 1M: 33 ops/sec
- 3-client realistic: 17 ops/sec
- GC sustained: 6 ops/sec

Expected improvements:
- 3-client realistic: >100 ops/sec (6x+)
- Single replica: modest improvement from B=6

- [ ] **Step 6: Write benchmark report**

If improvements are significant, create
`docs/reports/2026-04-05-inplace-mutations-benchmark.md` with before/after.

```bash
git add docs/reports/
git commit -m "docs: Phase 2 in-place mutations benchmark results"
```
