/// IdListOperations.h — Public IdList CRDT operation types for collabtext consumers
///
/// This header is part of the stable public API surface introduced under the
/// OpStream extraction (Task 1.1).  See also:
///   docs/handoff/2026-05-08-d5-joint-design-outcomes.md  §1 "Form 2 contract"
///
/// ─── Stable contract (safe to depend on across schema_version bumps) ────────
///
///   • idlist_op_identity(const IdListOperation& op) -> Identity
///       Free function returning the identity of any IdListOperation.
///
///   • Identity::actor / Identity::counter
///       The two halves of an operation identity: a persistent 128-bit ActorId
///       and a checked uint64_t counter, compared counter-first
///       (docs/specs/vnext-identity.md §1–§2).  See <collabtext/Operations.h>,
///       which carries the same contract and the note on what it supersedes.
///
///   • encode_idlist_operation / decode_idlist_operation
///       (in <collabtext/Serialization.h>)
///       Round-trip serialisation is the stable wire contract.
///       decode_idlist_operation returns std::nullopt on schema mismatch;
///       consumers MUST warn-and-skip rather than treating nullopt as fatal.
///
/// ─── Evolution-reserved (DO NOT depend on) ──────────────────────────────────
///
///   • Field layout and member ordering of all op structs.
///   • Op-variant discriminator values (std::variant index).
///   • Internal SumTree queue types (IdListOpSummary, IdListOpEntry,
///     IdListOperationQueue) — these are in src/crdt/ and are not exported.
///   • Locator, Global fields — evolution-reserved.
///
/// ─── Usage ───────────────────────────────────────────────────────────────────
///
///   #include <collabtext/IdListOperations.h>
///   #include <collabtext/Serialization.h>
///
///   auto op = decode_idlist_operation(wire_bytes);
///   if (!op) { warn("skipping unknown op"); continue; }
///   Identity id = idlist_op_identity(*op);   // stable accessor
///
#pragma once

#include <crdt/Clock.h>
#include <crdt/Locator.h>

#include <cstdint>
#include <variant>
#include <vector>

namespace CollabText::Crdt {

/// Insert a new element with the given opaque id at the pre-computed locator.
/// The receiver places the element using (locator, origin/timestamp) tiebreak.
///
/// All fields other than timestamp are evolution-reserved.
struct IdListInsertOp {
    Identity timestamp;    ///< Origin of the new element (== this operation's identity).
    Global version;        ///< Causal dependencies. Evolution-reserved.
    uint64_t id = 0;       ///< Opaque application-owned element value. Evolution-reserved.
    Locator locator;       ///< Pre-computed fractional position. Evolution-reserved.
};

/// Remove the element identified by target_origin.
/// The receiver finds the entry by its origin Lamport and tombstones it.
///
/// All fields other than timestamp are evolution-reserved.
struct IdListRemoveOp {
    Identity timestamp;     ///< Identity of this deletion op (the deletion_id).
    Global version;         ///< Causal dependencies. Evolution-reserved.
    Identity target_origin; ///< Item identity of the entry being removed. Evolution-reserved.
};

/// Undo/redo operation for IdList; mirrors Buffer's UndoOperation exactly.
///
/// All fields other than timestamp are evolution-reserved.
struct IdListUndoOpVariant {
    Identity timestamp;
    Global version;                                        ///< Evolution-reserved.
    std::vector<std::pair<Identity, uint32_t>> counts;    ///< Evolution-reserved.
};

/// The variant type wrapping all IdList operation kinds.
///
/// std::variant index (discriminator) is evolution-reserved.
/// Use idlist_op_identity() to extract the identity in a kind-agnostic way.
using IdListOperation =
    std::variant<IdListInsertOp, IdListRemoveOp, IdListUndoOpVariant>;

/// Stable accessor: returns the identity of any IdListOperation.
/// This is the canonical way to identify an IdList op across schema versions.
inline Identity idlist_op_identity(const IdListOperation& op) {
    return std::visit([](const auto& o) -> Identity { return o.timestamp; }, op);
}

/// Internal helper — also exposed here for code sharing with internal uses.
/// Prefer idlist_op_identity() in consumer code.
inline Identity get_idlist_op_identity(const IdListOperation& op) {
    return idlist_op_identity(op);
}

/// Internal helper — also exposed here for code sharing with internal uses.
inline const Global& get_idlist_op_version(const IdListOperation& op) {
    return std::visit([](const auto& o) -> const Global& { return o.version; }, op);
}

} // namespace CollabText::Crdt
