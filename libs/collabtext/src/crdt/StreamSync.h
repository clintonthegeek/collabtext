#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace CollabText::Crdt {

struct StreamEntry {
    std::string id;           // unique identifier for dedup
    uint16_t replica_id = 0;  // Lamport component (append-only ordering)
    uint64_t seq = 0;         // Lamport component (append-only ordering)
    std::string timestamp;    // ISO 8601 (LWW key for anchor-keyed)
    std::string payload;      // opaque JSON — consumer's problem
    bool tombstone = false;   // anchor-keyed only
};

} // namespace CollabText::Crdt
