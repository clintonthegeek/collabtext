#include "crdt/StreamSerialization.h"

#include <charconv>
#include <cstdio>

namespace CollabText::Crdt {

// ---- JSON Encoder ----

static std::string escape_json_string(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
    return out;
}

std::string encode_stream_entry(const StreamEntry& entry) {
    std::string out = "{";
    out += "\"id\":" + escape_json_string(entry.id);
    out += ",\"r\":" + std::to_string(entry.replica_id);
    out += ",\"s\":" + std::to_string(entry.seq);
    out += ",\"ts\":" + escape_json_string(entry.timestamp);
    out += ",\"p\":" + escape_json_string(entry.payload);
    if (entry.tombstone) {
        out += ",\"t\":true";
    }
    out += '}';
    return out;
}

// ---- JSON Decoder ----

namespace {

struct StreamParser {
    std::string_view src;
    size_t pos = 0;

    bool at_end() const { return pos >= src.size(); }
    char peek() const { return at_end() ? '\0' : src[pos]; }
    char advance() { return src[pos++]; }

    void skip_ws() {
        while (!at_end() && (src[pos] == ' ' || src[pos] == '\t' ||
               src[pos] == '\n' || src[pos] == '\r'))
            ++pos;
    }

    bool expect(char c) {
        skip_ws();
        if (peek() == c) { advance(); return true; }
        return false;
    }

    std::optional<uint64_t> parse_uint64() {
        skip_ws();
        uint64_t val = 0;
        size_t start = pos;
        while (!at_end() && src[pos] >= '0' && src[pos] <= '9') {
            uint64_t digit = src[pos] - '0';
            if (val > (UINT64_MAX - digit) / 10) return std::nullopt;
            val = val * 10 + digit;
            ++pos;
        }
        if (pos == start) return std::nullopt;
        return val;
    }

    std::optional<uint16_t> parse_uint16() {
        auto v = parse_uint64();
        if (!v || *v > UINT16_MAX) return std::nullopt;
        return static_cast<uint16_t>(*v);
    }

    std::optional<std::string> parse_string() {
        skip_ws();
        if (!expect('"')) return std::nullopt;
        std::string out;
        while (!at_end()) {
            char c = advance();
            if (c == '"') return out;
            if (c == '\\') {
                if (at_end()) return std::nullopt;
                char esc = advance();
                switch (esc) {
                    case '"':  out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/'; break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        if (pos + 4 > src.size()) return std::nullopt;
                        uint16_t cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = advance();
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
                            else return std::nullopt;
                        }
                        // Encode as UTF-8
                        if (cp < 0x80) {
                            out += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            out += static_cast<char>(0xC0 | (cp >> 6));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            out += static_cast<char>(0xE0 | (cp >> 12));
                            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: return std::nullopt;
                }
            } else {
                out += c;
            }
        }
        return std::nullopt; // Unterminated string
    }

    // Skip a JSON value (for unknown keys)
    bool skip_value() {
        skip_ws();
        char c = peek();
        if (c == '"') { return parse_string().has_value(); }
        if (c == '[') {
            advance();
            int depth = 1;
            while (!at_end() && depth > 0) {
                char ch = advance();
                if (ch == '[') ++depth;
                else if (ch == ']') --depth;
                else if (ch == '"') {
                    while (!at_end()) {
                        ch = advance();
                        if (ch == '"') break;
                        if (ch == '\\' && !at_end()) advance();
                    }
                }
            }
            return depth == 0;
        }
        if (c == '{') {
            advance();
            int depth = 1;
            while (!at_end() && depth > 0) {
                char ch = advance();
                if (ch == '{') ++depth;
                else if (ch == '}') --depth;
                else if (ch == '"') {
                    while (!at_end()) {
                        ch = advance();
                        if (ch == '"') break;
                        if (ch == '\\' && !at_end()) advance();
                    }
                }
            }
            return depth == 0;
        }
        // Number, true, false, null
        while (!at_end() && peek() != ',' && peek() != '}' && peek() != ']')
            advance();
        return true;
    }

    std::optional<std::string> next_key() {
        skip_ws();
        if (peek() == '}') return std::nullopt;
        if (peek() == ',') advance();
        auto key = parse_string();
        if (!key) return std::nullopt;
        skip_ws();
        if (!expect(':')) return std::nullopt;
        return key;
    }
};

} // anonymous namespace

std::optional<StreamEntry> decode_stream_entry(std::string_view json) {
    StreamParser p{json};
    p.skip_ws();
    if (!p.expect('{')) return std::nullopt;

    StreamEntry entry;
    bool has_id = false;

    while (auto key = p.next_key()) {
        if (*key == "id") {
            auto v = p.parse_string();
            if (!v) return std::nullopt;
            entry.id = std::move(*v);
            has_id = true;
        } else if (*key == "r") {
            auto v = p.parse_uint16();
            if (!v) return std::nullopt;
            entry.replica_id = *v;
        } else if (*key == "s") {
            auto v = p.parse_uint64();
            if (!v) return std::nullopt;
            entry.seq = *v;
        } else if (*key == "ts") {
            auto v = p.parse_string();
            if (!v) return std::nullopt;
            entry.timestamp = std::move(*v);
        } else if (*key == "p") {
            auto v = p.parse_string();
            if (!v) return std::nullopt;
            entry.payload = std::move(*v);
        } else if (*key == "t") {
            // Expect literal true
            p.skip_ws();
            if (p.src.substr(p.pos, 4) == "true") {
                p.pos += 4;
                entry.tombstone = true;
            } else {
                if (!p.skip_value()) return std::nullopt;
            }
        } else {
            if (!p.skip_value()) return std::nullopt;
        }
    }

    if (!p.expect('}')) return std::nullopt;
    if (!has_id) return std::nullopt;

    return entry;
}

} // namespace CollabText::Crdt
