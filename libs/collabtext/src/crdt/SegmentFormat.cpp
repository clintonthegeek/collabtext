#include "crdt/SegmentFormat.h"
#include "crdt/ZstdUtil.h"

#include <QCryptographicHash>

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
    QByteArray hash = QCryptographicHash::hash(
        QByteArray::fromRawData(input.data(), int(input.size())),
        QCryptographicHash::Sha256);
    std::array<uint8_t, 32> out{};
    std::memcpy(out.data(), hash.constData(), 32);
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
    h.kind = SegmentKind(uint8_t(bytes[5]));
    h.flags = read_u16_le(bytes.data() + 6);
    h.first_lamport = read_u64_le(bytes.data() + 8);
    h.last_lamport = read_u64_le(bytes.data() + 16);
    h.record_count = read_u32_le(bytes.data() + 24);
    std::memcpy(h.sha256.data(), bytes.data() + 28, 32);
    return h;
}

std::string encode_sealed_segment(const SegmentHeader& header_skeleton,
                                  const std::vector<std::string>& records,
                                  int zstd_level) {
    std::string payload;
    for (const auto& r : records) {
        payload += base64_encode(r);
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
    auto blob = zstd_decompress(sealed_bytes);
    if (!blob) return std::nullopt;
    if (blob->size() < kSegmentHeaderBytes) return std::nullopt;
    auto h = decode_segment_header(*blob);
    if (!h) return std::nullopt;
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
