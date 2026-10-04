#include "codec/Utf.h"

#include <cstdio>
#include <limits>

namespace CollabText::Codec {
namespace {

constexpr uint32_t kMaxScalar = 0x10FFFF;

constexpr bool is_continuation(uint8_t b) { return (b & 0xC0) == 0x80; }

/// Bytes a lead byte introduces, or 0 if it is not a legal lead byte.
///
/// Rejects 0xC0 and 0xC1 here: they can only ever begin an overlong encoding of
/// U+0000, so classifying them as leads just moves the error later.
constexpr uint8_t sequence_length(uint8_t lead) {
    if (lead < 0x80) return 1;
    if (lead < 0xC0) return 0;   // bare continuation byte
    if (lead < 0xE0) return 2;   // 0xC0/0xC1 reach here and are caught as overlong
    if (lead < 0xF0) return 3;
    if (lead < 0xF5) return 4;
    return 0;                    // 0xF5..0xFF cannot start a scalar
}

/// Bits of a lead byte that carry value: n-byte sequences carry 8-n of them.
/// Written out rather than derived, because the derived version is off by one
/// for the single-byte case, which silently maps 0x61 to 0x21.
constexpr uint32_t lead_payload_mask(uint8_t length) {
    switch (length) {
        case 1: return 0x7Fu;
        case 2: return 0x1Fu;
        case 3: return 0x0Fu;
        case 4: return 0x07u;
        default: return 0;
    }
}

/// The smallest value a sequence of `length` bytes can encode. Comparing against
/// this is what makes overlong encodings an error instead of a longer spelling.
constexpr uint32_t minimum_for_length(uint8_t length) {
    switch (length) {
        case 1: return 0;
        case 2: return 0x80;
        case 3: return 0x800;
        case 4: return 0x10000;
        default: return 0;
    }
}

/// Decode one scalar at `pos`, classifying every way it can be wrong.
/// `error.offset` is always the lead byte, including for truncation.
UtfError decode_scalar(std::string_view input, size_t pos, uint32_t& out_scalar,
                       size_t& out_bytes) {
    const uint8_t lead = static_cast<uint8_t>(input[pos]);
    const uint8_t length = sequence_length(lead);

    if (length == 0)
        return {UtfError::Kind::invalid_lead, pos, 1};

    if (input.size() - pos < length)
        return {UtfError::Kind::truncated, pos, input.size() - pos};

    uint32_t value = lead & lead_payload_mask(length);
    for (uint8_t i = 1; i < length; ++i) {
        const uint8_t b = static_cast<uint8_t>(input[pos + i]);
        if (!is_continuation(b))
            return {UtfError::Kind::bad_continuation, pos, length};
        value = (value << 6) | (b & 0x3Fu);
    }

    // Order matters. Overlong first: an overlong encoding of a surrogate is both
    // errors, and "you spelled this the long way" is the one worth reporting.
    if (value < minimum_for_length(length))
        return {UtfError::Kind::overlong, pos, length};
    if (value >= 0xD800 && value <= 0xDFFF)
        return {UtfError::Kind::surrogate, pos, length};
    if (value > kMaxScalar)
        return {UtfError::Kind::too_large, pos, length};

    out_scalar = value;
    out_bytes = length;
    return {};
}

bool is_high_surrogate(char16_t u) { return u >= 0xD800 && u <= 0xDBFF; }
bool is_low_surrogate(char16_t u) { return u >= 0xDC00 && u <= 0xDFFF; }

}  // namespace

const char* UtfError::reason() const {
    switch (kind) {
        case Kind::none: return "ok";
        case Kind::truncated: return "truncated sequence at end of input";
        case Kind::invalid_lead: return "invalid lead byte";
        case Kind::bad_continuation: return "missing continuation byte";
        case Kind::overlong: return "overlong encoding";
        case Kind::surrogate: return "encoded surrogate (U+D800..U+DFFF)";
        case Kind::too_large: return "scalar above U+10FFFF";
        case Kind::not_convertible: return "unpaired surrogate";
    }
    return "unknown";
}

std::string describe(UtfError error, std::string_view what) {
    if (error.ok()) return std::string(what) + ": ok";
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%.*s: %s at offset 0x%zx (0x%zx)",
                  static_cast<int>(what.size()), what.data(), error.reason(),
                  error.offset, error.length);
    return buffer;
}

UtfError validate_utf8(std::string_view input) {
    size_t pos = 0;
    while (pos < input.size()) {
        uint32_t scalar = 0;
        size_t bytes = 0;
        const UtfError error = decode_scalar(input, pos, scalar, bytes);
        if (!error.ok()) return error;
        pos += bytes;
    }
    return {};
}

bool is_valid_utf8(std::string_view input) {
    return validate_utf8(input).ok();
}

UtfError utf8_length(std::string_view input, size_t& out_scalars) {
    size_t pos = 0;
    size_t count = 0;
    while (pos < input.size()) {
        uint32_t scalar = 0;
        size_t bytes = 0;
        const UtfError error = decode_scalar(input, pos, scalar, bytes);
        if (!error.ok()) return error;
        pos += bytes;
        ++count;
    }
    out_scalars = count;
    return {};
}

UtfError utf16_code_unit_count(std::string_view input, size_t& out_units) {
    size_t pos = 0;
    size_t units = 0;
    while (pos < input.size()) {
        uint32_t scalar = 0;
        size_t bytes = 0;
        const UtfError error = decode_scalar(input, pos, scalar, bytes);
        if (!error.ok()) return error;
        units += scalar >= 0x10000 ? 2 : 1;
        pos += bytes;
    }
    out_units = units;
    return {};
}

bool append_scalar(std::string& out, uint32_t code_point) {
    if (code_point > kMaxScalar) return false;
    if (code_point >= 0xD800 && code_point <= 0xDFFF) return false;

    if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
    return true;
}

UtfError utf8_to_utf16(std::string_view input, std::u16string& out) {
    std::u16string built;
    built.reserve(input.size());

    size_t pos = 0;
    while (pos < input.size()) {
        uint32_t scalar = 0;
        size_t bytes = 0;
        const UtfError error = decode_scalar(input, pos, scalar, bytes);
        if (!error.ok()) return error;

        if (scalar < 0x10000) {
            built.push_back(static_cast<char16_t>(scalar));
        } else {
            const uint32_t adjusted = scalar - 0x10000;
            built.push_back(static_cast<char16_t>(0xD800 + (adjusted >> 10)));
            built.push_back(static_cast<char16_t>(0xDC00 + (adjusted & 0x3FF)));
        }
        pos += bytes;
    }

    out = std::move(built);
    return {};
}

UtfError utf16_to_utf8(std::u16string_view input, std::string& out) {
    std::string built;
    built.reserve(input.size());

    for (size_t i = 0; i < input.size(); ++i) {
        const char16_t unit = input[i];
        if (is_high_surrogate(unit)) {
            // Report at the high surrogate, not one unit later.
            if (i + 1 >= input.size() || !is_low_surrogate(input[i + 1]))
                return {UtfError::Kind::not_convertible, i, 1};
            const uint32_t scalar =
                0x10000 + ((static_cast<uint32_t>(unit - 0xD800) << 10) |
                           static_cast<uint32_t>(input[i + 1] - 0xDC00));
            append_scalar(built, scalar);
            ++i;
            continue;
        }
        if (is_low_surrogate(unit))
            return {UtfError::Kind::not_convertible, i, 1};
        append_scalar(built, unit);
    }

    out = std::move(built);
    return {};
}

}  // namespace CollabText::Codec