/// IdentityAllocationModel.h — independent model of the identity *allocation*
/// contract in docs/specs/vnext-identity.md §3.1–§3.7 (roadmap task P06a).
///
/// ─── What this is for ────────────────────────────────────────────────────────
///
/// P06a has a design gate: the allocation contract and an independent model must
/// exist and run before the engine's identity types change. `tst_identity_
/// ordering_model` proved the *order*'s laws. That is not enough, and the review
/// that demanded this file said why: legacy Buffer hands out one Lamport value
/// per inserted character and derives split origins arithmetically, so a
/// comparator model says nothing about whether one P02 operation reservation can
/// supply a whole run, whether two operations can collide, or what a crash
/// between reservation and log append leaves behind.
///
/// So this file implements the *allocation* rules from the specification text —
/// run-interval reservation, derived item identities for splits, deletion and
/// undo identities, the causal raise on remote observation, the exhaustion
/// boundary, and abandoned reservations becoming explicit gaps. It is written to
/// be read against that document, and it is the artefact that a later engine
/// adoption is checked against.
///
/// ─── Independence ────────────────────────────────────────────────────────────
///
/// The roadmap requires that a reference model not call the production helper
/// whose behaviour it validates. Nothing here calls `CheckedCounter`,
/// `ReplicaState`, or any engine type. The counter below is a second
/// implementation of the same arithmetic, with its own overflow-safe bound
/// check, and `tst_identity_allocation_model` compares the two implementations'
/// *answers* — that comparison is the only place they meet, and it is a
/// comparison rather than a tautology.
///
/// The model also uses its own tiny result type instead of `Result`/`Error`,
/// because the point is to reproduce the contract's outcomes, not to reuse the
/// production error vocabulary. The test maps model reasons onto production
/// `ErrorCode`s explicitly, so a mismatch in either direction is visible.
///
/// Note: escaped string literals throughout. Qt's moc silently drops every test
/// slot in a translation unit containing a raw string literal with braces (a P04
/// limitation).

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace collabtext_test {
namespace allocation_model {

// ---------------------------------------------------------------------------
// Reasons — the model's own outcome vocabulary
// ---------------------------------------------------------------------------

enum class Reason {
    ok,
    empty_run,           ///< a run of zero values is not an operation
    counter_zero,        ///< counter 0 is the invalid-identity sentinel
    exhausted,           ///< the run does not fit below the last value
    regression,          ///< a restore tried to move the counter backwards
};

/// Minimal result. No exceptions, no allocation beyond the value.
template <class T>
struct Outcome {
    bool ok = false;
    T value{};
    Reason reason = Reason::ok;

    static Outcome success(T v) { return Outcome{true, std::move(v), Reason::ok}; }
    static Outcome failure(Reason r) { return Outcome{false, T{}, r}; }

    explicit operator bool() const { return ok; }
};

/// The no-value case, for the counter's raise/restore outcomes.
template <>
struct Outcome<void> {
    bool ok = false;
    Reason reason = Reason::ok;

    static Outcome success() { return Outcome{true, Reason::ok}; }
    static Outcome failure(Reason r) { return Outcome{false, r}; }

    explicit operator bool() const { return ok; }
};

inline Outcome<void> ok_outcome() { return Outcome<void>::success(); }

// ---------------------------------------------------------------------------
// Actor identity and the order
// ---------------------------------------------------------------------------

/// 128-bit actor, compared byte-wise from the most significant byte down. Its
/// own implementation on purpose: §2.1 forbids deriving the order from anything
/// local, and a model that borrowed the production comparator could not catch a
/// mistake in it.
struct ModelActor {
    std::array<uint8_t, 16> bytes{};

    static ModelActor from_seed(uint64_t seed) {
        ModelActor a;
        for (size_t i = 0; i < a.bytes.size(); ++i)
            a.bytes[i] = static_cast<uint8_t>((seed >> ((i % 8) * 8)) ^ (i * 31) ^ 0x5A);
        a.bytes[0] |= 0x01;  // all-zero is invalid, so never produce it here
        return a;
    }

    /// Invalid means all-zero. There is no actor 0 (§1).
    [[nodiscard]] bool valid() const {
        for (uint8_t b : bytes)
            if (b != 0) return true;
        return false;
    }
};

inline int compare_actors(const ModelActor& a, const ModelActor& b) {
    for (size_t i = 0; i < a.bytes.size(); ++i) {
        if (a.bytes[i] != b.bytes[i]) return a.bytes[i] < b.bytes[i] ? -1 : 1;
    }
    return 0;
}

/// An identity: actor plus counter, ordered counter-first (§2).
struct ModelIdentity {
    ModelActor actor;
    uint64_t counter = 0;
};

inline bool operator<(const ModelIdentity& a, const ModelIdentity& b) {
    if (a.counter != b.counter) return a.counter < b.counter;
    return compare_actors(a.actor, b.actor) < 0;
}

inline bool operator==(const ModelIdentity& a, const ModelIdentity& b) {
    return a.counter == b.counter && compare_actors(a.actor, b.actor) == 0;
}

// ---------------------------------------------------------------------------
// The checked counter
// ---------------------------------------------------------------------------

/// Per (actor, stream) counter. `kExhausted` is a sentinel that is never issued,
/// so the highest value any reservation can hand out is `kExhausted - 1` (§3.5).
///
/// The bound is tested as `count - 1 > (kExhausted - 1) - next`. Adding first
/// would wrap: `next + count` can exceed `UINT64_MAX` while every value in the
/// run is still representable.
class ModelCounter {
public:
    static constexpr uint64_t kExhausted = std::numeric_limits<uint64_t>::max();

    explicit ModelCounter(uint64_t start = 1) : m_next(start) {}

    [[nodiscard]] uint64_t peek() const { return m_next; }
    [[nodiscard]] bool exhausted() const { return m_next == kExhausted; }

    /// One value: the operation-only case, and the shape the legacy engine used
    /// per character.
    [[nodiscard]] Outcome<uint64_t> reserve() { return reserve_run(1); }

    /// A whole run, all-or-nothing (§3.2).
    [[nodiscard]] Outcome<uint64_t> reserve_run(uint64_t count) {
        if (count == 0) return Outcome<uint64_t>::failure(Reason::empty_run);
        if (m_next == 0) return Outcome<uint64_t>::failure(Reason::counter_zero);
        if (m_next == kExhausted || count - 1 > (kExhausted - 1) - m_next)
            return Outcome<uint64_t>::failure(Reason::exhausted);
        const uint64_t base = m_next;
        m_next = base + count;
        return Outcome<uint64_t>::success(base);
    }

    /// The causal raise (§3.4). Being ahead of an observed value is ordinary;
    /// only observing the very last value fails.
    [[nodiscard]] Outcome<void> observe(uint64_t observed) {
        if (observed == kExhausted) return Outcome<void>::failure(Reason::exhausted);
        if (observed + 1 > m_next) m_next = observed + 1;
        return ok_outcome();
    }

    /// Refuses to go backwards, so a truncated or rewritten state file cannot
    /// reissue identifiers.
    [[nodiscard]] Outcome<void> restore(uint64_t next_value) {
        if (next_value < m_next) return Outcome<void>::failure(Reason::regression);
        m_next = next_value;
        return ok_outcome();
    }

private:
    uint64_t m_next;
};

// ---------------------------------------------------------------------------
// Allocation: one operation, its run interval, and its items
// ---------------------------------------------------------------------------

/// What one `allocate_operation` call produced, before anything is published.
///
/// `published` is the difference between an identity that reached the durable
/// log and one that was only reserved (§3.6). It is why an interval can be
/// abandoned without any identity ever being reissued.
struct ModelAllocation {
    ModelIdentity operation;   ///< base + item_count
    uint64_t item_base = 0;    ///< base + 0
    uint64_t item_count = 0;
    std::string stream;
    bool published = false;

    [[nodiscard]] uint64_t base() const { return item_base; }

    /// The operation's identity is strictly above every item it created and
    /// strictly below the next operation's base.
    [[nodiscard]] uint64_t operation_counter() const { return item_base + item_count; }

    [[nodiscard]] bool item_in_range(uint64_t counter) const {
        return counter >= item_base && counter < item_base + item_count;
    }

    /// The identity of one inserted scalar. Derived from the interval, which is
    /// what makes a split a derivation rather than an allocation.
    [[nodiscard]] ModelIdentity item_identity(uint64_t index) const {
        return ModelIdentity{operation.actor, item_base + index};
    }
};

/// Per (document, stream) allocator. One durable owner per private state
/// directory owns the real one; the engine borrows this (§3.7).
class ModelAllocator {
public:
    ModelAllocator(std::string document, std::string stream, ModelActor actor,
                   uint64_t start = 1)
        : m_document(std::move(document)), m_stream(std::move(stream)),
          m_actor(actor), m_counter(start) {}

    [[nodiscard]] const std::string& document() const { return m_document; }
    [[nodiscard]] const std::string& stream() const { return m_stream; }
    [[nodiscard]] const ModelActor& actor() const { return m_actor; }
    [[nodiscard]] uint64_t high_water() const { return m_counter.peek(); }

    /// Reserve `item_count + 1` values: the items, then the operation (§3.2).
    [[nodiscard]] Outcome<ModelAllocation> allocate_operation(uint64_t item_count) {
        const auto reserved = m_counter.reserve_run(item_count + 1);
        if (!reserved) return Outcome<ModelAllocation>::failure(reserved.reason);
        ModelAllocation alloc;
        alloc.operation = ModelIdentity{m_actor, reserved.value + item_count};
        alloc.item_base = reserved.value;
        alloc.item_count = item_count;
        alloc.stream = m_stream;
        m_allocations.push_back(alloc);
        return Outcome<ModelAllocation>::success(alloc);
    }

    void mark_published(size_t index) { m_allocations.at(index).published = true; }

    [[nodiscard]] const std::vector<ModelAllocation>& allocations() const {
        return m_allocations;
    }

    /// Reserved but never published: the crash window of §3.6.
    [[nodiscard]] std::vector<size_t> unpublished() const {
        std::vector<size_t> out;
        for (size_t i = 0; i < m_allocations.size(); ++i)
            if (!m_allocations[i].published) out.push_back(i);
        return out;
    }

    /// The causal raise, §3.4. Not durable: a lost raise cannot cause reuse,
    /// because no identity was issued while it was held.
    [[nodiscard]] Outcome<void> observe_remote(const ModelIdentity& remote_operation) {
        return m_counter.observe(remote_operation.counter);
    }

    /// Reopen at the persisted high-water mark. Same actor, never a restart at 1.
    [[nodiscard]] ModelAllocator reopen() const {
        return ModelAllocator(m_document, m_stream, m_actor, m_counter.peek());
    }

private:
    std::string m_document;
    std::string m_stream;
    ModelActor m_actor;
    ModelCounter m_counter;
    std::vector<ModelAllocation> m_allocations;
};

// ---------------------------------------------------------------------------
// Item identity derivation
// ---------------------------------------------------------------------------

/// One run of `length` scalars, identified by the interval it was allocated
/// from. A split partitions positions inside this; it never allocates (§3.3).
struct ModelRun {
    ModelAllocation allocation;
    uint64_t offset = 0;   ///< first position of this run within the interval
    uint64_t length = 0;

    [[nodiscard]] ModelIdentity item(uint64_t index) const {
        return ModelIdentity{allocation.operation.actor,
                             allocation.item_base + offset + index};
    }

    [[nodiscard]] bool holds(const ModelIdentity& id) const {
        return allocation.item_in_range(id.counter);
    }
};

/// Split at a scalar boundary. Derived identities must stay inside the owning
/// interval; an identity outside it would be a defect, not a new numbering.
[[nodiscard]] inline ModelRun split_run(const ModelRun& run, uint64_t at) {
    if (at > run.length) return run;
    ModelRun tail = run;
    tail.offset = run.offset + at;
    tail.length = run.length - at;
    return tail;
}

// ---------------------------------------------------------------------------
// Deletion and undo identities
// ---------------------------------------------------------------------------

/// A deletion is named by the deleting operation's own identity (§3.3) — no
/// extra counter tick, unlike legacy `deletion_ts`.
[[nodiscard]] inline ModelIdentity deletion_identity(const ModelAllocation& op) {
    return op.operation;
}

/// Undo-map entries are keyed by (item identity, undo-operation identity). The
/// two fields come from different domains, which is why an entry can never be
/// confused with the operation that created the characters it hides.
struct ModelUndoEntry {
    ModelIdentity edit_id;   ///< item identity
    ModelIdentity undo_id;   ///< undo operation identity
    uint32_t count = 0;
};

[[nodiscard]] inline ModelUndoEntry undo_entry(const ModelIdentity& item,
                                              const ModelAllocation& undo_op,
                                              uint32_t previous_count) {
    return ModelUndoEntry{item, undo_op.operation, previous_count + 1};
}

[[nodiscard]] inline bool is_undone(const std::vector<ModelUndoEntry>& entries,
                                    const ModelIdentity& item) {
    uint32_t highest = 0;
    for (const auto& e : entries)
        if (e.edit_id == item && e.count > highest) highest = e.count;
    return highest % 2 == 1;
}

// ---------------------------------------------------------------------------
// The receiving side: contiguous coverage and explicit gaps
// ---------------------------------------------------------------------------

/// One durable record occupying a half-open counter interval for one actor on
/// one stream: either an operation, or an explicit gap standing in for an
/// abandoned reservation. Both advance the frontier; nothing else does (§3.6).
struct ModelRecord {
    ModelActor actor;
    std::string stream;
    uint64_t lo = 0;   ///< inclusive
    uint64_t hi = 0;   ///< exclusive
    bool is_gap = false;
};

/// The contiguous-prefix frontier, per (stream, actor). A maximum observed
/// counter is not proof of contiguity: receiving (actor, 40) and (actor, 43)
/// leaves the frontier below 40 until 41 and 42 are accounted for, by operation
/// or by gap. Two streams holding the same counter value are two independent
/// facts, so they neither cover nor block each other (§3.7).
class ModelFrontier {
public:
    void receive(const ModelRecord& record) {
        auto& ranges = m_ranges[Key{record.stream, record.actor.bytes}];
        ranges.push_back({record.lo, record.hi});
        std::sort(ranges.begin(), ranges.end(),
                  [](const Range& a, const Range& b) { return a.lo < b.lo; });
        std::vector<Range> merged;
        for (const Range& r : ranges) {
            if (!merged.empty() && r.lo <= merged.back().hi)
                merged.back().hi = std::max(merged.back().hi, r.hi);
            else
                merged.push_back(r);
        }
        ranges = std::move(merged);
    }

    /// Highest counter such that every value from 1 up to it is accounted for.
    [[nodiscard]] uint64_t frontier(const std::string& stream, const ModelActor& actor) const {
        const auto it = m_ranges.find(Key{stream, actor.bytes});
        if (it == m_ranges.end() || it->second.empty()) return 0;
        const Range& first = it->second.front();
        if (first.lo > 1) return 0;  // nothing from the beginning is known
        return first.hi - 1;
    }

private:
    struct Range {
        uint64_t lo = 0;
        uint64_t hi = 0;
    };
    using Key = std::pair<std::string, std::array<uint8_t, 16>>;
    std::map<Key, std::vector<Range>> m_ranges;
};

/// Turn every reserved-but-unpublished interval into a gap record. This is what
/// reopen owes the stream: an accounted-for absence, not a stalled frontier.
[[nodiscard]] inline std::vector<ModelRecord> gap_records_for(
    const ModelAllocator& allocator) {
    std::vector<ModelRecord> records;
    for (size_t index : allocator.unpublished()) {
        const ModelAllocation& alloc = allocator.allocations().at(index);
        records.push_back(ModelRecord{allocator.actor(), allocator.stream(),
                                      alloc.item_base, alloc.operation_counter() + 1,
                                      true});
    }
    return records;
}

}  // namespace allocation_model
}  // namespace collabtext_test