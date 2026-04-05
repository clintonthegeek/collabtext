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
            m_root = make_leaf();
        }
        auto overflow = insert_item_recursive<D>(ensure_mutable(m_root), target, std::move(item), bias, D::zero());
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
                                         Item item, Bias bias, D cumulative) {
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

            auto right = make_leaf();
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
            ensure_mutable(in.children[child_idx]), target, std::move(item), bias, child_start);

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

        auto right = make_internal(node->height);
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
