/// tests/consumer-smoke/wave-contract/probes/probe_contract.h
/// The typed vocabulary the W01 consumer probes are stated in.
///
/// Roadmap task W01. **This header is a contract, not an implementation.** It
/// declares the enumerations that `docs/specs/vnext-consumer-probes.md` and the
/// fixtures in `libs/collabtext/tests/fixtures/external-position/` are written
/// against, so that W02, S04b and W03 cannot each invent their own spelling of
/// "wrong context" or "collapsed". There is deliberately no `main`, no engine
/// call and no passing assertion here: W01 specifies cases, and a fixture that
/// reports success before anything executes is the failure mode this roadmap
/// keeps running into.
///
/// Two rules the type system here is meant to make hard to violate:
///
///   1. A resolution carries **either** an offset **or** a typed error, never
///      both and never neither. `Resolved` with no offset is how "relocate to
///      end-of-document" gets typed into existence by accident.
///   2. `Collapsed` is a distinct state from `Resolved`. A boundary that moved
///      because the text around it moved has to say so; collapsing into
///      `Resolved` is exactly the silent relocation WP2 exists to detect.
///
/// What W01 does **not** decide, deliberately: the offset a strict attachment
/// collapses to when its referent is deleted. That rule is `design_pending`,
/// owned by S04a, and it is why `Collapsed` carries no implied placement here.

#ifndef COLLABTEXT_CONSUMER_PROBES_PROBE_CONTRACT_H
#define COLLABTEXT_CONSUMER_PROBES_PROBE_CONTRACT_H

#include <cstdint>
#include <optional>
#include <string_view>

