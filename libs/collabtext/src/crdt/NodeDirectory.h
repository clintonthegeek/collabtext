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
