#pragma once
#include "crdt/CounterGapLedger.h"
#include "crdt/PositionTraversal.h"

#include "crdt/Anchor.h"
#include "crdt/Clock.h"
#include "crdt/Fragment.h"
#include "crdt/IdentityAllocator.h"
#include "crdt/Placement.h"
#include "crdt/OperationQueue.h"
#include "crdt/OperationValidation.h"
#include "crdt/ReclamationPolicy.h"
#include "crdt/Operations.h"
#include "crdt/OriginIndex.h"
#include "crdt/DeletionIndex.h"
#include "crdt/NodeDirectory.h"
#include "crdt/RunDirectory.h"
#include "crdt/SumTree.h"
#include "crdt/UndoMap.h"

#include <collabtext/ApplyOutcome.h>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace CollabText { class DocumentSession; }
namespace CollabText::detail { class PositionTraversalAccess; }

namespace CollabText::Crdt {

// Branching factor for the fragment tree. 6 gives MaxChildren=12.
static constexpr std::size_t FRAG_TREE_B = 6;

using FragmentTree = SumTree<Fragment, FRAG_TREE_B>;
using BufferFragmentTree = SumTree<Fragment, FRAG_TREE_B, StableNodeIds>;

/// Logical Buffer state used by checkpoint adapters. Visibility and all tree
/// summaries are deliberately excluded; restore derives them from deletions,
/// UndoMap, and the logical run sequence.
struct BufferCheckpointState {
    Global version;
    std::vector<CounterGap> counter_gaps;
    std::vector<Fragment> fragments;
    std::vector<UndoMapEntry> undo_entries;
    std::vector<OperationEntry> deferred;
    std::vector<QuarantinedBufferOp> quarantined;
    ReclamationBinding reclamation;
};

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
    /// Takes the allocator this engine mints identities from.
    ///
    /// There is deliberately no constructor that takes only an ActorId. Holding
    /// an actor id is not authority to claim durable allocation, and a
    /// constructor that quietly started a counter at 1 would hand out
    /// identities a relaunch would hand out again (identity spec §3.7). An
    /// engine that cannot be given an allocator cannot mint an identity at all.
    explicit Buffer(SharedIdentityAllocator allocator);

    /// Local edit: ranges are byte offsets in visible text.
    /// Each range is a (start, end) pair denoting bytes to delete.
    /// new_text[i] is the replacement text for ranges[i].
    /// Returns the operation to broadcast.
    ///
    /// This entry point cannot report failure, so an invalid request returns a
    /// default-constructed Operation and mutates nothing. Prefer
    /// try_apply_local_edit(), which says why it refused. The reason this is
    /// not simply an assertion is that an assertion disappears in Release:
    /// mismatched range/text vectors used to read past the end of new_text.
    Operation apply_local_edit(
        const std::vector<std::pair<uint32_t, uint32_t>> &ranges,
        const std::vector<std::string> &new_text);

    /// Local edit that refuses invalid requests instead of asserting.
    ///
    /// Validates range/text cardinality, range ordering, overlap, and that
    /// every offset lies within the visible text before touching the tree, the
    /// clock, the version vector, or the undo stack.
    Result<Operation> try_apply_local_edit(
        const std::vector<std::pair<uint32_t, uint32_t>> &ranges,
        const std::vector<std::string> &new_text);

    Result<uint32_t> marker_offset(Identity) const;
    Result<std::pair<uint32_t,uint32_t>> utf16_range(int position, int length) const;
    size_t utf16_length() const;
    Result<Operation> try_insert_marker(uint32_t byte_offset);
    Result<Operation> try_insert_relative(Identity marker, MarkerSide side,
                                         const std::string& text, bool insert_marker);
    Result<Operation> try_remove_marker(Identity marker);

    /// Apply remote operations (handles causal ordering, deduplication).
    ///
    /// Returns one outcome per offered operation, in order. Each is `applied`,
    /// `duplicate`, `deferred`, or `rejected`, and a `rejected` or `deferred`
    /// operation has changed nothing at all. This is deliberately not `void`:
    /// callers must be able to tell "applied" from "waiting" from "will never
    /// apply", and ignoring the result is still allowed for callers that only
    /// want the side effect.
    std::vector<ApplyOutcome> apply_ops(const std::vector<Operation> &ops);

    /// Fired after an operation actually changes the document. Not fired for a
    /// duplicate, a deferral, or a rejection, so an observer can treat every
    /// notification as a real reconciliation point.
    using ChangeCallback = std::function<void()>;
    void set_on_change(ChangeCallback cb) { m_on_change = std::move(cb); }

