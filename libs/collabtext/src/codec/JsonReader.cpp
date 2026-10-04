#include "codec/JsonReader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "codec/Utf.h"

namespace CollabText::Codec {
namespace {

constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }

/// RFC 8259 literal text: no control characters below 0x20 may appear unescaped,
/// because several JSON consumers silently accept them and the disagreement is
/// the bug. UTF-8 continuation bytes are of course fine, so the check is on the
/// byte value, not on ASCII-ness.
constexpr bool is_literal_text_byte(uint8_t b) { return b >= 0x20; }

/// Render one byte for inclusion in a message. Control characters become
/// `\xNN`, because these strings are single-line log fields and a raw newline
/// inside one of them breaks every log parser downstream.
std::string printable(char c) {
    const uint8_t byte = static_cast<uint8_t>(c);
    if (byte == '\\') return "\\\\";
    if (byte == '\n') return "\\n";
    if (byte == '\r') return "\\r";
    if (byte == '\t') return "\\t";
    if (byte < 0x20 || byte == 0x7F) {
        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "\\x%02x", byte);
        return buffer;
    }
    return std::string(1, c);
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parse_hex4(std::string_view raw, size_t pos, uint32_t& out) {
    if (pos + 4 > raw.size()) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) {
        const int digit = hex_value(raw[pos + i]);
        if (digit < 0) return false;
        value = (value << 4) | static_cast<uint32_t>(digit);
    }
    out = value;
    return true;
}

}  // namespace

const char* to_string(JsonErrorKind kind) {
    switch (kind) {
        case JsonErrorKind::none: return "ok";
        case JsonErrorKind::empty_input: return "empty input";
        case JsonErrorKind::unexpected_end: return "unexpected end of input";
        case JsonErrorKind::unexpected_character: return "unexpected character";
        case JsonErrorKind::trailing_garbage: return "trailing garbage after value";
        case JsonErrorKind::invalid_escape: return "invalid escape";
        case JsonErrorKind::invalid_unicode_escape: return "invalid \\u escape";
        case JsonErrorKind::lone_surrogate_escape: return "lone surrogate in \\u escape";
        case JsonErrorKind::invalid_number: return "invalid number";
        case JsonErrorKind::duplicate_key: return "duplicate object key";
        case JsonErrorKind::unknown_identifier: return "unknown identifier";
        case JsonErrorKind::type_mismatch: return "type mismatch";
        case JsonErrorKind::missing_field: return "missing required field";
        case JsonErrorKind::input_too_large: return "input exceeds max_input_bytes";
        case JsonErrorKind::too_deep: return "nesting exceeds max_nesting_depth";
        case JsonErrorKind::too_many_elements: return "element count exceeds max_elements";
        case JsonErrorKind::text_too_large: return "string exceeds max_text_bytes";
        case JsonErrorKind::invalid_utf8: return "invalid UTF-8 in literal text";
        case JsonErrorKind::key_too_large: return "object key exceeds max_key_bytes";
        case JsonErrorKind::number_out_of_range: return "number out of range";
    }
    return "unknown error";
}

// ---- JsonValue -------------------------------------------------------------

const JsonValue* JsonValue::find(std::string_view key, bool* found) const {
    for (const auto& [name, value] : members_) {
        if (name == key) {
            if (found) *found = true;
            return &value;
        }
    }
    if (found) *found = false;
    return nullptr;
}

namespace {

/// Fill a `type_mismatch` error, naming what was wanted and what turned up.
bool mismatch(const JsonValue& value, JsonError& error, std::string_view what,
              const char* wanted) {
    static const char* names[] = {"null", "boolean", "number", "string",
                                  "array", "object"};
    error.kind = JsonErrorKind::type_mismatch;
    error.length = 0;
    error.context = std::string(what) + ": wanted " + wanted + ", got " +
                     names[static_cast<size_t>(value.type())];
    return false;
}

}  // namespace

bool JsonValue::as_bool(bool& out, JsonError& error, std::string_view what) const {
    if (!is_bool()) return mismatch(*this, error, what, "boolean");
    out = boolean_;
    return true;
}

