#pragma once

#include "crdt/Anchor.h"
#include "crdt/Clock.h"
#include "crdt/Fragment.h"
#include "crdt/InsertionIndex.h"
#include "crdt/Locator.h"
#include "crdt/SumTree.h"
#include "crdt/UndoMap.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace CollabText::Crdt {

// Branching factor for the fragment tree. 2 for testing (aggressive splits).
static constexpr std::size_t FRAG_TREE_B = 2;

using FragmentTree = SumTree<Fragment, FRAG_TREE_B>;

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

    // When inserting in the middle of a fragment, the local side splits
    // the fragment and gives the second half a new locator. Remote replicas
    // need to apply the same split to stay convergent.
    struct SplitRelocation {
        Lamport fragment_origin;  // origin of the fragment being split
        uint32_t split_offset;    // character offset within fragment where split happens
        uint32_t fragment_length; // total character length of the fragment being split
        Locator new_locator;      // new locator for the second half
    };
    std::vector<SplitRelocation> split_relocations;
};

struct UndoOperation {
    Lamport timestamp;
    Global version;
    std::vector<UndoMapKey> undo_keys;     // Characters to hide/show via undo map
    std::vector<UndoMapKey> undelete_keys;  // Characters to un-delete/re-delete via deleted flag
    bool is_redo = false;                   // True if this is a redo operation
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

    /// Create an Anchor at a visible byte offset.
    Anchor anchor_at(uint32_t byte_offset, Bias bias = Bias::Left) const;

    /// Resolve an Anchor back to a visible byte offset.
    /// Returns the document length if the anchor is past the end.
    uint32_t resolve_anchor(const Anchor& anchor) const;

    /// Compare two anchors by their resolved positions.
    int compare_anchors(const Anchor& a, const Anchor& b) const;

    /// For testing: access the internal fragment list.
    std::vector<Fragment> fragments() const;

private:
    // ---- Fragment vector helpers ----
    // Operations modify fragments via a temporary vector, then rebuild the tree.

    /// Get all fragments as a mutable vector.
    std::vector<Fragment> get_fragments() const;

    /// Rebuild the fragment tree from a vector of fragments (preserves order).
    void set_fragments(std::vector<Fragment>&& frags);

    /// Insert a fragment in sorted position in the fragment list.
    void insert_fragment(std::vector<Fragment>& frags, Fragment frag) const;

    /// Find the fragment index and byte offset within that fragment for a
    /// given visible byte offset. Returns (fragment_index, offset_within_fragment).
    std::pair<size_t, uint32_t> resolve_visible_offset(
        const std::vector<Fragment>& frags, uint32_t byte_offset) const;

    /// Split a fragment at the given byte offset within it.
    /// Returns the index of the second half.
    size_t split_fragment_at(std::vector<Fragment>& frags,
                             size_t frag_idx, uint32_t offset_in_frag) const;

    /// Find a locator for a new fragment between frags[ins_frag-1] and
    /// the next distinct greater locator.
    Locator locator_between(const std::vector<Fragment>& frags,
                            size_t ins_frag) const;

    /// Apply a single remote EditOperation.
    bool apply_remote_edit(const EditOperation &op);

    /// Apply a single remote UndoOperation.
    bool apply_remote_undo(const UndoOperation &op);

    /// Retry any deferred operations.
    void retry_deferred();

    /// Atomize multi-character fragments at shared locators.
    void normalize_fragments(std::vector<Fragment>& frags) const;

    /// Insert a fragment into the tree in O(log^2 n) using cursor seek/slice.
    void insert_fragment_into_tree(Fragment frag);

    uint16_t m_replica_id;
    Lamport m_clock;
    Global m_version;
    UndoMap m_undo_map;

    FragmentTree m_fragment_tree;
    InsertionIndex m_insertion_index;

    /// Rebuild the insertion index from the current fragment list.
    void rebuild_insertion_index(const std::vector<Fragment>& frags);

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
