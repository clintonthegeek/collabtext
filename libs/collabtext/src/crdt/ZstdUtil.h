#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace CollabText::Crdt {

constexpr size_t kMaxDecompressedBytes = 64u * 1024u * 1024u;
constexpr size_t kMaxEncodedBytes = 64u * 1024u * 1024u;

std::string zstd_compress(std::string_view input, int level = 3);
std::optional<std::string> zstd_decompress(
    std::string_view input, size_t max_output_bytes = kMaxDecompressedBytes);

std::string base64_encode(std::string_view input);
std::optional<std::string> base64_decode(std::string_view input);

} // namespace CollabText::Crdt
