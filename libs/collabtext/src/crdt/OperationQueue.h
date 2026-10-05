#pragma once

#include "crdt/Clock.h"
#include "crdt/Operations.h"
#include "crdt/SumTree.h"

namespace CollabText::Crdt {

/// Extract the Identity timestamp from any Operation variant.
inline Identity get_op_timestamp(const Operation& op) {
    return std::visit([](const auto& o) -> Identity { return o.timestamp; }, op);
}

// ============================================================================
// SumTree-backed operation queue, ordered by Identity timestamp
// ============================================================================

struct OperationSummary {
    Identity max_timestamp = Identity::min();

    static OperationSummary zero() { return {}; }
    void add_summary(const OperationSummary& other) {
        if (other.max_timestamp > max_timestamp)
            max_timestamp = other.max_timestamp;
    }
};

struct TimestampDim {
    Identity value = Identity::min();

    static TimestampDim zero() { return {Identity::min()}; }
    void add_summary(const OperationSummary& s) { value = s.max_timestamp; }

    auto operator<=>(const TimestampDim&) const = default;
    bool operator==(const TimestampDim&) const = default;
};

struct OperationEntry {
    using Summary = OperationSummary;

    Identity timestamp;
    Operation op;

    OperationSummary summary() const { return {timestamp}; }
};

static constexpr std::size_t OP_QUEUE_B = 2;
using OperationQueue = SumTree<OperationEntry, OP_QUEUE_B>;

} // namespace CollabText::Crdt
