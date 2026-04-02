#pragma once

#include "crdt/Clock.h"
#include <compare>
#include <cstdint>
#include <map>

namespace CollabText::Crdt {

/// Key for the undo/redo map: identifies a specific character by
/// (replica_id, lamport_value) so we can toggle its visibility.
struct UndoMapKey {
    uint16_t replica_id = 0;
    uint32_t lamport_value = 0;

    UndoMapKey() = default;
    UndoMapKey(uint16_t r, uint32_t v) : replica_id(r), lamport_value(v) {}
    explicit UndoMapKey(Lamport ts) : replica_id(ts.replica_id), lamport_value(ts.value) {}

    auto operator<=>(const UndoMapKey &) const = default;
    bool operator==(const UndoMapKey &) const = default;
};

/// Tracks undo/redo state for characters. Each entry counts how many
/// times a character has been "undone" (positive = hidden by undo,
/// zero = visible or re-done). This is separate from the CRDT delete
/// mechanism — undo hides without creating a tombstone.
class UndoMap {
public:
    UndoMap() = default;

    /// Increment the undo counter for a character (hide it via undo).
    void undo(UndoMapKey key);

    /// Decrement the undo counter for a character (redo / restore).
    /// Does nothing if the counter is already 0.
    void redo(UndoMapKey key);

    /// Returns true if the character is currently hidden by undo
    /// (counter > 0).
    bool is_undone(UndoMapKey key) const;

    /// Returns the undo counter for a character. 0 means not undone.
    uint32_t count(UndoMapKey key) const;

    /// Number of entries in the map (for testing/debugging).
    size_t size() const { return m_entries.size(); }

    /// Clear all undo state.
    void clear() { m_entries.clear(); }

    bool operator==(const UndoMap &other) const = default;

private:
    std::map<UndoMapKey, uint32_t> m_entries;
};

} // namespace CollabText::Crdt
