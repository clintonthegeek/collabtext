#pragma once

#include <collabtext/ActorId.h>

#include <cstddef>
#include <cstdint>
#include <compare>
#include <utility>

namespace CollabText::Crdt {

/// An operation's identity: a persistent 128-bit actor plus a checked 64-bit
/// counter (docs/specs/vnext-identity.md §1).
///
/// The order is `(counter, actor)`, counter first, and it is not negotiable.
/// Two fragments can share a locator — `Buffer::normalize_fragments` exists to
/// handle runs from different actors at one shared locator, and split
/// relocation deliberately leaves locators equal — so when locators tie,
/// origin decides, and a replica that disagrees about that order produces a
/// different document, permanently and invisibly.
///
/// The actor half is compared byte-wise through `ActorId::compare`, which is a
/// deterministic total order every replica computes identically. It must not
/// become a locally assigned dense slot: two replicas meeting the same actors
/// in opposite orders would then tie-break equal locators in opposite
/// directions. §2.1 of the specification rules that out.
struct Identity {
    ActorId actor;
    uint64_t counter = 0;

    Identity() = default;
    Identity(ActorId a, uint64_t c) : actor(std::move(a)), counter(c) {}

    /// The least identity: counter 0 and an invalid actor. True minimum, because
    /// counter 0 beats every higher counter and an all-zero actor beats every
    /// valid one. Fragment summaries seed with it, so a fold that starts here
    /// must not be able to lose a real identity.
    static Identity min() { return {ActorId(), 0}; }

    /// Take the next counter value for the same actor, returning the value this
    /// clock *had* before the call.
    ///
    /// The return-before-increment shape is deliberate and preserved from the
    /// legacy `Lamport::tick()`, whose body incremented `this` and returned a
    /// pre-increment copy. Call sites depend on both halves: `frag_origin =
    /// m_clock.tick()` wants the first item identity, and the operation's own
    /// identity is then `m_clock.counter - 1`, the last of its items. Callers
    /// that discard the result are using it as "advance by one".
    ///
    /// This is the legacy per-scalar allocation and it is what §3.2's run
    /// intervals replace; the awkward return is part of why. Do not "fix" it
    /// into a post-increment return without auditing every call site.
    Identity tick() {
        const Identity previous = *this;
        ++counter;
        return previous;
    }

    /// Raise past an observed identity. The actor half is deliberately ignored:
    /// this is the causal clock, and a local identity must sort after
    /// everything this replica has seen regardless of who authored it.
    void observe(Identity other) {
        if (other.counter >= counter)
            counter = other.counter + 1;
    }

    /// Stable public accessor: the logical clock value. Part of the OpStream
    /// narrow contract; safe to depend on.
    uint64_t counter_value() const noexcept { return counter; }

    auto operator<=>(const Identity &other) const {
        if (auto cmp = counter <=> other.counter; cmp != 0)
            return cmp;
        // ActorId::compare returns an int; fold it into an ordering so this is
        // a real three-way comparison rather than a mixed return type.
        const int by_actor = actor.compare(other.actor);
        if (by_actor < 0) return std::strong_ordering::less;
        if (by_actor > 0) return std::strong_ordering::greater;
        return std::strong_ordering::equal;
    }
    bool operator==(const Identity &other) const = default;
};

/// Version vector keyed by actor id. A sorted sequence of
/// `(ActorId, counter)` pairs, sparse: only actors that have been observed take
/// space. Up to SBO_CAP entries live inline; larger spill to the heap.
///
/// Sparseness is load-bearing. A dense array indexed by actor id was the
/// original O(actor) per-edit bug (docs/handoff/2026-05-09-collabtext-bug-report.md):
/// per-edit cost grew linearly with the magnitude of the key, because every
/// aggregated summary copied and zeroed the whole array. With 128-bit keys a
/// dense index is not even representable, so the sparse pair list is the only
/// option — but the per-edit cost argument is unchanged and is what the inline
/// capacity is sized against.
///
/// There is no dense view any more, and there cannot be one: `size()` and
/// `operator[]` used to mean "max observed replica_id + 1" and "the value at
/// replica_id == i", which had no meaning once keys stopped being consecutive
/// integers. Callers that need to walk the vector use `pair_count()`/`pair()`.
///
/// The inline capacity is 2 rather than the legacy 4 because a pair is 24 bytes
/// instead of 8. Three inline pairs would cost 72 bytes for a vector that is
/// usually empty or single-actor, and version vectors are stored per participant
/// rather than per summary — but this is a size/perf tradeoff to revisit with
/// measurement in the optimization phase, not a settled optimum.
class Global {
public:
    /// One observed component: the highest counter seen from one actor.
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
    /// identity (§3 of the identity specification), so it is discarded.
    void observe(Identity id);
    bool observed(Identity id) const;
    /// True when every component of `other` is covered here.
    bool observed_all(const Global &other) const;
    /// Per-actor maximum.
    void join(const Global &other);

