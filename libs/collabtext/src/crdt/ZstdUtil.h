#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace CollabText::Crdt {

std::string zstd_compress(std::string_view input, int level = 3);
std::optional<std::string> zstd_decompress(std::string_view input);

std::string base64_encode(std::string_view input);
std::optional<std::string> base64_decode(std::string_view input);

} // namespace CollabText::Crdt
