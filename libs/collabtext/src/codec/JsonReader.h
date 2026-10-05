#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "codec/Utf.h"

namespace CollabText::Codec {

/// Bounds applied before any allocation.
///
/// A decoder that reads first and complains afterwards has already lost: the
/// whole point of F10 is that a malformed envelope must be rejected on bounds
/// rather than on whatever the process's memory limit happens to be. These are
/// defaults for the operation payloads collabtext exchanges, chosen to be
/// generous for real documents and small enough that a hostile 4 KiB frame
/// cannot ask for a gigabyte.
///
/// Spec: `docs/specs/vnext-protocol.md` §11.
struct JsonLimits {
    size_t max_input_bytes = 8u * 1024u * 1024u;  ///< whole document
    size_t max_nesting_depth = 32;                 ///< containers
    size_t max_elements = 100000;                  ///< total values, anywhere
    size_t max_container_elements = 100000;        ///< members of one array/object
    size_t max_text_bytes = 4u * 1024u * 1024u;    ///< one string value
    size_t max_key_bytes = 1024;                   ///< one object key
    size_t max_number_digits = 20;                 ///< significant digits
    size_t max_number_exponent = 308;              ///< absolute exponent
};

/// Why a parse failed, in the two axes a caller needs.
///
/// `syntax` distinguishes "this is not JSON" from "this is not *our* JSON",
/// because the protocol treats them differently: a syntax error is a corrupt
/// frame to quarantine, while an unknown identifier is a newer writer whose
/// bytes must be *retained* so an upgrade can read them (vnext-protocol §3).
enum class JsonErrorKind : uint8_t {
    none = 0,
    empty_input,
    unexpected_end,
    unexpected_character,
    trailing_garbage,
    invalid_escape,
    invalid_unicode_escape,
    lone_surrogate_escape,
    invalid_number,
    duplicate_key,
    unknown_identifier,      ///< not an error for the reader; an error for callers
    type_mismatch,
    missing_field,
    input_too_large,
    too_deep,
    too_many_elements,
    text_too_large,
    invalid_utf8,           ///< literal text is not well-formed UTF-8
    key_too_large,
    number_out_of_range,
};

struct JsonError {
    JsonErrorKind kind = JsonErrorKind::none;
    size_t offset = 0;    ///< absolute byte offset into the input
    size_t length = 0;
    std::string context;  ///< near the failure, for a human reading a log
    /// When `kind` is `invalid_utf8`, the underlying UTF-8 failure is kept whole
    /// rather than flattened to a string: "overlong" and "encoded surrogate" are
    /// different bugs for whoever has to fix the writer.
    UtfError utf;

    bool ok() const { return kind == JsonErrorKind::none; }
    explicit operator bool() const { return ok(); }
};

const char* to_string(JsonErrorKind kind);

/// A parsed JSON value.
///
/// Objects keep their members in an ordered container rather than a hash map,
/// because two documents that differ only in key order must decode *identically*
/// — that is a written acceptance criterion, and a hash-ordered map would make
/// it true by luck rather than by construction. Lookup is by linear scan over a
/// small member list, which is the right cost for envelopes; `JsonLimits::
/// max_container_elements` is what keeps that bounded.
class JsonValue {
public:
    enum class Type : uint8_t { null, boolean, number, string, array, object };

    JsonValue() = default;

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::null; }
    bool is_bool() const { return type_ == Type::boolean; }
    bool is_number() const { return type_ == Type::number; }
    bool is_string() const { return type_ == Type::string; }
    bool is_array() const { return type_ == Type::array; }
    bool is_object() const { return type_ == Type::object; }

    bool boolean() const { return boolean_; }
    /// The literal text of the number, exactly as it appeared. No rounding, no
    /// reformatting: `1.0` and `1` are distinguishable on the wire and a
    /// decoder that collapses them has thrown away information it will be asked
    /// to reproduce on encode.
    const std::string& number_text() const { return text_; }
    const std::string& string_value() const { return text_; }

    const std::vector<JsonValue>& elements() const { return elements_; }
    const std::vector<std::pair<std::string, JsonValue>>& members() const {
        return members_;
    }

