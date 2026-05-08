/// Serialization.h — Public stable serialization API for collabtext consumers
///
/// This header is part of the stable public API surface introduced under the
/// OpStream extraction (Task 1.1).  See also:
///   docs/handoff/2026-05-08-d5-joint-design-outcomes.md  §1 "Form 2 contract"
///
/// ─── Stable contract (safe to depend on across schema_version bumps) ────────
///
///   • encode_operation(const Operation& op) -> std::string
///       Serialises a Buffer operation to a single-line JSON string (no
///       trailing newline).  The exact JSON field names and schema are
///       evolution-reserved; only the round-trip property is stable.
///
///   • decode_operation(std::string_view json) -> std::optional<Operation>
///       Deserialises JSON to an Operation.  Returns std::nullopt on parse
///       failure or schema mismatch.  Consumers MUST warn-and-skip nullopt
///       rather than treating it as fatal.
///
///   • encode_idlist_operation(const IdListOperation& op) -> std::string
///       Serialises an IdList operation to single-line JSON.
///
///   • decode_idlist_operation(std::string_view json)
///           -> std::optional<IdListOperation>
///       Deserialises JSON to an IdListOperation.  Returns std::nullopt on
///       failure.
///
///   • encode_global(const Global& g) -> std::string
///   • decode_global(std::string_view json) -> std::optional<Global>
///       Serialisation helpers for Global (version vector).  Transitively
///       included; field layout of Global is evolution-reserved.
///
/// ─── Guarantees ──────────────────────────────────────────────────────────────
///
///   Round-trip: encode then decode (or decode then encode) produces identical
///   output for the same logical value.
///
///   Schema mismatch: decode_* returns std::nullopt when the input was
///   produced by an incompatible schema_version.  Consumers should log and
///   continue, not abort.
///
#pragma once

#include <collabtext/Operations.h>
#include <collabtext/IdListOperations.h>

#include <optional>
#include <string>
#include <string_view>

namespace CollabText::Crdt {

/// Serialise a Buffer Operation to single-line JSON (no trailing newline).
std::string encode_operation(const Operation& op);

/// Deserialise single-line JSON to a Buffer Operation.
/// Returns std::nullopt on parse failure or schema mismatch.
std::optional<Operation> decode_operation(std::string_view json);

/// Serialise a Global (version vector) to JSON.
std::string encode_global(const Global& g);

/// Deserialise JSON to a Global.
std::optional<Global> decode_global(std::string_view json);

/// Serialise an IdList operation to single-line JSON.
std::string encode_idlist_operation(const IdListOperation& op);

/// Deserialise JSON to an IdListOperation.
/// Returns std::nullopt on parse failure or schema mismatch.
std::optional<IdListOperation> decode_idlist_operation(std::string_view json);

} // namespace CollabText::Crdt
