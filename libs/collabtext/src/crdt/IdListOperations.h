#pragma once

#include "crdt/Clock.h"
#include "crdt/Locator.h"
#include "crdt/SumTree.h"

#include <cstdint>
#include <variant>
#include <vector>

namespace CollabText::Crdt {

/// Insert a new element with the given opaque id at the pre-computed locator.
/// The receiver places the element using (locator, origin/timestamp) tiebreak.
struct IdListInsertOp {
    Lamport timestamp;     ///< Origin of the new element (== insertion Lamport)
    Global version;        ///< Causal dependencies at time of insertion
    uint64_t id = 0;       ///< Opaque application-owned element value
    Locator locator;       ///< Pre-computed fractional position
};

/// Remove the element identified by target_origin.
/// The receiver finds the entry by its origin Lamport and tombstones it.
struct IdListRemoveOp {
    Lamport timestamp;      ///< Lamport of this deletion op (the deletion_id)
    Global version;         ///< Causal dependencies
    Lamport target_origin;  ///< Origin of the entry being removed
};

/// Undo/redo operation; mirrors Buffer's UndoOperation exactly.
struct IdListUndoOpVariant {
    Lamport timestamp;
    Global version;
    std::vector<std::pair<Lamport, uint32_t>> counts;  ///< (edit_id, new_parity_count)
};

using IdListOperation =
    std::variant<IdListInsertOp, IdListRemoveOp, IdListUndoOpVariant>;

inline Lamport get_idlist_op_timestamp(const IdListOperation& op) {
    return std::visit([](const auto& o) -> Lamport { return o.timestamp; }, op);
}

inline const Global& get_idlist_op_version(const IdListOperation& op) {
    return std::visit([](const auto& o) -> const Global& { return o.version; }, op);
}

// ============================================================================
// SumTree-backed deferred queue for IdList operations
// ============================================================================

struct IdListOpSummary {
    Lamport max_timestamp = Lamport::min();

    static IdListOpSummary zero() { return {}; }
    void add_summary(const IdListOpSummary& other) {
        if (other.max_timestamp > max_timestamp)
            max_timestamp = other.max_timestamp;
    }
};

struct IdListOpEntry {
    using Summary = IdListOpSummary;

    Lamport timestamp;
    IdListOperation op;

    IdListOpSummary summary() const { return {timestamp}; }
};

static constexpr std::size_t IDLIST_OPQUEUE_B = 2;
using IdListOperationQueue = SumTree<IdListOpEntry, IDLIST_OPQUEUE_B>;

} // namespace CollabText::Crdt