bool JsonValue::as_uint64(uint64_t& out, JsonError& error, std::string_view what) const {
    if (!is_number()) return mismatch(*this, error, what, "number");
    if (!text_.empty() && (text_.find_first_of(".eE") != std::string::npos ||
                           text_[0] == '-'))
        return mismatch(*this, error, what, "non-negative integer");
    std::size_t consumed = 0;
    unsigned long long value = 0;
    try {
        value = std::stoull(text_, &consumed);
    } catch (const std::exception&) {
        error.kind = JsonErrorKind::number_out_of_range;
        error.context = std::string(what) + ": " + text_;
        return false;
    }
    if (consumed != text_.size()) {
        error.kind = JsonErrorKind::invalid_number;
        error.context = std::string(what) + ": " + text_;
        return false;
    }
    out = static_cast<uint64_t>(value);
    return true;
}

bool JsonValue::as_int64(int64_t& out, JsonError& error, std::string_view what) const {
    if (!is_number()) return mismatch(*this, error, what, "number");
    if (text_.find_first_of(".eE") != std::string::npos)
        return mismatch(*this, error, what, "integer");
    std::size_t consumed = 0;
    long long value = 0;
    try {
        value = std::stoll(text_, &consumed);
    } catch (const std::exception&) {
        error.kind = JsonErrorKind::number_out_of_range;
        error.context = std::string(what) + ": " + text_;
        return false;
    }
    if (consumed != text_.size()) {
        error.kind = JsonErrorKind::invalid_number;
        error.context = std::string(what) + ": " + text_;
        return false;
    }
    out = static_cast<int64_t>(value);
    return true;
}

bool JsonValue::as_double(double& out, JsonError& error, std::string_view what) const {
    if (!is_number()) return mismatch(*this, error, what, "number");
    std::size_t consumed = 0;
    double value = 0.0;
    try {
        value = std::stod(text_, &consumed);
    } catch (const std::exception&) {
        error.kind = JsonErrorKind::invalid_number;
        error.context = std::string(what) + ": " + text_;
        return false;
    }
    if (consumed != text_.size() || !std::isfinite(value)) {
        error.kind = JsonErrorKind::number_out_of_range;
        error.context = std::string(what) + ": " + text_;
        return false;
    }
    out = value;
    return true;
}

bool JsonValue::as_string(std::string_view& out, JsonError& error,
                         std::string_view what) const {
    if (!is_string()) return mismatch(*this, error, what, "string");
    out = text_;
    return true;
}

// ---- JsonReader ------------------------------------------------------------

JsonReader::JsonReader(const JsonLimits& limits) : limits_(limits) {}

void JsonReader::set_known_identifiers(std::vector<std::string> identifiers) {
    known_identifiers_ = std::move(identifiers);
}

std::string JsonReader::context_at(size_t offset) const {
    const size_t start = offset > 24 ? offset - 24 : 0;
    const size_t length = std::min<size_t>(48, input_.size() - start);
    std::string text(input_.substr(start, length));
    // Keep a one-line context: these strings end up in log lines.
    for (char& c : text)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    return text;
}

bool JsonReader::fail(JsonErrorKind kind, size_t offset, std::string message) {
    error_.kind = kind;
    error_.offset = offset;
    error_.length = 1;
    error_.context = std::move(message) + " | near: " + context_at(offset);
    error_ = error_;
    return false;
}

/// JSON's insignificant whitespace, exactly four characters. Anything else that
/// looks like whitespace is not, which is why this is an explicit list and not
/// `isspace`.
void JsonReader::skip_whitespace() {
    while (pos_ < input_.size()) {
        const char c = input_[pos_];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            ++pos_;
        else
            break;
    }
}

bool JsonReader::expect(char c, JsonError& error) {
    skip_whitespace();
    if (pos_ >= input_.size()) {
        error_ = error;
        return fail(JsonErrorKind::unexpected_end, pos_,
                    std::string("expected '") + printable(c) + "'");
    }
    if (input_[pos_] != c)
        return fail(JsonErrorKind::unexpected_character, pos_,
                    std::string("expected '") + printable(c) + "'");
    ++pos_;
    return true;
}

