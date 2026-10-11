/// ActorId.h — persistent actor identity, checked counters, and the private
/// replica state that owns them.
///
/// Roadmap task P02, implementing the contracts fixed in
/// docs/specs/vnext-public-api.md §2 and docs/specs/vnext-protocol.md §1.
///
/// Three properties this header is built around, each of which the previous
/// implementation got wrong in a way the evaluation caught:
///
///   * An actor identity is 128 bits of OS cryptographic randomness, persisted
///     atomically *before* any operation is created. It is never derived from
///     presence, a display name, or a 16-bit draw, and there is no default
///     identity that means "actor 1" — a default-constructed ActorId is invalid
///     and any operation carrying it is rejected.
///   * Counters are checked uint64 and are never reused. Exhaustion is an error
///     that mutates nothing, not a wrap.
///   * Counters are per (actor, stream). There is no scalar in this file that
///     could be mistaken for coverage across streams.
///
/// The private state that holds all of this lives outside shared document
/// stores, so copying a shared folder produces a *new* actor rather than a
/// duplicate one. Copying private device state requires an explicit
/// fork_identity() call, which mints a fresh actor.

#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <ostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace CollabText {
namespace Storage { struct ProcessLease; }

// ============================================================================
// Errors
// ============================================================================

enum class ErrorCode {
    // decoding
    malformed,
    unknown_format,
    unsupported_version,
    invalid_unicode,        // invalid UTF-8, lone surrogate, truncated sequence
    // identity and context
    invalid_identity,
    actor_conflict,
    pin_conflict,           // durable reference/event ID was reused inconsistently
    writer_conflict,
    unknown_stream,
    wrong_epoch,            // operation belongs to another membership era
    counter_regression,
    counter_exhausted,
    // apply
    unresolvable_anchor,    // anchor names a position this engine does not have
    // scoped positions
    off_end,
    not_scalar_boundary,
    not_found,
    wrong_document,
    wrong_stream,
    wrong_primitive,
    awaiting_state,
    detached,
    limit_exceeded,
    history_unavailable,
    // store
    corrupt_record,
    io_error,
    commit_uncertain,       // publication happened; its durability barrier failed
    /// The platform cannot provide the requested guarantee (e.g. asking for a
    /// directory sync where the OS has none). Reported, never approximated.
    unsupported_operation,
    random_source_unavailable,
    // limits
    deferred_queue_full,
    // callbacks
    reentrant_mutation,
};

/// A typed error. Cheap to copy, safe to keep, and always carries where it
/// happened, because "it failed" is not something a caller can act on.
class Error {
public:
    Error() = default;
    Error(ErrorCode code, std::string message)
        : m_code(code), m_message(std::move(message)) {}
    Error(ErrorCode code, std::string message, std::string context)
        : m_code(code), m_message(std::move(message)), m_context(std::move(context)) {}

    [[nodiscard]] ErrorCode code() const noexcept { return m_code; }
    [[nodiscard]] std::string_view message() const noexcept { return m_message; }
    [[nodiscard]] std::optional<std::string> context() const { return m_context; }

    /// A default-constructed Error carries no message and is therefore "no
    /// error". Every Error built from a code and a message is truthy, so this
    /// is unambiguous and lets a caller write `if (result.error)`.
    [[nodiscard]] bool empty() const noexcept { return m_message.empty(); }
    explicit operator bool() const noexcept { return !m_message.empty(); }

    friend bool operator==(const Error& a, const Error& b) {
        return a.m_code == b.m_code && a.m_message == b.m_message &&
               a.m_context == b.m_context;
    }

    /// Append context, for wrapping an error as it propagates outward.
    [[nodiscard]] Error with_context(std::string context) const {
        return m_context ? Error(m_code, m_message, *m_context + " <- " + context)
                         : Error(m_code, m_message, std::move(context));
    }

private:
    ErrorCode m_code = ErrorCode::malformed;
    std::string m_message;
    std::optional<std::string> m_context;
};

[[nodiscard]] std::string_view to_string(ErrorCode code);

/// Result<T>: the project deliberately does not require C++23 std::expected.
/// [[nodiscard]] requests a compiler diagnostic when a result is discarded.
/// value() has a checked-by-caller ok() precondition; it is not a throwing accessor.
template <class T>
class [[nodiscard]] Result {
public:
    Result(T value) : m_value(std::move(value)) {}          // NOLINT: implicit by design
    Result(Error error) : m_error(std::move(error)) {}       // NOLINT: implicit by design

