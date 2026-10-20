#pragma once

#include "crdt/Diagnostics.h"
#include "crdt/TreeNodeId.h"

#include <collabtext/Engine.h> // Bias

#include <algorithm>
#include <array>
#include <cassert>
#include <compare>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <variant>
#include <vector>

namespace CollabText::Crdt {

// ============================================================================
// Concepts
// ============================================================================

/// A Summary is a monoid: has identity (zero) and associative composition.
template<typename S>
concept SummaryType = requires(S s, const S& other) {
    { S::zero() } -> std::convertible_to<S>;
    { s.add_summary(other) };
};

/// An Item stored in a SumTree can produce a Summary.
template<typename T>
concept Summarizable = requires(T item) {
    typename T::Summary;
    { item.summary() } -> std::convertible_to<typename T::Summary>;
} && SummaryType<typename T::Summary>;

/// A Dimension extracts a comparable measurement from a Summary.
template<typename D, typename S>
concept DimensionOf = std::totally_ordered<D> && requires(D d, const S& summary) {
    { D::zero() } -> std::convertible_to<D>;
    { d.add_summary(summary) };
};

// ============================================================================
// SumTree
// ============================================================================

template<Summarizable Item, std::size_t B = 6, class NodeIdPolicy = NoNodeIds>
class SumTree {
public:
    using Summary = typename Item::Summary;
    static_assert(B >= 2, "Branching factor must be at least 2");
    static constexpr std::size_t MaxChildren = 2 * B;
    // ---- Node types (public for Cursor access) ----

    struct Node;
    using NodePtr = std::shared_ptr<Node>;

    struct Leaf {
        std::array<Item, MaxChildren> items{};
        std::array<Summary, MaxChildren> item_summaries{};
        uint16_t count = 0;
    };

    struct Internal {
        std::array<Summary, MaxChildren> child_summaries{};
        std::array<NodePtr, MaxChildren> children{};
        uint16_t count = 0;
    };

    struct Node {
        [[no_unique_address]] typename NodeIdPolicy::Id node_id{};
        uint8_t height = 0; // 0 = leaf
        Summary summary = Summary::zero();
        std::variant<Leaf, Internal> data;

        Node() : data(Leaf{}) {}

        bool is_leaf() const { return height == 0; }
        Leaf& leaf() { return std::get<Leaf>(data); }
        const Leaf& leaf() const { return std::get<Leaf>(data); }
        Internal& internal() { return std::get<Internal>(data); }
        const Internal& internal() const { return std::get<Internal>(data); }
    };

    // ---- Cursor ----

    template<typename D>
        requires DimensionOf<D, Summary>
    class Cursor {
    public:
        explicit Cursor(const SumTree* tree)
            : m_tree(tree), m_position(D::zero()) {}

        /// Seek to the item at the given target position.
        /// Returns true if an item was found, false if past end.
        bool seek(const D& target, Bias bias = Bias::Left) {
            m_stack.clear();
            m_position = D::zero();
            m_did_seek = true;

            if (!m_tree->m_root) return false;

            return seek_internal(m_tree->m_root.get(), target, bias);
        }

        /// Extract all items from current position to target as a new SumTree.
        /// Advances the cursor past the extracted items.
        SumTree slice(const D& target) {
            SumTree result(m_tree->m_node_ids);

            if (!m_did_seek) {
                // Position at start
                if (m_tree->m_root) {
                    descend_left(m_tree->m_root.get());
                    m_did_seek = true;
                }
            }

            while (!at_end()) {
                if (!(m_position < target)) break;

                auto& level = m_stack.back();

                if (level.node->is_leaf()) {
                    auto& lf = level.node->leaf();
                    while (level.index < lf.count) {
                        D item_end = m_position;
                        item_end.add_summary(lf.item_summaries[level.index]);
                        if (item_end <= target) {
                            result.push_item(Item(lf.items[level.index]));
                            m_position = item_end;
                            level.index++;
                        } else {
                            return result;
                        }
                    }
                    // Finished this leaf, pop up
                    ascend();
                } else {
                    auto& in = level.node->internal();
                    bool descended = false;
                    while (level.index < in.count) {
                        D child_end = m_position;
                        child_end.add_summary(in.child_summaries[level.index]);
                        if (child_end <= target) {
                            // Whole subtree fits — push by shared_ptr
                            result.push_tree(SumTree(in.children[level.index],
                                                     m_tree->m_node_ids));
                            m_position = child_end;
                            level.index++;
                        } else {
                            // Straddles boundary — descend
                            descend_into(level.index);
                            descended = true;
                            break;
                        }
                    }
                    if (!descended) {
                        ascend();
                    }
                }
            }

            // Settle: if cursor ended up at an internal node (happens when
            // the last extracted item was the final item in its leaf — ascend
            // moved to the parent, then the loop broke because position >=
            // target), descend into the next child's leftmost leaf so that
            // item() returns the correct next item.
            settle_at_leaf();

            return result;
        }

        /// Extract all remaining items from cursor to end as a new SumTree.
        SumTree suffix() {
            SumTree result(m_tree->m_node_ids);

            if (!m_did_seek) {
                if (m_tree->m_root) {
                    descend_left(m_tree->m_root.get());
                    m_did_seek = true;
                }
            }

            while (!at_end()) {
                auto& level = m_stack.back();

                if (level.node->is_leaf()) {
                    auto& lf = level.node->leaf();
                    while (level.index < lf.count) {
                        D item_end = m_position;
                        item_end.add_summary(lf.item_summaries[level.index]);
                        result.push_item(Item(lf.items[level.index]));
                        m_position = item_end;
                        level.index++;
                    }
                    ascend();
                } else {
                    auto& in = level.node->internal();
                    while (level.index < in.count) {
                        D child_end = m_position;
                        child_end.add_summary(in.child_summaries[level.index]);
                        result.push_tree(SumTree(in.children[level.index],
                                                 m_tree->m_node_ids));
                        m_position = child_end;
                        level.index++;
                    }
                    ascend();
                }
            }

            return result;
        }

        /// Move to the next item. Returns true if successful.
        bool next() {
            if (m_stack.empty()) return false;

            auto& level = m_stack.back();
            if (level.node->is_leaf()) {
                auto& lf = level.node->leaf();
                // Advance past current item
                if (level.index < lf.count) {
                    m_position.add_summary(lf.item_summaries[level.index]);
                    level.index++;
                }
                // If more items in this leaf, done
                if (level.index < lf.count) return true;
                // Else ascend and find next leaf
                return advance_to_next_leaf();
            }
            // Shouldn't be at internal level after seek
            return false;
        }

        /// Current item, or nullptr if at end.
        const Item* item() const {
            if (m_stack.empty()) return nullptr;
            auto& level = m_stack.back();
            if (!level.node->is_leaf()) return nullptr;
            auto& lf = level.node->leaf();
            if (level.index >= lf.count) return nullptr;
            return &lf.items[level.index];
        }

