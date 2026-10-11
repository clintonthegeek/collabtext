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
    /// Clears local retained history without resetting the allocator/version.
    /// Invalidates position history, including empty edge tokens; no callback/op.
    void local_clear() noexcept;

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
    void insert(int position, const std::string &text);
    void remove(int position, int length);
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

    // Change notification
    using ChangeCallback = std::function<void()>;
    void setOnChange(ChangeCallback cb);

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