namespace collabtext::consumer {

/// The eight consumer probes. These labels are ours; the consumer numbers its
/// own probes 1-8. The mapping is fixed by vnext-consumer-probes.md §0 and must
/// not be renumbered, because W02/W03/E05 result matrices name cases by it.
enum class ProbeId : std::uint8_t {
    WP1_AffinityUnderInsertion,
    WP2_DeletedReferent,
    WP3_DeletedContainingBlock,
    WP4_ScalarBoundaryMinting,
    WP5_TokenRoundTrip,
    WP6_SplitPasteInterleave,
    WP7_WrongContextLookup,
    WP8_RetentionAndPins,
};

constexpr std::string_view probe_name(ProbeId id) noexcept {
    switch (id) {
    case ProbeId::WP1_AffinityUnderInsertion: return "WP1";
    case ProbeId::WP2_DeletedReferent: return "WP2";
    case ProbeId::WP3_DeletedContainingBlock: return "WP3";
    case ProbeId::WP4_ScalarBoundaryMinting: return "WP4";
    case ProbeId::WP5_TokenRoundTrip: return "WP5";
    case ProbeId::WP6_SplitPasteInterleave: return "WP6";
    case ProbeId::WP7_WrongContextLookup: return "WP7";
    case ProbeId::WP8_RetentionAndPins: return "WP8";
    }
    return "WP?";
}

/// Offset units are not interchangeable. A list index reinterpreted as a byte
/// offset is a silent corruption, and Qt's UTF-16 positions are a third unit
/// again (W2).
enum class OffsetUnit : std::uint8_t { Utf8Bytes, ListIndex };

/// What a consumer attached the token to. This distinction drives WP2: a caret
/// may survive losing a neighbour, a strict content attachment may not, and
/// inferring universal failure from every deletion is explicitly forbidden.
enum class AttachmentKind : std::uint8_t { Caret, Strict };

/// How a boundary resolved. `Gone` and `Stale` are the consumer's own names
/// (wave 0001-protocol:666-667) and are kept verbatim so the mapping into a
/// consumer-visible state is a switch rather than a reinterpretation.
enum class PositionState : std::uint8_t {
    Resolved,    ///< an offset, and the boundary still means what it meant
    Collapsed,   ///< an offset, but the referent moved or was deleted
    Detached,    ///< a strict attachment lost its referent; no offset is implied
    Stale,       ///< token no longer resolves in a live scope; original bytes kept
    Gone,        ///< the containing block is gone (a consumer-owned check)
    Unavailable, ///< required history/retention is absent
};

/// Typed rejection codes. These are the codes the fixtures name in
/// `invalid_requests[].error`; a run that produces a different spelling is a
/// runner bug, not a passing case.
enum class ProbeError : std::uint8_t {
    None,
    WrongContext,        ///< wrong document, stream or block
    WrongPrimitive,      ///< text token resolved against a list, or the reverse
    WrongEpoch,          ///< token minted in another epoch without translation
    OffEnd,              ///< mint offset past the end
    NotScalarBoundary,   ///< offset inside a multi-byte scalar
    NotFound,            ///< list id lookup miss
    UnsupportedVersion,  ///< token version this build does not know
    Malformed,           ///< bad enum, truncated field, unsupported affinity
    TrailingData,        ///< valid token bytes followed by extra bytes
    Truncated,           ///< token bytes cut short
    HistoryUnavailable,  ///< below the supported history floor
};

constexpr std::string_view error_name(ProbeError e) noexcept {
    switch (e) {
    case ProbeError::None: return "none";
    case ProbeError::WrongContext: return "wrong_context";
    case ProbeError::WrongPrimitive: return "wrong_primitive";
    case ProbeError::WrongEpoch: return "wrong_epoch";
    case ProbeError::OffEnd: return "off_end";
    case ProbeError::NotScalarBoundary: return "not_scalar_boundary";
    case ProbeError::NotFound: return "not_found";
    case ProbeError::UnsupportedVersion: return "unsupported_version";
    case ProbeError::Malformed: return "malformed";
    case ProbeError::TrailingData: return "trailing_data";
    case ProbeError::Truncated: return "truncated";
    case ProbeError::HistoryUnavailable: return "history_unavailable";
    }
    return "unknown_error";
}

/// Machine-readable outcome vocabulary (vnext-consumer-probes.md §1).
///
/// `KnownFailure` and `Unsupported` are how an honest baseline stays green, and
/// neither counts as evidence for a feature. `Error` covers an abort, signal,
/// assertion failure, unexpected mutation, or a zero-case selection, and is
/// always fatal.
enum class ProbeOutcome : std::uint8_t { Pass, KnownFailure, Unsupported, Error };

/// A boundary resolution: an offset **or** a typed error, never both.
///
/// A failure state must also say *why*. A default-constructed resolution is
/// `Unavailable` with no error and is deliberately **not** well formed, because a
/// value that reads as "a position was produced" before anything ran is exactly
/// the shape that reports success after failing.
struct PositionResolution {
    PositionState state = PositionState::Unavailable;
    std::optional<std::uint32_t> offset;
    ProbeError error = ProbeError::None;

    [[nodiscard]] bool well_formed() const noexcept {
        const bool has_offset = offset.has_value();
        const bool has_error = error != ProbeError::None;
        if (has_offset == has_error) return false; // both, or neither
        switch (state) {
        case PositionState::Resolved:
        case PositionState::Collapsed:
            return has_offset;
        case PositionState::Detached:
        case PositionState::Stale:
        case PositionState::Gone:
        case PositionState::Unavailable:
            // A typed loss, and no fabricated placement to paper over it.
            return has_error;
        }
        return false;
    }
};

/// One recorded case. W02 fills these in; W03 promotes the strict subset.
struct ProbeCase {
    ProbeId probe = ProbeId::WP1_AffinityUnderInsertion;
    std::string_view fixture;      ///< fixture id under fixtures/external-position/
    std::string_view case_id;      ///< the named expectation inside that fixture
    ProbeOutcome outcome = ProbeOutcome::Error;
    PositionResolution observed;
    /// Any abort/signal from an isolated worker. Non-zero is an `Error`, and a
    /// worker that was skipped is never a pass.
    int worker_exit = 0;
    bool worker_signalled = false;
};

} // namespace collabtext::consumer

#endif // COLLABTEXT_CONSUMER_PROBES_PROBE_CONTRACT_H