    /// Nonzero-minimum aggregation. NOT a coverage intersection.
    ///
    /// For each component present in `*this`, take the smaller of the two
    /// values **when the other vector has a nonzero value for that actor**;
    /// when it does not, leave this vector's value alone. So
    /// `{1:10}.meet({}) == {1:10}`.
    ///
    /// That is the right answer for one narrow question — "what is the largest
    /// counter that some vector in this set has *not* gone below?" — and the
    /// wrong answer for "what has every peer seen?", because a peer that has
    /// never observed actor 1 constrains actor 1's common coverage to zero,
    /// not to whatever it happens to hold locally. Finding F3 is exactly this
    /// conflation, and NetworkSim::compact_all() still builds its GC watermark
    /// this way; it is inert only because V03 made reclamation a no-op.
    ///
    /// New code that means "common coverage" must call coverage_intersection().
    void meet(const Global &other);

    /// Coverage intersection: the per-actor minimum over the **union** of both
    /// vectors, with an absent component counting as zero, and zero components
    /// canonicalized away.
    ///
    /// This is the operation a watermark needs, and it is deliberately not the
    /// same operation as meet():
    ///
    ///   {1:10} ∩ {}      == {}      a peer that never observed actor 1 has
    ///   {1:10} ∩ {2:5}   == {}      seen none of it, so common coverage of
    ///   {1:10} ∩ {1:5}   == {1:5}   actor 1 is zero
    ///   {1:10} ∩ {1:10}  == {1:10}
    ///
    /// Laws: commutative, associative, idempotent. There is no identity element
    /// in this representation — the mathematical identity is "every counter at
    /// its maximum", which a sparse vector cannot hold — and the empty vector is
    /// the *absorbing* element, because a peer with no history has seen nothing.
    /// Folding over a set of peers therefore starts from the local vector and
    /// intersects each peer in turn; it never starts from empty.
    [[nodiscard]] Global coverage_intersection(const Global &other) const;

    /// True when the vector holds no component. Because zero components are
    /// canonicalized away, this is also the emptiness a watermark check wants.
    [[nodiscard]] bool empty() const noexcept { return m_size == 0; }

    bool operator==(const Global &other) const;

    /// Sparse iteration: number of present (actor, counter) pairs.
    size_t pair_count() const noexcept { return m_size; }
    /// Sparse iteration: i-th present pair, sorted by actor ascending.
    Pair pair(size_t i) const noexcept { return data()[i]; }

private:
    static constexpr uint32_t SBO_CAP = 2;

    bool on_heap() const noexcept { return m_capacity > SBO_CAP; }

    /// Inline storage is raw bytes rather than a `Pair[]` member because
    /// `std::pair<ActorId, uint64_t>` has a non-trivial default constructor —
    /// ActorId value-initializes — and so cannot be a union member. The
    /// reinterpret_cast is well-defined: the buffer is suitably aligned and
    /// every object in it is created by copy before it is read.
    Pair *data_mut() noexcept {
        return on_heap() ? m_heap_data
                         : reinterpret_cast<Pair *>(m_inline_storage);
    }
    const Pair *data() const noexcept {
        return on_heap() ? m_heap_data
                         : reinterpret_cast<const Pair *>(m_inline_storage);
    }

    /// Ensure room for at least `need` pairs. May reallocate; do NOT hold
    /// pointers across this call.
    void ensure_capacity(uint32_t need);

    /// Find the index of `actor`, or the insertion point if absent.
    /// Returns (idx, found).
    std::pair<uint32_t, bool> find_index(const ActorId &actor) const noexcept;

    uint32_t m_size = 0;       ///< number of present pairs
    uint32_t m_capacity = SBO_CAP;
    alignas(Pair) std::byte m_inline_storage[sizeof(Pair) * SBO_CAP];
    Pair *m_heap_data = nullptr;
};

} // namespace CollabText::Crdt
