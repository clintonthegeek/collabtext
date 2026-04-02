#pragma once

#include "crdt/Clock.h"
#include "crdt/SumTree.h"
#include <cstdint>

namespace CollabText::Crdt {

/// Key identifying a character by its Lamport timestamp.
/// Retained for backward compatibility with Buffer::UndoEntry and UndoOperation.
struct UndoMapKey {
    uint16_t replica_id = 0;
    uint32_t lamport_value = 0;

    UndoMapKey() = default;
    UndoMapKey(uint16_t r, uint32_t v) : replica_id(r), lamport_value(v) {}
    explicit UndoMapKey(Lamport ts) : replica_id(ts.replica_id), lamport_value(ts.value) {}

    auto operator<=>(const UndoMapKey &) const = default;
    bool operator==(const UndoMapKey &) const = default;
};

// ============================================================================
// SumTree types for the UndoMap
// ============================================================================

struct UndoTreeKey {
    Lamport edit_id;
    Lamport undo_id;

    auto operator<=>(const UndoTreeKey &) const = default;
    bool operator==(const UndoTreeKey &) const = default;
};

struct UndoMapSummary {
    UndoTreeKey max_key;

    static UndoMapSummary zero() { return {}; }
    void add_summary(const UndoMapSummary &other) {
        if (other.max_key > max_key)
            max_key = other.max_key;
    }
};

struct UndoTreeKeyDim {
    UndoTreeKey value;

    static UndoTreeKeyDim zero() { return {}; }
    void add_summary(const UndoMapSummary &s) { value = s.max_key; }

    auto operator<=>(const UndoTreeKeyDim &) const = default;
    bool operator==(const UndoTreeKeyDim &) const = default;
};

struct UndoMapEntry {
    using Summary = UndoMapSummary;

    UndoTreeKey key;
    uint32_t undo_count = 0;

    UndoMapSummary summary() const { return {key}; }
};

static constexpr std::size_t UNDO_MAP_B = 2;

class UndoMap {
public:
    UndoMap() = default;

    void insert(UndoMapEntry entry);
    uint32_t undo_count(Lamport edit_id) const;
    bool is_undone(Lamport edit_id) const;
    bool was_undone(Lamport edit_id, const Global &version) const;

    bool is_undone(UndoMapKey key) const {
        return is_undone(Lamport(key.replica_id, key.lamport_value));
    }

    // Legacy shims (used until Buffer migrates)
    void undo(UndoMapKey key);
    void redo(UndoMapKey key);
    uint32_t count(UndoMapKey key) const {
        return undo_count(Lamport(key.replica_id, key.lamport_value));
    }

    size_t size() const;
    void clear();

private:
    SumTree<UndoMapEntry, UNDO_MAP_B> m_tree;
    uint32_t m_shim_counter = 0;
};

} // namespace CollabText::Crdt
