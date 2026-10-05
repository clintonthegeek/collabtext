/// IdentityAllocator.h — the engine's borrowed handle to identity allocation.
///
/// Roadmap task P06a, implementing docs/specs/vnext-identity.md §3.2 and §3.7.
///
/// ─── Why this is an interface and not a constructor argument ────────────────
///
/// Before this, `Buffer(uint16_t replica_id)` held its own Lamport counter and
/// ticked it once per inserted scalar. Two things were wrong with that, and
/// widening the counter did not fix either:
///
///   * The counter was per *process*. A relaunch restarted it at 1 while old
///     operations still claimed the old values — finding F4, the identity an
///     engine can present twice.
///   * Nothing about holding an actor id made the engine's counter durable, so
///     nothing stopped a caller from believing it was.
///
/// §3.7 is explicit that constructing an engine is *not* authority to claim
/// durable allocation: the counter lives in one `ReplicaState` per private state
/// directory (P02), and an engine borrows a handle naming its (document,
/// stream). So there is deliberately no `Buffer(const ActorId&)` constructor.
/// An engine that cannot be given an allocator cannot mint an identity at all,
/// which is a much better failure than a plausible one.
///
/// ─── Run-interval allocation ────────────────────────────────────────────────
///
/// `reserve_run(count)` hands out `count` consecutive counters and returns the
/// first. A local operation that inserts `n` scalars reserves `n + 1`: its item
/// identities are `first .. first + n - 1`, and the operation's own identity is
/// `first + n` — strictly above every item it created and strictly below the
/// next operation's base. That is the whole of §3.2, and it is why the
/// reservation is one checked step rather than `n + 1` ticks.
///
/// ─── Ownership ─────────────────────────────────────────────────────────────
///
/// The engine does not own its allocator and never persists anything itself.
/// A test that wants a throwaway counter must say so by constructing a volatile
/// one (tests/VolatileCounter.h); reusing a production allocator path for that
/// would make the durability guarantees untested in the one place they are easy
/// to check.

#pragma once

#include <collabtext/ActorId.h>

#include <cstdint>
#include <memory>

namespace CollabText::Crdt {

/// Source of operation identities for one (document, stream) of one actor.
///
/// Implementations must make `reserve_run` all-or-nothing: on failure the
/// counter is exactly where it was, so a refused reservation cannot burn a
/// prefix of the space.
class IdentityAllocator {
public:
    virtual ~IdentityAllocator() = default;

    IdentityAllocator() = default;
    IdentityAllocator(const IdentityAllocator &) = delete;
    IdentityAllocator &operator=(const IdentityAllocator &) = delete;

    /// Reserve `count` consecutive counters and return the first.
    ///
    /// `count == 0` is refused: every operation consumes its own identity, so a
    /// zero-length reservation is a caller error rather than an empty range. On
    /// exhaustion this returns `counter_exhausted` and changes nothing.
    [[nodiscard]] virtual Result<uint64_t> reserve_run(uint64_t count) = 0;

    /// Raise the counter above an identity this replica has observed.
    ///
    /// This is derived state, not durable allocation (§3.4). It need not be
    /// persisted: on reopen the engine replays its own log and observes again,
    /// and a lost raise cannot cause reuse because no identity was issued while
    /// it was held. Being *behind* an observation is ordinary — a peer has
    /// simply seen more than we have.
    virtual void observe(uint64_t counter) = 0;

    /// The value the next reservation will use, without reserving it.
    [[nodiscard]] virtual uint64_t peek() const noexcept = 0;

    /// The actor whose identities this allocator hands out.
    [[nodiscard]] virtual const ActorId &actor() const noexcept = 0;
};

/// A shared handle, because the allocator is a mutable resource: handing the
/// same one to two engines is meaningful (§3.7's "one durable owner per private
/// state directory" means one *owner*, not one engine per owner), and the handle
/// must not be copyable state. The pointee is non-const because reserving from it
/// mutates it; what is borrowed is the allocation, not the handle's value.
using SharedAllocator = std::shared_ptr<IdentityAllocator>;

} // namespace CollabText::Crdt