#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace CollabText::Crdt {

// Immutable, path-copied AVL map. Copies share their roots; updates allocate
// only the search path and any nodes needed to restore AVL balance.
template<class Key, class Value, class Compare = std::less<Key>>
class PersistentAvlMap {
    struct Node;
    using Root = std::shared_ptr<const Node>;
    struct Node {
        Key key;
        Value value;
        Root left;
        Root right;
        int height;
        size_t count;

        Node(Key k, Value v, Root l, Root r)
            : key(std::move(k)), value(std::move(v)), left(std::move(l)),
              right(std::move(r)),
              height(1 + std::max(node_height(left), node_height(right))),
              count(1 + node_count(left) + node_count(right)) {}
        static int node_height(const Root& n) { return n ? n->height : 0; }
        static size_t node_count(const Root& n) { return n ? n->count : 0; }
    };

public:
    PersistentAvlMap() = default;
    size_t size() const noexcept { return Node::node_count(m_root); }
    bool empty() const noexcept { return !m_root; }

    const Value* find(const Key& key) const noexcept {
        auto n = m_root;
        while (n) {
            if (m_compare(key, n->key)) n = n->left;
            else if (m_compare(n->key, key)) n = n->right;
            else return &n->value;
        }
        return nullptr;
    }

    // Greatest entry whose key is <= key.
    std::optional<std::pair<Key, Value>> predecessor(const Key& key) const {
        auto n = m_root;
        Root best;
        while (n) {
            if (!m_compare(key, n->key)) { best = n; n = n->right; }
            else n = n->left;
        }
        if (!best) return std::nullopt;
        return std::pair<Key, Value>{best->key, best->value};
    }

    // Least entry whose key is >= key.
    std::optional<std::pair<Key, Value>> lower_bound(const Key& key) const {
        auto n = m_root;
        Root best;
        while (n) {
            if (!m_compare(n->key, key)) { best = n; n = n->left; }
            else n = n->right;
        }
        if (!best) return std::nullopt;
        return std::pair<Key, Value>{best->key, best->value};
    }

    PersistentAvlMap insert_or_assign(Key key, Value value) const {
        PersistentAvlMap result = *this;
        result.m_root = insert(m_root, std::move(key), std::move(value));
        return result;
    }

    // Build a balanced tree from strictly increasing keys in linear node
    // allocations. Used when a derived index is reconstructed from a scan.
    static PersistentAvlMap from_sorted(
        const std::vector<std::pair<Key, Value>>& entries) {
        PersistentAvlMap result;
        result.m_root = build_balanced(entries, 0, entries.size());
        return result;
    }

    PersistentAvlMap erase(const Key& key) const {
        PersistentAvlMap result = *this;
        result.m_root = remove(m_root, key);
        return result;
    }

    template<class Fn> void for_each(Fn&& fn) const { visit(m_root, fn); }

    // Visit keys in ascending order from lower inclusive; return false from
    // fn to stop the traversal. Subtrees below the lower bound are pruned.
    template<class Fn> void for_each_from(const Key& lower, Fn&& fn) const {
        visit_from(m_root, lower, fn, m_compare);
    }

private:
    static int height(const Root& n) { return Node::node_height(n); }
    static Root make(const Key& k, const Value& v, Root l, Root r) {
        return std::make_shared<Node>(k, v, std::move(l), std::move(r));
    }
    static Root build_balanced(const std::vector<std::pair<Key, Value>>& entries,
                               size_t first, size_t last) {
        if (first == last) return {};
        const size_t middle = first + (last - first) / 2;
        auto left = build_balanced(entries, first, middle);
        auto right = build_balanced(entries, middle + 1, last);
        return make(entries[middle].first, entries[middle].second,
                    std::move(left), std::move(right));
    }
    static Root rotate_left(const Root& n) {
        const auto& r = n->right;
        auto left = make(n->key, n->value, n->left, r->left);
        return make(r->key, r->value, std::move(left), r->right);
    }
    static Root rotate_right(const Root& n) {
        const auto& l = n->left;
        auto right = make(n->key, n->value, l->right, n->right);
        return make(l->key, l->value, l->left, std::move(right));
    }
    static Root rebalance(Root n) {
        const int balance = height(n->left) - height(n->right);
        if (balance > 1) {
            if (height(n->left->left) < height(n->left->right))
                n = make(n->key, n->value, rotate_left(n->left), n->right);
            return rotate_right(n);
        }
        if (balance < -1) {
            if (height(n->right->right) < height(n->right->left))
                n = make(n->key, n->value, n->left, rotate_right(n->right));
            return rotate_left(n);
        }
        return n;
    }
    Root insert(const Root& n, Key key, Value value) const {
        if (!n) return make(key, value, {}, {});
        if (m_compare(key, n->key))
            return rebalance(make(n->key, n->value,
                                  insert(n->left, std::move(key), std::move(value)), n->right));
        if (m_compare(n->key, key))
            return rebalance(make(n->key, n->value, n->left,
                                  insert(n->right, std::move(key), std::move(value))));
        return make(key, value, n->left, n->right);
    }
    Root remove(const Root& n, const Key& key) const {
        if (!n) return {};
        if (m_compare(key, n->key)) {
            auto left = remove(n->left, key);
            if (left == n->left) return n;
            return rebalance(make(n->key, n->value, std::move(left), n->right));
        }
        if (m_compare(n->key, key)) {
            auto right = remove(n->right, key);
            if (right == n->right) return n;
            return rebalance(make(n->key, n->value, n->left, std::move(right)));
        }
        if (!n->left) return n->right;
        if (!n->right) return n->left;
        auto successor = n->right;
        while (successor->left) successor = successor->left;
        return rebalance(make(successor->key, successor->value, n->left,
                              remove(n->right, successor->key)));
    }
    template<class Fn> static void visit(const Root& n, Fn& fn) {
        if (!n) return;
        visit(n->left, fn);
        fn(n->key, n->value);
        visit(n->right, fn);
    }
    template<class Fn>
    static bool visit_from(const Root& n, const Key& lower, Fn& fn,
                           const Compare& compare) {
        if (!n) return true;
        if (compare(n->key, lower))
            return visit_from(n->right, lower, fn, compare);
        if (!visit_from(n->left, lower, fn, compare)) return false;
        if (!fn(n->key, n->value)) return false;
        return visit_from(n->right, lower, fn, compare);
    }

    Root m_root;
    Compare m_compare;
};

} // namespace CollabText::Crdt
