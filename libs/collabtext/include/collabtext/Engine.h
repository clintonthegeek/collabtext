/// Engine.h — the engine's value types, promoted to the public surface.
///
/// Roadmap task A02a: "Every advertised public header compiles independently
/// without `crdt/` or `src` includes."
///
/// Six public headers included `crdt/Clock.h` or `crdt/Locator.h`, and one
/// included `crdt/Anchor.h`. That is why `src/` was on the build interface: the
/// values an operation is made of lived under `src/`, so any consumer of the
/// public headers was reaching into the implementation tree to get them. The
/// fix is not a longer include path, it is giving those values a public home.
///
/// `src/crdt/Clock.h`, `src/crdt/Locator.h` and `src/crdt/Anchor.h` are now
/// one-line shims that include this header, so the ~60 engine translation units
/// and the tests that reach for `crdt/Clock.h` keep working unchanged. This is
/// the same promotion `src/crdt/Operations.h` already went through.
///
/// ─── What is here, and what is deliberately not ───────────────────────────
///
/// Here: `Identity`, `Global`, `Locator`, `Anchor`, `Bias` and the
/// `IdentityAllocator` interface — the values the public operation and outcome
/// types are built from. Not here: `Fragment`, `SumTree`, `UndoMap`, `Buffer`,
/// `IdList`. Those are the engine's storage and internals; a consumer reaches
/// them through `<collabtext/CrdtEngine.h>` and `<collabtext/IdList.h>`, which
/// are facades. The stability contract for all of it is in
/// <collabtext/Operations.h>; nothing here is a licence to depend on layout.
///
/// Note the naming collision this file walks past: `CollabText::Identity` is a
/// *namespace* holding presence and profile records (see
/// <collabtext/Identity.h>), while the operation identity below is
/// `CollabText::Crdt::Identity`. Both predate A02a. The names are qualified
/// everywhere they meet, and the capability manifest records which is which.

#pragma once

#include <collabtext/ActorId.h>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace CollabText::Crdt {

// ============================================================================
// Identity
// ============================================================================

/// An operation's identity: a persistent 128-bit actor plus a checked 64-bit
/// counter, ordered counter-first and then by the actor's own byte-wise
/// comparison.
///
/// The order is not negotiable. Two fragments can share a locator —
/// `Buffer::normalize_fragments` exists to handle runs from different actors at
/// one shared locator — so when locators tie, origin decides, and a replica that
/// disagrees about that order produces a different document, permanently and
/// invisibly. The contract and its laws are in
/// docs/specs/vnext-identity.md; §2.1 of that document is why the actor half is
/// the actor itself and never a locally assigned slot.
struct Identity {
    ActorId actor;
    uint64_t counter = 0;

    Identity() = default;
    Identity(ActorId a, uint64_t c) : actor(std::move(a)), counter(c) {}

    /// The least identity: counter 0 and an invalid actor. A true minimum,
    /// because counter 0 beats every higher counter and an all-zero actor beats
    /// every valid one. Aggregates seed with it, so a fold starting here must not
    /// be able to lose a real identity.
    static Identity min() { return {ActorId(), 0}; }

    /// Take the next counter value for the same actor, returning the value this
    /// identity *had* before the call. Both halves matter to callers; see the
    /// engine's allocation sites. Prefer an allocator's `reserve_run` when you
    /// need a whole interval.
    Identity tick() {
        const Identity previous = *this;
        ++counter;
        return previous;
    }

    /// Raise past an observed identity. The actor half is deliberately ignored:
    /// this is the causal clock, so it rises past what was seen without becoming
    /// somebody else's identity.
    void observe(Identity other) {
        if (other.counter >= counter)
            counter = other.counter + 1;
    }

    auto operator<=>(const Identity &other) const {
        if (auto cmp = counter <=> other.counter; cmp != 0)
            return cmp;
        const int by_actor = actor.compare(other.actor);
        if (by_actor < 0) return std::strong_ordering::less;
        if (by_actor > 0) return std::strong_ordering::greater;
        return std::strong_ordering::equal;
    }
    bool operator==(const Identity &other) const = default;
};

