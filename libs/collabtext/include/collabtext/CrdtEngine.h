#pragma once

#include <collabtext/Operations.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace CollabText {

class CrdtEngine {
public:
    explicit CrdtEngine(uint16_t replica_id = 0);
    ~CrdtEngine();
    CrdtEngine(const CrdtEngine &) = delete;
    CrdtEngine &operator=(const CrdtEngine &) = delete;

    // Text operations. Offsets are UTF-16 code unit positions.
    void insert(int position, const std::string &text);
    void remove(int position, int length);
    std::string text() const;
    int length() const;

    // Undo / redo
    bool undo();
    bool redo();

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
    /// op_lamport() to identify the op, and encode_operation() /
    /// decode_operation() in <collabtext/Serialization.h> for wire transport.
    /// See docs/handoff/2026-05-08-d5-joint-design-outcomes.md §1 for the full
    /// contract.
    ///
    /// Replaces any previously registered callback.  Pass a default-constructed
    /// (null) function to clear the callback.
    using LocalOpCallback = std::function<void(const Crdt::Operation &)>;
    void setOnLocalOp(LocalOpCallback cb);

    /// Apply a remote operation to this engine.
    ///
    /// Accepts ops that were emitted by another CrdtEngine via its
    /// LocalOpCallback (or decoded from wire bytes via decode_operation()).
    /// The op is applied with causal ordering: if a dependency is not yet
    /// satisfied, Buffer will defer it internally.
    ///
    /// Returns true on successful acceptance (the op was handed to Buffer).
    /// Returns false for ops that cannot be applied (e.g., dependency unmet —
    /// caller may retry or buffer for replay).  Never throws on in-domain ops;
    /// malformed ops should be caught at decode_operation() before reaching
    /// here.
    ///
    /// See docs/handoff/2026-05-08-d5-joint-design-outcomes.md §3.3 for the
    /// error-path contract agreed with the Markoff side.
    bool applyRemoteOp(const Crdt::Operation &op);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace CollabText