bool JsonReader::parse(std::string_view input, JsonValue& out, JsonError& error) {
    // The output starts empty. A caller that reuses one JsonValue across parses
    // would otherwise accumulate the previous document's members, and a
    // two-field object would be reported as a duplicate key on the second call.
    out = JsonValue{};
    input_ = input;
    pos_ = 0;
    identifier_.clear();
    version_ = 0;
    has_version_ = false;
    unsupported_ = false;
    error_ = JsonError{};

    if (input_.size() > limits_.max_input_bytes) {
        fail(JsonErrorKind::input_too_large, limits_.max_input_bytes,
             "input is " + std::to_string(input_.size()) + " bytes, limit is " +
                 std::to_string(limits_.max_input_bytes));
        error = error_;
        return false;
    }
    if (input_.empty()) {
        fail(JsonErrorKind::empty_input, 0, "no input");
        error = error_;
        return false;
    }

    size_t budget = limits_.max_elements;
    JsonError local;
    if (!parse_value(out, 0, budget, local)) {
        error = error_;
        return false;
    }

    // Complete consumption, before any header interpretation. A frame that
    // carries a valid object and then garbage is not a valid object.
    // Trailing *whitespace* is insignificant, so complete consumption means "no
    // second value", not "cursor at end". `{"t":"u"}garbage` still fails.
    skip_whitespace();
    if (pos_ != input_.size()) {
        fail(JsonErrorKind::trailing_garbage, pos_,
             std::to_string(input_.size() - pos_) + " bytes unconsumed");
        error = error_;
        return false;
    }

    if (out.is_object()) {
        if (const JsonValue* id = out.find("t"))
            if (id->is_string()) identifier_ = id->string_value();
        if (const JsonValue* version = out.find("v")) {
            JsonError version_error;
            uint64_t parsed = 0;
            if (!version->as_uint64(parsed, version_error, "v")) {
                fail(JsonErrorKind::type_mismatch, 0, version_error.context);
                error = error_;
                return false;
            }
            version_ = parsed;
            has_version_ = true;
        }
        if (!known_identifiers_.empty() && !identifier_.empty()) {
            unsupported_ = std::find(known_identifiers_.begin(),
                                     known_identifiers_.end(),
                                     identifier_) == known_identifiers_.end();
        }
    }

    error = JsonError{};
    return true;
}

bool JsonReader::parse_value(JsonValue& out, size_t depth, size_t& element_budget,
                             JsonError& error) {
    if (element_budget == 0)
        return fail(JsonErrorKind::too_many_elements, pos_,
                    "element budget exhausted");
    --element_budget;

    skip_whitespace();
    if (pos_ >= input_.size()) {
        error_ = error;
        fail(JsonErrorKind::unexpected_end, pos_, "no value");
        return false;
    }

    switch (input_[pos_]) {
        case '{': return parse_object(out, depth, element_budget, error);
        case '[': return parse_array(out, depth, element_budget, error);
        case '"': {
            out.type_ = JsonValue::Type::string;
            return parse_string(input_, out.text_, pos_, error);
        }
        case 't':
            if (input_.compare(pos_, 4, "true") == 0) {
                pos_ += 4;
                out.type_ = JsonValue::Type::boolean;
                out.boolean_ = true;
                return true;
            }
            return fail(JsonErrorKind::unexpected_character, pos_, "expected 'true'");
        case 'f':
            if (input_.compare(pos_, 5, "false") == 0) {
                pos_ += 5;
                out.type_ = JsonValue::Type::boolean;
                out.boolean_ = false;
                return true;
            }
            return fail(JsonErrorKind::unexpected_character, pos_, "expected 'false'");
        case 'n':
            if (input_.compare(pos_, 4, "null") == 0) {
                pos_ += 4;
                out.type_ = JsonValue::Type::null;
                return true;
            }
            return fail(JsonErrorKind::unexpected_character, pos_, "expected 'null'");
        default:
            if (input_[pos_] == '-' || is_digit(input_[pos_]))
                return parse_number(input_, out, pos_, error);
            return fail(JsonErrorKind::unexpected_character, pos_,
                        std::string("unexpected '") + printable(input_[pos_]) + "'");
    }
}