    [[nodiscard]] bool ok() const noexcept { return m_value.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    // A non-const lvalue yields a mutable reference: `result.value().mutate()`
    // should not require copying the value out first.
    [[nodiscard]] T& value() & {
        assert(ok() && "Result::value() on a failed Result");
        return *m_value;
    }
    [[nodiscard]] const T& value() const& {
        assert(ok() && "Result::value() on a failed Result");
        return *m_value;
    }
    [[nodiscard]] T&& value() && {
        assert(ok() && "Result::value() on a failed Result");
        return std::move(*m_value);
    }
    [[nodiscard]] const Error& error() const noexcept { return m_error; }

    template <class U>
    [[nodiscard]] T value_or(U fallback) const {
        return ok() ? *m_value : static_cast<T>(std::move(fallback));
    }

private:
    std::optional<T> m_value;
    Error m_error;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : m_error(std::move(error)), m_ok(false) {}  // NOLINT: implicit by design

    [[nodiscard]] bool ok() const noexcept { return m_ok; }
    explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] const Error& error() const noexcept { return m_error; }

    /// Shorthand for `return {}` / `return Error(...)` at call sites.
    static Result<void> success() { return Result<void>(); }

private:
    Error m_error;
    bool m_ok = true;
};

// ============================================================================
// Actor identity
// ============================================================================

/// 128-bit persistent actor identity.
///
/// Opaque: no ordering, no arithmetic, no default identity. The only
/// constructor from nothing yields an *invalid* id, so code cannot accidentally
/// operate as "actor 0" or "actor 1".
class ActorId {
public:
    static constexpr size_t kBytes = 16;

    ActorId() = default;

    /// Generate from OS cryptographic randomness through a narrow adapter.
    /// `source` exists so tests can inject collisions and failures; production
    /// passes nothing and gets the platform source.
    using Source = std::function<Result<std::array<uint8_t, kBytes>>(std::string_view purpose)>;

    [[nodiscard]] static Result<ActorId> generate();
    [[nodiscard]] static Result<ActorId> generate(const Source& source);

    /// Canonical lowercase hex, 32 characters.
    [[nodiscard]] static Result<ActorId> parse(std::string_view text);

    [[nodiscard]] static ActorId from_bytes(std::array<uint8_t, kBytes> bytes) noexcept {
        ActorId id;
        id.m_bytes = bytes;
        return id;
    }
    [[nodiscard]] std::array<uint8_t, kBytes> bytes() const noexcept { return m_bytes; }

    /// An all-zero id is not a valid actor. There is no such thing as actor 0.
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::string to_string() const;

    /// Deterministic total order, so an actor id can key a map and compare
    /// equal results across processes.
    [[nodiscard]] int compare(const ActorId& other) const noexcept;

    friend bool operator==(const ActorId& a, const ActorId& b) noexcept {
        return a.m_bytes == b.m_bytes;
    }
    friend bool operator!=(const ActorId& a, const ActorId& b) noexcept {
        return !(a == b);
    }
    friend bool operator<(const ActorId& a, const ActorId& b) noexcept {
        return a.compare(b) < 0;
    }

    /// Stream insertion as the canonical hex form.
    ///
    /// Added with the identity widening: diagnostics used to print a 16-bit
    /// replica id as a bare integer, and a bare integer for an identity is
    /// ambiguous between actors that happen to share one — which is exactly the
    /// collision this type exists to prevent. An error message that cannot say
    /// *which* actor is not much of an error message.
    friend std::ostream& operator<<(std::ostream& out, const ActorId& id) {
        return out << id.to_string();
    }

private:
    std::array<uint8_t, kBytes> m_bytes{};
};

/// Hash for opaque actor bytes, so an ActorId can key an unordered container.
///
/// FNV-1a over the 16 bytes. The choice is performance-only and never
/// load-bearing: nothing may depend on iteration order of a map keyed by
/// ActorId, because that order is not the identity order (see
/// docs/specs/vnext-identity.md §2 — the *comparison* is what must be a global
/// function of the actor, and an unordered container has no such requirement).
struct ActorIdHash {
    [[nodiscard]] size_t operator()(const ActorId& id) const noexcept {
        size_t hash = 1469598103934665603ULL;
        for (uint8_t byte : id.bytes()) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
        return hash;
    }
};

/// A unique operation identity: an actor and its counter *within one stream*.
struct OperationId {
    ActorId actor;
    uint64_t counter = 0;

    [[nodiscard]] bool operator==(const OperationId&) const = default;
    [[nodiscard]] int compare(const OperationId& other) const noexcept {
        if (const int by_actor = actor.compare(other.actor); by_actor != 0)
            return by_actor;
        if (counter < other.counter) return -1;
        return counter > other.counter ? 1 : 0;
    }
    friend bool operator<(const OperationId& a, const OperationId& b) noexcept {
        return a.compare(b) < 0;
    }
};

using StreamId = std::string;
using DocumentId = std::string;

