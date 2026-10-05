#pragma once

#include "crdt/Clock.h"
#include "crdt/SumTree.h"
#include <cstdint>

namespace CollabText::Crdt {

/// Key identifying one inserted item, by that item's identity.
///
/// This is the *item* domain, not the operation domain
/// (docs/specs/vnext-identity.md §3.1): an UndoMap entry is keyed by what was
/// inserted, while the undo that reverses it is an operation of its own. No
/// UndoMap entry can therefore collide with the operation that produced it.
struct UndoMapKey {
    Identity position;

    UndoMapKey() = default;
    explicit UndoMapKey(Identity id) : position(std::move(id)) {}

    auto operator<=>(const UndoMapKey &) const = default;
    bool operator==(const UndoMapKey &) const = default;
};

// ============================================================================
// SumTree types for the UndoMap
// ============================================================================

struct UndoTreeKey {
    Identity edit_id;   ///< Item identity of what was edited
    Identity undo_id;   ///< Operation identity of the undo

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
    uint32_t undo_count(Identity edit_id) const;
    bool is_undone(Identity edit_id) const;
    bool was_undone(Identity edit_id, const Global &version) const;

    bool is_undone(UndoMapKey key) const { return is_undone(key.position); }

    size_t size() const;
    void clear();

private:
    SumTree<UndoMapEntry, UNDO_MAP_B> m_tree;
};

} // namespace CollabText::Crdt
