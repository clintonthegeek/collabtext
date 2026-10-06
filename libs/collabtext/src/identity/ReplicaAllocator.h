/// ReplicaAllocator.h — binds the engine's borrowed allocator to durable state.
///
/// Roadmap task P06a, implementing docs/specs/vnext-identity.md §3.7: "An engine
/// borrows its allocator... the counter lives in `ReplicaState`, one durable
/// owner per private state directory (P02), and the engine holds a handle naming
/// its (document, stream)."
///
/// The handle is what names (document, stream). An engine that is handed one of
/// these cannot accidentally mint identities into the wrong stream's counter
/// space, because it never sees a stream name at all.
///
/// Lifetime: the allocator holds a raw `ReplicaState*` and does not extend its
/// life. `ReplicaState` owns an OS lock on its state directory, so outliving it
/// would mean handing out identities from a counter no process is protecting —
/// exactly the two-writers-one-stream case the lease exists to prevent. Hold the
/// state for at least as long as every engine built on it.

#pragma once

#include <collabtext/ActorId.h>

#include "crdt/IdentityAllocator.h"

#include <string>

namespace CollabText::Identity {

/// An `IdentityAllocator` backed by one (document, stream) of a `ReplicaState`.
class ReplicaAllocator final : public Crdt::IdentityAllocator {
public:
    /// `state` must outlive this allocator. `document` and `stream` name the
    /// counter this handle may draw from and are recorded in every reservation.
    ReplicaAllocator(ReplicaState* state, std::string document, std::string stream,
                     uint64_t epoch = 1);

    [[nodiscard]] Result<uint64_t> reserve_run(uint64_t count) override;
    void observe(uint64_t counter) override;
    [[nodiscard]] uint64_t peek() const noexcept override;
    [[nodiscard]] const ActorId& actor() const noexcept override;

    /// True once a reservation has failed. A failed durable write leaves the
    /// in-memory counter ahead of the file, so the owner cannot be trusted to
    /// issue anything further in this process.
    [[nodiscard]] bool failed() const noexcept { return m_failed; }
    [[nodiscard]] const Error& failure() const noexcept { return m_error; }

private:
    ReplicaState* m_state;
    std::string m_document;
    std::string m_stream;
    uint64_t m_epoch = 1;
    bool m_failed = false;
    Error m_error;
    uint64_t m_peek = 0;
};

} // namespace CollabText::Identity
