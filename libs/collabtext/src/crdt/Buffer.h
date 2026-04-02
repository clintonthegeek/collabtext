#pragma once

#include "crdt/Clock.h"
#include "crdt/Fragment.h"
#include "crdt/Locator.h"
#include "crdt/UndoMap.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace CollabText::Crdt {

// Operation types for sync
struct EditOperation {
    Lamport timestamp;        // Lamport clock value when edit was created
    Global version;           // Causal dependencies (version vector at time of edit)
    std::vector<std::pair<uint32_t, uint32_t>> ranges;  // (start, end) byte offsets in visible text
    std::vector<std::string> new_text;                   // One replacement string per range

    // For remote application: the fragments that were inserted, with their
    // locators and origins, so the remote can place them correctly.
    struct InsertedFragment {
        Lamport origin;
        Locator locator;
        std::string content;
        uint32_t length;
    };
    std::vector<InsertedFragment> inserted_fragments;

    // Timestamps of characters that were deleted by this edit.
    std::vector<Lamport> deleted_timestamps;
};

struct UndoOperation {
    Lamport timestamp;
    Global version;
    std::vector<UndoMapKey> keys;   // Characters to undo/redo
    bool is_redo = false;           // True if this is a redo operation
};

using Operation = std::variant<EditOperation, UndoOperation>;

class Buffer {
public:
    explicit Buffer(uint16_t replica_id);

    /// Local edit: ranges are byte offsets in visible text.
    /// Each range is a (start, end) pair denoting bytes to delete.
    /// new_text[i] is the replacement text for ranges[i].
    /// Returns the operation to broadcast.
    Operation apply_local_edit(
        const std::vector<std::pair<uint32_t, uint32_t>> &ranges,
        const std::vector<std::string> &new_text);

    /// Apply remote operations (handles causal ordering, deduplication).
    void apply_ops(const std::vector<Operation> &ops);

    /// Undo the last local edit. Returns the operation to broadcast, or
    /// nullopt if there is nothing to undo.
    std::optional<Operation> undo();

    /// Redo the last undone local edit. Returns the operation to broadcast,
    /// or nullopt if there is nothing to redo.
    std::optional<Operation> redo();

    /// Returns the full visible text of the document.
    std::string text() const;

    /// Returns the byte length of the visible text.
    uint32_t visible_length() const;

    /// Returns the current version vector.
    const Global &version() const;

    /// Returns the replica ID of this buffer.
    uint16_t replica_id() const;

    /// For testing: access the internal fragment list.
    const std::vector<Fragment> &fragments() const;

private:
    /// Insert a fragment in sorted position in the fragment list.
    void insert_fragment(Fragment frag);

    /// Find the fragment index and byte offset within that fragment for a
    /// given visible byte offset. Returns (fragment_index, offset_within_fragment).
    /// If byte_offset equals visible_length(), returns (m_fragments.size(), 0).
    std::pair<size_t, uint32_t> resolve_visible_offset(uint32_t byte_offset) const;

    /// Split a fragment at the given byte offset within it.
    /// Both halves keep the same locator. Used for delete boundaries.
    /// Returns the index of the second half.
    size_t split_fragment_at(size_t frag_idx, uint32_t offset_in_frag);

    /// Find a locator for a new fragment given its predecessor and successor
    /// locators. Handles the case where lo == hi (split fragments) by searching
    /// for truly distinct boundaries.
    Locator locator_between(size_t ins_frag) const;

    /// Apply a single remote EditOperation.
    bool apply_remote_edit(const EditOperation &op);

    /// Apply a single remote UndoOperation.
    bool apply_remote_undo(const UndoOperation &op);

    /// Retry any deferred operations.
    void retry_deferred();

    uint16_t m_replica_id;
    Lamport m_clock;
    Global m_version;
    UndoMap m_undo_map;

    std::vector<Fragment> m_fragments;

    /// Deferred operations awaiting causal dependencies.
    std::vector<Operation> m_deferred;

    /// Undo history: each entry is a list of UndoMapKeys for the characters
    /// created by that edit, plus keys for characters deleted.
    struct UndoEntry {
        std::vector<UndoMapKey> inserted_keys;  // Characters we inserted
        std::vector<UndoMapKey> deleted_keys;    // Characters we deleted
    };
    std::vector<UndoEntry> m_undo_stack;
    size_t m_undo_cursor = 0;  // Points past the last undoable entry
};

} // namespace CollabText::Crdt
