#pragma once

#include "crdt/Clock.h"
#include "crdt/Locator.h"
#include "crdt/SumTree.h"
#include <cstdint>

namespace CollabText::Crdt {

// ============================================================================
// InsertionFragment — tracks where fragments of an insertion ended up
// ============================================================================

struct InsertionSummary {
    Lamport max_timestamp = Lamport::min();
    uint32_t max_split_offset = 0;

    static InsertionSummary zero() { return {}; }

    void add_summary(const InsertionSummary& other) {
        if (other.max_timestamp > max_timestamp)
            max_timestamp = other.max_timestamp;
        if (other.max_split_offset > max_split_offset)
            max_split_offset = other.max_split_offset;
    }
};

/// Dimension for seeking by (timestamp, split_offset).
/// Timestamps increase monotonically, so we can seek to a specific
/// insertion operation's fragments.
struct InsertionKey {
    Lamport timestamp = Lamport::min();
    uint32_t split_offset = 0;

    static InsertionKey zero() { return {}; }
    void add_summary(const InsertionSummary& s) {
        timestamp = s.max_timestamp;
        split_offset = s.max_split_offset;
    }

    auto operator<=>(const InsertionKey& other) const {
        if (auto cmp = timestamp <=> other.timestamp; cmp != 0)
            return cmp;
        return split_offset <=> other.split_offset;
    }
    bool operator==(const InsertionKey&) const = default;
};

/// An InsertionFragment maps an anchor's (timestamp, offset) to the
/// fragment's Locator in the fragment tree.
struct InsertionFragment {
    using Summary = InsertionSummary;

    Lamport timestamp;         ///< Insertion operation timestamp
    uint32_t split_offset = 0; ///< Byte offset within the insertion where this fragment starts
    Locator fragment_id;       ///< The fragment's Locator in the fragment tree
    uint32_t length = 0;       ///< Length of this fragment (bytes)

    InsertionFragment() = default;
    InsertionFragment(Lamport ts, uint32_t offset, Locator fid, uint32_t len)
        : timestamp(ts), split_offset(offset), fragment_id(std::move(fid)), length(len) {}

    InsertionSummary summary() const {
        return {timestamp, split_offset};
    }
};

// Branching factor for insertion index (matches fragment tree)
static constexpr std::size_t INSERTION_INDEX_B = 2;

using InsertionIndex = SumTree<InsertionFragment, INSERTION_INDEX_B>;

} // namespace CollabText::Crdt