bool JsonReader::parse_object(JsonValue& out, size_t depth, size_t& element_budget,
                              JsonError& error) {
    if (depth >= limits_.max_nesting_depth)
        return fail(JsonErrorKind::too_deep, pos_,
                    "nesting depth " + std::to_string(depth + 1) + ", limit is " +
                        std::to_string(limits_.max_nesting_depth));
    skip_whitespace();
    ++pos_;  // '{'
    out.type_ = JsonValue::Type::object;

    if (pos_ < input_.size() && input_[pos_] == '}') {
        ++pos_;
        return true;
    }

    for (;;) {
        skip_whitespace();
        if (out.members_.size() >= limits_.max_container_elements)
            return fail(JsonErrorKind::too_many_elements, pos_,
                        "object exceeds max_container_elements");

        if (pos_ >= input_.size()) {
            error_ = error;
            return fail(JsonErrorKind::unexpected_end, pos_, "unterminated object");
        }
        if (input_[pos_] != '"')
            return fail(JsonErrorKind::unexpected_character, pos_,
                        "object key must be a quoted string");

        std::string key;
        const size_t key_offset = pos_;
        if (!parse_string(input_, key, key_offset, error)) return false;
        if (key.size() > limits_.max_key_bytes)
            return fail(JsonErrorKind::key_too_large, key_offset,
                        "key is " + std::to_string(key.size()) + " bytes");

        if (!expect(':', error)) return false;
        skip_whitespace();

        JsonValue value;
        if (!parse_value(value, depth + 1, element_budget, error)) return false;

        // Duplicate keys are an error, not last-wins. Two writers that disagree
        // about a field must not be resolved by whichever one happened to be
        // serialized second.
        for (const auto& [name, existing] : out.members_) {
            if (name != key) continue;
            return fail(JsonErrorKind::duplicate_key, key_offset,
                        "duplicate key \"" + key + "\"");
        }
        out.members_.emplace_back(std::move(key), std::move(value));

        skip_whitespace();
        if (pos_ < input_.size() && input_[pos_] == ',') {
            ++pos_;
            skip_whitespace();
            continue;  // duplicate key allowed to be diagnosed on the next key
        }
        if (pos_ < input_.size() && input_[pos_] == '}') {
            ++pos_;
            return true;
        }
        if (pos_ >= input_.size()) {
            error_ = error;
            return fail(JsonErrorKind::unexpected_end, pos_, "unterminated object");
        }
        return fail(JsonErrorKind::unexpected_character, pos_,
                    "expected ',' or '}'");
    }
}

bool JsonReader::parse_array(JsonValue& out, size_t depth, size_t& element_budget,
                             JsonError& error) {
    if (depth >= limits_.max_nesting_depth)
        return fail(JsonErrorKind::too_deep, pos_,
                    "nesting depth " + std::to_string(depth + 1) + ", limit is " +
                        std::to_string(limits_.max_nesting_depth));
    skip_whitespace();
    ++pos_;  // '['
    out.type_ = JsonValue::Type::array;

    if (pos_ < input_.size() && input_[pos_] == ']') {
        ++pos_;
        return true;
    }

    for (;;) {
        skip_whitespace();
        if (out.elements_.size() >= limits_.max_container_elements)
            return fail(JsonErrorKind::too_many_elements, pos_,
                        "array exceeds max_container_elements");

        JsonValue value;
        if (!parse_value(value, depth + 1, element_budget, error)) return false;
        out.elements_.push_back(std::move(value));

        skip_whitespace();
        if (pos_ < input_.size() && input_[pos_] == ',') {
            ++pos_;
            skip_whitespace();
            if (pos_ < input_.size() && input_[pos_] == ']')
                return fail(JsonErrorKind::unexpected_character, pos_,
                            "trailing comma in array");
            continue;
        }
        if (pos_ < input_.size() && input_[pos_] == ']') {
            ++pos_;
            return true;
        }
        if (pos_ >= input_.size()) {
            error_ = error;
            return fail(JsonErrorKind::unexpected_end, pos_, "unterminated array");
        }
        return fail(JsonErrorKind::unexpected_character, pos_,
                    "expected ',' or ']'");
    }
}

