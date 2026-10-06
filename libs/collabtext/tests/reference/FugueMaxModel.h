#pragma once
// Independent FugueMax reference (roadmap task S03).
//
// A literal transcription of Algorithm 1 from Weidner and Kleppmann, "The Art
// of the Fugue: Minimizing Interleaving in Collaborative Text Editing" (arXiv
// 2305.00583), with the two FugueMax modifications of their Section 5.3:
//
//   1. a right child is additionally tagged with its right origin;
//   2. right-side siblings are ordered by the REVERSE list order of their right
//      origins, ties broken by identity.
//
// It is deliberately a tree with explicit left/right child lists and an in-order
// traversal -- the paper's own representation -- and shares nothing with the
// production engine, `Locator`, fragments or the S02 `SequenceReference` model.
// Its job is to be a second opinion that the production placement code is
// differentially tested against, and that the S02 model is cross-checked with.
//
// What "identity" order means here. Fugue only needs *some* total order on
// element ids ("lower ID earlier", Definition 4 condition 3; Theorem 10 shows
// the order is otherwise forced). The engine uses its existing Identity order:
// counter first, then actor bytes. The caller supplies it as `IdLess`.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace FugueReference {

struct Id {
    std::string actor;   // opaque; ordered by the caller's IdLess
    uint64_t counter = 0;
    bool operator==(const Id&) const = default;
};

enum class Side { Left, Right };

/// The message a replica broadcasts for ONE element (Algorithm 1, line 29).
/// A multi-element insert is a chain of these, each a right child of the last.
struct NodeMsg {
    Id id;
    std::string value;
    bool parent_is_root = false;   // parent is the virtual root (document start)
    Id parent;                     // meaningful unless parent_is_root
    Side side = Side::Right;
    bool ro_is_end = true;         // right origin is the end of the list
    Id right_origin;               // meaningful unless ro_is_end; == parent for Side::Left
    // Not used by the algorithm. Recorded so a history can be re-expressed in the
    // S02 model's (left boundary, right boundary) vocabulary for cross-checking.
    bool left_origin_is_start = true;
    Id left_origin;
};

class Tree {
public:
    using IdLess = std::function<bool(const Id&, const Id&)>;

    explicit Tree(IdLess less) : less_(std::move(less)) {}

    // ----- local operations (Algorithm 1 `insert`, `delete`) -----------------

    /// Mint and apply the nodes for inserting `values` at visible index `index`.
    /// `first_id` is the first element's identity; the rest follow with
    /// consecutive counters. Returns one message per element, in order.
    std::vector<NodeMsg> insert(size_t index, const std::vector<std::string>& values,
                                const Id& first_id) {
        std::vector<NodeMsg> out;
        for (size_t n = 0; n < values.size(); ++n) {
            Id id{first_id.actor, first_id.counter + n};
            out.push_back(mint(index + n, values[n], id));
        }
        return out;
    }

    /// Visible index -> element id.
    std::optional<Id> visible_at(size_t index) const {
        size_t seen = 0;
        for (const auto& id : traverse_all()) {
            if (nodes_.at(id).deleted) continue;
            if (seen++ == index) return id;
        }
        return std::nullopt;
    }

    void remove(const Id& id) { nodes_.at(id).deleted = true; }

    // ----- remote delivery (Algorithm 1 `on delivering insert`) ---------------

    void deliver(const NodeMsg& msg) {
        if (nodes_.count(msg.id)) return;   // idempotent
        Node node;
        node.id = msg.id;
        node.value = msg.value;
        node.parent_is_root = msg.parent_is_root;
        node.parent = msg.parent;
        node.side = msg.side;
        node.ro_is_end = msg.ro_is_end;
        node.right_origin = msg.right_origin;

        auto& sibs = siblings(msg.parent_is_root, msg.parent, msg.side);
        size_t index = sibs.size();
        if (msg.side == Side::Right) {
            // Positions are only needed to compare right origins.
            const auto pos = positions();
            auto rank = [&](bool is_end, const Id& id) {
                return is_end ? SIZE_MAX : pos.at(id);
            };
            const size_t mine = rank(node.ro_is_end, node.right_origin);
            for (size_t i = 0; i < sibs.size(); ++i) {
                const Node& other = nodes_.at(sibs[i]);
                const size_t theirs = rank(other.ro_is_end, other.right_origin);
                if (mine > theirs || (mine == theirs && less_(node.id, other.id))) {
                    index = i;
                    break;
                }
            }
        } else {
            for (size_t i = 0; i < sibs.size(); ++i) {
                if (less_(node.id, sibs[i])) { index = i; break; }
            }
        }
        const Id id = node.id;
        sibs.insert(sibs.begin() + static_cast<ptrdiff_t>(index), id);
        nodes_.emplace(id, std::move(node));
    }

    // ----- queries -------------------------------------------------------------