    /// Bound on the deferral queue. Reaching the bound is reported to the
    /// caller as ErrorCode::deferred_queue_full. A bounded diagnostic copy may
    /// be retained in quarantined(); the caller still owns the rejected input.
    void set_deferred_budget(const DeferredBudget &budget) { m_deferred_budget = budget; }
    [[nodiscard]] const DeferredBudget &deferred_budget() const { return m_deferred_budget; }

    /// Operations currently waiting for a causal dependency.
    [[nodiscard]] size_t deferred_count() const { return m_deferred_count; }
    [[nodiscard]] size_t deferred_bytes() const { return m_deferred_bytes; }

    /// Operations evicted from the deferral queue by the budget, each with the
    /// reason it was refused. This diagnostic retention has its own budget;
    /// absence here never authorizes a caller to discard an offered record.
    /// Bound the diagnostic overflow copies separately from the causal queue.
    /// Rejected input stays caller-owned even if this retention budget is full.
    Result<void> set_quarantine_budget(const DeferredBudget& budget) {
        if (m_quarantined.size() > budget.max_operations || m_quarantined_bytes > budget.max_bytes)
            return Error(ErrorCode::malformed, "drain quarantine before lowering its budget");
        m_quarantine_budget = budget;
        return Result<void>::success();
    }
    std::vector<QuarantinedBufferOp> take_quarantined() {
        m_quarantined_bytes = 0;
        auto retained = std::move(m_quarantined);
        m_quarantined.clear();
        return retained;
    }

    [[nodiscard]] const std::vector<QuarantinedBufferOp> &quarantined() const {
        return m_quarantined;
    }

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
    /// Checked undo that preserves allocator and identity-reservation errors.
    [[nodiscard]] Result<std::optional<Operation>> try_undo();
    /// Undo an exact currently active group, including one below newer groups.
    [[nodiscard]] Result<std::optional<Operation>> try_undo_group(const Identity &group_id);

    /// Redo the last undone local edit. Returns the operation to broadcast,
    /// or nullopt if there is nothing to redo.
    std::optional<Operation> redo();
    /// Checked redo that preserves allocator and identity-reservation errors.
    [[nodiscard]] Result<std::optional<Operation>> try_redo();
    /// Redo an exact currently undone group without redoing newer selections.
    [[nodiscard]] Result<std::optional<Operation>> try_redo_group(const Identity &group_id);

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

    /// Copy the local undo/redo stack for persistence. Restore only after the
    /// corresponding operation history has been replayed into this buffer.
    [[nodiscard]] LocalHistorySnapshot local_history_snapshot() const;
    [[nodiscard]] std::optional<LocalUndoEntrySnapshot>
    next_undo_group() const;
    [[nodiscard]] std::optional<LocalUndoEntrySnapshot>
    next_redo_group() const;
    [[nodiscard]] Result<void> acknowledge_undo_retirements(
        const std::vector<Identity> &group_ids);
    [[nodiscard]] Result<void>
    restore_local_history(const LocalHistorySnapshot &snapshot);

    /// Returns the full visible text of the document.
    std::string text() const;

    /// Returns the byte length of the visible text.
    uint32_t visible_length() const;

    /// Returns the current version vector.
    const Global &version() const;

    /// The persistent actor this buffer mints identities for, which is the
    /// allocator's actor rather than a constructor argument.
    const ActorId &actor_id() const;

    /// The allocator this engine borrows. Exposed for diagnostics and for the
    /// tests that assert *how many* reservations an operation performed.
    const SharedIdentityAllocator &allocator() const { return m_allocator; }

    /// Create an Anchor at a visible byte offset.
    Anchor anchor_at(uint32_t byte_offset, Bias bias = Bias::Left) const;

    /// Resolve an Anchor back to a visible byte offset.
    /// Returns the document length if the anchor is past the end.
    uint32_t resolve_anchor(const Anchor& anchor) const;

    /// Compare two anchors by their resolved positions.
    int compare_anchors(const Anchor& a, const Anchor& b) const;

    /// For testing: access the internal fragment list.
    std::vector<Fragment> fragments() const;

    /// Capture or replace shareable logical state. Replacement builds and
    /// validates a temporary Buffer first, preserving this instance's private
    /// actor history, allocator, budgets, and callback on success.
    [[nodiscard]] ApplyOutcome apply_counter_gap(const CounterGap& gap);
    [[nodiscard]] BufferCheckpointState checkpoint_state() const;
    [[nodiscard]] Result<void>
    restore_checkpoint_state(const BufferCheckpointState &snapshot);

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

    /// Set the maximum undo stack depth (at most 100,000). Refuses atomically
    /// if trimming would exceed the pending-retirement queue's count or byte
    /// budget.
    Result<void> set_max_undo_depth(size_t depth);

