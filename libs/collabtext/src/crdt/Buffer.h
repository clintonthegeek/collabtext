#pragma once

#include "crdt/Anchor.h"
#include "crdt/Clock.h"
#include "crdt/Fragment.h"
#include "crdt/InsertionIndex.h"
#include "crdt/Locator.h"
#include "crdt/OperationQueue.h"
#include "crdt/Operations.h"
#include "crdt/SumTree.h"
#include "crdt/UndoMap.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace CollabText::Crdt {

// Branching factor for the fragment tree. 6 gives MaxChildren=12.
static constexpr std::size_t FRAG_TREE_B = 6;

using FragmentTree = SumTree<Fragment, FRAG_TREE_B>;

/// Describes a change in the visible text between two versions.
/// In the OLD visible text, the range [old_start, old_end) was replaced
/// by new_text. An insertion has old_start == old_end. A deletion has
/// empty new_text. A replacement has both.
struct TextEdit {
    uint32_t old_start = 0;   ///< Byte offset in the OLD visible text
    uint32_t old_end = 0;     ///< Byte offset in the OLD visible text
    std::string new_text;     ///< Replacement (empty for deletion)
};

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

    /// Compute the edits (insertions, deletions, replacements) that
    /// transformed the visible text from its state at `since` to the
    /// current state. Each TextEdit is a surgical change in the old
    /// text's coordinate space.
    ///
    /// Use this after apply_ops() to learn what changed:
    ///   Global old_version = buf.version();
    ///   buf.apply_ops(remote_ops);
    ///   auto edits = buf.edits_since(old_version);
    ///   // Apply edits surgically to the display
    std::vector<TextEdit> edits_since(const Global &since) const;

    /// Undo the last local edit. Returns the operation to broadcast, or
    /// nullopt if there is nothing to undo.
    std::optional<Operation> undo();

    /// Redo the last undone local edit. Returns the operation to broadcast,
    /// or nullopt if there is nothing to redo.
    std::optional<Operation> redo();

    /// Number of entries on the undo stack that can currently be undone.
    /// (Increases by one per local edit; decreases by one per undo; restored
    /// by redo.) Used by editor UIs to detect whether the previous entry on
    /// the stack is still the one they expect — important for time-based
    /// coalescing when other code paths (e.g. background tasks) may also
    /// push edits.
    size_t undo_depth() const { return m_undo_cursor; }

    /// Merge the most recent undo entry into the one before it. The two
    /// entries become a single Ctrl+Z step: their inserted characters and
    /// deletion ids are concatenated. Returns true if a merge happened, or
    /// false if there are fewer than two undoable entries on the stack.
    ///
    /// This is a purely local operation — it does not modify any CRDT state
    /// and produces no operation to broadcast. Editor UIs call it after a
    /// new local edit when they want that edit to be grouped with the
    /// previous one (e.g., consecutive keystrokes inside a word).
    bool coalesce_last_undo();

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

    /// For testing: visible text byte length.
    uint32_t visible_rope_len() const;

    /// For testing: deleted text byte length.
    uint32_t deleted_rope_len() const;

    /// Number of tombstone (invisible) fragments in the tree.
    size_t tombstone_count() const;

    /// Total fragment count (visible + tombstone).
    size_t fragment_count() const;

    /// Maximum undo stack depth. Oldest entries are discarded when exceeded.
    size_t max_undo_depth() const;

    /// Set the maximum undo stack depth.
    void set_max_undo_depth(size_t depth);

    /// Run garbage collection: remove tombstones whose deletions are no longer
    /// in the undo stack. Returns the number of tombstones removed.
    size_t collect_garbage();

    /// Watermark-based GC: remove tombstones whose deletions have ALL been
    /// observed by the watermark (all replicas have seen them) AND are not
    /// in the local undo stack.  Call when all replicas have synced past
    /// the watermark.  Returns the number of tombstones removed.
    size_t compact(const Global& watermark);

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

    /// Fast path for apply_remote_edit: insertion-only single-char edits.
    bool apply_remote_edit_fast(const EditOperation &op);

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

    /// Apply deletion runs: batch-delete characters identified by Lamport
    /// timestamp ranges, splitting fragments as needed.
    void apply_deletion_runs(
        std::vector<Fragment>& frags,
        const std::vector<EditOperation::DeletionRun>& runs,
        Lamport deletion_id);

    uint16_t m_replica_id;
    Lamport m_clock;
    Global m_version;
    UndoMap m_undo_map;

    FragmentTree m_fragment_tree;
    InsertionIndex m_insertion_index;

    /// Per-replica origin index: maps (replica_id -> sorted map of origin_value -> Locator).
    /// Used by the fast path to find a fragment's locator in O(log n).
    std::unordered_map<uint16_t, std::map<uint32_t, Locator>> m_origin_index;
    void rebuild_origin_index();
    std::optional<Locator> origin_index_lookup(uint16_t replica_id, uint32_t origin_value) const;

    /// Rebuild the insertion index from the current fragment list.
    void rebuild_insertion_index(const std::vector<Fragment>& frags);

    /// Trim the undo stack to m_max_undo_depth entries.
    void trim_undo_stack();

    /// Merge adjacent fragments that meet coalescing conditions.
    static void coalesce_fragments(std::vector<Fragment>& frags);

    /// Shared implementation: remove tombstones matching predicate, then
    /// coalesce.  The predicate returns true for GC-eligible fragments.
    template<typename Pred>
    size_t sweep_and_coalesce(Pred is_gc_eligible);

    /// Deferred operations awaiting causal dependencies.
    OperationQueue m_deferred_queue;
    std::set<uint16_t> m_deferred_replicas;

    /// Undo history: each entry tracks the inserted characters and the
    /// deletion-operation IDs that this entry should undo. A normal,
    /// uncoalesced entry has at most one deletion id; coalesced entries
    /// (created by `coalesce_last_undo`) may carry several so that one
    /// Ctrl+Z reverses every grouped local edit at once.
    struct UndoEntry {
        std::vector<UndoMapKey> inserted_keys;
        std::vector<Lamport> deletion_ids;
    };
    std::vector<UndoEntry> m_undo_stack;
    size_t m_undo_cursor = 0;  // Points past the last undoable entry
    size_t m_max_undo_depth = 1000;
};

} // namespace CollabText::Crdt
