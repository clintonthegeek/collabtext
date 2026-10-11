#pragma once
#include "crdt/PositionTraversal.h"

#include "crdt/Anchor.h"
#include "crdt/IdentityAllocator.h"
#include "crdt/Clock.h"
#include "crdt/Placement.h"
#include "crdt/OperationValidation.h"
#include "crdt/SumTree.h"
#include "crdt/UndoMap.h"
#include "crdt/IdListOperations.h"

#include <collabtext/ApplyOutcome.h>

#include <cassert>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace CollabText::Crdt {

static constexpr std::size_t IDLIST_TREE_B = 6;

/// The aggregate IdListTree nodes cache. Holds only what a seek reads; see
/// FragmentSummary for why the version-vector fields are gone (P03, finding F3).
struct IdListSummary {
    uint32_t visible_count = 0;
    uint32_t deleted_count = 0;

    static IdListSummary zero() { return {}; }

    void add_summary(const IdListSummary& other) {
        visible_count += other.visible_count;
        deleted_count += other.deleted_count;
    }
};

struct VisibleIndex {
    uint32_t value = 0;
    static VisibleIndex zero() { return {0}; }
    void add_summary(const IdListSummary& s) { value += s.visible_count; }
    auto operator<=>(const VisibleIndex&) const = default;
    bool operator==(const VisibleIndex&) const = default;
};

struct IdListEntry {
    using Summary = IdListSummary;

    Identity origin;
    Link link;              ///< Where this element attaches in the FugueMax tree
    uint64_t id = 0;
    std::vector<Identity> deletions;
    bool visible = true;

    IdListEntry() = default;
    IdListEntry(Identity orig, Link l, uint64_t i)
        : origin(std::move(orig)), link(std::move(l)), id(i) {}

    // The run interface SequenceOrder is written against: an entry is a run of
    // exactly one item, so there is nothing to derive and nothing to split.
    static constexpr uint32_t length = 1;
    Identity item_at(uint32_t) const { return origin; }
    Link link_at(uint32_t) const { return link; }
    IdListEntry split_off(uint32_t) { assert(false && "a list entry is one item"); return *this; }

    bool was_visible() const { return deletions.empty(); }

    bool compute_visible(const UndoMap& undo_map) const {
        if (undo_map.is_undone(origin)) return false;
        for (const auto& del : deletions)
            if (!undo_map.is_undone(del)) return false;
        return true;
    }

    bool is_visible(const UndoMap& undo_map) const { return compute_visible(undo_map); }

    bool was_visible_at(const Global& version, const UndoMap& undo_map) const {
        if (!version.observed(origin)) return false;
        if (undo_map.was_undone(origin, version)) return false;
        for (const auto& del : deletions) {
            if (version.observed(del) && !undo_map.was_undone(del, version))
                return false;
        }
        return true;
    }

    IdListSummary summary() const {
        IdListSummary s;
        if (visible) s.visible_count = 1; else s.deleted_count = 1;
        return s;
    }
};

using IdListTree = SumTree<IdListEntry, IDLIST_TREE_B>;

/// Logical IdList state used by checkpoint adapters. Visibility and cached
/// tree summaries are rebuilt from deletion records and UndoMap state.
struct IdListCheckpointState {
    Global version;
    std::vector<IdListEntry> entries;
    std::vector<UndoMapEntry> undo_entries;
    std::vector<IdListOpEntry> deferred;
    std::vector<QuarantinedIdListOp> quarantined;
};

class IdList {
public:
    /// Takes the allocator this list mints identities from. There is
    /// deliberately no ActorId-only constructor: holding an actor id is not
    /// authority to claim durable allocation (identity spec §3.7).
    explicit IdList(SharedIdentityAllocator allocator);

    IdListOperation insert_after(const Anchor& after, uint64_t id);
    IdListOperation remove_at(const Anchor& target);

    /// Local mutations that refuse an unresolvable anchor instead of asserting.
    ///
    /// The plain `insert_after`/`remove_at` above cannot report a failure, so
    /// they delegate to these and return an empty operation when refused. The
    /// distinction is not cosmetic: the old code asserted that the anchor had
    /// been found and then used it anyway, which in a Release build meant
    /// `remove_at` read one element past the end of the entry vector and
    /// `insert_after` positioned the new element against a default-constructed
    /// locator.
    Result<IdListOperation> try_insert_after(const Anchor& after, uint64_t id);
    Result<IdListOperation> try_remove_at(const Anchor& target);