    /// Upper bound on history's retained restoration bytes and identity keys.
    size_t max_undo_bytes() const { return m_max_undo_bytes; }
    /// Set the maximum undo byte charge with the same retirement backpressure.
    Result<void> set_max_undo_bytes(size_t bytes);
    size_t undo_history_bytes() const { return m_undo_bytes; }

    /// Temporarily retains all fragments, including restoration/anchor context.
    /// Returns zero until certified reclamation is implemented.
    size_t collect_garbage();

    /// Discard local buffer view state while detached. Preserves allocator,
    /// observed version, limits, callbacks and pending undo-retirement records;
    /// drops CRDT content, undo/redo, deferred and quarantined input. No
    /// callback or operation is emitted. Covered log replay cannot rebuild it.
    void discard_local_view() noexcept;
    Result<std::vector<CollabText::detail::PositionItem>> position_items() const;

private:
    friend class ::CollabText::DocumentSession;
    friend class ::CollabText::detail::PositionTraversalAccess;
    [[nodiscard]] Result<size_t> collect_garbage(
        const std::set<Identity> &pinned_identities);
    void set_reclamation_binding(ReclamationBinding binding) {
        m_reclamation = std::move(binding);
    }

    struct UndoEntry;
    Result<Operation> try_insert_item(std::optional<Identity> left,
                                      const std::string& text, bool marker);
    void commit_local_history(UndoEntry&&);
    std::optional<std::vector<EditOperation::DeletionRun>>
    try_delete_exact_visible_runs(
        uint32_t start, uint32_t end, const Identity& deletion_id);

    struct LocatedRunItem {
        BufferFragmentTree::NodePtr leaf;
        const Fragment* fragment = nullptr;
        uint32_t offset = 0;
        uint64_t rank = 0;
    };
    std::optional<LocatedRunItem> locate_retained_identity(Identity) const;
    bool is_descendant_identity(Identity item, Identity ancestor) const;
    std::optional<Identity> visible_element_before_tree(uint32_t byte_offset) const;
    std::optional<std::pair<Link, uint64_t>>
    simple_middle_insertion(uint32_t byte_offset) const;
    bool try_insert_fragment_at_rank(Fragment fragment, uint64_t rank);

    // ---- Fragment vector helpers ----
    // Operations modify fragments via a temporary vector, then rebuild the tree.

    /// Get all fragments as a mutable vector.
    std::vector<Fragment> get_fragments() const;

    /// Rebuild the fragment tree from a vector of fragments (preserves order).
    void set_fragments(std::vector<Fragment>&& frags);
    /// Publish a proven retained-tail append by path-copying the tree and
    /// inserting only the new persistent index rows.
    bool try_append_fragment(Fragment fragment);
    std::optional<uint64_t> retained_rank_for_run(RunId run) const;
    void recompute_visibility_for(const std::vector<Identity>& identities);

    /// Find the fragment index and byte offset within that fragment for a
    /// given visible byte offset. Returns (fragment_index, offset_within_fragment).
    std::pair<size_t, uint32_t> resolve_visible_offset(
        const std::vector<Fragment>& frags, uint32_t byte_offset) const;

    /// Split a fragment at the given byte offset within it.
    /// Returns the index of the second half.
    size_t split_fragment_at(std::vector<Fragment>& frags,
                             size_t frag_idx, uint32_t offset_in_frag) const;

    /// What happened when an operation was offered to the engine.
    ///
    /// `not_ready` means a causal prerequisite has not arrived and the operation is
    /// still owed; `malformed` means it can never apply (for example, its placement
    /// names an element no replica could have had), the reason is in
    /// `m_apply_error`, and parking it in the deferral queue would wait forever.
    enum class ApplyStep { applied, not_ready, malformed };

    /// Apply a single remote EditOperation.
    ApplyStep apply_remote_edit(const EditOperation &op);

    /// Apply a single remote UndoOperation.
    ApplyStep apply_remote_undo(const UndoOperation &op);

    /// Try to apply a single operation.
    ApplyStep try_apply(const Operation& op);
    std::optional<Error> m_apply_error;

    /// Validate one operation against the current state and report what
    /// happened, queueing it or refusing it as the verdict requires.
    ApplyOutcome apply_one(const Operation& op);

    /// Queue `op` if the budget allows, otherwise quarantine it with a reason.
    /// Never drops it silently: a dropped operation is lost work.
    ApplyOutcome defer_or_quarantine(const Operation& op, Identity awaiting);

    /// The existing edit body, which assumes its arguments were validated.
    ///
    /// Fallsible even though its arguments are not: the identity reservation is
    /// the first thing it does, and a counter that cannot cover the edit is a
    /// refusal, not a silent wrap.
    Result<Operation> try_apply_validated_local_edit(
        const std::vector<std::pair<uint32_t, uint32_t>> &ranges,
        const std::vector<std::string> &new_text);