// ============================================================================
// Version vector
// ============================================================================

/// A sparse version vector keyed by actor.
///
/// There is no dense view, and there cannot be one: keys are 128-bit opaque
/// values, so `size()`/`operator[]` over "replica id i" has no meaning. Sparse
/// iteration is the whole interface. Absence is zero coverage, which is why
/// `coverage_intersection` is not the same operation as `meet` — see the note on
/// each.
class Global {
public:
    using Pair = std::pair<ActorId, uint64_t>;

    Global() noexcept = default;
    Global(const Global &other);
    Global(Global &&other) noexcept;
    Global &operator=(const Global &other);
    Global &operator=(Global &&other) noexcept;
    ~Global();

    /// Highest counter observed for `actor`, or 0 when never observed.
    uint64_t get(const ActorId &actor) const;
    /// Record `id` as seen. Counter 0 is the "unseen" sentinel rather than an
    /// identity, so it is discarded.
    void observe(Identity id);
    bool observed(Identity id) const;
    /// True when every component of `other` is covered here.
    bool observed_all(const Global &other) const;
    /// Per-actor maximum.
    void join(const Global &other);
    /// Nonzero-minimum aggregation. **Not** a coverage intersection: an absent
    /// component leaves this vector's value alone. Asking "what has every peer
    /// seen?" requires `coverage_intersection`.
    void meet(const Global &other);
    /// Per-actor minimum over the union of both vectors, absent counting as
    /// zero and zero results dropped. Commutative, associative, idempotent; the
    /// empty vector is absorbing, because a peer with no history has seen
    /// nothing.
    [[nodiscard]] Global coverage_intersection(const Global &other) const;

    /// True when the vector holds no component. Zero components are canonicalized
    /// away, so this is also the emptiness a watermark check wants.
    [[nodiscard]] bool empty() const noexcept { return m_size == 0; }

    bool operator==(const Global &other) const;

    /// Sparse iteration: number of present (actor, counter) pairs.
    size_t pair_count() const noexcept { return m_size; }
    /// Sparse iteration: i-th present pair, sorted by actor ascending.
    Pair pair(size_t i) const noexcept { return data()[i]; }

private:
    static constexpr uint32_t SBO_CAP = 2;

    bool on_heap() const noexcept { return m_capacity > SBO_CAP; }
    // Inline storage is raw bytes because std::pair<ActorId, uint64_t> has a
    // non-trivial default constructor — ActorId value-initializes — and so
    // cannot be a union member.
    Pair *data_mut() noexcept {
        return on_heap() ? m_heap_data
                         : reinterpret_cast<Pair *>(m_inline_storage);
    }
    const Pair *data() const noexcept {
        return on_heap() ? m_heap_data
                         : reinterpret_cast<const Pair *>(m_inline_storage);
    }

    void ensure_capacity(uint32_t need);
    std::pair<uint32_t, bool> find_index(const ActorId &actor) const noexcept;

    uint32_t m_size = 0;
    uint32_t m_capacity = SBO_CAP;
    alignas(Pair) std::byte m_inline_storage[sizeof(Pair) * SBO_CAP];
    Pair *m_heap_data = nullptr;
};

// ============================================================================
// Locator
// ============================================================================

/// A fractional position identifier: a variable-length sequence of uint64_t
/// digits compared lexicographically.
///
/// A locator is a *position*, not an identity — which is why it keeps its
/// representation when identity changed (docs/specs/vnext-identity.md §6).
class Locator {
public:
    Locator() = default;
    explicit Locator(std::vector<uint64_t> digits) : m_digits(std::move(digits)) {}
    explicit Locator(uint64_t single) : m_digits{single} {}

    static Locator min();
    static Locator max();
    /// A locator strictly between `lo` and `hi`. Precondition: lo < hi.
    static Locator between(const Locator &lo, const Locator &hi);

    const std::vector<uint64_t> &digits() const { return m_digits; }
    size_t depth() const { return m_digits.size(); }

