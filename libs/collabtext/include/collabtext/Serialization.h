/// Interim public serialization API on roadmap/reliability-v2.
///
/// Current payloads are single-line JSON without an explicit payload-format
/// version. P06b replaces them with independently versioned, checked codecs.
/// Opaque operation values come from engine output or decoding; their fields
/// and layout are private.
///
/// Decoding validates bounded complete JSON syntax, duplicate keys and Unicode,
/// then maps the interim fields. Valid escaped surrogate pairs preserve scalar
/// UTF-8. A successful decode does not establish semantic/causal applicability;
/// the engine still validates before mutation.
///
/// The current optional result cannot classify unsupported versions or provide
/// typed diagnostics. Keep original failed input and block any coverage/cursor
/// advancement over its record. Never interpret failure as permission to skip.
/// P06b's remaining Result API supplies format/version/error distinctions.
///
/// Encode/decode of an accepted value preserves its logical data and canonical
/// encoder output. Only library-owned bytes are wire data; C++ layouts are not.
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
/// Returns std::nullopt on bounded JSON syntax or payload failure.
/// Retain original bytes on failure; this interim API has no typed version errors.
std::optional<Operation> decode_operation(std::string_view json);

/// Serialise a Global (version vector) to JSON.
std::string encode_global(const Global& g);

/// Deserialise JSON to a Global.
std::optional<Global> decode_global(std::string_view json);

/// Serialise an IdList operation to single-line JSON.
std::string encode_idlist_operation(const IdListOperation& op);

/// Deserialise JSON to an IdListOperation.
/// Returns std::nullopt on bounded JSON syntax or payload failure.
/// Retain original bytes on failure; this interim API has no typed version errors.
std::optional<IdListOperation> decode_idlist_operation(std::string_view json);

} // namespace CollabText::Crdt