    /// Apply remote operations. Returns one outcome per offered operation, in
    /// order; see Buffer::apply_ops for why this is not `void`.
    std::vector<ApplyOutcome> apply_ops(const std::vector<IdListOperation>& ops);

    void set_deferred_budget(const DeferredBudget& budget) { m_deferred_budget = budget; }
    [[nodiscard]] const DeferredBudget& deferred_budget() const { return m_deferred_budget; }
    [[nodiscard]] size_t deferred_count() const { return m_deferred_count; }
    /// Bound the diagnostic overflow copies separately from the causal queue.
    /// Rejected input stays caller-owned even if this retention budget is full.
    Result<void> set_quarantine_budget(const DeferredBudget& budget) {
        if (m_quarantined.size() > budget.max_operations || m_quarantined_bytes > budget.max_bytes)
            return Error(ErrorCode::malformed, "drain quarantine before lowering its budget");
        m_quarantine_budget = budget;
        return Result<void>::success();
    }
    std::vector<QuarantinedIdListOp> take_quarantined() {
        m_quarantined_bytes = 0;
        auto retained = std::move(m_quarantined);
        m_quarantined.clear();
        return retained;
    }

    [[nodiscard]] const std::vector<QuarantinedIdListOp>& quarantined() const {
        return m_quarantined;
    }

    std::optional<IdListOperation> undo();
    std::optional<IdListOperation> redo();
    [[nodiscard]] Result<std::optional<IdListOperation>> try_undo();
    [[nodiscard]] Result<std::optional<IdListOperation>> try_redo();
    size_t undo_depth() const { return m_undo_cursor; }
    bool coalesce_last_undo();
    size_t max_undo_depth() const { return m_max_undo_depth; }
    Result<void> set_max_undo_depth(size_t depth);
    size_t max_undo_bytes() const { return m_max_undo_bytes; }
    Result<void> set_max_undo_bytes(size_t bytes);
    size_t undo_history_bytes() const { return m_undo_bytes; }
    [[nodiscard]] LocalHistorySnapshot local_history_snapshot() const;
    [[nodiscard]] std::optional<LocalUndoEntrySnapshot>
    next_undo_group() const;
    [[nodiscard]] std::optional<LocalUndoEntrySnapshot>
    next_redo_group() const;
    [[nodiscard]] Result<void> acknowledge_undo_retirements(
        const std::vector<Identity> &group_ids);
    [[nodiscard]] Result<void>
    restore_local_history(const LocalHistorySnapshot &snapshot);

    std::vector<uint64_t> ids() const;
    uint32_t size() const;

    /// Checked visible-element lookup. A missing id is an explicit error.
    Result<Anchor> try_anchor_of(uint64_t id, Bias bias = Bias::Left) const;
    Anchor anchor_of(uint64_t id, Bias bias = Bias::Left) const;
    Anchor anchor_at_index(uint32_t index, Bias bias = Bias::Left) const;
    uint32_t resolve_anchor(const Anchor& a) const;
    int compare_anchors(const Anchor& a, const Anchor& b) const;

    const Global& version() const { return m_version; }
    const ActorId &actor_id() const { return m_allocator->actor(); }

    /// The allocator this list borrows.
    const SharedIdentityAllocator &allocator() const { return m_allocator; }

    /// Temporarily retains all entries, including restoration/anchor context.
    /// Returns zero until certified reclamation is implemented.
    size_t collect_garbage();

    std::vector<IdListEntry> entries() const;
    [[nodiscard]] IdListCheckpointState checkpoint_state() const;
    [[nodiscard]] Result<void>
    restore_checkpoint_state(const IdListCheckpointState &snapshot);
    size_t tombstone_count() const;
    size_t entry_count() const;

    using ChangeCallback = std::function<void()>;
    void set_on_change(ChangeCallback cb) { m_on_change = std::move(cb); }

    /// Callback fired after every successful local op (insert_after, remove_at,
    /// undo, redo). Not fired when apply_remote_op is called.
    using LocalOpCallback = std::function<void(const IdListOperation&)>;
    void set_on_local_op(LocalOpCallback cb) { m_on_local_op = std::move(cb); }

    /// Apply a single remote op. Reports whether the document changed, which is
    /// what this legacy signature can express. Prefer apply_ops() or
    /// apply_remote_op_checked() when the distinction matters.
    /// Fires set_on_change for an applied op only; does NOT fire
    /// set_on_local_op.
    bool apply_remote_op(const IdListOperation& op);

