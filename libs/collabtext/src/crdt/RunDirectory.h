#pragma once

#include "crdt/OriginIndex.h"
#include "crdt/PersistentAvlMap.h"
#include "crdt/TreeNodeId.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace CollabText::Crdt {

struct RunLocation {
    NodeId leaf;
    uint16_t slot = 0;
    auto operator<=>(const RunLocation&) const = default;
};

// Maps state-local stable run handles to a checked leaf identity and slot.
// Retained rank is resolved through the owning state's NodeDirectory.
class RunDirectory {
    using Map = PersistentAvlMap<RunId, RunLocation>;

public:
    static constexpr size_t node_payload_bytes() noexcept {
        return Map::node_payload_bytes();
    }
    size_t size() const noexcept { return m_entries.size(); }
    size_t payload_bytes() const noexcept { return size() * node_payload_bytes(); }

    std::optional<RunLocation> locate(RunId run) const {
        const auto entry = m_entries.find(run);
        if (!entry) return std::nullopt;
        return *entry;
    }

    bool insert(RunId run, RunLocation location) {
        if (!location.leaf.valid()) return false;
        if (!run || m_entries.find(run)) return false;
        m_entries = m_entries.insert_or_assign(run, location);
        return true;
    }

    bool insert_or_assign(RunId run, RunLocation location) {
        if (!run || !location.leaf.valid()) return false;
        m_entries = m_entries.insert_or_assign(run, location);
        return true;
    }

    bool erase(RunId run) {
        if (!m_entries.find(run)) return false;
        m_entries = m_entries.erase(run);
        return true;
    }

    bool rebuild(std::vector<std::pair<RunId, RunLocation>> entries) {
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            return a.first < b.first;
        });
        for (size_t i = 0; i < entries.size(); ++i) {
            if (!entries[i].first || (i && entries[i - 1].first == entries[i].first))
                return false;
        }
        std::vector<RunLocation> locations;
        locations.reserve(entries.size());
        for (const auto& entry : entries) {
            if (!entry.second.leaf.valid()) return false;
            locations.push_back(entry.second);
        }
        std::sort(locations.begin(), locations.end());
        if (std::adjacent_find(locations.begin(), locations.end()) != locations.end())
            return false;
        m_entries = Map::from_sorted(entries);
        return true;
    }

private:
    Map m_entries;
};

} // namespace CollabText::Crdt
