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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CollabText {

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
    writer_conflict,
    unknown_stream,
    wrong_epoch,            // operation belongs to another membership era
    counter_regression,
    counter_exhausted,
    // apply
    unresolvable_anchor,    // anchor names a position this engine does not have
    // store
    corrupt_record,
    io_error,
    random_source_unavailable,
    // limits
    deferred_queue_full,
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
/// [[nodiscard]] with no throwing accessor, so ignoring an error is a compile
/// error rather than a runtime surprise.
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

private:
    std::array<uint8_t, kBytes> m_bytes{};
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
    /// UINT64_MAX, which is a real counter value and must still be issued
    /// before the stream is declared exhausted.
    [[nodiscard]] bool exhausted() const noexcept { return m_next == kExhausted; }

    /// Reserve and return the next value. On exhaustion the counter is
    /// unchanged and the error is counter_exhausted.
    [[nodiscard]] Result<uint64_t> reserve(std::string_view context = {});

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
    uint64_t m_next = 0;
};

// ============================================================================
// Private replica state
// ============================================================================

/// A cross-process writer lease over one (document, stream).
///
/// Acquired by creating the lease file exclusively, so two local writers cannot
/// both believe they hold it. The holder's identity is recorded, so a conflict
/// names who holds it. There is deliberately no automatic takeover: a lease that
/// is reclaimed by timeout is a silent way to lose operations, and C2's rule —
/// nothing changes membership or ownership without an explicit act — applies
/// here too.
struct WriterLease {
    ActorId holder;
    uint64_t pid = 0;
    std::filesystem::path path;

    [[nodiscard]] std::string describe() const;
};

class ReplicaState {
public:
    /// Open (or create) the private state for this device.
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

    /// Peek without reserving. For tests and diagnostics.
    [[nodiscard]] Result<uint64_t> peek_stream(const StreamId& stream) const;

    /// Acquire the writer lease for (document, stream). Fails with
    /// ErrorCode::writer_conflict, naming the holder, if someone else holds it.
    [[nodiscard]] Result<WriterLease> acquire_writer(const DocumentId& document,
                                                     const StreamId& stream);
    void release_writer();
    [[nodiscard]] bool holds_writer(const DocumentId& document,
                                    const StreamId& stream) const;

    /// Explicitly fork this device's private state onto a fresh identity.
    ///
    /// Copying a shared document never does this, because shared documents do
    /// not contain actor identity. Copying *private* state does, and the only
    /// correct answer is a new actor — minting one is the caller's explicit
    /// statement that two independent writers now exist.
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
    ReplicaState() = default;

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
};

}  // namespace CollabText