bool JsonReader::parse_string(std::string_view raw, std::string& out, size_t start,
                              JsonError& error) {
    size_t pos = start;
    if (pos >= raw.size() || raw[pos] != '"') {
        error_ = error;
        return fail(JsonErrorKind::unexpected_character, pos, "expected '\"'");
    }
    ++pos;

    out.clear();
    size_t literal_start = pos;
    auto flush_literal = [&](size_t end) {
        // Literal runs are copied in one go, but validated first: a truncated
        // multi-byte sequence must be reported here rather than after the fact.
        const std::string_view run = raw.substr(literal_start, end - literal_start);
        const UtfError utf_error = validate_utf8(run);
        if (!utf_error.ok()) {
            // Rebase onto the whole document: the run starts partway into the
            // input, and an offset relative to it would be a lie.
            UtfError rebased = utf_error;
            rebased.offset = literal_start + utf_error.offset;
            error_ = error;
            error_.kind = JsonErrorKind::invalid_utf8;
            error_.offset = rebased.offset;
            error_.length = rebased.length;
            error_.utf = rebased;
            error_.context = std::string("literal text is not valid UTF-8: ") +
                             rebased.reason() + " | near: " +
                             context_at(rebased.offset);
            return false;
        }
        out.append(run);
        literal_start = end;
        return true;
    };

    // One pass over the string's bytes. Literal runs and escapes are handled in
    // sequence inside a single loop rather than in two passes, because a string
    // can be `abc\ndef\tghij` and a reader that treats everything after the
    // first escape as escapes will reject it.
    while (pos < raw.size()) {
        const uint8_t byte = static_cast<uint8_t>(raw[pos]);

        if (byte == '"') {
            if (!flush_literal(pos)) return false;
            ++pos;
            pos_ = pos;
            return true;
        }

        if (byte != '\\') {
            if (!is_literal_text_byte(byte))
                return fail(JsonErrorKind::unexpected_character, pos,
                            "raw control character in string");
            ++pos;
            continue;
        }

        // An escape starts here: commit the literal run before it, so that the
        // bytes the escape occupies are never copied a second time.
        if (!flush_literal(pos)) return false;

        const size_t escape_offset = pos;
        if (pos + 1 >= raw.size()) {
            error_ = error;
            return fail(JsonErrorKind::invalid_escape, pos, "escape at end of input");
        }
        const char kind = raw[pos + 1];
        pos += 2;

        switch (kind) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                uint32_t unit = 0;
                if (!parse_hex4(raw, pos, unit)) {
                    error_ = error;
                    return fail(JsonErrorKind::invalid_unicode_escape, escape_offset,
                                "\\u needs four hex digits");
                }
                pos += 4;

                if (unit >= 0xD800 && unit <= 0xDFFF) {
                    // A surrogate is only meaningful as a pair, and the failure
                    // belongs at the high surrogate's own escape rather than
                    // wherever the pair would have ended.
                    if (unit >= 0xDC00) {
                        error_ = error;
                        return fail(JsonErrorKind::lone_surrogate_escape, escape_offset,
                                    "low surrogate without a pair");
                    }
                    if (pos + 6 > raw.size() || raw[pos] != '\\' || raw[pos + 1] != 'u') {
                        error_ = error;
                        return fail(JsonErrorKind::lone_surrogate_escape, escape_offset,
                                    "high surrogate without a low surrogate");
                    }
                    uint32_t low = 0;
                    if (!parse_hex4(raw, pos + 2, low)) {
                        error_ = error;
                        return fail(JsonErrorKind::invalid_unicode_escape, pos,
                                    "\\u needs four hex digits");
                    }
                    if (low < 0xDC00 || low > 0xDFFF) {
                        error_ = error;
                        return fail(JsonErrorKind::lone_surrogate_escape, escape_offset,
                                    "high surrogate not followed by a low surrogate");
                    }
                    pos += 6;
                    const uint32_t scalar =
                        0x10000 + (((unit - 0xD800u) << 10) | (low - 0xDC00u));
                    if (!append_scalar(out, scalar)) {
                        error_ = error;
                        return fail(JsonErrorKind::lone_surrogate_escape, escape_offset,
                                    "surrogate pair did not decode");
                    }
                    break;
                }
                if (!append_scalar(out, unit)) {
                    error_ = error;
                    return fail(JsonErrorKind::invalid_unicode_escape, escape_offset,
                                "escape is not a scalar value");
                }
                break;
            }
            default:
                error_ = error;
                return fail(JsonErrorKind::invalid_escape, escape_offset,
                            std::string("unknown escape \\") + printable(kind));
        }

        // Resume the literal scan immediately after the escape, so both
        // "a\tb\nc" and "\u0041tail" work.
        literal_start = pos;

        if (out.size() > limits_.max_text_bytes) {
            error_ = error;
            return fail(JsonErrorKind::text_too_large, start,
                        "string is " + std::to_string(out.size()) +
                            " bytes, limit is " +
                            std::to_string(limits_.max_text_bytes));
        }
    }

    error_ = error;
    return fail(JsonErrorKind::unexpected_end, pos, "unterminated string");
}

