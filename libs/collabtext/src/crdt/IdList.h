#pragma once

#include "crdt/Anchor.h"
#include "crdt/IdentityAllocator.h"
#include "crdt/Clock.h"
#include "crdt/Locator.h"
#include "crdt/OperationValidation.h"
#include "crdt/SumTree.h"
#include "crdt/UndoMap.h"
#include "crdt/IdListOperations.h"

#include <collabtext/ApplyOutcome.h>

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
    Locator max_locator;
    Identity max_origin = Identity::min();

    static IdListSummary zero() { return {}; }

    void add_summary(const IdListSummary& other) {
        visible_count += other.visible_count;
        deleted_count += other.deleted_count;
        if (other.max_locator > max_locator ||
            (other.max_locator == max_locator && other.max_origin > max_origin)) {
            max_locator = other.max_locator;
            max_origin = other.max_origin;
        }
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
    Locator locator;
    uint64_t id = 0;
    std::vector<Identity> deletions;
    bool visible = true;

    IdListEntry() = default;
    IdListEntry(Identity orig, Locator loc, uint64_t i)
        : origin(std::move(orig)), locator(std::move(loc)), id(i) {}

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
        s.max_locator = locator;
        s.max_origin = origin;
        return s;
    }
};

using IdListTree = SumTree<IdListEntry, IDLIST_TREE_B>;

class IdList {
public:
    /// Takes the allocator this list mints identities from. There is
    /// deliberately no ActorId-only constructor: holding an actor id is not
    /// authority to claim durable allocation (identity spec §3.7).
    explicit IdList(SharedAllocator allocator);

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
    size_t undo_depth() const { return m_undo_cursor; }
    bool coalesce_last_undo();
    size_t max_undo_depth() const { return m_max_undo_depth; }
    void set_max_undo_depth(size_t depth);

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
    const SharedAllocator &allocator() const { return m_allocator; }

    /// Temporarily retains all entries, including restoration/anchor context.
    /// Returns zero until certified reclamation is implemented.
    size_t collect_garbage();

    /// Temporarily retains all entries and returns zero. Observation in a
    /// raw watermark does not establish safe reclamation or release remote undo.
    size_t compact(const Global& watermark);

    std::vector<IdListEntry> entries() const;
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

    /// Single-replica reset primitive. Drops all entries (visible +
    /// tombstones), the undo stack, and the deferred-op queue.
    /// Preserves actor, clock, version, max_undo_depth, and
    /// registered callbacks. Does NOT fire set_on_change or
    /// set_on_local_op. For use when the canonical content is
    /// replaced from outside the CRDT (file reload, revert-to-saved,
    /// programmatic content swap); calling on a connected collab
    /// session is allowed but remote peers will not see the clear —
    /// that's a higher-layer concern.
    ///
    /// Note: m_version is preserved, so calling apply_ops() or
    /// apply_remote_op() with previously-seen timestamps after a
    /// local_clear() will be silent no-ops. Rebuild must supply
    /// fresh ops with new timestamps (e.g. via a re-parse + insert
    /// pass), not a raw replay of the original CRDT log.
    ///
    void local_clear() noexcept;

private:
    SharedAllocator m_allocator;
    Global m_version;
    UndoMap m_undo_map;
    IdListTree m_entry_tree;

    struct UndoEntry {
        std::vector<UndoMapKey> inserted_keys;
        std::vector<Identity> deletion_ids;
    };
    std::vector<UndoEntry> m_undo_stack;
    size_t m_undo_cursor = 0;
    size_t m_max_undo_depth = 1000;

    ChangeCallback m_on_change;
    LocalOpCallback m_on_local_op;

    void emit_local_op(const IdListOperation& op) {
        if (m_on_local_op) m_on_local_op(op);
    }

    std::vector<IdListEntry> get_entries() const;
    void set_entries(std::vector<IdListEntry>&& entries);
    void insert_entry(std::vector<IdListEntry>& entries, IdListEntry entry) const;
    bool try_apply(const IdListOperation& op);

    /// Validate one operation and report what happened, queueing or refusing
    /// it as the verdict requires.
    ApplyOutcome apply_one(const IdListOperation& op);

    /// Queue `op` if the budget allows, otherwise quarantine it with a reason.
    ApplyOutcome defer_or_quarantine(const IdListOperation& op, Identity awaiting);

    /// The existing bodies, which assume their anchor was resolved.
    IdListOperation insert_after_resolved(const Anchor& after, uint64_t id);
    IdListOperation remove_at_resolved(const Anchor& target);

    /// Index of the entry whose origin matches `target`, or npos.
    [[nodiscard]] size_t find_origin(const std::vector<IdListEntry>& entries,
                                     const Anchor& target) const;

    /// Raise the allocator above an identity observed from a peer. Derived
    /// state, not durable allocation (identity spec §3.4).
    void observe_remote(const Identity &id) const {
        m_allocator->observe(id.counter);
    }

    void retry_deferred();
    void enqueue_deferred(IdListOpEntry entry);
    void trim_undo_stack();

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