    std::strong_ordering operator<=>(const Locator &other) const;
    bool operator==(const Locator &other) const = default;

private:
    std::vector<uint64_t> m_digits;
};

// ============================================================================
// Anchors
// ============================================================================

/// Which side of a position an anchor falls on when text is inserted there.
/// Public because it is part of what an anchor *means*, not because callers
/// should be choosing it freely: affinity semantics are S04a's subject and are
/// specified in docs/specs/vnext-external-position-contract.md.
enum class Bias { Left, Right };

/// A stable reference to one item, by that item's identity.
///
/// Unlike a byte offset, an anchor does not move when text is inserted or deleted
/// elsewhere. `at_end` is an explicit past-the-end position rather than a magic
/// identity: a 128-bit actor space has no largest identity, so a sentinel built
/// from one would be an ordinary value a caller could not tell apart from a real
/// position.
///
/// Whether an anchor *resolves* — and to what — is engine behaviour, and the
/// current behaviour is a known gap: `resolve_anchor` does not yet honour
/// `bias`, and it falls back to end-of-document when an anchor names an item it
/// does not have. The capability manifest records both as unavailable rather
/// than letting a consumer discover them.
struct Anchor {
    Identity position;
    Bias bias = Bias::Left;
    bool at_end = false;

    Anchor() = default;
    Anchor(Identity id, Bias b = Bias::Left, bool end = false)
        : position(std::move(id)), bias(b), at_end(end) {}

    /// Document start.
    static Anchor min() { return {Identity::min(), Bias::Left}; }
    /// Document end.
    static Anchor end() { return {Identity::min(), Bias::Right, true}; }

    bool is_min() const { return !at_end && position == Identity::min(); }
    bool is_end() const { return at_end; }
    /// True when this names a real item identity rather than a sentinel. An
    /// anchor carrying an invalid actor is a sentinel: there is no actor 0.
    [[nodiscard]] bool is_item() const { return !at_end && position.actor.valid(); }

    bool operator==(const Anchor&) const = default;
};

// ============================================================================
// Identity allocation
// ============================================================================

/// Source of operation identities for one (document, stream) of one actor.
///
/// An engine borrows this and never owns identity allocation. There is
/// deliberately no engine constructor that takes only an `ActorId`: holding an
/// actor id is not authority to claim durable allocation, and a constructor that
/// quietly started a counter at 1 would hand out identities a relaunch hands out
/// again. An engine that cannot be given an allocator cannot mint an identity at
/// all, which is a much better failure than a plausible one.
class IdentityAllocator {
public:
    virtual ~IdentityAllocator() = default;

    IdentityAllocator() = default;
    IdentityAllocator(const IdentityAllocator &) = delete;
    IdentityAllocator &operator=(const IdentityAllocator &) = delete;

    /// Reserve `count` consecutive counters and return the first. All-or-nothing:
    /// on failure the counter is exactly where it was.
    ///
    /// A local operation that inserts `n` scalars reserves `n + 1`: its item
    /// identities are `first .. first + n - 1` and the operation's own identity
    /// is `first + n` — strictly above every item it created and strictly below
    /// the next reservation's base.
    [[nodiscard]] virtual Result<uint64_t> reserve_run(uint64_t count) = 0;

    /// Raise the counter above an identity observed from a peer. Derived state,
    /// not durable allocation: on reopen the engine replays its own log and
    /// observes again.
    virtual void observe(uint64_t counter) = 0;

    /// The value the next reservation will use, without reserving it.
    [[nodiscard]] virtual uint64_t peek() const noexcept = 0;

    /// The actor whose identities this allocator hands out.
    [[nodiscard]] virtual const ActorId &actor() const noexcept = 0;
};

/// A shared handle, because the allocator is a mutable resource and handing the
/// same one to two engines is meaningful. The pointee is non-const because
/// reserving from it mutates it; what is borrowed is the allocation.
using SharedIdentityAllocator = std::shared_ptr<IdentityAllocator>;

}  // namespace CollabText::Crdt