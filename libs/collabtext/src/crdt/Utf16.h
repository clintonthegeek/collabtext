#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace CollabText::Crdt {

/// Convert a UTF-16 code-unit offset to a byte offset in a UTF-8 string.
/// Scans the UTF-8 bytes, counting UTF-16 code units:
///   - 4-byte UTF-8 sequences (U+10000+) = 2 UTF-16 code units (surrogate pair)
///   - All other sequences = 1 UTF-16 code unit
/// Returns the byte position corresponding to the given UTF-16 offset.
/// If utf16_offset exceeds the string length, returns utf8.size().
inline size_t utf16_to_byte_offset(std::string_view utf8, size_t utf16_offset) {
    size_t u16 = 0;
    size_t i = 0;
    while (i < utf8.size() && u16 < utf16_offset) {
        uint8_t byte = static_cast<uint8_t>(utf8[i]);
        size_t char_bytes;
        size_t u16_units;

        if (byte < 0x80) {
            char_bytes = 1;
            u16_units = 1;
        } else if ((byte & 0xE0) == 0xC0) {
            char_bytes = 2;
            u16_units = 1;
        } else if ((byte & 0xF0) == 0xE0) {
            char_bytes = 3;
            u16_units = 1;
        } else if ((byte & 0xF8) == 0xF0) {
            char_bytes = 4;
            u16_units = 2; // surrogate pair in UTF-16
        } else {
            // Invalid UTF-8 lead byte — treat as 1 byte, 1 unit
            char_bytes = 1;
            u16_units = 1;
        }

        u16 += u16_units;
        i += char_bytes;
    }
    return i;
}

/// Convert a byte offset in a UTF-8 string to a UTF-16 code-unit offset.
/// Scans the UTF-8 bytes up to byte_offset, counting UTF-16 code units.
inline size_t byte_to_utf16_offset(std::string_view utf8, size_t byte_offset) {
    size_t u16 = 0;
    size_t i = 0;
    while (i < utf8.size() && i < byte_offset) {
        uint8_t byte = static_cast<uint8_t>(utf8[i]);
        size_t char_bytes;
        size_t u16_units;

        if (byte < 0x80) {
            char_bytes = 1;
            u16_units = 1;
        } else if ((byte & 0xE0) == 0xC0) {
            char_bytes = 2;
            u16_units = 1;
        } else if ((byte & 0xF0) == 0xE0) {
            char_bytes = 3;
            u16_units = 1;
        } else if ((byte & 0xF8) == 0xF0) {
            char_bytes = 4;
            u16_units = 2;
        } else {
            char_bytes = 1;
            u16_units = 1;
        }

        u16 += u16_units;
        i += char_bytes;
    }
    return u16;
}

/// Return the total length of a UTF-8 string in UTF-16 code units.
inline size_t utf16_length(std::string_view utf8) {
    return byte_to_utf16_offset(utf8, utf8.size());
}

} // namespace CollabText::Crdt
