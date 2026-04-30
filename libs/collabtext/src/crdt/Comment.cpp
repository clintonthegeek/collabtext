#include "crdt/Comment.h"

#include <cstdio>

namespace CollabText::Crdt {

// ---- local JSON helpers ----

static std::string escape_json(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
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

namespace {

struct CommentPayloadParser {
    std::string_view src;
    size_t pos = 0;

    bool at_end() const { return pos >= src.size(); }
    char peek()  const { return at_end() ? '\0' : src[pos]; }
    char advance()     { return src[pos++]; }

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

    std::optional<uint64_t> parse_uint() {
        skip_ws();
        if (at_end() || peek() < '0' || peek() > '9') return std::nullopt;
        uint64_t val = 0;
        while (!at_end() && peek() >= '0' && peek() <= '9')
            val = val * 10 + (advance() - '0');
        return val;
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
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
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
                            if      (h >= '0' && h <= '9') cp |= (h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
                            else return std::nullopt;
                        }
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
        return std::nullopt; // unterminated string
    }

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
                    while (!at_end()) { ch = advance(); if (ch == '"') break; if (ch == '\\' && !at_end()) advance(); }
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
                    while (!at_end()) { ch = advance(); if (ch == '"') break; if (ch == '\\' && !at_end()) advance(); }
                }
            }
            return depth == 0;
        }
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

    std::optional<Anchor> parse_anchor_object() {
        if (!expect('{')) return std::nullopt;
        uint16_t a_rid = 0;
        uint32_t a_cv  = 0;
        Bias     a_bias = Bias::Left;
        while (auto akey = next_key()) {
            if (*akey == "r") {
                auto v = parse_uint();
                if (v) a_rid = static_cast<uint16_t>(*v);
            } else if (*akey == "s") {
                auto v = parse_uint();
                if (v) a_cv = static_cast<uint32_t>(*v);
            } else if (*akey == "b") {
                auto v = parse_string();
                if (v && *v == "right") a_bias = Bias::Right;
            } else {
                skip_value();
            }
        }
        if (!expect('}')) return std::nullopt;
        return Anchor(a_rid, a_cv, a_bias);
    }
};

} // anonymous namespace

// ---- Public API ----

StreamEntry comment_to_entry(const Comment& comment) {
    StreamEntry entry;
    entry.id         = comment.id;
    entry.replica_id = comment.replica_id;
    entry.seq        = comment.seq;
    entry.timestamp  = comment.timestamp;

    std::string payload = "{";
    payload += "\"author\":"       + escape_json(comment.author);
    payload += ",\"author_name\":" + escape_json(comment.author_name);
    payload += ",\"body\":"        + escape_json(comment.body);
    payload += ",\"resolved\":";
    payload += (comment.resolved ? "true" : "false");
    payload += ",\"range\":{";
    payload += "\"start\":{\"r\":";
    payload += std::to_string(comment.range_start.replica_id);
    payload += ",\"s\":";
    payload += std::to_string(comment.range_start.char_value);
    payload += ",\"b\":";
    payload += (comment.range_start.bias == Bias::Left) ? "\"left\"" : "\"right\"";
    payload += "},\"end\":{\"r\":";
    payload += std::to_string(comment.range_end.replica_id);
    payload += ",\"s\":";
    payload += std::to_string(comment.range_end.char_value);
    payload += ",\"b\":";
    payload += (comment.range_end.bias == Bias::Left) ? "\"left\"" : "\"right\"";
    payload += "}}";
    payload += "}";
    entry.payload = std::move(payload);

    return entry;
}

std::optional<Comment> comment_from_entry(const StreamEntry& entry) {
    CommentPayloadParser p{entry.payload};
    p.skip_ws();
    if (!p.expect('{')) return std::nullopt;

    std::string author;
    std::string author_name;
    std::string body;
    bool has_author = false;
    bool has_body   = false;
    Anchor range_start_val;
    Anchor range_end_val;
    bool has_range  = false;
    bool resolved   = false;

    while (auto key = p.next_key()) {
        if (*key == "author") {
            auto v = p.parse_string();
            if (!v) return std::nullopt;
            author     = std::move(*v);
            has_author = true;
        } else if (*key == "author_name") {
            auto v = p.parse_string();
            if (!v) return std::nullopt;
            author_name = std::move(*v);
        } else if (*key == "body") {
            auto v = p.parse_string();
            if (!v) return std::nullopt;
            body     = std::move(*v);
            has_body = true;
        } else if (*key == "range") {
            if (!p.expect('{')) { p.skip_value(); continue; }
            bool has_start = false;
            bool has_end   = false;
            while (auto rkey = p.next_key()) {
                if (*rkey == "start") {
                    auto a = p.parse_anchor_object();
                    if (a) { range_start_val = *a; has_start = true; }
                    else { p.skip_value(); }
                } else if (*rkey == "end") {
                    auto a = p.parse_anchor_object();
                    if (a) { range_end_val = *a; has_end = true; }
                    else { p.skip_value(); }
                } else {
                    if (!p.skip_value()) return std::nullopt;
                }
            }
            if (!p.expect('}')) return std::nullopt;
            if (has_start && has_end) has_range = true;
        } else if (*key == "resolved") {
            p.skip_ws();
            if (p.peek() == 't') {
                if (p.pos + 4 > p.src.size()) return std::nullopt;
                if (p.src.substr(p.pos, 4) != "true") return std::nullopt;
                p.pos += 4;
                resolved = true;
            } else if (p.peek() == 'f') {
                if (p.pos + 5 > p.src.size()) return std::nullopt;
                if (p.src.substr(p.pos, 5) != "false") return std::nullopt;
                p.pos += 5;
                resolved = false;
            } else {
                return std::nullopt;
            }
        } else {
            if (!p.skip_value()) return std::nullopt;
        }
    }

    if (!p.expect('}')) return std::nullopt;
    if (!has_author || !has_body || !has_range) return std::nullopt;

    Comment comment;
    comment.id          = entry.id;
    comment.replica_id  = entry.replica_id;
    comment.seq         = entry.seq;
    comment.timestamp   = entry.timestamp;
    comment.author      = std::move(author);
    comment.author_name = std::move(author_name);
    comment.body        = std::move(body);
    comment.range_start = range_start_val;
    comment.range_end   = range_end_val;
    comment.resolved    = resolved;
    return comment;
}

} // namespace CollabText::Crdt
