#pragma once
#include <collabtext/Anchor.h>

#include <collabtext/ActorId.h>
#include <collabtext/ApplyOutcome.h>
#include <collabtext/Engine.h>
#include <collabtext/Operations.h>

#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace CollabText {

namespace detail { struct CallbackSubscriptionState; }

/// Memory limits for operations retained while waiting for causal dependencies.
/// An offered operation refused at the limit remains caller-owned.
struct DeferredOperationBudget {
    size_t max_operations = 4096;
    size_t max_bytes = 8u << 20;
};

/// Move-only RAII registration. Destroying or resetting the handle disconnects
/// its callback; it is safe to outlive the engine. Use and destroy it on the
/// engine's owner thread.
class CallbackSubscription {
public:
    CallbackSubscription() noexcept = default;
    ~CallbackSubscription();
    CallbackSubscription(CallbackSubscription &&) noexcept;
    CallbackSubscription &operator=(CallbackSubscription &&) noexcept;
    CallbackSubscription(const CallbackSubscription &) = delete;
    CallbackSubscription &operator=(const CallbackSubscription &) = delete;
    void reset() noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;

private:
    explicit CallbackSubscription(
        std::weak_ptr<detail::CallbackSubscriptionState> state) noexcept;
    std::weak_ptr<detail::CallbackSubscriptionState> m_state;
    friend class CrdtEngine;
};

class CrdtEngine {
public:
    /// Checked scope binding. No implicit default document or stream.
    static Result<std::unique_ptr<CrdtEngine>> create(Crdt::SharedIdentityAllocator, PositionScope);
    /// UTF-8 byte offsets at scalar boundaries; editing remains UTF-16.
    Result<TextBoundary> boundary_at(uint32_t utf8_byte_offset, Affinity) const;
    Result<TextAttachment> attachment_at(uint32_t utf8_byte_offset, Affinity, ReferentSide) const;
    Result<ResolvedTextPosition> resolve(const TextBoundary&) const;
    Result<ResolvedTextPosition> resolve(const TextAttachment&) const;
    Result<int> compare(const TextBoundary&, const TextBoundary&) const;
    /// Discard this engine's local view without creating a collaborative edit.
    /// Preserves its allocator, observed version and callbacks, but drops CRDT
    /// content, undo/redo state, and deferred/quarantined input. Existing
    /// scoped positions become unavailable. Replaying already-observed
    /// operations will be treated as duplicates and cannot rebuild this view.
    /// Use a fresh engine or a valid checkpoint restore for recovery; remove
    /// content collaboratively with try_replace() to emit an operation.
    void discard_local_view() noexcept;

    /// Takes the allocator this engine mints identities from, which is also where
    /// it learns its actor.
    ///
    /// There is no ActorId-only constructor, and there never was a safe one. The
    /// previous signature was `CrdtEngine(uint16_t replica_id = 0)`, whose
    /// default argument silently produced an engine that stamped every
    /// operation as actor 0 — and two processes constructed that way produced
    /// indistinguishable operations. Holding an actor id is not authority to
    /// claim durable allocation, so the allocator is the only way in
    /// (docs/specs/vnext-identity.md §3.7).
    explicit CrdtEngine(Crdt::SharedIdentityAllocator allocator);
    ~CrdtEngine();
    CrdtEngine(const CrdtEngine &) = delete;
    CrdtEngine &operator=(const CrdtEngine &) = delete;

    // Text operations. Offsets are UTF-16 code unit positions.
    // The legacy void wrappers leave state unchanged on failure; checked
    // callers should use try_insert/try_remove/try_replace.
    void insert(int position, const std::string &text);
    void remove(int position, int length);
    [[nodiscard]] Result<Crdt::Operation>
    try_insert(int position, const std::string &text);
    [[nodiscard]] Result<Crdt::Operation> try_remove(int position, int length);
    /// Replace one UTF-16 range as a single undoable CRDT operation. Positions
    /// inside a surrogate pair and ranges outside the text are rejected.
    [[nodiscard]] Result<Crdt::Operation>
    try_replace(int position, int length, const std::string &text);
    std::string text() const;
    int length() const;

    // Undo / redo
    bool undo();
    bool redo();
    [[nodiscard]] Result<std::optional<Crdt::Operation>> try_undo();
    [[nodiscard]] Result<std::optional<Crdt::Operation>> try_redo();
    [[nodiscard]] size_t undo_depth() const;
    [[nodiscard]] size_t undo_history_bytes() const;
    [[nodiscard]] size_t max_undo_depth() const;
    [[nodiscard]] size_t max_undo_bytes() const;
    [[nodiscard]] Result<void> set_max_undo_depth(size_t depth);
    [[nodiscard]] Result<void> set_max_undo_bytes(size_t bytes);

    /// Configure the causal deferral queue. Lowering below current retained
    /// usage is refused so accepted operations are never evicted by a limit
    /// change. A refused incoming operation reports `deferred_queue_full` in
    /// its ApplyOutcome and remains available to the caller for retry.
    [[nodiscard]] Result<void>
    set_deferred_operation_budget(DeferredOperationBudget budget);
    [[nodiscard]] DeferredOperationBudget deferred_operation_budget() const;
    [[nodiscard]] size_t deferred_operation_count() const;
    [[nodiscard]] size_t deferred_operation_bytes() const;

    // Change notification. Callbacks run synchronously on the calling thread.
    // Callback registration may be changed during dispatch; the active event
    // uses a stable callback snapshot. Mutating this engine from either
    // callback returns `reentrant_mutation` through checked APIs.
    using ChangeCallback = std::function<void()>;
    void setOnChange(ChangeCallback cb);
    /// Replaces the change callback and returns an RAII handle. Destroying or
    /// resetting the handle disconnects it and releases its captured state.
    [[nodiscard]] Result<CallbackSubscription>
    subscribe_on_change(ChangeCallback cb);

    /// Local-op emission callback.
    ///
    /// The callback fires synchronously after every local edit that produces an
    /// Operation: insert, remove, undo, and redo.  The caller receives the raw
    /// Crdt::Operation and decides whether to encode and broadcast it.
    ///
    /// Only a stable subset of Operation is part of the public contract — use
    /// op_identity() to identify the op, and encode_operation() /
    /// decode_operation() in <collabtext/Serialization.h> for wire transport.
    /// See docs/handoff/2026-05-08-d5-joint-design-outcomes.md §1 for the full
    /// contract.
    ///
    /// Replaces any previously registered callback.  Pass a default-constructed
    /// (null) function to clear the callback.
    using LocalOpCallback = std::function<void(const Crdt::Operation &)>;
    void setOnLocalOp(LocalOpCallback cb);
    /// RAII alternative to `setOnLocalOp`; the move-only handle disconnects on
    /// destruction and remains safe if it outlives this engine.
    [[nodiscard]] Result<CallbackSubscription>
    subscribe_on_local_op(LocalOpCallback cb);

    /// Apply remote operations in order and return one typed result per offered
    /// operation. Deferred operations remain queued by the engine.
    [[nodiscard]] std::vector<ApplyOutcome> apply_ops(
        const std::vector<Crdt::Operation>& operations);
    /// Single-operation form with the same applied/duplicate/deferred/rejected
    /// distinction.
    [[nodiscard]] ApplyOutcome apply_remote(const Crdt::Operation& operation);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace CollabText
