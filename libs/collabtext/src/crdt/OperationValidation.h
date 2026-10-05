/// OperationValidation.h — semantic validation applied before any mutation.
///
/// Roadmap task P05, implementing docs/specs/vnext-protocol.md §3 step 3 and
/// §4. Finding F10 is that the engine used to accept operations it should have
/// refused, because nothing checked an operation's internal consistency before
/// acting on it: an `InsertedFragment` could claim a character count unrelated
/// to its bytes, a deletion run could wrap its interval, ranges could overlap
/// or arrive out of order, and a local edit's range/text vectors could disagree
/// in length. Each of those produced a document whose derived indexes
/// disagreed with its bytes.
///
/// The order of the checks is the contract, not an implementation detail:
///
///   1. **Validity first.** An operation that can never be applied as addressed
///      is `rejected` and quarantined, whether or not its dependencies have
///      arrived. Queuing a malformed operation would park it forever waiting
///      for a dependency that cannot make it valid.
///   2. **Then causality.** An operation that is well formed but whose
///      dependencies are missing is `deferred`, and names the identity it is
///      waiting for.
///
/// `duplicate` is decided by the engine rather than here, because it depends on
/// what this replica has already applied rather than on the operation itself.
///
/// This header is internal. The public surface is the value it produces,
/// `ApplyOutcome`.

#pragma once

#include <collabtext/ActorId.h>   // Error, Result
#include <collabtext/ApplyOutcome.h>

#include "crdt/Clock.h"
#include "crdt/IdListOperations.h"
#include "crdt/Operations.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace CollabText::Crdt {

enum class ValidationVerdict {
    valid,
    deferred,
    rejected,
};

struct ValidationResult {
    ValidationVerdict verdict = ValidationVerdict::valid;
    Error error;                       ///< meaningful when rejected
    std::optional<Identity> awaiting;   ///< meaningful when deferred

    [[nodiscard]] static ValidationResult valid();
    [[nodiscard]] static ValidationResult defer(Identity missing);
    [[nodiscard]] static ValidationResult reject(Error error);

    [[nodiscard]] bool ok() const noexcept {
        return verdict == ValidationVerdict::valid;
    }
};

/// Bounds on what a single operation may declare.
///
/// These are a denial-of-service guard, not a semantic limit: an operation
/// claiming a million deletion runs is not describing a plausible edit, and
/// walking it before rejecting it is how a malformed record turns into a hang.
/// The defaults are far above any real edit and are recorded as normative in
/// docs/specs/vnext-protocol.md §11.1 alongside the decoder's bounds.
struct ValidationLimits {
    uint32_t max_ranges = 1u << 16;
    uint32_t max_inserted_fragments = 1u << 16;
    uint32_t max_deletion_runs = 1u << 16;
    uint32_t max_split_relocations = 1u << 16;
    uint32_t max_undo_targets = 1u << 16;
};

[[nodiscard]] const ValidationLimits& default_validation_limits();

// ---------------------------------------------------------------------------
// Operation validation
// ---------------------------------------------------------------------------

/// Cheap identity sanity, run *before* the duplicate check.
///
/// This exists because of a trap in the coverage test: `Global::observed`
/// returns true for counter 0, since `get(replica) >= 0` holds for every
/// replica. An operation stamped 0 would therefore be reported as a duplicate
/// no-op forever, never refused — which is precisely the "silently accept
/// whatever arrives" behaviour F5/F10 describe. Identity has to be established
/// before "have I seen this?" can be asked meaningfully.
///
/// It checks only counters, so it is cheap enough to run on every inbound
/// record, including re-deliveries.
[[nodiscard]] ValidationResult validate_operation_identity(const Operation& op);
[[nodiscard]] ValidationResult validate_idlist_operation_identity(
    const IdListOperation& op);

/// Validate one EditOperation against what `current` has already observed.
[[nodiscard]] ValidationResult validate_edit_operation(const EditOperation& op,
                                                      const Global& current);

/// Validate one UndoOperation against what `current` has already observed.
[[nodiscard]] ValidationResult validate_undo_operation(const UndoOperation& op,
                                                       const Global& current);

/// Dispatch over the Buffer operation variant.
[[nodiscard]] ValidationResult validate_operation(const Operation& op,
                                                  const Global& current);

/// Validate one IdListOperation against what `current` has already observed.
[[nodiscard]] ValidationResult validate_idlist_operation(const IdListOperation& op,
                                                         const Global& current);

// ---------------------------------------------------------------------------
// Local edit validation
// ---------------------------------------------------------------------------

/// Validate a local edit request against the document before touching it.
///
/// Checks that the range and replacement vectors agree in length, that every
/// range is ordered (start <= end), that ranges ascend and do not overlap, and
/// that every offset lies inside the current visible text. Offsets are byte
/// offsets, and an offset one past the last byte is the legal insertion point
/// at the end.
///
/// This exists because `apply_local_edit` could only assert. In a Release build
/// the assertion vanished and a caller passing mismatched vectors read past the
/// end of `new_text`.
[[nodiscard]] Result<void> validate_local_edit(
    const std::vector<std::pair<uint32_t, uint32_t>>& ranges,
    const std::vector<std::string>& new_text,
    uint32_t visible_length);

// ---------------------------------------------------------------------------
// Deferred queue budget
// ---------------------------------------------------------------------------

/// Backpressure for the deferral queue.
///
/// vnext-protocol.md §4 requires the queue to be bounded and requires that
/// reaching the bound be *reported* rather than silently absorbed. Without a
/// bound, an operation whose dependency never arrives is retained forever,
/// which is an unbounded memory leak driven by remote input.
struct DeferredBudget {
    size_t max_operations = 4096;
    size_t max_bytes = 8u << 20;
};

/// Approximate retained size of a queued operation, for the byte budget.
[[nodiscard]] size_t estimate_operation_bytes(const Operation& op);
[[nodiscard]] size_t estimate_operation_bytes(const IdListOperation& op);

/// An operation that exceeded the deferred budget.
///
/// It is kept, with the reason, rather than dropped: a dropped operation is
/// lost work, and the caller has no way to distinguish "never arrived" from
/// "discarded under memory pressure".
template <class Op>
struct QuarantinedOp {
    Op op;
    Error reason;
};

using QuarantinedBufferOp = QuarantinedOp<Operation>;
using QuarantinedIdListOp = QuarantinedOp<IdListOperation>;

/// True when `queued_operations` / `queued_bytes` cannot accommodate one more
/// operation of `bytes` under `budget`.
[[nodiscard]] bool deferred_budget_exhausted(const DeferredBudget& budget,
                                             size_t queued_operations,
                                             size_t queued_bytes,
                                             size_t incoming_bytes);

}  // namespace CollabText::Crdt
