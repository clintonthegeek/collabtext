#include "crdt/SegmentFormat.h"
#include "crdt/ZstdUtil.h"
#include "core/Sha256.h"

#include <cstring>

namespace CollabText::Crdt {

namespace {
constexpr char kMagic[4] = {'C', 'T', 'S', 'G'};

void write_u16_le(std::string& s, uint16_t v) {
    s += char(v & 0xFF);
    s += char((v >> 8) & 0xFF);
}
void write_u32_le(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; ++i) s += char((v >> (i * 8)) & 0xFF);
}
void write_u64_le(std::string& s, uint64_t v) {
    for (int i = 0; i < 8; ++i) s += char((v >> (i * 8)) & 0xFF);
}
uint16_t read_u16_le(const char* p) {
    return uint16_t(uint8_t(p[0])) | (uint16_t(uint8_t(p[1])) << 8);
}
uint32_t read_u32_le(const char* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= uint32_t(uint8_t(p[i])) << (i * 8);
    return v;
}
uint64_t read_u64_le(const char* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= uint64_t(uint8_t(p[i])) << (i * 8);
    return v;
}
} // namespace

std::array<uint8_t, 32> sha256_bytes(std::string_view input) {
    // The portable Core hash rather than QCryptographicHash, so the segment
    // format builds without Qt (roadmap task A01). Byte-identical output: the
    // digest is part of the stored segment header, so a change here would
    // invalidate every existing segment file. tst_sha256 checks the
    // implementation against Qt's.
    const std::string hex = CollabText::sha256_hex(input);
    std::array<uint8_t, 32> out{};
    const auto nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
        return 0;
    };
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<uint8_t>(
            static_cast<uint8_t>(nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
    return out;
}

std::string encode_segment_header(const SegmentHeader& h) {
    std::string out;
    out.reserve(kSegmentHeaderBytes);
    out.append(kMagic, 4);
    out += char(h.format_version);
    out += char(uint8_t(h.kind));
    write_u16_le(out, h.flags);
    write_u64_le(out, h.first_lamport);
    write_u64_le(out, h.last_lamport);
    write_u32_le(out, h.record_count);
    out.append(reinterpret_cast<const char*>(h.sha256.data()), 32);
    return out;
}

std::optional<SegmentHeader> decode_segment_header(std::string_view bytes) {
    if (bytes.size() < kSegmentHeaderBytes) return std::nullopt;
    if (std::memcmp(bytes.data(), kMagic, 4) != 0) return std::nullopt;
    SegmentHeader h;
    h.format_version = uint8_t(bytes[4]);
    if (h.format_version != 1) return std::nullopt;
    const uint8_t kind = uint8_t(bytes[5]);
    if (kind != uint8_t(SegmentKind::Ops) && kind != uint8_t(SegmentKind::Stream))
        return std::nullopt;
    h.kind = SegmentKind(kind);
    h.flags = read_u16_le(bytes.data() + 6);
    if (h.flags != 0) return std::nullopt;
    h.first_lamport = read_u64_le(bytes.data() + 8);
    h.last_lamport = read_u64_le(bytes.data() + 16);
    h.record_count = read_u32_le(bytes.data() + 24);
    if (h.record_count > kMaxSegmentRecords) return std::nullopt;
    std::memcpy(h.sha256.data(), bytes.data() + 28, 32);
    return h;
}

std::string encode_sealed_segment(const SegmentHeader& header_skeleton,
                                  const std::vector<std::string>& records,
                                  int zstd_level) {
    if (records.size() > kMaxSegmentRecords) return {};
    std::string payload;
    for (const auto& r : records) {
        if (r.size() > kMaxEncodedBytes) return {};
        const auto encoded = base64_encode(r);
        if (encoded.size() > kMaxEncodedBytes - payload.size() - 1) return {};
        payload += encoded;
        payload += '\n';
    }
    SegmentHeader h = header_skeleton;
    h.record_count = uint32_t(records.size());
    h.sha256 = sha256_bytes(payload);
    std::string blob = encode_segment_header(h);
    blob += payload;
    return zstd_compress(blob, zstd_level);
}

std::optional<SealedSegment> decode_sealed_segment(std::string_view sealed_bytes) {
    auto blob = zstd_decompress(sealed_bytes, kMaxDecompressedBytes);
    if (!blob) return std::nullopt;
    if (blob->size() < kSegmentHeaderBytes) return std::nullopt;
    auto h = decode_segment_header(*blob);
    if (!h) return std::nullopt;
    if (h->record_count > kMaxSegmentRecords) return std::nullopt;
    std::string_view payload(blob->data() + kSegmentHeaderBytes,
                             blob->size() - kSegmentHeaderBytes);
    if (sha256_bytes(payload) != h->sha256) return std::nullopt;
    SealedSegment out;
    out.header = *h;
    size_t i = 0;
    while (i < payload.size()) {
        size_t nl = payload.find('\n', i);
        if (nl == std::string_view::npos) return std::nullopt;
        auto line = payload.substr(i, nl - i);
        auto decoded = base64_decode(line);
        if (!decoded) return std::nullopt;
        out.records.push_back(std::move(*decoded));
        i = nl + 1;
    }
    if (out.records.size() != h->record_count) return std::nullopt;
    return out;
}

} // namespace CollabText::Crdt
