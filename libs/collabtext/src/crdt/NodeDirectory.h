#pragma once

#include "crdt/PersistentAvlMap.h"
#include "crdt/TreeNodeId.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace CollabText::Crdt {

template<class Node>
struct NodeLocation {
    std::weak_ptr<Node> node;
    std::optional<NodeId> parent;
    uint16_t child_slot = 0;
};

// Persistent, non-owning topology lookup for one SumTree state. The tree root
// owns nodes; weak entries here must never affect copy-on-write ownership.
template<class Node>
class NodeDirectory {
    using Entry = NodeLocation<Node>;
    using Map = PersistentAvlMap<NodeId, Entry>;

public:
    static constexpr size_t node_payload_bytes() noexcept {
        return Map::node_payload_bytes();
    }
    size_t size() const noexcept { return m_entries.size(); }
    size_t payload_bytes() const noexcept { return size() * node_payload_bytes(); }

    std::optional<Entry> locate(NodeId id) const {
        const auto* entry = m_entries.find(id);
        if (!entry) return std::nullopt;
        return *entry;
    }

    // Return the retained-sequence position of an item in a leaf by walking
    // this tree version's parent directory and composing sibling summaries.
    // The weak references are checked against every recorded edge so a stale
    // directory cannot turn into a plausible but incorrect rank.
    template<class Dimension>
    std::optional<Dimension> rank_of_item(
        const std::shared_ptr<Node>& root, NodeId leaf_id,
        uint16_t item_slot) const {
        if (!root || !leaf_id.valid()) return std::nullopt;
        auto leaf_location = locate(leaf_id);
        if (!leaf_location) return std::nullopt;
        auto node = leaf_location->node.lock();
        if (!node || node->node_id != leaf_id || !node->is_leaf() ||
            item_slot >= node->leaf().count)
            return std::nullopt;

        Dimension rank = Dimension::zero();
        const auto& leaf = node->leaf();
        for (uint16_t slot = 0; slot < item_slot; ++slot)
            rank.add_summary(leaf.item_summaries[slot]);

        size_t climbed = 0;
        while (true) {
            if (++climbed > m_entries.size()) return std::nullopt;
            const auto current_location = locate(node->node_id);
            if (!current_location || current_location->node.lock().get() != node.get())
                return std::nullopt;
            if (!current_location->parent) {
                if (node.get() != root.get() || current_location->child_slot != 0)
                    return std::nullopt;
                return rank;
            }

            const NodeId parent_id = *current_location->parent;
            const auto parent_location = locate(parent_id);
            if (!parent_location) return std::nullopt;
            auto parent = parent_location->node.lock();
            if (!parent || parent->node_id != parent_id || parent->is_leaf())
                return std::nullopt;
            const auto& children = parent->internal();
            const uint16_t child_slot = current_location->child_slot;
            if (child_slot >= children.count ||
                children.children[child_slot].get() != node.get())
                return std::nullopt;
            for (uint16_t slot = 0; slot < child_slot; ++slot)
                rank.add_summary(children.child_summaries[slot]);
            node = std::move(parent);
        }
    }

    bool insert_or_assign(NodeId id, Entry entry) {
        if (!id.valid()) return false;
        m_entries = m_entries.insert_or_assign(id, std::move(entry));
        return true;
    }

    bool erase(NodeId id) {
        if (!m_entries.find(id)) return false;
        m_entries = m_entries.erase(id);
        return true;
    }

    // Refresh the nodes whose ownership or parent/slot edge can change after
    // a path-copy mutation. A path node's direct children are included because
    // a split can insert a sibling or move a bounded set of children. This is
    // O(B log N) map work per tree level and leaves unrelated subtrees alone.
    // It does not remove IDs for nodes deleted by a structural edit; callers
    // must erase those entries or rebuild the directory for that candidate.
    template<class Tree, class Dimension>
    bool refresh_path(const Tree& tree, const Dimension& target) {
        bool updated = true;
        bool complete = tree.template for_each_node_on_path<Dimension>(target,
            [&](const typename Tree::NodePtr& node, NodeId id,
                std::optional<NodeId> parent, uint16_t slot) {
                updated = insert_or_assign(id, Entry{node, parent, slot}) && updated;
                if (node->is_leaf()) return;
                const auto& children = node->internal();
                for (uint16_t child = 0; child < children.count; ++child) {
                    const auto& child_node = children.children[child];
                    updated = insert_or_assign(child_node->node_id,
                        Entry{child_node, id, child}) && updated;
                }
            });
        return complete && updated;
    }

    bool rebuild(std::vector<std::pair<NodeId, Entry>> entries) {
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t i = 0; i < entries.size(); ++i) {
            if (!entries[i].first.valid() ||
                (i && entries[i - 1].first == entries[i].first)) return false;
        }
        m_entries = Map::from_sorted(entries);
        return true;
    }

private:
    Map m_entries;
};

} // namespace CollabText::Crdt