        /// Summary of the current item, or zero if at end.
        Summary item_summary() const {
            if (m_stack.empty()) return Summary::zero();
            auto& level = m_stack.back();
            if (!level.node->is_leaf()) return Summary::zero();
            auto& lf = level.node->leaf();
            if (level.index >= lf.count) return Summary::zero();
            return lf.item_summaries[level.index];
        }

        /// Cumulative position at the START of the current item.
        const D& position() const { return m_position; }

        /// True if cursor is past all items.
        bool at_end() const {
            return m_stack.empty();
        }

    private:
        struct StackEntry {
            const Node* node;
            uint16_t index;
        };

        const SumTree* m_tree;
        std::vector<StackEntry> m_stack;
        D m_position;
        bool m_did_seek = false;

        bool seek_internal(const Node* node, const D& target, Bias bias) {
            if (node->is_leaf()) {
                auto& lf = node->leaf();
                for (uint16_t i = 0; i < lf.count; ++i) {
                    D item_end = m_position;
                    item_end.add_summary(lf.item_summaries[i]);
                    bool past = (bias == Bias::Left)
                        ? (target < item_end)
                        : !(item_end < target);
                    if (past) {
                        m_stack.push_back({node, i});
                        return true;
                    }
                    m_position = item_end;
                }
                // Past all items
                return false;
            }

            auto& in = node->internal();
            for (uint16_t i = 0; i < in.count; ++i) {
                D child_end = m_position;
                child_end.add_summary(in.child_summaries[i]);
                bool past = (bias == Bias::Left)
                    ? (target < child_end)
                    : !(child_end < target);
                if (past) {
                    m_stack.push_back({node, i});
                    return seek_internal(in.children[i].get(), target, bias);
                }
                m_position = child_end;
            }
            // Past all children
            return false;
        }

        void descend_left(const Node* node) {
            while (!node->is_leaf()) {
                auto& in = node->internal();
                if (in.count == 0) return;
                m_stack.push_back({node, 0});
                node = in.children[0].get();
            }
            auto& lf = node->leaf();
            if (lf.count > 0) {
                m_stack.push_back({node, 0});
            }
        }

        void descend_into(uint16_t child_index) {
            auto& level = m_stack.back();
            const Node* child = level.node->internal().children[child_index].get();
            // Don't advance the parent index — we're descending into it
            // Actually we need to set the parent to point at this child index
            level.index = child_index;

            // Now descend to leftmost leaf of this child
            const Node* node = child;
            while (!node->is_leaf()) {
                auto& in = node->internal();
                if (in.count == 0) return;
                m_stack.push_back({node, 0});
                node = in.children[0].get();
            }
            if (node->leaf().count > 0) {
                m_stack.push_back({node, 0});
            }
        }

        /// If the cursor is at an internal node, descend to the leftmost
        /// leaf of the current child so that item() works. No-op when
        /// already at a leaf or at end.
        void settle_at_leaf() {
            while (!m_stack.empty() && !m_stack.back().node->is_leaf()) {
                auto& level = m_stack.back();
                auto& in = level.node->internal();
                if (level.index < in.count) {
                    const Node* node = in.children[level.index].get();
                    while (!node->is_leaf()) {
                        auto& cin = node->internal();
                        if (cin.count == 0) { m_stack.clear(); return; }
                        m_stack.push_back({node, 0});
                        node = cin.children[0].get();
                    }
                    if (node->leaf().count > 0) {
                        m_stack.push_back({node, 0});
                    } else {
                        m_stack.clear();
                    }
                    return;
                }
                // This internal node is exhausted, ascend
                ascend();
            }
        }

        void ascend() {
            if (m_stack.empty()) return;
            m_stack.pop_back(); // pop current level
            if (m_stack.empty()) return;
            m_stack.back().index++; // advance parent past the child we just finished
        }

        bool advance_to_next_leaf() {
            while (true) {
                // Pop the exhausted leaf
                m_stack.pop_back();
                if (m_stack.empty()) return false;

                auto& level = m_stack.back();
                // We just finished a child; advance to next
                level.index++;

                if (level.node->is_leaf()) {
                    // Shouldn't happen (parent of a leaf is internal)
                    return false;
                }

                auto& in = level.node->internal();
                if (level.index < in.count) {
                    // Descend into next child
                    const Node* child = in.children[level.index].get();
                    while (!child->is_leaf()) {
                        auto& cin = child->internal();
                        if (cin.count == 0) return false;
                        m_stack.push_back({child, 0});
                        child = cin.children[0].get();
                    }
                    if (child->leaf().count > 0) {
                        m_stack.push_back({child, 0});
                        return true;
                    }
                    return false;
                }
                // This internal node is exhausted too, continue popping
            }
        }
    };

    // ---- Construction ----

    SumTree() = default;

    explicit SumTree(NodeIdPolicy node_ids)
        : m_node_ids(std::move(node_ids)) {}

    /// Construct a subtree while retaining the allocator lineage for new nodes.
    SumTree(NodePtr root, NodeIdPolicy node_ids)
        : m_root(std::move(root)), m_node_ids(std::move(node_ids)) {}

    // ---- Queries ----

    const Summary& summary() const {
        static const Summary s_zero = Summary::zero();
        return m_root ? m_root->summary : s_zero;
    }

    /// The current root owner, for state-local weak directories. Keep this
    /// read-only handle out of scope before mutating the tree so it does not
    /// affect copy-on-write uniqueness.
    const NodePtr& root_node() const noexcept { return m_root; }

    /// Visit only the root-to-item path selected by a summary coordinate. The
    /// target must fall inside a positive-width item; zero-width spans at that
    /// coordinate are skipped. Each callback receives the owning node and its
    /// parent ID/slot (the root has no parent).
    template<class D, class Fn>
        requires DimensionOf<D, Summary>
    bool for_each_node_on_path(const D& target, Fn&& fn) const {
        if (!m_root) return false;
        D start = D::zero();
        if (!node_path_exists(m_root, target, start)) return false;
        start = D::zero();
        auto visitor = std::forward<Fn>(fn);
        return visit_node_path(m_root, std::nullopt, 0, target, start, visitor);
    }

    bool empty() const {
        if (!m_root) return true;
        if (m_root->is_leaf()) return m_root->leaf().count == 0;
        return false;
    }

    /// Verify the B+ shape used by the path-copy splice implementation. This
    /// is an explicit O(number of nodes) diagnostic and is never called by
    /// mutation paths.
    bool has_valid_occupancy() const {
        if (!m_root) return true;
        std::optional<uint8_t> leaf_height;
        return validate_occupancy(m_root, true, leaf_height);
    }

    bool has_valid_summaries() const
        requires std::equality_comparable<Summary>
    {
        return !m_root || validate_summaries(m_root);
    }

