#pragma once

#include "crdt/Operations.h"

#include <optional>
#include <string>
#include <string_view>

namespace CollabText::Crdt {

/// Serialize an Operation to a single-line JSON string (no trailing newline).
std::string encode_operation(const Operation& op);

/// Deserialize a single-line JSON string to an Operation.
/// Returns std::nullopt on parse failure.
std::optional<Operation> decode_operation(std::string_view json);

/// Serialize a Global (vector clock) to JSON.
std::string encode_global(const Global& g);

/// Deserialize a Global from JSON.
std::optional<Global> decode_global(std::string_view json);

} // namespace CollabText::Crdt
