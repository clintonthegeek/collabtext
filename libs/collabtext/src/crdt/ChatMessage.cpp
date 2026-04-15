#include "crdt/ChatMessage.h"

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

struct ChatPayloadParser {
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
};

} // anonymous namespace

// ---- Public API ----

StreamEntry chat_message_to_entry(const ChatMessage& msg) {
    StreamEntry entry;
    entry.id         = msg.id;
    entry.replica_id = msg.replica_id;
    entry.seq        = msg.seq;
    entry.timestamp  = msg.timestamp;

    std::string payload = "{";
    payload += "\"author\":"      + escape_json(msg.author);
    payload += ",\"author_name\":" + escape_json(msg.author_name);
    payload += ",\"body\":"        + escape_json(msg.body);
    payload += "}";
    entry.payload = std::move(payload);

    return entry;
}

std::optional<ChatMessage> chat_message_from_entry(const StreamEntry& entry) {
    ChatPayloadParser p{entry.payload};
    p.skip_ws();
    if (!p.expect('{')) return std::nullopt;

    std::string author;
    std::string author_name;
    std::string body;
    bool has_author = false;
    bool has_body   = false;

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
        } else {
            if (!p.skip_value()) return std::nullopt;
        }
    }

    if (!p.expect('}')) return std::nullopt;
    if (!has_author || !has_body) return std::nullopt;

    ChatMessage msg;
    msg.id          = entry.id;
    msg.replica_id  = entry.replica_id;
    msg.seq         = entry.seq;
    msg.timestamp   = entry.timestamp;
    msg.author      = std::move(author);
    msg.author_name = std::move(author_name);
    msg.body        = std::move(body);
    return msg;
}

} // namespace CollabText::Crdt