    bool has_valid_node_ids() const {
        return m_node_ids.valid() && (!m_root || node_ids_valid(m_root));
    }

    bool has_unique_node_ids() const
        requires std::same_as<typename NodeIdPolicy::Id, NodeId>
    {
        if (!has_valid_node_ids()) return false;
        std::vector<NodeId> ids;
        for_each_node([&](const NodePtr&, NodeId id,
                          std::optional<NodeId>, uint16_t) {
            ids.push_back(id);
        });
        std::sort(ids.begin(), ids.end());
        return std::adjacent_find(ids.begin(), ids.end()) == ids.end();
    }

    template<class Fn>
    void for_each_node(Fn&& fn) const {
        if (!m_root) return;
        auto visitor = std::forward<Fn>(fn);
        visit_nodes_recursive(m_root, std::nullopt, 0, visitor);
    }

    /// Create a cursor for seeking by dimension D.
    template<typename D>
        requires DimensionOf<D, Summary>
    Cursor<D> cursor() const {
        return Cursor<D>(this);
    }

    /// Iterate all items in order.
    template<typename F>
    void for_each(F&& fn) const {
        if (!m_root) return;
        auto counted = [&](const Item& item) {
            Diagnostics::record(Diagnostics::Counter::sumtree_items_visited);
            fn(item);
        };
        for_each_node(m_root.get(), counted);
    }

    /// Flatten to a vector of items.
    std::vector<Item> items() const {
        std::vector<Item> result;
        for_each([&](const Item& item) {
            Diagnostics::record(Diagnostics::Counter::sumtree_items_flattened);
            result.push_back(item);
        });
        return result;
    }

    /// First item (undefined if empty).
    const Item& first() const {
        const Node* n = m_root.get();
        while (!n->is_leaf()) n = n->internal().children[0].get();
        assert(n->leaf().count > 0);
        return n->leaf().items[0];
    }

    /// Last item (undefined if empty).
    const Item& last() const {
        const Node* n = m_root.get();
        while (!n->is_leaf()) {
            auto& in = n->internal();
            n = in.children[in.count - 1].get();
        }
        auto& lf = n->leaf();
        assert(lf.count > 0);
        return lf.items[lf.count - 1];
    }

    // ---- Building ----

