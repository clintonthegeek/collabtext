/// Actor-local undo history snapshots that consumers may persist beside their
/// own canonical event log and restore after replaying the referenced events.
#pragma once

#include <collabtext/Engine.h>

#include <cstddef>
#include <vector>

namespace CollabText::Crdt {

/// Identity references for one coalesced local undo step. `retained_bytes` is
/// the engine's conservative policy charge, not an allocator measurement.
struct LocalUndoEntrySnapshot {
    Identity group_id;
    std::vector<Identity> operation_ids;
    std::vector<Identity> inserted_items;
    std::vector<Identity> deletion_operations;
    std::size_t retained_bytes = 0;
};

/// Actor-local undo/redo cursor and retained steps. Restore only after operation
/// replay; the engine rejects references outside the owner's replayed history.
struct LocalHistorySnapshot {
    std::vector<LocalUndoEntrySnapshot> entries;
    /// Trimmed or redo-truncated groups awaiting durable retirement publication.
    /// Consumers acknowledge these only after publishing each group durably.
    std::vector<LocalUndoEntrySnapshot> retired_entries;
    std::size_t cursor = 0;
};

} // namespace CollabText::Crdt
