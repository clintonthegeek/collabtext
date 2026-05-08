/// src/crdt/IdListOperations.h — internal shim after Task 1.1 promotion
///
/// Public op types (IdListInsertOp, IdListRemoveOp, IdListUndoOpVariant,
/// IdListOperation, idlist_op_lamport, get_idlist_op_timestamp,
/// get_idlist_op_version) now live in <collabtext/IdListOperations.h>.
/// This file re-exports them and adds the SumTree-backed deferred queue
/// types that are internal to IdList.
#pragma once

#include <collabtext/IdListOperations.h>

#include "crdt/SumTree.h"

namespace CollabText::Crdt {

// ============================================================================
// SumTree-backed deferred queue for IdList operations (INTERNAL — not public)
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
