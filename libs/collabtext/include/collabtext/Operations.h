/// Operations.h — Public CRDT operation types for collabtext consumers
///
/// This header is part of the stable public API surface introduced under the
/// OpStream extraction (Task 1.1).  See also:
///   docs/handoff/2026-05-08-d5-joint-design-outcomes.md  §1 "Form 2 contract"
///
/// ─── Stable contract (safe to depend on across schema_version bumps) ────────
///
///   • Lamport::counter() const -> uint64_t
///       Returns the logical clock value for this timestamp.
///
///   • Lamport::replica_id (field, uint16_t)
///       The replica that produced this timestamp.  Directly accessible.
///
///   • op_lamport(const Operation& op) -> Lamport
///       Free function returning the Lamport timestamp of any Operation.
///
///   • encode_operation / decode_operation  (in <collabtext/Serialization.h>)
///       Round-trip serialisation is the stable wire contract.
///       decode_operation returns std::nullopt on schema mismatch; consumers
///       MUST warn-and-skip rather than treating nullopt as fatal.
///
/// ─── Evolution-reserved (DO NOT depend on) ──────────────────────────────────
///
///   • Field layout and member ordering of EditOperation / UndoOperation.
///   • Op-variant discriminator values (std::variant index).
///   • Internal types transitively visible through this header:
///       Fragment, Anchor, Locator, Global — their fields are NOT stable.
///   • Public construction APIs.  Consumers obtain ops only via decode_* or
///     via setOnLocalOp callbacks (Task 1.2).  No stable constructors are
///     provided; construct ops only inside the engine.
///
/// ─── Usage ───────────────────────────────────────────────────────────────────
///
///   #include <collabtext/Operations.h>
///   #include <collabtext/Serialization.h>
///
///   // Receiving an op from the network:
///   auto op = decode_operation(wire_bytes);
///   if (!op) { warn("skipping unknown op"); continue; }
///   Lamport ts = op_lamport(*op);   // stable accessor
///
#pragma once

#include "crdt/Clock.h"
#include "crdt/Locator.h"

#include <string>
#include <variant>
#include <vector>

namespace CollabText::Crdt {

/// An insert-or-replace operation produced by Buffer.
///
/// All fields are evolution-reserved except what is listed in the stable
/// contract above.  In particular, do NOT rely on the internal types
/// EditOperation::InsertedFragment::locator (Locator) or any field of Global.
struct EditOperation {
    Lamport timestamp;        ///< Lamport clock value when edit was created.
    Global version;           ///< Causal dependencies (version vector). Evolution-reserved.
    std::vector<std::pair<uint32_t, uint32_t>> ranges;  ///< (start, end) byte offsets in visible text. Evolution-reserved.
    std::vector<std::string> new_text;                   ///< One replacement string per range. Evolution-reserved.

    struct InsertedFragment {
        Lamport origin;
        Locator locator;      ///< Evolution-reserved.
        std::string content;
        uint32_t length;
    };
    std::vector<InsertedFragment> inserted_fragments;    ///< Evolution-reserved.

    struct DeletionRun {
        uint16_t replica_id;
        uint32_t start_value;
        uint32_t count;
    };
    std::vector<DeletionRun> deletion_runs;              ///< Evolution-reserved.

    Lamport deletion_id;                                 ///< Evolution-reserved.

    struct SplitRelocation {
        Lamport fragment_origin;
        uint32_t split_offset;
        uint32_t fragment_length;
        Locator new_locator;  ///< Evolution-reserved.
    };
    std::vector<SplitRelocation> split_relocations;      ///< Evolution-reserved.
};

/// An undo/redo operation produced by Buffer.
///
/// All fields are evolution-reserved except the stable contract accessors.
struct UndoOperation {
    Lamport timestamp;
    Global version;                                       ///< Evolution-reserved.
    std::vector<std::pair<Lamport, uint32_t>> counts;    ///< Evolution-reserved.
};

/// The variant type wrapping all Buffer operation kinds.
///
/// std::variant index (discriminator) is evolution-reserved.
/// Use op_lamport() to extract the Lamport timestamp in a kind-agnostic way.
using Operation = std::variant<EditOperation, UndoOperation>;

/// Stable accessor: returns the Lamport timestamp of any Operation.
/// This is the canonical way to identify an op across schema versions.
inline Lamport op_lamport(const Operation& op) {
    return std::visit([](const auto& o) -> Lamport { return o.timestamp; }, op);
}

} // namespace CollabText::Crdt
