#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CollabText::Crdt {

enum class SegmentKind : uint8_t {
    Ops    = 0x01,
    Stream = 0x02,
};

struct SegmentHeader {
    uint8_t  format_version = 1;
    SegmentKind kind        = SegmentKind::Ops;
    uint16_t flags          = 0;
    uint64_t first_lamport  = 0;
    uint64_t last_lamport   = 0;
    uint32_t record_count   = 0;
    std::array<uint8_t, 32> sha256{};
};

constexpr size_t kSegmentHeaderBytes = 4 + 1 + 1 + 2 + 8 + 8 + 4 + 32; // = 60

std::string encode_segment_header(const SegmentHeader& h);
std::optional<SegmentHeader> decode_segment_header(std::string_view bytes);

std::string encode_sealed_segment(const SegmentHeader& header_skeleton,
                                  const std::vector<std::string>& records,
                                  int zstd_level = 3);

struct SealedSegment {
    SegmentHeader header;
    std::vector<std::string> records;
};

std::optional<SealedSegment> decode_sealed_segment(std::string_view sealed_bytes);

std::array<uint8_t, 32> sha256_bytes(std::string_view input);

} // namespace CollabText::Crdt