/// Everything an operation needs to be placed. An operation whose context the
/// receiver does not recognise is rejected, not guessed at.
struct OperationContext {
    DocumentId document;
    StreamId stream;
    uint64_t epoch = 1;
    OperationId id;

    [[nodiscard]] bool operator==(const OperationContext&) const = default;
};

/// A checked uint64 counter for one stream.
///
/// `reserve()` hands out the next value and never reuses one. At
/// UINT64_MAX it returns ErrorCode::counter_exhausted and mutates nothing: a
/// wrap would let a new operation impersonate an old one, which is finding F4 in
/// a different costume.
class CheckedCounter {
public:
    CheckedCounter() = default;
    explicit CheckedCounter(uint64_t start) : m_next(start) {}

    /// The value the next reservation will use, without reserving it.
    [[nodiscard]] uint64_t peek() const noexcept { return m_next; }

    /// True once the counter can no longer reserve: its next value is
    /// UINT64_MAX. That value is the exhaustion sentinel, not issued, since a
    /// following reservation must remain representable.
    [[nodiscard]] bool exhausted() const noexcept { return m_next == kExhausted; }

    /// Reserve and return the next value. On exhaustion the counter is
    /// unchanged and the error is counter_exhausted.
    [[nodiscard]] Result<uint64_t> reserve(std::string_view context = {});

    /// Reserve a *run* of `count` consecutive values and return the first.
    ///
    /// This is the allocation one local operation actually needs: the operation's
    /// own identity plus one identity per inserted scalar (docs/specs/
    /// vnext-identity.md §3.2). Handing out a whole interval in one checked step
    /// is what keeps a single crash from leaving an operation's identities
    /// partly issued and partly reissuable, and it is why an engine borrows a
    /// counter rather than ticking one value at a time.
    ///
    /// All-or-nothing. `count == 0` is refused: every operation consumes at least
    /// its own identity, so a zero-length reservation is a caller error rather
    /// than an empty range. The bound is tested by subtraction, because
    /// `m_next + count` may itself overflow. `kExhausted` is never issued, so the
    /// highest value `reserve_run` can hand out is `kExhausted - 1`.
    [[nodiscard]] Result<uint64_t> reserve_run(uint64_t count, std::string_view context = {});

    /// Raise the counter to cover a value observed from a peer: the next
    /// reservation becomes `observed + 1`, so a locally minted operation can
    /// never collide with one the peer already has.
    ///
    /// Being *ahead* of an observed value is normal and never an error. Only two
    /// things fail: observing the very last value, which cannot be raised past,
    /// and a corrupted value that would move the counter backwards.
    [[nodiscard]] Result<void> observe(uint64_t observed, std::string_view context = {});

    /// Restore a persisted high-water mark. Refuses to go backwards, so a
    /// truncated or rewritten state file cannot reissue identifiers.
    [[nodiscard]] Result<void> restore(uint64_t next_value, std::string_view context = {});

    static constexpr uint64_t kExhausted = UINT64_MAX;

private:
    uint64_t m_next = 1;
};

// ============================================================================
// Private replica state
// ============================================================================

/// A cross-process writer lease over one (document, stream).
///
/// Owned by ReplicaState through an OS lock. Process death/release frees the
/// local writer lock; it never retires a device or changes membership. This
/// returned value is diagnostic metadata, not transferable lock ownership.
struct WriterLease {
    ActorId holder;
    uint64_t pid = 0;
    std::filesystem::path path;

    [[nodiscard]] std::string describe() const;
};

class ReplicaState {
public:
    ReplicaState(const ReplicaState&) = delete;
    ReplicaState& operator=(const ReplicaState&) = delete;
    ReplicaState(ReplicaState&&) noexcept;
    ReplicaState& operator=(ReplicaState&&) noexcept;
    ~ReplicaState();
    /// Open (or create) the private state for this device.
    /// One owner holds the OS lock for this entire directory until destruction.
    /// Multiple documents share that owner; a second open returns writer_conflict.
    ///
    /// `state_dir` must be outside every shared document store. The actor
    /// identity is generated once and persisted atomically; a corrupt state file
    /// is reported, never replaced, because replacing it silently would mint a
    /// new actor while old operations still claim the old one.
    [[nodiscard]] static Result<ReplicaState> open(std::filesystem::path state_dir);

    /// As above, with an injected randomness source, for collision and failure
    /// tests. A source that fails produces a failed Result and no state file.
    [[nodiscard]] static Result<ReplicaState> open(std::filesystem::path state_dir,
                                                   const ActorId::Source& source);

    [[nodiscard]] const ActorId& actor() const noexcept { return m_actor; }
    [[nodiscard]] const std::filesystem::path& state_dir() const noexcept { return m_state_dir; }

