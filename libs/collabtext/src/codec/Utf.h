#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace CollabText::Codec {

/// Result of validating or converting one input buffer.
///
/// A single offset plus a reason, and nothing else. The offset always points at
/// the **start of the offending scalar or surrogate pair**, never at a byte
/// inside one: pointing into the middle of a four-byte sequence is technically
/// true and practically useless, because nobody can act on it. A truncated
/// sequence reports the offset of its lead byte, which is the position a fix
/// would start at.
struct UtfError {
    enum class Kind : uint8_t {
        none = 0,
        truncated,        ///< input ended inside a sequence
        invalid_lead,     ///< not a legal UTF-8 lead byte, or a bare continuation
        bad_continuation, ///< a continuation byte was expected and not found
        overlong,         ///< encoded in more bytes than necessary
        surrogate,        ///< U+D800..U+DFFF encoded as UTF-8 (CESU-8 / WTF-8)
        too_large,        ///< scalar above U+10FFFF
        not_convertible,  ///< no valid mapping from UTF-16 (lone surrogate)
    };

    Kind kind = Kind::none;
    size_t offset = 0;
    size_t length = 0;   ///< bytes the offending scalar occupies, 0 if unknown

    bool ok() const { return kind == Kind::none; }
    explicit operator bool() const { return ok(); }

    const char* reason() const;
};

/// Describe `error` for a log or a quarantine record. Offsets are absolute and
/// hex, because a JSON offset is not a line number.
std::string describe(UtfError error, std::string_view what = "utf8");

/// Validate `input` as well-formed UTF-8.
///
/// Accepts exactly the Unicode scalar range: no overlong forms, no encoded
/// surrogates (which are legal in WTF-8 and CESU-8 and are the usual way a
/// "valid" decoder smuggles unpaired surrogates through), nothing above
/// U+10FFFF, no truncated tails.
UtfError validate_utf8(std::string_view input);

/// True when every byte in `input` forms a complete, minimal, non-surrogate
/// scalar sequence. Equivalent to `validate_utf8(input).ok()`.
bool is_valid_utf8(std::string_view input);

/// Number of Unicode scalars, or an error at the first invalid sequence.
///
/// This is a validating counter: it fails on the same inputs `validate_utf8`
/// does, so a caller can size a buffer and trust the count.
UtfError utf8_length(std::string_view input, size_t& out_scalars);

/// Number of UTF-16 code units the text would occupy, counting a non-BMP scalar
/// as two. Errors on invalid UTF-8.
UtfError utf16_code_unit_count(std::string_view input, size_t& out_units);

/// Convert UTF-8 to UTF-16, rejecting anything unpaired.
///
/// On failure `out` is left untouched: a partial string is not a value, and
/// leaving one behind is how a caller ends up applying half an operation.
UtfError utf8_to_utf16(std::string_view input, std::u16string& out);

/// Convert UTF-16 to UTF-8. A high surrogate must be followed by its low
/// surrogate; an unpaired surrogate is `not_convertible` at the surrogate's own
/// index, not at the position where the pair would have ended.
UtfError utf16_to_utf8(std::u16string_view input, std::string& out);

/// Append one scalar, given as a code point, to `out` as UTF-8.
/// `code_point` must be in the scalar range; anything else appends nothing and
/// returns false. Used by the JSON reader to emit `\u` escapes without
/// duplicating the encoding table.
bool append_scalar(std::string& out, uint32_t code_point);

}  // namespace CollabText::Codec