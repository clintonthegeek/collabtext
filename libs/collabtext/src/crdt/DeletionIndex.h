#pragma once

#include "crdt/PersistentAvlMap.h"
#include "crdt/OriginIndex.h"
#include <vector>
#include <algorithm>

namespace CollabText::Crdt {

struct DeletionIncidence {
    ActorId actor;
    uint64_t first = 0;
    uint64_t end = 0; // exclusive
    RunId run = 0;
};

// Reverse map from a deleting operation to the retained identity intervals it
// labels. Overlapping deletions have independent keys and are retained through
// undo; undo changes visibility, not incidence.
class DeletionIndex {
    using Key = std::tuple<Identity, ActorId, uint64_t>;
    using Map = PersistentAvlMap<Key, DeletionIncidence>;
public:
    size_t size() const noexcept { return m_entries.size(); }

    bool insert(const Identity& deletion, const ActorId& actor,
                uint64_t first, uint64_t end, RunId run) {
        if (!deletion.actor.valid() || !deletion.counter || !actor.valid() ||
            first == 0 || end <= first || run == 0) return false;
        const Key key{deletion, actor, first};
        if (m_entries.find(key)) return false;
        auto before = m_entries.predecessor(key);
        if (before && std::get<0>(before->first) == deletion &&
            std::get<1>(before->first) == actor && before->second.end > first)
            return false;
        auto after = m_entries.lower_bound(key);
        if (after && std::get<0>(after->first) == deletion &&
            std::get<1>(after->first) == actor && std::get<2>(after->first) < end)
            return false;
        m_entries = m_entries.insert_or_assign(
            key, DeletionIncidence{actor, first, end, run});
        return true;
    }

    bool rebuild(std::vector<std::pair<Key, DeletionIncidence>> sorted) {
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
            return a.first < b.first;
        });
        for (size_t i = 0; i < sorted.size(); ++i) {
            const auto& [key, value] = sorted[i];
            if (!std::get<0>(key).actor.valid() || !std::get<0>(key).counter ||
                !value.actor.valid() || !value.first || value.end <= value.first ||
                !value.run || value.actor != std::get<1>(key) ||
                value.first != std::get<2>(key)) return false;
            if (i && std::get<0>(sorted[i - 1].first) == std::get<0>(key) &&
                std::get<1>(sorted[i - 1].first) == std::get<1>(key) &&
                std::get<2>(sorted[i - 1].first) == std::get<2>(key)) return false;
            if (i && std::get<0>(sorted[i - 1].first) == std::get<0>(key) &&
                std::get<1>(sorted[i - 1].first) == std::get<1>(key) &&
                sorted[i - 1].second.end > value.first) return false;
        }
        m_entries = Map::from_sorted(sorted);
        return true;
    }

    bool erase(const Identity& deletion, const ActorId& actor, uint64_t first) {
        const Key key{deletion, actor, first};
        if (!m_entries.find(key)) return false;
        m_entries = m_entries.erase(key);
        return true;
    }

    std::vector<DeletionIncidence> incidences(const Identity& deletion) const {
        std::vector<DeletionIncidence> result;
        const Key lower{deletion, ActorId{}, 0};
        m_entries.for_each_from(lower, [&](const Key& key, const DeletionIncidence& value) {
            if (std::get<0>(key) != deletion) return false;
            result.push_back(value);
            return true;
        });
        return result;
    }

private:
    Map m_entries;
};

} // namespace CollabText::Crdt
