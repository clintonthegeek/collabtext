/// Operations.h — Public CRDT operation types for collabtext consumers
///
/// This header is part of the stable public API surface introduced under the
/// OpStream extraction (Task 1.1).  See also:
///   docs/handoff/2026-05-08-d5-joint-design-outcomes.md  §1 "Form 2 contract"
///
/// ─── Stable contract (safe to depend on across schema_version bumps) ────────
///
///   • Identity::actor / Identity::counter
///       The two halves of an operation identity: a persistent 128-bit ActorId
///       and a checked uint64_t counter, compared counter-first
///       (docs/specs/vnext-identity.md §1–§2).  Both are public fields, and
///       both are stable, because a consumer that cannot read the identity of
///       an operation it received cannot deduplicate it.
///
///   • op_identity(const Operation& op) -> Identity
///       Free function returning the identity of any Operation.
///
///   • Identity::valid_reachable() is NOT part of the contract: whether an
///       identity is acceptable is the receiver's decision, made by P05's
///       validation, not something a consumer should infer from the bytes.
///
///   • encode_operation / decode_operation  (in <collabtext/Serialization.h>)
///       Round-trip serialisation is the stable wire contract.
///       decode_operation returns std::nullopt on schema mismatch; consumers
///       MUST warn-and-skip rather than treating nullopt as fatal.
///
/// ─── Superseded ─────────────────────────────────────────────────────────────
///
///   The previous contract exposed `Lamport::counter()`, the public
///   `Lamport::replica_id` field, and `op_lamport()`.  All three are gone: a
///   16-bit replica id is not a persistent identity, it is a number a process
///   happened to be constructed with, so two devices could be handed the same
///   one.  This is a deliberate break, not an oversight — there is no production
///   data to migrate and no compatibility shim is planned.
///
/// ─── Evolution-reserved (DO NOT depend on) ──────────────────────────────────
///
///   • Field layout of EditOperation / UndoOperation (not carved out above).
///   • Op-variant discriminator values (std::variant index).
///   • Internal types transitively visible through this header:
///       Fragment, Anchor, Locator, Global — their fields are NOT stable.
///   • Public construction APIs.  Consumers obtain ops only via decode_* or
///     via setOnLocalOp callbacks.  No stable constructors are provided;
///     construct ops only inside the engine.
///
/// ─── Usage ───────────────────────────────────────────────────────────────────
///
///   #include <collabtext/Operations.h>
///   #include <collabtext/Serialization.h>
///
///   // Receiving an op from the network:
///   auto op = decode_operation(wire_bytes);
///   if (!op) { warn("skipping unknown op"); continue; }
///   Identity id = op_identity(*op);   // stable accessor
///
#pragma once

#include <crdt/Clock.h>
#include <crdt/Locator.h>

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
    Identity timestamp;       ///< Operation identity of the edit.
    Global version;           ///< Causal dependencies (version vector). Evolution-reserved.
    std::vector<std::pair<uint32_t, uint32_t>> ranges;  ///< (start, end) byte offsets in visible text. Evolution-reserved.
    std::vector<std::string> new_text;                   ///< One replacement string per range. Evolution-reserved.

    struct InsertedFragment {
        Identity origin;        ///< Item identity of this fragment's first item.
        Locator locator;      ///< Evolution-reserved.
        std::string content;
        uint32_t length;
    };
    std::vector<InsertedFragment> inserted_fragments;    ///< Evolution-reserved.

    /// A run of consecutive item identities from one actor, all deleted by the
    /// same operation. One actor per run is a property of the allocation scheme
    /// (docs/specs/vnext-identity.md §3.2): an operation's items are consecutive
    /// positions of its reserved interval, so a fragment's items are consecutive.
    struct DeletionRun {
        Identity first;
        uint32_t count;
    };
    std::vector<DeletionRun> deletion_runs;              ///< Evolution-reserved.

    Identity deletion_id;     ///< Operation identity that deleted. Evolution-reserved.

    struct SplitRelocation {
        Identity fragment_origin;
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
    Identity timestamp;
    Global version;                                       ///< Evolution-reserved.
    std::vector<std::pair<Identity, uint32_t>> counts;   ///< Evolution-reserved.
};

/// The variant type wrapping all Buffer operation kinds.
///
/// std::variant index (discriminator) is evolution-reserved.
/// Use op_identity() to extract the identity in a kind-agnostic way.
using Operation = std::variant<EditOperation, UndoOperation>;

/// Stable accessor: returns the identity of any Operation.
/// This is the canonical way to identify an op across schema versions.
inline Identity op_identity(const Operation& op) {
    return std::visit([](const auto& o) -> Identity { return o.timestamp; }, op);
}

} // namespace CollabText::Crdt
