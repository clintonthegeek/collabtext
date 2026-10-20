#pragma once

#include "crdt/PersistentAvlMap.h"
#include <collabtext/Engine.h>
#include <cstdint>
#include <optional>
#include <tuple>
#include <vector>
#include <algorithm>

namespace CollabText::Crdt {

using RunId = uint64_t;

struct OriginInterval {
    uint64_t end = 0; // exclusive
    RunId run = 0;
};

struct OriginIntervalEntry {
    ActorId actor;
    uint64_t first = 0;
    uint64_t end = 0;
    RunId run = 0;
};

// Actor/counter intervals partition retained CRDT identities without
// allocating one index entry per scalar.
class OriginIndex {
    using Key = std::pair<ActorId, uint64_t>;
    using Map = PersistentAvlMap<Key, OriginInterval>;
public:
    size_t size() const noexcept { return m_entries.size(); }
    bool empty() const noexcept { return m_entries.empty(); }

    // Insert a non-empty half-open interval. Overlap or invalid run/range
    // returns false and leaves this value unchanged.
    bool insert(const ActorId& actor, uint64_t first, uint64_t end, RunId run) {
        if (!actor.valid() || first == 0 || end <= first || run == 0) return false;
        const Key key{actor, first};
        if (m_entries.find(key)) return false;
        auto before = m_entries.predecessor(key);
        if (before && before->first.first == actor && before->second.end > first)
            return false;
        auto after = m_entries.lower_bound(key);
        if (after && after->first.first == actor && after->first.second < end)
            return false;
        m_entries = m_entries.insert_or_assign(key, {end, run});
        return true;
    }

    bool rebuild(std::vector<std::tuple<ActorId, uint64_t, OriginInterval>> entries) {
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            return std::tie(std::get<0>(a), std::get<1>(a)) <
                   std::tie(std::get<0>(b), std::get<1>(b));
        });
        std::vector<std::pair<Key, OriginInterval>> sorted;
        sorted.reserve(entries.size());
        std::optional<std::pair<ActorId, uint64_t>> previous;
        uint64_t previous_end = 0;
        for (const auto& [actor, first, value] : entries) {
            if (!actor.valid() || !first || value.end <= first || !value.run)
                return false;
            if (previous && previous->first == actor &&
                (previous->second >= first || previous_end > first)) return false;
            previous = std::pair{actor, first};
            previous_end = value.end;
            sorted.emplace_back(Key{actor, first}, value);
        }
        m_entries = Map::from_sorted(sorted);
        return true;
    }

    bool erase(const ActorId& actor, uint64_t first) {
        const Key key{actor, first};
        if (!m_entries.find(key)) return false;
        m_entries = m_entries.erase(key);
        return true;
    }

    std::optional<OriginInterval> locate(const Identity& identity) const {
        auto entry = m_entries.predecessor({identity.actor, identity.counter});
        if (!entry || entry->first.first != identity.actor ||
            identity.counter >= entry->second.end) return std::nullopt;
        return entry->second;
    }

    std::vector<OriginIntervalEntry> overlapping(
        const ActorId& actor, uint64_t first, uint64_t end) const {
        std::vector<OriginIntervalEntry> result;
        if (!actor.valid() || first == 0 || end <= first) return result;
        m_entries.for_each_from({actor, first}, [&](const Key& key,
                                                    const OriginInterval& value) {
            if (key.first != actor || key.second >= end) return false;
            if (value.end > first)
                result.push_back({actor, key.second, value.end, value.run});
            return true;
        });
        // A predecessor may begin before the requested interval and overlap its
        // first identity; include it without scanning unrelated actors/runs.
        auto before = m_entries.predecessor({actor, first});
        if (before && before->first.first == actor && before->second.end > first &&
            before->first.second < first) {
            result.insert(result.begin(), {actor, before->first.second,
                                           before->second.end, before->second.run});
        }
        return result;
    }

    template<class Fn> void for_each(Fn&& fn) const {
        m_entries.for_each([&](const Key& key, const OriginInterval& value) {
            fn(key.first, key.second, value.end, value.run);
        });
    }

private:
    Map m_entries;
};

} // namespace CollabText::Crdt