    /// Refuse to mint identity for an invalid actor.
    ///
    /// An all-zero ActorId is not actor 0 (P02); it is the absence of an actor.
    /// Minting from one would produce operations no peer could attribute and no
    /// peer could order against any real identity without inventing a tie-break,
    /// so this is a refusal in Release exactly as it is in Debug — the assert in
    /// the constructor is a developer tripwire, not the check.
    [[nodiscard]] Result<void> check_allocator() const {
        if (!m_fragment_tree.has_valid_node_ids())
            return Error(ErrorCode::limit_exceeded,
                         "cannot edit: fragment-tree node identity space is unavailable");
        if (!m_allocator->actor().valid())
            return Error(ErrorCode::invalid_identity,
                         "cannot mint operation identity: the allocator has no "
                         "valid actor identity");
        return Result<void>::success();
    }

    /// Raise the allocator above an identity observed from a peer.
    ///
    /// Derived state, deliberately not durable (identity spec §3.4): it need not
    /// be persisted because on reopen the engine replays its own log and
    /// observes again, and a lost raise cannot cause reuse because no identity
    /// was issued while it was held.
    void observe_remote(const Identity &id) const {
        m_allocator->observe(id.counter);
    }

    /// Retry any deferred operations.
    void retry_deferred();

    /// Insert an entry into the deferred queue in timestamp order.
    void enqueue_deferred(OperationEntry entry);

    /// Apply deletion runs: batch-delete characters identified by runs of
    /// consecutive item identities, splitting fragments as needed.
    void apply_deletion_runs(
        std::vector<Fragment>& frags,
        const std::vector<EditOperation::DeletionRun>& runs,
        Identity deletion_id);

    SharedIdentityAllocator m_allocator;
    Global m_version;
    CounterGapLedger m_counter_gaps;
    UndoMap m_undo_map;
    ReclamationBinding m_reclamation;

    /// The document, in document order, tombstones included. The order of this
    /// tree IS the list order: placement is decided by SequenceOrder::integrate,
    /// not by a key stored on each fragment.
    BufferFragmentTree m_fragment_tree;
    NodeDirectory<BufferFragmentTree::Node> m_node_directory;
    // Rebuilt derived lookups, sharing persistent AVL pages across Buffer
    // copies. RunDirectory maps RunIds to a leaf NodeId and slot; rank is
    // resolved through NodeDirectory against this exact tree version.
    OriginIndex m_origin_index;
    DeletionIndex m_deletion_index;
    RunDirectory m_run_directory;
    RunId m_next_run_id = 1;
    bool m_fragment_indexes_valid = true;

    /// Trim the undo stack to m_max_undo_depth entries.
    void trim_undo_stack();
    void recalculate_undo_bytes();
    Result<void> check_retirement_budget_for_limits(size_t depth,
                                                     size_t bytes) const;
    Result<void> check_retirement_budget_for_new_entry(
        size_t retained_bytes, size_t inserted_count,
        size_t deletion_count) const;
    void retire_undo_entry(const UndoEntry &entry);

    /// Deferred operations awaiting causal dependencies.
    OperationQueue m_deferred_queue;
    std::set<ActorId> m_deferred_actors;

    /// Size of the deferral queue and its approximate retained bytes, tracked
    /// explicitly because the queue is a tree with no O(1) length.
    size_t m_deferred_count = 0;
    size_t m_deferred_bytes = 0;
    DeferredBudget m_deferred_budget;
    std::vector<QuarantinedBufferOp> m_quarantined;
    DeferredBudget m_quarantine_budget;
    size_t m_quarantined_bytes = 0;

    /// Fired only for operations that actually changed the document.
    ChangeCallback m_on_change;

    /// Undo history: each entry tracks the inserted characters and the
    /// deletion-operation IDs that this entry should undo. A normal,
    /// uncoalesced entry has at most one deletion id; coalesced entries
    /// (created by `coalesce_last_undo`) may carry several so that one
    /// Ctrl+Z reverses every grouped local edit at once.
    struct UndoEntry {
        Identity group_id;
        std::vector<Identity> operation_ids;
        std::vector<UndoMapKey> inserted_keys;
        std::vector<Identity> deletion_ids;
        size_t retained_bytes = 0;
    };
    [[nodiscard]] LocalUndoEntrySnapshot
    snapshot_undo_entry(const UndoEntry &) const;
    std::vector<UndoEntry> m_undo_stack;
    std::vector<LocalUndoEntrySnapshot> m_retired_undo_entries;
    size_t m_retired_undo_bytes = 0;
    size_t m_undo_cursor = 0;  // Points past the last undoable entry
    size_t m_max_undo_depth = 1000;
    size_t m_undo_bytes = 0;
    size_t m_max_undo_bytes = 16 * 1024 * 1024;
};

} // namespace CollabText::Crdt
