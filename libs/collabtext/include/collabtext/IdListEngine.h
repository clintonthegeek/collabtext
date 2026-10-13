/// IdList.h — the public facade over the engine's structural list.
///
/// Roadmap task A02a. `Crdt::IdList` itself is engine storage: its header names
/// `SumTree`, `UndoMap` and `IdListEntry`, none of which a consumer should see.
/// This is the same arrangement `<collabtext/CrdtEngine.h>` already uses for
/// `Buffer`, and it exists for the same reason — the public surface should be
/// the operations you can perform, not the tree that stores them.
///
/// ─── What this exposes, and what it does not ───────────────────────────────
///
/// Every method here maps one-to-one onto something `Crdt::IdList` implements
/// today. Nothing is invented, and nothing is promised that the engine does not
/// already do:
///
///   * Construction takes a borrowed allocator; there is no `ActorId`-only
///     constructor (see <collabtext/Engine.h> for why).
///   * Local mutation is anchor-based, because that is what the engine's items
///     are identified by. `insert_after`/`remove_at` return empty operations on
///     refusal because they cannot report; the `try_` forms are the ones that
///     say why.
///   * `anchor_of` has a `try_` form for the same reason: a missing id is an
///     explicit `unresolvable_anchor`, never a fabricated end position.
///   * `undo`/`redo` return an optional operation and **nullopt on a refused
///     reservation** — the counter is exhausted, or the allocator has no valid
///     actor. A `Result` is owed because persistence can fail; that is A02's
///     rename, not this task's.
///
/// ─── Known gaps, recorded rather than hidden ────────────────────────────────
///
/// Scoped boundary/element queries return typed outcomes and explicit collapse.
/// Legacy internal Anchor resolution is approximate; do not persist Anchor fields.
/// Construct with create(allocator, scope) to use scoped position queries.
///
/// The same counter value in two streams is two independent facts, so an
/// element id is only unique within the list that produced it. Do not hash or
/// truncate a caller's own identifier into `insert_after`'s `id` expecting it to
/// be globally unique — it is a caller-owned opaque value.
///
/// Scoped reads use element indices. Existing mutations use item anchors.

#pragma once
#include <collabtext/Anchor.h>

#include <collabtext/ActorId.h>
#include <collabtext/ApplyOutcome.h>
#include <collabtext/Engine.h>
#include <collabtext/IdListOperations.h>
#include <collabtext/LocalHistory.h>
#include <collabtext/Serialization.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace CollabText {

struct VisibleIdListEntry {
    uint64_t application_value;
    Crdt::Identity identity;
    bool operator==(const VisibleIdListEntry&) const = default;
};

/// A replicated ordered list of caller-owned opaque values.
class IdListEngine {
public:
    static Result<std::unique_ptr<IdListEngine>> create(Crdt::SharedIdentityAllocator, PositionScope);
    Result<ListBoundary> boundary_at(uint32_t element_index, Affinity) const;
    Result<ListElement> element_of(uint64_t application_value) const;
    Result<ListElement> element_of(const Crdt::Identity&) const;
    [[nodiscard]] Result<std::vector<VisibleIdListEntry>> visible_entries() const;
    [[nodiscard]] Result<Crdt::Anchor> try_anchor_of(const Crdt::Identity&, Crdt::Bias = Crdt::Bias::Left) const;
    /// Apply an already committed, host-authorized scoped terminal CTGP record.
    [[nodiscard]] ApplyOutcome apply_counter_gap(const PositionScope&, const Crdt::CounterGap&);
    /// Durable envelopes use canonical CTGP bytes; alternate spellings fail.
    [[nodiscard]] ApplyOutcome apply_counter_gap(const PositionScope&, std::string_view committed_bytes);
    Result<ResolvedListPosition> resolve(const ListBoundary&) const;
    Result<ResolvedListPosition> resolve(const ListElement&) const;
    Result<int> compare(const ListBoundary&, const ListBoundary&) const;

    /// Takes the allocator this list mints identities from, and learns its actor
    /// from it. Constructing with no allocator is not possible by design.
    explicit IdListEngine(Crdt::SharedIdentityAllocator allocator);

    // ---- local mutation ----
    //
    // Offsets are not a thing here: the only positions this list understands are
    // anchors and element ids.

    /// Insert `id` after the element `after` names. `IdListAnchor::min()` means
    /// "at the head".
    [[nodiscard]] Result<Crdt::IdListOperation> try_insert_after(
        const Crdt::Anchor &after, uint64_t id);

    /// Remove the element `target` names, as a tombstone. The element's identity
    /// survives removal, so an anchor to it still resolves to its position.
    [[nodiscard]] Result<Crdt::IdListOperation> try_remove_at(const Crdt::Anchor &target);

    // ---- reads ----

    /// Ids of the *visible* elements, in document order. A removed element keeps
    /// its identity -- its anchor still resolves to its position -- but it is
    /// not in this list; `tombstone_count()` and `entry_count()` are how you ask
    /// about the others.
    [[nodiscard]] std::vector<uint64_t> ids() const;
    /// Number of visible elements.
    [[nodiscard]] uint32_t size() const;
    [[nodiscard]] size_t tombstone_count() const;
    [[nodiscard]] size_t entry_count() const;

