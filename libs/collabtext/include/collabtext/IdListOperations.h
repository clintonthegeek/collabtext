/// IdListOperations.h — Public IdList CRDT operation types for collabtext consumers
///
/// This header is part of the stable public API surface introduced under the
/// OpStream extraction (Task 1.1).  See also:
///   docs/handoff/2026-05-08-d5-joint-design-outcomes.md  §1 "Form 2 contract"
///
/// ─── Stable contract (safe to depend on across schema_version bumps) ────────
///
///   • idlist_op_lamport(const IdListOperation& op) -> Lamport
///       Free function returning the Lamport timestamp of any IdListOperation.
///
///   • Lamport::counter() const -> uint64_t
///       Returns the logical clock value (see <collabtext/Operations.h>).
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
///   Lamport ts = idlist_op_lamport(*op);   // stable accessor
///
#pragma once

#include "crdt/Clock.h"
#include "crdt/Locator.h"

#include <cstdint>
#include <variant>
#include <vector>

namespace CollabText::Crdt {

/// Insert a new element with the given opaque id at the pre-computed locator.
/// The receiver places the element using (locator, origin/timestamp) tiebreak.
///
/// All fields other than timestamp are evolution-reserved.
struct IdListInsertOp {
    Lamport timestamp;     ///< Origin of the new element (== insertion Lamport).
    Global version;        ///< Causal dependencies. Evolution-reserved.
    uint64_t id = 0;       ///< Opaque application-owned element value. Evolution-reserved.
    Locator locator;       ///< Pre-computed fractional position. Evolution-reserved.
};

/// Remove the element identified by target_origin.
/// The receiver finds the entry by its origin Lamport and tombstones it.
///
/// All fields other than timestamp are evolution-reserved.
struct IdListRemoveOp {
    Lamport timestamp;      ///< Lamport of this deletion op (the deletion_id).
    Global version;         ///< Causal dependencies. Evolution-reserved.
    Lamport target_origin;  ///< Origin of the entry being removed. Evolution-reserved.
};

/// Undo/redo operation for IdList; mirrors Buffer's UndoOperation exactly.
///
/// All fields other than timestamp are evolution-reserved.
struct IdListUndoOpVariant {
    Lamport timestamp;
    Global version;                                        ///< Evolution-reserved.
    std::vector<std::pair<Lamport, uint32_t>> counts;     ///< Evolution-reserved.
};

/// The variant type wrapping all IdList operation kinds.
///
/// std::variant index (discriminator) is evolution-reserved.
/// Use idlist_op_lamport() to extract the Lamport timestamp in a kind-agnostic way.
using IdListOperation =
    std::variant<IdListInsertOp, IdListRemoveOp, IdListUndoOpVariant>;

/// Stable accessor: returns the Lamport timestamp of any IdListOperation.
/// This is the canonical way to identify an IdList op across schema versions.
inline Lamport idlist_op_lamport(const IdListOperation& op) {
    return std::visit([](const auto& o) -> Lamport { return o.timestamp; }, op);
}

/// Internal helper — also exposed here for code sharing with internal uses.
/// Prefer idlist_op_lamport() in consumer code.
inline Lamport get_idlist_op_timestamp(const IdListOperation& op) {
    return idlist_op_lamport(op);
}

/// Internal helper — also exposed here for code sharing with internal uses.
inline const Global& get_idlist_op_version(const IdListOperation& op) {
    return std::visit([](const auto& o) -> const Global& { return o.version; }, op);
}

} // namespace CollabText::Crdt
