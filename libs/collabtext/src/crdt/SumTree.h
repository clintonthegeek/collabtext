#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <compare>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
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
// Bias
// ============================================================================

enum class Bias { Left, Right };

// ============================================================================
// SumTree
// ============================================================================

template<Summarizable Item, std::size_t B = 6>
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
            SumTree result;

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
                            result.push_tree(SumTree(in.children[level.index]));
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

            return result;
        }

        /// Extract all remaining items from cursor to end as a new SumTree.
        SumTree suffix() {
            SumTree result;

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
                        result.push_tree(SumTree(in.children[level.index]));
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

    /// Construct a tree wrapping a single node (for slice/push_tree).
    explicit SumTree(NodePtr root) : m_root(std::move(root)) {}

    // ---- Queries ----

    const Summary& summary() const {
        static const Summary s_zero = Summary::zero();
        return m_root ? m_root->summary : s_zero;
    }

    bool empty() const {
        if (!m_root) return true;
        if (m_root->is_leaf()) return m_root->leaf().count == 0;
        return false;
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
        if (m_root) for_each_node(m_root.get(), fn);
    }

    /// Flatten to a vector of items.
    std::vector<Item> items() const {
        std::vector<Item> result;
        for_each([&](const Item& item) { result.push_back(item); });
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
        Summary item_sum = item.summary();

        if (!m_root) {
            m_root = make_leaf();
        }

        // Push into the rightmost leaf. If it overflows, split upward.
        auto overflow = push_item_recursive(ensure_mutable(m_root), std::move(item), item_sum);
        if (overflow) {
            // Root split — create new root
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
            merge_same_height(std::move(other.m_root));
        } else if (self_h > other_h) {
            // Push other into our rightmost path
            auto overflow = push_tree_right(ensure_mutable(m_root), std::move(other.m_root));
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
        } else {
            // Other is taller — grow self to match, then merge
            while (m_root->height < other_h) {
                auto wrapper = make_internal(m_root->height + 1);
                auto& in = wrapper->internal();
                in.children[0] = std::move(m_root);
                in.child_summaries[0] = in.children[0]->summary;
                in.count = 1;
                recompute_summary(*wrapper);
                m_root = std::move(wrapper);
            }
            merge_same_height(std::move(other.m_root));
        }
    }

    /// Extend with items from a vector.
    void extend(const std::vector<Item>& items) {
        for (auto& item : items) push_item(Item(item));
    }

private:
    NodePtr m_root;

    // ---- Node creation helpers ----

    static NodePtr make_leaf() {
        auto node = std::make_shared<Node>();
        node->height = 0;
        node->data = Leaf{};
        return node;
    }

    static NodePtr make_internal(uint8_t height) {
        assert(height > 0);
        auto node = std::make_shared<Node>();
        node->height = height;
        node->data = Internal{};
        return node;
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

    // ---- push_item internals ----

    /// Push item into rightmost leaf. Returns overflow node if split needed.
    static NodePtr push_item_recursive(NodePtr& node, Item item, const Summary& item_sum) {
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
            return split_leaf_and_push(node, std::move(item), item_sum);
        }

        // Internal node — recurse into rightmost child
        auto& in = node->internal();
        assert(in.count > 0);
        auto overflow = push_item_recursive(
            ensure_mutable(in.children[in.count - 1]),
            std::move(item), item_sum);

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
        return split_internal_and_add(node, std::move(overflow));
    }

    /// Split a full leaf, push item into the appropriate half, return right half.
    static NodePtr split_leaf_and_push(NodePtr& node, Item item, const Summary& item_sum) {
        auto& lf = node->leaf();
        assert(lf.count == MaxChildren);

        uint16_t mid = static_cast<uint16_t>(MaxChildren / 2);
        auto right = make_leaf();
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
    static NodePtr split_internal_and_add(NodePtr& node, NodePtr overflow) {
        auto& in = node->internal();
        assert(in.count == MaxChildren);

        uint16_t mid = static_cast<uint16_t>(MaxChildren / 2);
        auto right = make_internal(node->height);
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

    // ---- push_tree internals ----

    void merge_same_height(NodePtr other) {
        // Same height — try to merge children, else create new root
        if (m_root->is_leaf()) {
            auto& lf = ensure_mutable(m_root)->leaf();
            auto& olf = other->leaf();
            if (lf.count + olf.count <= MaxChildren) {
                for (uint16_t i = 0; i < olf.count; ++i) {
                    lf.items[lf.count] = Item(olf.items[i]);
                    lf.item_summaries[lf.count] = olf.item_summaries[i];
                    lf.count++;
                }
                recompute_summary(*m_root);
                return;
            }
        } else {
            auto& in = ensure_mutable(m_root)->internal();
            auto& oin = other->internal();
            if (in.count + oin.count <= MaxChildren) {
                for (uint16_t i = 0; i < oin.count; ++i) {
                    in.children[in.count] = oin.children[i];
                    in.child_summaries[in.count] = oin.child_summaries[i];
                    in.count++;
                }
                recompute_summary(*m_root);
                return;
            }
        }

        // Can't merge — create new root
        auto new_root = make_internal(m_root->height + 1);
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
    static NodePtr push_tree_right(NodePtr& node, NodePtr other) {
        ensure_mutable(node);
        assert(node->height > other->height);

        auto& in = node->internal();
        assert(in.count > 0);

        if (node->height == other->height + 1) {
            // other should become a child or merge with rightmost child
            auto& rightmost = in.children[in.count - 1];

            // Try to merge rightmost child with other
            if (rightmost->is_leaf() && other->is_leaf()) {
                auto& rlf = ensure_mutable(rightmost)->leaf();
                auto& olf = other->leaf();
                if (rlf.count + olf.count <= MaxChildren) {
                    for (uint16_t i = 0; i < olf.count; ++i) {
                        rlf.items[rlf.count] = Item(olf.items[i]);
                        rlf.item_summaries[rlf.count] = olf.item_summaries[i];
                        rlf.count++;
                    }
                    recompute_summary(*rightmost);
                    in.child_summaries[in.count - 1] = rightmost->summary;
                    recompute_summary(*node);
                    return nullptr;
                }
            } else if (!rightmost->is_leaf() && !other->is_leaf()) {
                auto& rin = ensure_mutable(rightmost)->internal();
                auto& oin = other->internal();
                if (rin.count + oin.count <= MaxChildren) {
                    for (uint16_t i = 0; i < oin.count; ++i) {
                        rin.children[rin.count] = oin.children[i];
                        rin.child_summaries[rin.count] = oin.child_summaries[i];
                        rin.count++;
                    }
                    recompute_summary(*rightmost);
                    in.child_summaries[in.count - 1] = rightmost->summary;
                    recompute_summary(*node);
                    return nullptr;
                }
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
            return split_internal_and_add(node, std::move(other));
        }

        // Recurse deeper
        auto overflow = push_tree_right(
            ensure_mutable(in.children[in.count - 1]),
            std::move(other));

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

        return split_internal_and_add(node, std::move(overflow));
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
};

} // namespace CollabText::Crdt