bool JsonReader::parse_number(std::string_view raw, JsonValue& out, size_t start,
                              JsonError& error) {
    size_t pos = start;
    if (pos < raw.size() && raw[pos] == '-') ++pos;

    const size_t int_start = pos;
    while (pos < raw.size() && is_digit(raw[pos])) ++pos;
    if (pos == int_start) {
        error_ = error;
        return fail(JsonErrorKind::invalid_number, start, "no digits after '-'");
    }
    // Leading zeros are not JSON: `01` is two tokens, not the number one.
    if (raw[int_start] == '0' && pos - int_start > 1)
        return fail(JsonErrorKind::invalid_number, int_start, "leading zero");

    size_t significant_digits = pos - int_start;
    if (pos < raw.size() && raw[pos] == '.') {
        ++pos;
        const size_t frac_start = pos;
        while (pos < raw.size() && is_digit(raw[pos])) ++pos;
        if (pos == frac_start)
            return fail(JsonErrorKind::invalid_number, frac_start,
                        "no digits after decimal point");
        significant_digits += pos - frac_start;
    }

    if (pos < raw.size() && (raw[pos] == 'e' || raw[pos] == 'E')) {
        ++pos;
        if (pos < raw.size() && (raw[pos] == '+' || raw[pos] == '-')) ++pos;
        const size_t exp_start = pos;
        while (pos < raw.size() && is_digit(raw[pos])) ++pos;
        if (pos == exp_start)
            return fail(JsonErrorKind::invalid_number, exp_start,
                        "no digits in exponent");
        long long exponent = 0;
        try {
            exponent = std::stoll(std::string(raw.substr(exp_start, pos - exp_start)));
        } catch (const std::exception&) {
            return fail(JsonErrorKind::number_out_of_range, exp_start,
                        "exponent does not fit in 64 bits");
        }
        const long long magnitude = exponent < 0 ? -exponent : exponent;
        if (magnitude > static_cast<long long>(limits_.max_number_exponent))
            return fail(JsonErrorKind::number_out_of_range, exp_start,
                        "exponent magnitude " + std::to_string(magnitude) +
                            " exceeds max_number_exponent");
    }

    if (significant_digits > limits_.max_number_digits)
        return fail(JsonErrorKind::number_out_of_range, start,
                    "significant digits " + std::to_string(significant_digits) +
                        " exceed max_number_digits");

    out.type_ = JsonValue::Type::number;
    out.text_.assign(raw.substr(start, pos - start));
    pos_ = pos;
    return true;
}

bool parse_object_envelope(std::string_view input, const JsonLimits& limits,
                           JsonValue& out, JsonError& error, bool& retained) {
    JsonReader reader(limits);
    if (!reader.parse(input, out, error)) return false;

    if (!out.is_object()) {
        error.kind = JsonErrorKind::type_mismatch;
        error.context = "envelope root must be an object";
        return false;
    }
    if (reader.root_identifier().empty()) {
        error.kind = JsonErrorKind::missing_field;
        error.context = "envelope has no \"t\" identifier";
        return false;
    }

    // An unknown identifier or a newer version is not corruption: the bytes are
    // valid and a later build may understand them, so the caller retains them
    // rather than quarantining (vnext-protocol §3 step 2).
    retained = reader.unsupported_version();
    return true;
}

}  // namespace CollabText::Codec