#pragma once

#include "crdt/OriginIndex.h"
#include "crdt/PersistentAvlMap.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace CollabText::Crdt {

// Maps state-local stable run handles to the current zero-based tree ordinal.
// The ordinal is a short-lived locator; callers retain only RunId.
class RunDirectory {
    using Map = PersistentAvlMap<RunId, uint64_t>;

public:
    size_t size() const noexcept { return m_entries.size(); }

    std::optional<uint64_t> ordinal(RunId run) const {
        const auto entry = m_entries.find(run);
        if (!entry) return std::nullopt;
        return *entry;
    }

    bool rebuild(std::vector<std::pair<RunId, uint64_t>> entries) {
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            return a.first < b.first;
        });
        for (size_t i = 0; i < entries.size(); ++i) {
            if (!entries[i].first || (i && entries[i - 1].first == entries[i].first))
                return false;
        }
        m_entries = Map::from_sorted(entries);
        return true;
    }

private:
    Map m_entries;
};

} // namespace CollabText::Crdt