    /// Append a single item.
    void push_item(Item item) {
        Diagnostics::record(Diagnostics::Counter::sumtree_items_pushed);
        Summary item_sum = item.summary();

        if (!m_root) {
            m_root = make_leaf(m_node_ids);
        }

        // Push into the rightmost leaf. If it overflows, split upward.
        auto overflow = push_item_recursive(ensure_mutable(m_root),
                                            std::move(item), item_sum,
                                            m_node_ids);
        if (overflow) {
            // Root split — create new root
            auto new_root = make_internal(m_root->height + 1, m_node_ids);
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

    /// Append another tree.
    void push_tree(SumTree other) {
        if (!other.m_root || other.empty()) return;
        if (!m_root || empty()) {
            m_root = std::move(other.m_root);
            return;
        }

        uint8_t self_h = m_root->height;
        uint8_t other_h = other.m_root->height;

        if (self_h == other_h) {
            merge_same_height(std::move(other.m_root), m_node_ids);
        } else if (self_h > other_h) {
            // Push other into our rightmost path
            auto overflow = push_tree_right(ensure_mutable(m_root),
                                            std::move(other.m_root),
                                            m_node_ids);
            if (overflow) {
                auto new_root = make_internal(m_root->height + 1, m_node_ids);
                auto& in = new_root->internal();
                in.children[0] = std::move(m_root);
                in.child_summaries[0] = in.children[0]->summary;
                in.children[1] = std::move(overflow);
                in.child_summaries[1] = in.children[1]->summary;
                in.count = 2;
                recompute_summary(*new_root);
                m_root = std::move(new_root);
            }
        } else {
            // Other is taller. Insert down its left spine instead of wrapping
            // this root in one-child nodes, which would violate B+ occupancy.
            NodePtr taller = std::move(other.m_root);
            auto overflow = push_tree_left(taller, std::move(m_root),
                                           m_node_ids);
            if (overflow) {
                auto new_root = make_internal(taller->height + 1, m_node_ids);
                auto& in = new_root->internal();
                in.children[0] = std::move(overflow);
                in.child_summaries[0] = in.children[0]->summary;
                in.children[1] = std::move(taller);
                in.child_summaries[1] = in.children[1]->summary;
                in.count = 2;
                recompute_summary(*new_root);
                m_root = std::move(new_root);
            } else {
                m_root = std::move(taller);
            }
        }
    }

    /// Return a copy-on-write tree with the item-aligned range [begin, end)
    /// replaced by `replacement`. Whole unaffected subtrees are shared from
    /// this tree; only the boundary paths and joins are rebuilt. `D` must be a
    /// strictly advancing rank dimension (each item contributes a positive
    /// amount), so item boundaries are unambiguous. A dimension target inside
    /// an item, an inverted range, or a range past the end is a checked failure
    /// and leaves this tree untouched.
    template<typename D>
        requires DimensionOf<D, Summary>
    std::optional<SumTree> splice_range(const D& begin, const D& end,
                                        SumTree replacement) const {
        if (end < begin) return std::nullopt;
        D total = D::zero();
        if (m_root) total.add_summary(m_root->summary);
        if (total < end) return std::nullopt;
        if (begin == end && replacement.empty()) return *this;

        RangeParts parts;
        SumTree result(m_node_ids);
        if (m_root)
            split_range_node<D>(m_root, begin, end, D::zero(), parts,
                                result.m_node_ids);
        if (!parts.valid) return std::nullopt;

        for (const auto& root : parts.prefix)
            result.push_tree(SumTree(NodePtr(root), result.m_node_ids));
        result.push_tree(std::move(replacement));
        for (const auto& root : parts.suffix)
            result.push_tree(SumTree(NodePtr(root), result.m_node_ids));
        return result;
    }

    /// Extend with items from a vector.
    void extend(const std::vector<Item>& items) {
        for (auto& item : items) push_item(Item(item));
    }

    // ---- In-place mutations ----

    /// Mutable iteration: call fn on each item, updating summaries bottom-up.
    template<typename F>
    void for_each_mut(F&& fn) {
        if (m_root) for_each_node_mut(ensure_mutable(m_root), fn);
    }

    /// Recompute all summaries bottom-up (items unchanged, just re-aggregate).
    void recompute_all_summaries() {
        if (m_root) recompute_all_summaries_recursive(ensure_mutable(m_root));
    }

    /// Seek to item at target position, call fn(item), propagate summaries up.
    /// Returns true if item was found and edited.
    template<typename D, typename F>
        requires DimensionOf<D, Summary>
    bool edit_item(const D& target, F&& fn, Bias bias = Bias::Left) {
        if (!m_root) return false;
        return edit_item_recursive<D>(ensure_mutable(m_root), target, fn, bias, D::zero());
    }

    /// Seek to position, insert item before the first item at that position.
    /// Handles leaf splitting and root growth.
    template<typename D>
        requires DimensionOf<D, Summary>
    void insert_item(const D& target, Item item, Bias bias = Bias::Left) {
        if (!m_root) {
            m_root = make_leaf(m_node_ids);
        }
        auto overflow = insert_item_recursive<D>(ensure_mutable(m_root), target,
                                                 std::move(item), bias,
                                                 D::zero(), m_node_ids);
        if (overflow) {
            auto new_root = make_internal(m_root->height + 1, m_node_ids);
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

    /// Seek to item at target position, remove it.
    /// Handles empty-child cleanup and root shrinking.
    /// Returns true if an item was found and removed.
    template<typename D>
        requires DimensionOf<D, Summary>
    bool remove_item(const D& target, Bias bias = Bias::Left) {
        if (!m_root) return false;
        bool found = remove_item_recursive<D>(ensure_mutable(m_root), target, bias, D::zero());
        if (!found) return false;
        // Shrink root if needed
        if (m_root->is_leaf()) {
            if (m_root->leaf().count == 0) m_root = nullptr;
        } else {
            if (m_root->internal().count == 1) {
                m_root = std::move(m_root->internal().children[0]);
            } else if (m_root->internal().count == 0) {
                m_root = nullptr;
            }
        }
        return true;
    }

private:
    [[no_unique_address]] NodeIdPolicy m_node_ids{};
    NodePtr m_root;

    template<class D>
    static bool node_path_exists(const NodePtr& node, const D& target,
                                 D& start) {
        if (node->is_leaf()) {
            const auto& leaf = node->leaf();
            D position = start;
            for (uint16_t slot = 0; slot < leaf.count; ++slot) {
                D end = position;
                end.add_summary(leaf.item_summaries[slot]);
                if (position <= target && target < end) {
                    start = position;
                    return true;
                }
                position = end;
            }
            return false;
        }

        const auto& internal = node->internal();
        D position = start;
        for (uint16_t slot = 0; slot < internal.count; ++slot) {
            D end = position;
            end.add_summary(internal.child_summaries[slot]);
            if (position <= target && target < end) {
                start = position;
                return node_path_exists(internal.children[slot], target, start);
            }
            position = end;
        }
        return false;
    }

    template<class D, class Fn>
    static bool visit_node_path(const NodePtr& node,
                                std::optional<typename NodeIdPolicy::Id> parent,
                                uint16_t child_slot, const D& target,
                                D& start, Fn& fn) {
        fn(node, node->node_id, parent, child_slot);
        if (node->is_leaf()) {
            const auto& leaf = node->leaf();
            D position = start;
            for (uint16_t slot = 0; slot < leaf.count; ++slot) {
                D end = position;
                end.add_summary(leaf.item_summaries[slot]);
                if (position <= target && target < end) return true;
                position = end;
            }
            return false;
        }

        const auto& internal = node->internal();
        D position = start;
        for (uint16_t slot = 0; slot < internal.count; ++slot) {
            D end = position;
            end.add_summary(internal.child_summaries[slot]);
            if (position <= target && target < end) {
                start = position;
                return visit_node_path(internal.children[slot], node->node_id,
                                       slot, target, start, fn);
            }
            position = end;
        }
        return false;
    }

    struct RangeParts {
        std::vector<NodePtr> prefix;
        std::vector<NodePtr> suffix;
        bool valid = true;
    };

    static void append_tree(std::vector<NodePtr>& destination,
                            const NodePtr& node) {
        if (node) destination.push_back(node);
    }

    static void append_leaf_items(std::vector<NodePtr>& destination,
                                  const Leaf& source,
                                  const std::vector<uint16_t>& indices,
                                  NodeIdPolicy& node_ids) {
        if (indices.empty()) return;
        auto leaf = make_leaf(node_ids);
        auto& output = leaf->leaf();
        for (const uint16_t index : indices) {
            output.items[output.count] = Item(source.items[index]);
            output.item_summaries[output.count] = source.item_summaries[index];
            ++output.count;
        }
        recompute_summary(*leaf);
        destination.push_back(std::move(leaf));
    }

    template<typename D>
        requires DimensionOf<D, Summary>
    static void split_range_node(const NodePtr& node, const D& begin,
                                 const D& end, const D& base,
                                 RangeParts& parts,
                                 NodeIdPolicy& node_ids) {
        D node_end = base;
        node_end.add_summary(node->summary);
        if (node_end <= begin) {
            append_tree(parts.prefix, node);
            return;
        }
        if (base >= end) {
            append_tree(parts.suffix, node);
            return;
        }
        if (begin <= base && node_end <= end) {
            return;
        }

        if (node->is_leaf()) {
            const auto& leaf = node->leaf();
            D item_start = base;
            std::vector<uint16_t> prefix_items;
            std::vector<uint16_t> suffix_items;
            for (uint16_t i = 0; i < leaf.count; ++i) {
                D item_end = item_start;
                item_end.add_summary(leaf.item_summaries[i]);
                if (item_end <= begin) {
                    prefix_items.push_back(i);
                } else if (item_start >= end) {
                    suffix_items.push_back(i);
                } else if (begin <= item_start && item_end <= end) {
                    // The item is inside the removed interval.
                } else {
                    parts.valid = false; // a splice boundary cuts through an item
                    return;
                }
                item_start = item_end;
            }
            append_leaf_items(parts.prefix, leaf, prefix_items, node_ids);
            append_leaf_items(parts.suffix, leaf, suffix_items, node_ids);
            return;
        }

        const auto& internal = node->internal();
        D child_start = base;
        for (uint16_t i = 0; i < internal.count; ++i) {
            const auto& child = internal.children[i];
            D child_end = child_start;
            child_end.add_summary(internal.child_summaries[i]);
            if (child_end <= begin) {
                append_tree(parts.prefix, child);
            } else if (child_start >= end) {
                append_tree(parts.suffix, child);
            } else if (begin <= child_start && child_end <= end) {
                // The entire subtree is removed; do not enumerate it.
            } else {
                split_range_node<D>(child, begin, end, child_start, parts,
                                    node_ids);
                if (!parts.valid) return;
            }
            child_start = child_end;
        }
    }

    // ---- Node creation helpers ----

    static NodePtr make_leaf(NodeIdPolicy& node_ids) {
        auto node = std::make_shared<Node>();
        node->node_id = node_ids.allocate();
        node->height = 0;
        node->data = Leaf{};
        return node;
    }

    static NodePtr make_internal(uint8_t height, NodeIdPolicy& node_ids) {
        assert(height > 0);
        auto node = std::make_shared<Node>();
        node->node_id = node_ids.allocate();
        node->height = height;
        node->data = Internal{};
        return node;
    }

    static bool node_ids_valid(const NodePtr& node) {
        if (!node->node_id.valid()) return false;
        if (node->is_leaf()) return true;
        const auto& internal = node->internal();
        for (uint16_t i = 0; i < internal.count; ++i)
            if (!internal.children[i] || !node_ids_valid(internal.children[i]))
                return false;
        return true;
    }

    template<class Fn>
    static void visit_nodes_recursive(
        const NodePtr& node,
        std::optional<typename NodeIdPolicy::Id> parent_id,
        uint16_t child_slot, Fn& fn) {
        fn(node, node->node_id, parent_id, child_slot);
        if (node->is_leaf()) return;
        const auto& internal = node->internal();
        for (uint16_t i = 0; i < internal.count; ++i)
            visit_nodes_recursive(internal.children[i], node->node_id, i, fn);
    }

    /// Ensure we have a mutable (unshared) node. Clone if shared.
    static NodePtr& ensure_mutable(NodePtr& ptr) {
        if (ptr.use_count() > 1) {
            ptr = std::make_shared<Node>(*ptr);
        }
        return ptr;
    }

    static void recompute_summary(Node& node) {
        node.summary = Summary::zero();
        if (node.is_leaf()) {
            auto& lf = node.leaf();
            for (uint16_t i = 0; i < lf.count; ++i) {
                node.summary.add_summary(lf.item_summaries[i]);
            }
        } else {
            auto& in = node.internal();
            for (uint16_t i = 0; i < in.count; ++i) {
                node.summary.add_summary(in.child_summaries[i]);
            }
        }
    }

    /// Combine adjacent nodes at the same height, redistributing their
    /// bounded contents so every returned non-root node has B..2B entries.
    /// Returns one when the pair coalesced and two when it remains split.
    static uint16_t combine_adjacent(NodePtr& left, NodePtr& right) {
        assert(left && right && left->height == right->height);
        ensure_mutable(left);
        ensure_mutable(right);
        if (left->is_leaf()) {
            auto& left_leaf = left->leaf();
            const auto& right_leaf = right->leaf();
            const uint16_t total = left_leaf.count + right_leaf.count;
            assert(total <= 2 * MaxChildren);
            std::array<Item, 2 * MaxChildren> items{};
            std::array<Summary, 2 * MaxChildren> summaries{};
            uint16_t at = 0;
            for (uint16_t i = 0; i < left_leaf.count; ++i, ++at) {
                items[at] = left_leaf.items[i];
                summaries[at] = left_leaf.item_summaries[i];
            }
            for (uint16_t i = 0; i < right_leaf.count; ++i, ++at) {
                items[at] = right_leaf.items[i];
                summaries[at] = right_leaf.item_summaries[i];
            }
            const uint16_t left_count = total <= MaxChildren
                ? total : static_cast<uint16_t>(total / 2);
            const uint16_t right_count = total - left_count;
            left_leaf.count = left_count;
            for (uint16_t i = 0; i < left_count; ++i) {
                left_leaf.items[i] = std::move(items[i]);
                left_leaf.item_summaries[i] = summaries[i];
            }
            if (right_count) {
                auto& output = right->leaf();
                output.count = right_count;
                for (uint16_t i = 0; i < right_count; ++i) {
                    output.items[i] = std::move(items[left_count + i]);
                    output.item_summaries[i] = summaries[left_count + i];
                }
            }
            recompute_summary(*left);
            if (right_count) recompute_summary(*right);
            return right_count ? 2 : 1;
        }

        auto& left_internal = left->internal();
        const auto& right_internal = right->internal();
        const uint16_t total = left_internal.count + right_internal.count;
        assert(total <= 2 * MaxChildren);
        std::array<NodePtr, 2 * MaxChildren> children{};
        std::array<Summary, 2 * MaxChildren> summaries{};
        uint16_t at = 0;
        for (uint16_t i = 0; i < left_internal.count; ++i, ++at) {
            children[at] = left_internal.children[i];
            summaries[at] = left_internal.child_summaries[i];
        }
        for (uint16_t i = 0; i < right_internal.count; ++i, ++at) {
            children[at] = right_internal.children[i];
            summaries[at] = right_internal.child_summaries[i];
        }
        const uint16_t left_count = total <= MaxChildren
            ? total : static_cast<uint16_t>(total / 2);
        const uint16_t right_count = total - left_count;
        left_internal.count = left_count;
        for (uint16_t i = 0; i < left_count; ++i) {
            left_internal.children[i] = std::move(children[i]);
            left_internal.child_summaries[i] = summaries[i];
        }
        if (right_count) {
            auto& output = right->internal();
            output.count = right_count;
            for (uint16_t i = 0; i < right_count; ++i) {
                output.children[i] = std::move(children[left_count + i]);
                output.child_summaries[i] = summaries[left_count + i];
            }
        }
        recompute_summary(*left);
        if (right_count) recompute_summary(*right);
        return right_count ? 2 : 1;
    }

    bool validate_occupancy(const NodePtr& node, bool is_root,
                            std::optional<uint8_t>& leaf_height) const {
        const uint16_t count = node->is_leaf() ? node->leaf().count
                                                : node->internal().count;
        if (!count || count > MaxChildren || (!is_root && count < B)) return false;
        if (node->is_leaf()) {
            if (!leaf_height) leaf_height = node->height;
            return *leaf_height == node->height;
        }
        const auto& internal = node->internal();
        for (uint16_t i = 0; i < internal.count; ++i) {
            if (!internal.children[i] ||
                internal.children[i]->height + 1 != node->height ||
                !validate_occupancy(internal.children[i], false, leaf_height))
                return false;
        }
        return true;
    }

    static bool validate_summaries(const NodePtr& node)
        requires std::equality_comparable<Summary>
    {
        Summary expected = Summary::zero();
        if (node->is_leaf()) {
            const auto& leaf = node->leaf();
            for (uint16_t i = 0; i < leaf.count; ++i) {
                const Summary item_summary = leaf.items[i].summary();
                if (leaf.item_summaries[i] != item_summary) return false;
                expected.add_summary(item_summary);
            }
        } else {
            const auto& internal = node->internal();
            for (uint16_t i = 0; i < internal.count; ++i) {
                if (!internal.children[i] ||
                    internal.child_summaries[i] != internal.children[i]->summary ||
                    !validate_summaries(internal.children[i]))
                    return false;
                expected.add_summary(internal.children[i]->summary);
            }
        }
        return node->summary == expected;
    }

    // ---- push_item internals ----

    /// Push item into rightmost leaf. Returns overflow node if split needed.
    static NodePtr push_item_recursive(NodePtr& node, Item item,
                                       const Summary& item_sum,
                                       NodeIdPolicy& node_ids) {
        ensure_mutable(node);

        if (node->is_leaf()) {
            auto& lf = node->leaf();
            if (lf.count < MaxChildren) {
                lf.items[lf.count] = std::move(item);
                lf.item_summaries[lf.count] = item_sum;
                lf.count++;
                node->summary.add_summary(item_sum);
                return nullptr;
            }
            // Leaf is full — split
            return split_leaf_and_push(node, std::move(item), item_sum,
                                       node_ids);
        }

        // Internal node — recurse into rightmost child
        auto& in = node->internal();
        assert(in.count > 0);
        auto overflow = push_item_recursive(
            ensure_mutable(in.children[in.count - 1]),
            std::move(item), item_sum, node_ids);

        // Update child summary
        in.child_summaries[in.count - 1] = in.children[in.count - 1]->summary;
        recompute_summary(*node);

        if (!overflow) return nullptr;

        // Accommodate the overflow
        if (in.count < MaxChildren) {
            in.children[in.count] = std::move(overflow);
            in.child_summaries[in.count] = in.children[in.count]->summary;
            in.count++;
            recompute_summary(*node);
            return nullptr;
        }

        // Internal node is full — split
        return split_internal_and_add(node, std::move(overflow), node_ids);
    }

    /// Split a full leaf, push item into the appropriate half, return right half.
    static NodePtr split_leaf_and_push(NodePtr& node, Item item,
                                       const Summary& item_sum,
                                       NodeIdPolicy& node_ids) {
        auto& lf = node->leaf();
        assert(lf.count == MaxChildren);

        uint16_t mid = static_cast<uint16_t>(MaxChildren / 2);
        auto right = make_leaf(node_ids);
        auto& rlf = right->leaf();

        // Move right half to new node
        for (uint16_t i = mid; i < MaxChildren; ++i) {
            rlf.items[rlf.count] = std::move(lf.items[i]);
            rlf.item_summaries[rlf.count] = lf.item_summaries[i];
            rlf.count++;
        }
        lf.count = mid;

        // Push new item into right half (it's the rightmost insertion)
        rlf.items[rlf.count] = std::move(item);
        rlf.item_summaries[rlf.count] = item_sum;
        rlf.count++;

        recompute_summary(*node);
        recompute_summary(*right);
        return right;
    }

    /// Split a full internal node, add overflow child to right half.
    static NodePtr split_internal_and_add(NodePtr& node, NodePtr overflow,
                                          NodeIdPolicy& node_ids) {
        auto& in = node->internal();
        assert(in.count == MaxChildren);

        uint16_t mid = static_cast<uint16_t>(MaxChildren / 2);
        auto right = make_internal(node->height, node_ids);
        auto& rin = right->internal();

        // Move right half to new node
        for (uint16_t i = mid; i < MaxChildren; ++i) {
            rin.children[rin.count] = std::move(in.children[i]);
            rin.child_summaries[rin.count] = in.child_summaries[i];
            rin.count++;
            in.children[i] = nullptr;
        }
        in.count = mid;

        // Add overflow to right half
        rin.children[rin.count] = std::move(overflow);
        rin.child_summaries[rin.count] = rin.children[rin.count]->summary;
        rin.count++;

        recompute_summary(*node);
        recompute_summary(*right);
        return right;
    }

    /// Split a full internal node after adding a child at the front. The
    /// original node remains the right half and the new left sibling is
    /// returned to its parent.
    static NodePtr split_internal_and_prepend(NodePtr& node, NodePtr overflow,
                                              NodeIdPolicy& node_ids) {
        auto& in = node->internal();
        assert(in.count == MaxChildren);
        const uint16_t mid = static_cast<uint16_t>(MaxChildren / 2);
        auto left = make_internal(node->height, node_ids);
        left->node_id = node->node_id; // preserve the original ID on the left
        node->node_id = node_ids.allocate();
        auto& lin = left->internal();

        lin.children[0] = std::move(overflow);
        lin.child_summaries[0] = lin.children[0]->summary;
        lin.count = 1;
        for (uint16_t i = 0; i < mid - 1; ++i) {
            lin.children[lin.count] = std::move(in.children[i]);
            lin.child_summaries[lin.count] = in.child_summaries[i];
            ++lin.count;
        }

        const uint16_t right_count = static_cast<uint16_t>(MaxChildren - (mid - 1));
        for (uint16_t i = 0; i < right_count; ++i) {
            const uint16_t old_index = static_cast<uint16_t>(mid - 1 + i);
            if (old_index != i) {
                in.children[i] = std::move(in.children[old_index]);
                in.child_summaries[i] = in.child_summaries[old_index];
            }
        }
        for (uint16_t i = right_count; i < MaxChildren; ++i)
            in.children[i] = nullptr;
        in.count = right_count;
        recompute_summary(*left);
        recompute_summary(*node);
        return left;
    }

    // ---- push_tree internals ----

    void merge_same_height(NodePtr other, NodeIdPolicy& node_ids) {
        // Same height: coalesce or evenly redistribute the boundary arrays
        // before making them children of a new root.
        if (combine_adjacent(m_root, other) == 1) return;
        auto new_root = make_internal(m_root->height + 1, node_ids);
        auto& in = new_root->internal();
        in.children[0] = std::move(m_root);
        in.child_summaries[0] = in.children[0]->summary;
        in.children[1] = std::move(other);
        in.child_summaries[1] = in.children[1]->summary;
        in.count = 2;
        recompute_summary(*new_root);
        m_root = std::move(new_root);
    }

    /// Push a shorter tree into the rightmost path. Returns overflow if split.
    static NodePtr push_tree_right(NodePtr& node, NodePtr other,
                                   NodeIdPolicy& node_ids) {
        ensure_mutable(node);
        assert(node->height > other->height);

        auto& in = node->internal();
        assert(in.count > 0);

        if (node->height == other->height + 1) {
            // other should become a child or merge with rightmost child
            auto& rightmost = in.children[in.count - 1];

            assert(rightmost->height == other->height);
            const uint16_t combined = combine_adjacent(rightmost, other);
            in.child_summaries[in.count - 1] = rightmost->summary;
            if (combined == 1) {
                recompute_summary(*node);
                return nullptr;
            }

            // Can't merge — add other as new child
            if (in.count < MaxChildren) {
                in.children[in.count] = std::move(other);
                in.child_summaries[in.count] = in.children[in.count]->summary;
                in.count++;
                recompute_summary(*node);
                return nullptr;
            }

            // Node is full — split and return overflow
            return split_internal_and_add(node, std::move(other), node_ids);
        }

        // Recurse deeper
        auto overflow = push_tree_right(
            ensure_mutable(in.children[in.count - 1]),
            std::move(other), node_ids);

        in.child_summaries[in.count - 1] = in.children[in.count - 1]->summary;
        recompute_summary(*node);

        if (!overflow) return nullptr;

        if (in.count < MaxChildren) {
            in.children[in.count] = std::move(overflow);
            in.child_summaries[in.count] = in.children[in.count]->summary;
            in.count++;
            recompute_summary(*node);
            return nullptr;
        }

        return split_internal_and_add(node, std::move(overflow), node_ids);
    }

    /// Push a shorter tree into the leftmost path. Returns an overflow sibling
    /// when the current node must split.
    static NodePtr push_tree_left(NodePtr& node, NodePtr other,
                                  NodeIdPolicy& node_ids) {
        ensure_mutable(node);
        assert(node->height > other->height);
        auto& in = node->internal();
        assert(in.count > 0);

        if (node->height == other->height + 1) {
            auto& leftmost = in.children[0];
            assert(leftmost->height == other->height);
            const uint16_t combined = combine_adjacent(other, leftmost);
            if (combined == 1) {
                in.children[0] = std::move(other);
                in.child_summaries[0] = in.children[0]->summary;
                recompute_summary(*node);
                return nullptr;
            }
            in.child_summaries[0] = leftmost->summary;
            if (in.count < MaxChildren) {
                for (uint16_t i = in.count; i > 0; --i) {
                    in.children[i] = std::move(in.children[i - 1]);
                    in.child_summaries[i] = in.child_summaries[i - 1];
                }
                in.children[0] = std::move(other);
                in.child_summaries[0] = in.children[0]->summary;
                ++in.count;
                recompute_summary(*node);
                return nullptr;
            }
            return split_internal_and_prepend(node, std::move(other), node_ids);
        }

        auto overflow = push_tree_left(ensure_mutable(in.children[0]),
                                       std::move(other), node_ids);
        in.child_summaries[0] = in.children[0]->summary;
        recompute_summary(*node);
        if (!overflow) return nullptr;
        if (in.count < MaxChildren) {
            for (uint16_t i = in.count; i > 0; --i) {
                in.children[i] = std::move(in.children[i - 1]);
                in.child_summaries[i] = in.child_summaries[i - 1];
            }
            in.children[0] = std::move(overflow);
            in.child_summaries[0] = in.children[0]->summary;
            ++in.count;
            recompute_summary(*node);
            return nullptr;
        }
        return split_internal_and_prepend(node, std::move(overflow), node_ids);
    }

    // ---- Iteration ----

    template<typename F>
    static void for_each_node(const Node* node, F& fn) {
        if (node->is_leaf()) {
            auto& lf = node->leaf();
            for (uint16_t i = 0; i < lf.count; ++i) {
                fn(lf.items[i]);
            }
        } else {
            auto& in = node->internal();
            for (uint16_t i = 0; i < in.count; ++i) {
                for_each_node(in.children[i].get(), fn);
            }
        }
    }

    // ---- Mutable iteration ----

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
                for_each_node_mut(in.children[i], fn);
                in.child_summaries[i] = in.children[i]->summary;
            }
            recompute_summary(*node);
        }
    }

    // ---- Recompute all summaries ----

    static void recompute_all_summaries_recursive(NodePtr& node) {
        ensure_mutable(node);
        if (node->is_leaf()) {
            auto& lf = node->leaf();
            for (uint16_t i = 0; i < lf.count; ++i) {
                lf.item_summaries[i] = lf.items[i].summary();
            }
            recompute_summary(*node);
        } else {
            auto& in = node->internal();
            for (uint16_t i = 0; i < in.count; ++i) {
                recompute_all_summaries_recursive(in.children[i]);
                in.child_summaries[i] = in.children[i]->summary;
            }
            recompute_summary(*node);
        }
    }

    // ---- edit_item recursive helper ----

    template<typename D, typename F>
    static bool edit_item_recursive(NodePtr& node, const D& target, F& fn,
                                    Bias bias, D cumulative) {
        ensure_mutable(node);
        if (node->is_leaf()) {
            auto& lf = node->leaf();
            for (uint16_t i = 0; i < lf.count; ++i) {
                Diagnostics::record(Diagnostics::Counter::sumtree_items_visited);
                D item_end = cumulative;
                item_end.add_summary(lf.item_summaries[i]);
                bool past = (bias == Bias::Left)
                    ? (target < item_end)
                    : !(item_end < target);
                if (past) {
                    fn(lf.items[i]);
                    lf.item_summaries[i] = lf.items[i].summary();
                    recompute_summary(*node);
                    return true;
                }
                cumulative = item_end;
            }
            return false;
        }

        auto& in = node->internal();
        for (uint16_t i = 0; i < in.count; ++i) {
            D child_end = cumulative;
            child_end.add_summary(in.child_summaries[i]);
            bool past = (bias == Bias::Left)
                ? (target < child_end)
                : !(child_end < target);
            if (past) {
                bool found = edit_item_recursive<D>(
                    ensure_mutable(in.children[i]), target, fn, bias, cumulative);
                if (found) {
                    in.child_summaries[i] = in.children[i]->summary;
                    recompute_summary(*node);
                }
                return found;
            }
            cumulative = child_end;
        }
        return false;
    }

    // ---- insert_item recursive helper ----

    /// Insert item at sought position. Returns overflow node if leaf/internal splits.
    template<typename D>
    static NodePtr insert_item_recursive(NodePtr& node, const D& target,
                                         Item item, Bias bias, D cumulative,
                                         NodeIdPolicy& node_ids) {
        ensure_mutable(node);
        if (node->is_leaf()) {
            auto& lf = node->leaf();
            // Find insertion index: first item where cumulative > target (Left)
            // or cumulative >= target (Right)
            uint16_t insert_idx = lf.count; // default: append
            D pos = cumulative;
            for (uint16_t i = 0; i < lf.count; ++i) {
                D item_end = pos;
                item_end.add_summary(lf.item_summaries[i]);
                bool past = (bias == Bias::Left)
                    ? (target < item_end)
                    : !(item_end < target);
                if (past) {
                    insert_idx = i;
                    break;
                }
                pos = item_end;
            }

            Summary item_sum = item.summary();

            if (lf.count < MaxChildren) {
                // Shift items right to make room
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

            // Leaf is full — split using temp arrays
            // Build a virtual array of MaxChildren+1 items
            uint16_t total = static_cast<uint16_t>(MaxChildren + 1);
            uint16_t mid = total / 2;

            auto right = make_leaf(node_ids);
            auto& rlf = right->leaf();

            // We need to distribute items [0..insert_idx) + new_item + [insert_idx..MaxChildren)
            // Left gets [0..mid), right gets [mid..total)

            // Helper: get item/summary at virtual index j
            // j < insert_idx: original[j]
            // j == insert_idx: new item
            // j > insert_idx: original[j-1]

            // Populate left half
            uint16_t left_count = mid;
            uint16_t right_count = total - mid;

            // We'll rebuild lf in-place for the left, and populate rlf for right.
            // First, save all items we need (since we're modifying lf in place).
            // Actually, we can be clever: if insert_idx >= mid, left half is just
            // lf.items[0..mid) unchanged, and we only need to handle the right half.
            // But for simplicity and correctness, use temp arrays.

            std::array<Item, MaxChildren + 1> tmp_items;
            std::array<Summary, MaxChildren + 1> tmp_summaries;
            for (uint16_t i = 0; i < insert_idx; ++i) {
                tmp_items[i] = std::move(lf.items[i]);
                tmp_summaries[i] = lf.item_summaries[i];
            }
            tmp_items[insert_idx] = std::move(item);
            tmp_summaries[insert_idx] = item_sum;
            for (uint16_t i = insert_idx; i < static_cast<uint16_t>(MaxChildren); ++i) {
                tmp_items[i + 1] = std::move(lf.items[i]);
                tmp_summaries[i + 1] = lf.item_summaries[i];
            }

            // Fill left (reuse current node)
            for (uint16_t i = 0; i < left_count; ++i) {
                lf.items[i] = std::move(tmp_items[i]);
                lf.item_summaries[i] = tmp_summaries[i];
            }
            lf.count = left_count;

            // Fill right
            for (uint16_t i = 0; i < right_count; ++i) {
                rlf.items[i] = std::move(tmp_items[mid + i]);
                rlf.item_summaries[i] = tmp_summaries[mid + i];
            }
            rlf.count = right_count;

            recompute_summary(*node);
            recompute_summary(*right);
            return right;
        }

        // Internal node — seek into the correct child
        auto& in = node->internal();
        uint16_t child_idx = in.count - 1; // default: last child
        D child_start = cumulative; // cumulative position at start of chosen child
        {
            D pos = cumulative;
            for (uint16_t i = 0; i < in.count; ++i) {
                D child_end = pos;
                child_end.add_summary(in.child_summaries[i]);
                bool past = (bias == Bias::Left)
                    ? (target < child_end)
                    : !(child_end < target);
                if (past) {
                    child_idx = i;
                    child_start = pos;
                    break;
                }
                // If this is the last child and we didn't break,
                // child_start should be pos (before advancing past it)
                child_start = pos;
                pos = child_end;
            }
        }

        auto overflow = insert_item_recursive<D>(
            ensure_mutable(in.children[child_idx]), target, std::move(item), bias,
            child_start, node_ids);

        in.child_summaries[child_idx] = in.children[child_idx]->summary;
        recompute_summary(*node);

        if (!overflow) return nullptr;

        // Need to insert overflow after child_idx
        uint16_t insert_at = child_idx + 1;

        if (in.count < MaxChildren) {
            // Shift children right
            for (uint16_t i = in.count; i > insert_at; --i) {
                in.children[i] = std::move(in.children[i - 1]);
                in.child_summaries[i] = in.child_summaries[i - 1];
            }
            in.children[insert_at] = std::move(overflow);
            in.child_summaries[insert_at] = in.children[insert_at]->summary;
            in.count++;
            recompute_summary(*node);
            return nullptr;
        }

        // Internal node full — split
        // Build virtual array of MaxChildren+1 children
        uint16_t total = static_cast<uint16_t>(MaxChildren + 1);
        uint16_t mid = total / 2;

        std::array<NodePtr, MaxChildren + 1> tmp_children;
        std::array<Summary, MaxChildren + 1> tmp_summaries;
        for (uint16_t i = 0; i < insert_at; ++i) {
            tmp_children[i] = std::move(in.children[i]);
            tmp_summaries[i] = in.child_summaries[i];
        }
        tmp_children[insert_at] = std::move(overflow);
        tmp_summaries[insert_at] = tmp_children[insert_at]->summary;
        for (uint16_t i = insert_at; i < static_cast<uint16_t>(MaxChildren); ++i) {
            tmp_children[i + 1] = std::move(in.children[i]);
            tmp_summaries[i + 1] = in.child_summaries[i];
        }

        auto right = make_internal(node->height, node_ids);
        auto& rin = right->internal();

        uint16_t left_count = mid;
        uint16_t right_count = total - mid;

        for (uint16_t i = 0; i < left_count; ++i) {
            in.children[i] = std::move(tmp_children[i]);
            in.child_summaries[i] = tmp_summaries[i];
        }
        // Clear any stale entries in left node
        for (uint16_t i = left_count; i < static_cast<uint16_t>(MaxChildren); ++i) {
            in.children[i] = nullptr;
        }
        in.count = left_count;

        for (uint16_t i = 0; i < right_count; ++i) {
            rin.children[i] = std::move(tmp_children[mid + i]);
            rin.child_summaries[i] = tmp_summaries[mid + i];
        }
        rin.count = right_count;

        recompute_summary(*node);
        recompute_summary(*right);
        return right;
    }

    // ---- remove_item recursive helper ----

    /// Remove item at sought position. Returns true if found and removed.
    template<typename D>
    static bool remove_item_recursive(NodePtr& node, const D& target,
                                      Bias bias, D cumulative) {
        ensure_mutable(node);
        if (node->is_leaf()) {
            auto& lf = node->leaf();
            for (uint16_t i = 0; i < lf.count; ++i) {
                D item_end = cumulative;
                item_end.add_summary(lf.item_summaries[i]);
                bool past = (bias == Bias::Left)
                    ? (target < item_end)
                    : !(item_end < target);
                if (past) {
                    // Remove by shifting left
                    for (uint16_t j = i; j + 1 < lf.count; ++j) {
                        lf.items[j] = std::move(lf.items[j + 1]);
                        lf.item_summaries[j] = lf.item_summaries[j + 1];
                    }
                    lf.count--;
                    recompute_summary(*node);
                    return true;
                }
                cumulative = item_end;
            }
            return false;
        }

        auto& in = node->internal();
        for (uint16_t i = 0; i < in.count; ++i) {
            D child_end = cumulative;
            child_end.add_summary(in.child_summaries[i]);
            bool past = (bias == Bias::Left)
                ? (target < child_end)
                : !(child_end < target);
            if (past) {
                bool found = remove_item_recursive<D>(
                    ensure_mutable(in.children[i]), target, bias, cumulative);
                if (!found) return false;

                // Check if child became empty
                bool child_empty = in.children[i]->is_leaf()
                    ? (in.children[i]->leaf().count == 0)
                    : (in.children[i]->internal().count == 0);

                if (child_empty) {
                    // Remove empty child by shifting left
                    for (uint16_t j = i; j + 1 < in.count; ++j) {
                        in.children[j] = std::move(in.children[j + 1]);
                        in.child_summaries[j] = in.child_summaries[j + 1];
                    }
                    in.children[in.count - 1] = nullptr;
                    in.count--;
                } else {
                    in.child_summaries[i] = in.children[i]->summary;
                }
                recompute_summary(*node);
                return true;
            }
            cumulative = child_end;
        }
        return false;
    }
};

} // namespace CollabText::Crdt
