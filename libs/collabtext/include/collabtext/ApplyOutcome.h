/// ApplyOutcome.h — what happened when an operation was offered to an engine.
///
/// Roadmap task P05, implementing the contract fixed in
/// docs/specs/vnext-public-api.md §5 and docs/specs/vnext-protocol.md §4.
///
/// The engine used to expose `void apply_ops(...)`, which made four materially
/// different results indistinguishable to a caller:
///
///   applied     the operation is integrated
///   duplicate   already seen; identity matched, so re-delivery is a no-op
///   deferred    causal dependencies are missing; queued until they arrive
///   rejected    this can never be applied as addressed
///
/// Collapsing those into a `void` is how a document converges on one machine
/// and quietly loses work on another: a deferred operation that is treated as
/// applied is dropped, and a rejected one that is treated as applied corrupts
/// the document. The distinction is therefore part of the return type, not a
/// diagnostic.
///
/// `awaiting()` is `std::optional<Crdt::Identity>` — the engine's identity is
/// already an `ActorId` plus a checked counter, so this is no longer the old
/// replica/value pair. It is not spelled `OperationId` because the two are the
/// same shape with different roles: an `Identity` is a position in the total
/// order that breaks equal-locator ties, while an `OperationId` is what an
/// envelope claims and what a receiver checks a claim against. Unifying them is
/// P07's call, when the envelope stops being a separate layer.
///
/// Callers only ever compare or display it, so either spelling is a source-level
/// change for them and nothing more.

#pragma once

#include <collabtext/ActorId.h>   // Error, ErrorCode

#include <crdt/Clock.h>           // Identity

#include <optional>
#include <string_view>

namespace CollabText {

enum class ApplyOutcomeKind {
    applied,
    duplicate,
    deferred,
    rejected,
};

/// Cheap to copy and safe to keep, like `Error`.
class ApplyOutcome {
public:
    ApplyOutcome() = default;  ///< `applied`

    [[nodiscard]] static ApplyOutcome applied();
    [[nodiscard]] static ApplyOutcome duplicate();
    [[nodiscard]] static ApplyOutcome deferred(Crdt::Identity awaiting);
    [[nodiscard]] static ApplyOutcome rejected(Error error);

    [[nodiscard]] ApplyOutcomeKind kind() const noexcept { return m_kind; }

    /// Deferred only: the identity this operation is waiting for. Present
    /// exactly when `kind() == deferred`.
    [[nodiscard]] std::optional<Crdt::Identity> awaiting() const noexcept {
        return m_awaiting;
    }

    /// Rejected only: why. Present exactly when `kind() == rejected`.
    [[nodiscard]] std::optional<Error> error() const {
        if (!m_error) return std::nullopt;
        return *m_error;
    }

    /// True only for a real state change. `duplicate` and `deferred` are not
    /// visible changes, so a UI reconciler should do nothing for them.
    [[nodiscard]] bool is_visible_change() const noexcept {
        return m_kind == ApplyOutcomeKind::applied;
    }

    friend bool operator==(const ApplyOutcome& a, const ApplyOutcome& b) {
        return a.m_kind == b.m_kind && a.m_awaiting == b.m_awaiting &&
               a.m_error == b.m_error;
    }

private:
    ApplyOutcomeKind m_kind = ApplyOutcomeKind::applied;
    std::optional<Crdt::Identity> m_awaiting;
    std::optional<Error> m_error;
};

[[nodiscard]] std::string_view to_string(ApplyOutcomeKind kind);

}  // namespace CollabText