    /// In-order traversal, tombstones included.
    std::vector<Id> traverse_all() const {
        std::vector<Id> out;
        for (const auto& id : root_right_) walk(id, out);
        return out;
    }
    std::vector<Id> visible() const {
        std::vector<Id> out;
        for (const auto& id : traverse_all())
            if (!nodes_.at(id).deleted) out.push_back(id);
        return out;
    }
    std::string text() const {
        std::string out;
        for (const auto& id : visible()) out += nodes_.at(id).value;
        return out;
    }
    bool contains(const Id& id) const { return nodes_.count(id) != 0; }

    /// The element after `left` in the full list (the document start when
    /// `left_is_start`), tombstones included; nullopt at the end of the list.
    std::optional<Id> next_in_list(bool left_is_start, const Id& left) const {
        return next_after(left_is_start, left);
    }
    /// Like next_in_list, but skipping tombstones: the neighbour a reader of the
    /// visible text would call "to the right". S01's fixtures spell boundaries
    /// this way; FugueMax's right origin deliberately does not.
    std::optional<Id> next_visible_in_list(bool left_is_start, const Id& left) const {
        const auto all = traverse_all();
        size_t at = 0;
        if (!left_is_start) {
            while (at < all.size() && !(all[at] == left)) ++at;
            ++at;
        }
        for (; at < all.size(); ++at)
            if (!nodes_.at(all[at]).deleted) return all[at];
        return std::nullopt;
    }
    /// Whether `left` (or the root) already has a right child.
    bool has_right_child(bool left_is_start, const Id& left) const {
        return left_is_start ? !root_right_.empty() : !nodes_.at(left).right.empty();
    }
    size_t size() const { return nodes_.size(); }

    /// True when `child` has `ancestor` on its path to the root, in the Fugue
    /// tree (the tree whose traversal is the list). Used by the property checks.
    bool is_descendant(const Id& child, const Id& ancestor) const {
        const Node* at = &nodes_.at(child);
        while (!at->parent_is_root) {
            if (at->parent == ancestor) return true;
            at = &nodes_.at(at->parent);
        }
        return false;
    }

private:
    struct Node {
        Id id;
        std::string value;
        bool parent_is_root = true;
        Id parent;
        Side side = Side::Right;
        bool ro_is_end = true;
        Id right_origin;
        bool deleted = false;
        std::vector<Id> left;
        std::vector<Id> right;
    };

    IdLess less_;
    struct Key {
        bool operator()(const Id& a, const Id& b) const {
            return a.actor != b.actor ? a.actor < b.actor : a.counter < b.counter;
        }
    };
    struct NodeMap {
        std::map<Id, Node, Key> map;
        Node& at(const Id& id) { return map.at(id); }
        const Node& at(const Id& id) const { return map.at(id); }
        size_t count(const Id& id) const { return map.count(id); }
        size_t size() const { return map.size(); }
        void emplace(const Id& id, Node n) { map.emplace(id, std::move(n)); }
    } nodes_;
    std::vector<Id> root_right_;   // the root has only right children

    std::vector<Id>& siblings(bool parent_is_root, const Id& parent, Side side) {
        if (parent_is_root) return root_right_;
        Node& p = nodes_.at(parent);
        return side == Side::Left ? p.left : p.right;
    }

    void walk(const Id& id, std::vector<Id>& out) const {
        const Node& n = nodes_.at(id);
        for (const auto& child : n.left) walk(child, out);
        out.push_back(id);
        for (const auto& child : n.right) walk(child, out);
    }

    std::map<Id, size_t, Key> positions() const {
        std::map<Id, size_t, Key> pos;
        size_t at = 0;
        for (const auto& id : traverse_all()) pos[id] = at++;
        return pos;
    }

    /// The element after `id` in the traversal, tombstones included.
    std::optional<Id> next_after(bool is_root, const Id& id) const {
        const auto all = traverse_all();
        if (is_root) return all.empty() ? std::nullopt : std::optional<Id>(all.front());
        for (size_t i = 0; i < all.size(); ++i)
            if (all[i] == id) return i + 1 < all.size() ? std::optional<Id>(all[i + 1]) : std::nullopt;
        return std::nullopt;
    }

    NodeMsg mint(size_t index, const std::string& value, const Id& id) {
        // leftOrigin: the (index-1)th VISIBLE element, or the root.
        bool left_is_root = (index == 0);
        Id left;
        if (!left_is_root) left = *visible_at(index - 1);
        // rightOrigin: the next node after leftOrigin, tombstones included.
        const auto right = next_after(left_is_root, left);

        const bool has_right_child = left_is_root
            ? !root_right_.empty()
            : !nodes_.at(left).right.empty();

        NodeMsg msg;
        msg.id = id;
        msg.value = value;
        msg.left_origin_is_start = left_is_root;
        msg.left_origin = left;
        msg.ro_is_end = !right.has_value();
        if (right) msg.right_origin = *right;
        if (!has_right_child) {
            msg.parent_is_root = left_is_root;
            msg.parent = left;
            msg.side = Side::Right;
        } else {
            // leftOrigin has right children, so rightOrigin exists and is a
            // descendant of it (Theorem 1's proof): attach on the left of it.
            msg.parent_is_root = false;
            msg.parent = *right;
            msg.side = Side::Left;
        }
        deliver(msg);
        return msg;
    }
};

}  // namespace FugueReference