    /// Member lookup by key, or nullptr. `found` distinguishes "absent" from
    /// "present and null" for callers that care.
    const JsonValue* find(std::string_view key, bool* found = nullptr) const;

    /// Typed accessors. Each returns false with `error` set to a
    /// `type_mismatch` naming the key, rather than throwing or asserting: a
    /// decoder whose failure mode is a crash is not a decoder.
    bool as_bool(bool& out, JsonError& error, std::string_view what = {}) const;
    bool as_uint64(uint64_t& out, JsonError& error, std::string_view what = {}) const;
    bool as_int64(int64_t& out, JsonError& error, std::string_view what = {}) const;
    bool as_double(double& out, JsonError& error, std::string_view what = {}) const;
    bool as_string(std::string_view& out, JsonError& error,
                   std::string_view what = {}) const;

private:
    friend class JsonReader;

    Type type_ = Type::null;
    bool boolean_ = false;
    std::string text_;
    std::vector<JsonValue> elements_;
    std::vector<std::pair<std::string, JsonValue>> members_;
};

/// Parse `input` completely or not at all.
///
/// "Completely" is the load-bearing word: the reader requires the whole input to
/// be consumed, so `{"t":"u"}garbage` is an error (`trailing_garbage`), which is
/// the whole of finding F10's first clause.
///
/// Strictness, all of it deliberate:
///   * duplicate object keys are rejected, not last-wins — a duplicate means two
///     writers disagreed, and silently picking one is how that becomes invisible;
///   * object keys may appear in any order, and any two documents that differ
///     only in order produce equal values;
///   * no trailing commas, no comments, no unquoted keys, no `NaN`/`Infinity`;
///   * `\u` escapes must form surrogate pairs; a lone surrogate is an error;
///   * literal text in the input must itself be valid UTF-8;
///   * every bound in `limits` is checked *before* the corresponding allocation.
class JsonReader {
public:
    explicit JsonReader(const JsonLimits& limits = JsonLimits{});

    /// Parse and return the root value. Requires complete consumption.
    bool parse(std::string_view input, JsonValue& out, JsonError& error);

    /// Identifier of the root object, for schema resolution. Empty when the root
    /// is not an object. Callers that must distinguish "newer writer" from
    /// "corrupt" should use this instead of rejecting an unknown `v` themselves.
    const std::string& root_identifier() const { return identifier_; }
    uint64_t root_version() const { return version_; }
    bool has_root_version() const { return has_version_; }

    /// Identifiers this build understands. Unknown ones parse successfully and
    /// are reported by `unsupported_version()`; the bytes are the caller's to
    /// retain.
    void set_known_identifiers(std::vector<std::string> identifiers);

    bool unsupported_version() const { return unsupported_; }
    const JsonError& error() const { return error_; }

private:
    bool parse_value(JsonValue& out, size_t depth, size_t& element_budget,
                     JsonError& error);
    bool parse_object(JsonValue& out, size_t depth, size_t& element_budget,
                      JsonError& error);
    bool parse_array(JsonValue& out, size_t depth, size_t& element_budget,
                     JsonError& error);
    bool parse_string(std::string_view raw, std::string& out, size_t start,
                      JsonError& error, size_t byte_limit, JsonErrorKind limit_kind);
    bool parse_number(std::string_view raw, JsonValue& out, size_t start,
                      JsonError& error);
    bool expect(char c, JsonError& error);
    void skip_whitespace();
    bool fail(JsonErrorKind kind, size_t offset, std::string message);
    std::string context_at(size_t offset) const;

    JsonLimits limits_;
    std::string_view input_;
    size_t pos_ = 0;
    std::string identifier_;
    uint64_t version_ = 0;
    bool has_version_ = false;
    bool unsupported_ = false;
    std::vector<std::string> known_identifiers_;
    JsonError error_;
};

/// Convenience wrapper: parse, then resolve the root identifier and version.
///
/// Returns false for syntax failures and for `type_mismatch` in the header. An
/// unknown identifier or a newer version is *not* a failure: it is reported
/// through `retained` so the caller knows to keep the bytes (R1).
bool parse_object_envelope(std::string_view input, const JsonLimits& limits,
                           JsonValue& out, JsonError& error, bool& retained);

}  // namespace CollabText::Codec