    /// The anchor naming element `id`, or an explicit `unresolvable_anchor` when
    /// there is no such visible element. Never a fabricated end position.
    [[nodiscard]] Result<Crdt::Anchor> try_anchor_of(uint64_t id,
                                                     Crdt::Bias bias = Crdt::Bias::Left) const;

    /// Causal dependencies observed so far. Sparse; see `Crdt::Global`.
    [[nodiscard]] const Crdt::Global &version() const;
    /// The persistent actor this list mints identities for.
    [[nodiscard]] const ActorId &actor_id() const;

    // ---- remote application ----

    /// Apply remote operations, one outcome per offered operation, in order. Each
    /// is applied, duplicate, deferred or rejected; a rejected or deferred
    /// operation changed nothing at all.
    [[nodiscard]] std::vector<ApplyOutcome> apply_ops(
        const std::vector<Crdt::IdListOperation> &ops);

    /// Fired after a local mutation that actually changed the list, and after a
    /// remote operation that applied. Pass a default-constructed function to
    /// clear it.
    using LocalOpCallback = std::function<void(const Crdt::IdListOperation &)>;
    void set_on_local_op(LocalOpCallback cb);
    using ChangeCallback = std::function<void()>;
    void set_on_change(ChangeCallback cb);

    // ---- undo ----
    //
    // Undo is actor-local. Consumers that own persistence can store the
    // identity-only snapshot beside their canonical operation history.

    /// Undo the last local mutation. Returns an error if identity allocation
    /// fails; an empty optional means there is no local mutation to undo.
    [[nodiscard]] Result<std::optional<Crdt::IdListOperation>> try_undo();
    /// Redo the last undone mutation. Returns an error if identity allocation
    /// fails; an empty optional means there is nothing to redo.
    [[nodiscard]] Result<std::optional<Crdt::IdListOperation>> try_redo();
    /// Compensate the exact active local history group, including a non-top group.
    [[nodiscard]] Result<std::optional<Crdt::IdListOperation>>
    try_undo_group(const Crdt::Identity &group_id);
    /// Reapply the exact undone local history group without redoing other groups.
    [[nodiscard]] Result<std::optional<Crdt::IdListOperation>>
    try_redo_group(const Crdt::Identity &group_id);
    /// Compatibility wrapper; returns nullopt for empty history or a refused
    /// identity reservation. Prefer try_undo() to distinguish them.
    [[nodiscard]] std::optional<Crdt::IdListOperation> undo();
    /// Compatibility wrapper; prefer try_redo() to distinguish refusal.
    [[nodiscard]] std::optional<Crdt::IdListOperation> redo();
    [[nodiscard]] size_t undo_depth() const;
    [[nodiscard]] Crdt::LocalHistorySnapshot local_history_snapshot() const;
    [[nodiscard]] Result<void> restore_local_history(
        const Crdt::LocalHistorySnapshot &snapshot);
    [[nodiscard]] std::optional<Crdt::LocalUndoEntrySnapshot>
    next_undo_group() const;
    [[nodiscard]] std::optional<Crdt::LocalUndoEntrySnapshot>
    next_redo_group() const;
    /// Remove pending retirement records only after the consumer's durable
    /// retirement event is committed; persist the resulting snapshot afterward.
    [[nodiscard]] Result<void> acknowledge_undo_retirements(
        const std::vector<Crdt::Identity> &group_ids);
    /// Merge the newest undo entry into the one before it, so consecutive
    /// insertions become one step. Purely local; mutates no CRDT state.
    bool coalesce_last_undo();
    /// Set the active history step limit (at most 100,000). Refuses atomically
    /// if trimming would exceed the pending-retirement queue's count or byte
    /// budget.
    Result<void> set_max_undo_depth(size_t depth);
    [[nodiscard]] size_t max_undo_depth() const;
    /// Set the active history byte limit with the same retirement backpressure.
    Result<void> set_max_undo_bytes(size_t bytes);
    [[nodiscard]] size_t max_undo_bytes() const;
    [[nodiscard]] size_t undo_history_bytes() const;

    // ---- reset ----

    /// Discard this engine's local view without creating a collaborative edit.
    /// Preserves its actor, observed version, undo limits and callbacks, but
    /// drops entries, undo/redo state, and deferred/quarantined input. Existing
    /// scoped positions become unavailable. Replaying already-observed
    /// operations will be treated as duplicates and cannot rebuild this view.
    /// Use a fresh engine or a valid checkpoint restore for recovery; remove
    /// elements collaboratively with try_remove_at() to emit operations.
    void discard_local_view() noexcept;

    ~IdListEngine();
    IdListEngine(IdListEngine &&) noexcept;
    IdListEngine &operator=(IdListEngine &&) noexcept;
    IdListEngine(const IdListEngine &) = delete;
    IdListEngine &operator=(const IdListEngine &) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace CollabText