    /// As apply_remote_op, but distinguishes applied from duplicate, deferred,
    /// and rejected.
    [[nodiscard]] ApplyOutcome apply_remote_op_checked(const IdListOperation& op);

    /// Discard local list state while detached from collaborative use. Drops
    /// entries/tombstones, undo/redo, deferred and quarantined input. Preserves
    /// the allocator, observed version, undo limits, callbacks, and pending
    /// undo-retirement records. Fires no callbacks. Since version is preserved,
    /// already-observed operations are duplicates and cannot restore this view;
    /// recovery must use a fresh engine or a valid checkpoint restore.
    void discard_local_view() noexcept;

    Result<std::vector<CollabText::detail::PositionItem>> position_items() const;

private:
    SharedIdentityAllocator m_allocator;
    Global m_version;
    UndoMap m_undo_map;
    IdListTree m_entry_tree;

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
    size_t m_undo_cursor = 0;
    size_t m_max_undo_depth = 1000;
    size_t m_undo_bytes = 0;
    size_t m_max_undo_bytes = 16 * 1024 * 1024;

    ChangeCallback m_on_change;
    LocalOpCallback m_on_local_op;

    void emit_local_op(const IdListOperation& op) {
        if (m_on_local_op) m_on_local_op(op);
    }

    std::vector<IdListEntry> get_entries() const;
    void set_entries(std::vector<IdListEntry>&& entries);
    bool try_apply(const IdListOperation& op);
    /// Why the last try_apply refused an operation that can never apply, as
    /// opposed to one that is merely waiting on a prerequisite.
    std::optional<Error> m_apply_error;

    /// Validate one operation and report what happened, queueing or refusing
    /// it as the verdict requires.
    ApplyOutcome apply_one(const IdListOperation& op);

    /// Queue `op` if the budget allows, otherwise quarantine it with a reason.
    ApplyOutcome defer_or_quarantine(const IdListOperation& op, Identity awaiting);

    /// The existing bodies, which assume their anchor was resolved — but not
    /// that identity allocation will succeed, so they report rather than
    /// returning an empty operation. A default-constructed IdListOperation is
    /// indistinguishable from a real one whose fields happen to be zeroed, and
    /// the caller cannot tell a refusal from a mutation.
    Result<IdListOperation> insert_after_resolved(const Anchor& after, uint64_t id);
    Result<IdListOperation> remove_at_resolved(const Anchor& target);

    /// Index of the entry whose origin matches `target`, or npos.
    [[nodiscard]] size_t find_origin(const std::vector<IdListEntry>& entries,
                                     const Anchor& target) const;

    /// Refuse to mint identity for an invalid actor. See
    /// Buffer::check_allocator(): an all-zero ActorId is not actor 0, and the
    /// refusal has to hold in Release, not only behind an assert.
    [[nodiscard]] Result<void> check_allocator() const {
        if (!m_allocator->actor().valid())
            return Error(ErrorCode::invalid_identity,
                         "cannot mint operation identity: the allocator has no "
                         "valid actor identity");
        return Result<void>::success();
    }

    /// Raise the allocator above an identity observed from a peer. Derived
    /// state, not durable allocation (identity spec §3.4).
    void observe_remote(const Identity &id) const {
        m_allocator->observe(id.counter);
    }

    void retry_deferred();
    void enqueue_deferred(IdListOpEntry entry);
    void trim_undo_stack();
    void recalculate_undo_bytes();
    Result<void> check_retirement_budget_for_limits(size_t depth,
                                                     size_t bytes) const;
    Result<void> check_retirement_budget_for_new_entry(
        size_t retained_bytes, size_t inserted_count,
        size_t deletion_count) const;
    void retire_undo_entry(const UndoEntry &entry);

    bool apply_concrete(const IdListInsertOp& op);
    bool apply_concrete(const IdListRemoveOp& op);
    bool apply_concrete(const IdListUndoOpVariant& op);

    IdListOperationQueue m_deferred_queue;
    size_t m_deferred_count = 0;
    size_t m_deferred_bytes = 0;
    DeferredBudget m_deferred_budget;
    std::vector<QuarantinedIdListOp> m_quarantined;
    DeferredBudget m_quarantine_budget;
    size_t m_quarantined_bytes = 0;
};

} // namespace CollabText::Crdt
