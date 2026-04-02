#pragma once

#include "crdt/Anchor.h"
#include "crdt/Clock.h"
#include "crdt/Fragment.h"
#include "crdt/Rope.h"
#include "crdt/InsertionIndex.h"
#include "crdt/Locator.h"
#include "crdt/OperationQueue.h"
#include "crdt/Operations.h"
#include "crdt/SumTree.h"
#include "crdt/UndoMap.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace CollabText::Crdt {

// Branching factor for the fragment tree. 2 for testing (aggressive splits).
static constexpr std::size_t FRAG_TREE_B = 2;

using FragmentTree = SumTree<Fragment, FRAG_TREE_B>;

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

    /// For testing: visible rope byte length.
    uint32_t visible_rope_len() const;

    /// For testing: deleted rope byte length.
    uint32_t deleted_rope_len() const;

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

    /// Try to apply a single operation; returns true if applied, false if deferred.
    bool try_apply(const Operation& op);

    /// Retry any deferred operations.
    void retry_deferred();

    /// Insert an entry into the deferred queue in timestamp order.
    void enqueue_deferred(OperationEntry entry);

    /// Atomize multi-character fragments at shared locators.
    void normalize_fragments(std::vector<Fragment>& frags) const;

    /// Insert a fragment into the tree in O(log^2 n) using cursor seek/slice.
    void insert_fragment_into_tree(Fragment frag);

    /// Seek to a fragment by timestamp, for use in remote edit application.
    /// Returns pointer to fragment and its index, or nullptr if not found.
    struct VersionedSeekResult {
        const Fragment* fragment = nullptr;
        size_t fragment_index = 0;
        uint32_t offset_in_fragment = 0;
    };
    VersionedSeekResult versioned_seek_by_timestamp(
        const std::vector<Fragment>& frags,
        Lamport target_ts) const;

    uint16_t m_replica_id;
    Lamport m_clock;
    Global m_version;
    UndoMap m_undo_map;

    FragmentTree m_fragment_tree;
    InsertionIndex m_insertion_index;
    Rope m_visible_text;
    Rope m_deleted_text;

    /// Rebuild the insertion index from the current fragment list.
    void rebuild_insertion_index(const std::vector<Fragment>& frags);

    /// Deferred operations awaiting causal dependencies.
    OperationQueue m_deferred_queue;
    std::set<uint16_t> m_deferred_replicas;

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
