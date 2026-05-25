#pragma once

#include "crdt/Anchor.h"
#include "crdt/Clock.h"
#include "crdt/Locator.h"
#include "crdt/SumTree.h"
#include "crdt/UndoMap.h"
#include "crdt/IdListOperations.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace CollabText::Crdt {

static constexpr std::size_t IDLIST_TREE_B = 6;

struct IdListSummary {
    uint32_t visible_count = 0;
    uint32_t deleted_count = 0;
    Locator max_locator;
    Lamport max_origin = Lamport::min();
    Global max_version;
    Global min_insertion_version;
    Global max_insertion_version;

    static IdListSummary zero() { return {}; }

    void add_summary(const IdListSummary& other) {
        visible_count += other.visible_count;
        deleted_count += other.deleted_count;
        if (other.max_locator > max_locator ||
            (other.max_locator == max_locator && other.max_origin > max_origin)) {
            max_locator = other.max_locator;
            max_origin = other.max_origin;
        }
        max_version.join(other.max_version);
        min_insertion_version.meet(other.min_insertion_version);
        max_insertion_version.join(other.max_insertion_version);
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

    Lamport origin;
    Locator locator;
    uint64_t id = 0;
    std::vector<Lamport> deletions;
    bool visible = true;

    IdListEntry() = default;
    IdListEntry(Lamport orig, Locator loc, uint64_t i)
        : origin(orig), locator(std::move(loc)), id(i) {}

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
        s.max_version.observe(origin);
        s.min_insertion_version.observe(origin);
        s.max_insertion_version.observe(origin);
        return s;
    }
};

using IdListTree = SumTree<IdListEntry, IDLIST_TREE_B>;

class IdList {
public:
    explicit IdList(uint16_t replica_id);

    IdListOperation insert_after(const Anchor& after, uint64_t id);
    IdListOperation remove_at(const Anchor& target);

    void apply_ops(const std::vector<IdListOperation>& ops);

    std::optional<IdListOperation> undo();
    std::optional<IdListOperation> redo();
    size_t undo_depth() const { return m_undo_cursor; }
    bool coalesce_last_undo();
    size_t max_undo_depth() const { return m_max_undo_depth; }
    void set_max_undo_depth(size_t depth);

    std::vector<uint64_t> ids() const;
    uint32_t size() const;

    Anchor anchor_of(uint64_t id, Bias bias = Bias::Left) const;
    Anchor anchor_at_index(uint32_t index, Bias bias = Bias::Left) const;
    uint32_t resolve_anchor(const Anchor& a) const;
    int compare_anchors(const Anchor& a, const Anchor& b) const;

    const Global& version() const { return m_version; }
    uint16_t replica_id() const { return m_replica_id; }

    size_t collect_garbage();
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

    /// Apply a single remote op. Returns true (false reserved for future use;
    /// deferred ops are handled internally by apply_ops).
    /// Fires set_on_change; does NOT fire set_on_local_op.
    bool apply_remote_op(const IdListOperation& op);

    /// Single-replica reset primitive. Drops all entries (visible +
    /// tombstones), the undo stack, and the deferred-op queue.
    /// Preserves replica_id, clock, version, max_undo_depth, and
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
    uint16_t m_replica_id;
    Lamport m_clock;
    Global m_version;
    UndoMap m_undo_map;
    IdListTree m_entry_tree;

    struct UndoEntry {
        std::vector<UndoMapKey> inserted_keys;
        std::vector<Lamport> deletion_ids;
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
    void retry_deferred();
    void enqueue_deferred(IdListOpEntry entry);
    void trim_undo_stack();

    bool apply_concrete(const IdListInsertOp& op);
    bool apply_concrete(const IdListRemoveOp& op);
    bool apply_concrete(const IdListUndoOpVariant& op);

    IdListOperationQueue m_deferred_queue;
};

} // namespace CollabText::Crdt