    /// Reserve the next operation id for `stream`, persisting the reservation
    /// before returning it.
    ///
    /// Persisting before returning is the whole point: an id that exists only in
    /// memory is an id a crash will hand out twice.
    [[nodiscard]] Result<OperationId> next_operation(const StreamId& stream,
                                                     const DocumentId& document,
                                                     uint64_t epoch);

    /// Reserve a whole run of `count` counters for `stream` and return the first,
    /// persisting the reservation before returning it.
    ///
    /// This is the allocation one local operation actually needs: `n` item
    /// identities followed by the operation's own, from a single durable write
    /// (docs/specs/vnext-identity.md §3.2). Reserving them one at a time would
    /// persist `n + 1` times, and a crash in the middle would leave an
    /// operation's identities partly issued and partly reissuable — which is
    /// indistinguishable, to a reader, from an operation that never existed.
    ///
    /// The whole `count` is consumed or none of it is. A failure after the
    /// counter moved does not roll back, for the same reason `next_operation`
    /// does not: a skipped value is a gap, and a reissued value is a duplicated
    /// identity. Peers resolve the gap across an explicit gap record (L02 owns
    /// its publication).
    [[nodiscard]] Result<uint64_t> reserve_run(const StreamId& stream,
                                               const DocumentId& document,
                                               uint64_t epoch,
                                               uint64_t count);

    /// Raise `stream`'s counter above a counter observed from a peer, without
    /// persisting.
    ///
    /// Deliberately not durable (identity spec §3.4). The raise exists so a
    /// later local identity sorts after what this replica has seen; on reopen
    /// the engine replays its own log and observes again, and a lost raise
    /// cannot cause reuse because no identity was issued while it was held.
    /// Persisting it would be defensible too, but it would turn a cheap
    /// derived value into a write on the remote-apply path.
    [[nodiscard]] Result<void> observe_remote(const StreamId& stream, uint64_t counter);

    /// Peek without reserving. For tests and diagnostics.
    [[nodiscard]] Result<uint64_t> peek_stream(const StreamId& stream) const;

    /// Acquire the writer lease for (document, stream). Fails with
    /// ErrorCode::writer_conflict, naming the holder, if someone else holds it.
    [[nodiscard]] Result<WriterLease> acquire_writer(const DocumentId& document,
                                                     const StreamId& stream);
    void release_writer();
    /// Release all writer locks and the private-state owner; safe twice.
    /// Mutations on a closed owner return writer_conflict.
    [[nodiscard]] Result<void> close();
    [[nodiscard]] bool holds_writer(const DocumentId& document,
                                    const StreamId& stream) const;

    /// Explicitly fork this device's private state onto a fresh identity.
    ///
    /// Copying a shared document never does this, because shared documents do
    /// not contain actor identity. Copying *private* state does, and the only
    /// correct answer is a new actor — minting one is the caller's explicit
    /// statement that two independent writers now exist. Active writer leases
    /// must first be released. A failed fork makes this owner unusable until
    /// reopen, since publication may have happened before a barrier failed.
    [[nodiscard]] Result<ActorId> fork_identity(std::string_view reason);

    /// Check an operation envelope that claims to be from this actor.
    ///
    /// A peer claiming our ActorId with a counter at or below the high-water
    /// mark we have already issued on `stream` is an observable identity
    /// conflict: two devices are producing operations under one identity. It is
    /// reported, never resolved by "last writer wins". A counter above our mark
    /// is a peer that has advanced further than our persisted state knows, which
    /// is a corrupt or truncated local state and is reported as such.
    [[nodiscard]] Result<void> check_claim(const StreamId& stream,
                                           const ActorId& claimant,
                                           uint64_t their_counter,
                                           std::string_view context = {}) const;

    /// Highest counter reserved for `stream`, or 0 if none.
    [[nodiscard]] uint64_t high_water(const StreamId& stream) const;

    /// Flush pending reservations durably. Called automatically by
    /// next_operation(); exposed for tests that need an explicit barrier.
    [[nodiscard]] Result<void> flush();

private:
    ReplicaState();

    struct StreamCounters {
        CheckedCounter counter;
    };

    [[nodiscard]] Result<void> load();
    [[nodiscard]] Result<void> persist() const;
    [[nodiscard]] Result<StreamCounters*> stream(const StreamId& name);
    [[nodiscard]] std::filesystem::path lease_path(const DocumentId& document,
                                                   const StreamId& stream) const;

    std::filesystem::path m_state_dir;
    ActorId m_actor;
    std::vector<std::pair<StreamId, StreamCounters>> m_streams;
    std::vector<WriterLease> m_leases;
    std::unique_ptr<Storage::ProcessLease> m_state_lease;
    std::vector<std::unique_ptr<Storage::ProcessLease>> m_writer_locks;
    bool m_failed = false;
};

}  // namespace CollabText
