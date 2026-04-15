#pragma once
#include "crdt/StreamSync.h"
#include <optional>
#include <string>
#include <string_view>

namespace CollabText::Crdt {

std::string encode_stream_entry(const StreamEntry& entry);
std::optional<StreamEntry> decode_stream_entry(std::string_view json);

} // namespace CollabText::Crdt
