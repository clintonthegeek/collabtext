#include "crdt/ZstdUtil.h"

#include <zstd.h>

#include <array>
#include <cstdint>
#include <vector>

namespace CollabText::Crdt {

std::string zstd_compress(std::string_view input, int level) {
    size_t bound = ZSTD_compressBound(input.size());
    std::string out;
    out.resize(bound);
    size_t written = ZSTD_compress(out.data(), bound, input.data(), input.size(), level);
    if (ZSTD_isError(written)) return {};
    out.resize(written);
    return out;
}

std::optional<std::string> zstd_decompress(std::string_view input) {
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    if (!dctx) return std::nullopt;

    std::string out;
    constexpr size_t CHUNK = 64 * 1024;
    std::vector<char> buf(CHUNK);

    ZSTD_inBuffer in{input.data(), input.size(), 0};
    while (in.pos < in.size) {
        ZSTD_outBuffer ob{buf.data(), buf.size(), 0};
        size_t rc = ZSTD_decompressStream(dctx, &ob, &in);
        if (ZSTD_isError(rc)) {
            ZSTD_freeDCtx(dctx);
            return std::nullopt;
        }
        out.append(buf.data(), ob.pos);
        if (rc == 0 && in.pos < in.size) {
            ZSTD_freeDCtx(dctx);
            return std::nullopt;
        }
        if (rc == 0) break;
    }
    ZSTD_freeDCtx(dctx);
    return out;
}

namespace {
constexpr char B64_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::array<int8_t, 256> make_b64_decode_table() {
    std::array<int8_t, 256> t{};
    for (auto& v : t) v = -1;
    for (int i = 0; i < 64; ++i) t[(unsigned char)B64_ALPHABET[i]] = (int8_t)i;
    t[(unsigned char)'='] = -2;
    return t;
}
const auto B64_DECODE = make_b64_decode_table();
}  // namespace

std::string base64_encode(std::string_view input) {
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= input.size()) {
        uint32_t n = (uint32_t(uint8_t(input[i])) << 16)
                   | (uint32_t(uint8_t(input[i + 1])) << 8)
                   | uint32_t(uint8_t(input[i + 2]));
        out += B64_ALPHABET[(n >> 18) & 0x3F];
        out += B64_ALPHABET[(n >> 12) & 0x3F];
        out += B64_ALPHABET[(n >> 6) & 0x3F];
        out += B64_ALPHABET[n & 0x3F];
        i += 3;
    }
    if (i < input.size()) {
        uint32_t n = uint32_t(uint8_t(input[i])) << 16;
        if (i + 1 < input.size())
            n |= uint32_t(uint8_t(input[i + 1])) << 8;
        out += B64_ALPHABET[(n >> 18) & 0x3F];
        out += B64_ALPHABET[(n >> 12) & 0x3F];
        if (i + 1 < input.size()) {
            out += B64_ALPHABET[(n >> 6) & 0x3F];
            out += '=';
        } else {
            out += "==";
        }
    }
    return out;
}

std::optional<std::string> base64_decode(std::string_view input) {
    if (input.size() % 4 != 0) return std::nullopt;
    std::string out;
    out.reserve((input.size() / 4) * 3);
    for (size_t i = 0; i < input.size(); i += 4) {
        int8_t a = B64_DECODE[(unsigned char)input[i]];
        int8_t b = B64_DECODE[(unsigned char)input[i + 1]];
        int8_t c = B64_DECODE[(unsigned char)input[i + 2]];
        int8_t d = B64_DECODE[(unsigned char)input[i + 3]];
        if (a < 0 || b < 0) return std::nullopt;
        if (c == -1 || d == -1) return std::nullopt;
        uint32_t n = (uint32_t(a) << 18) | (uint32_t(b) << 12);
        out += char((n >> 16) & 0xFF);
        if (c >= 0) {
            n |= uint32_t(c) << 6;
            out += char((n >> 8) & 0xFF);
            if (d >= 0) {
                n |= uint32_t(d);
                out += char(n & 0xFF);
            }
        }
    }
    return out;
}

} // namespace CollabText::Crdt
