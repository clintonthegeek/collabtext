#include "collabtext/Identity.h"

#include <charconv>
#include <string_view>

namespace CollabText::Identity {

// ============================================================================
// JSON writer helpers
// ============================================================================

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

static std::string encode_anchor(const Crdt::Anchor &a) {
    std::string out = "{\"replica\":";
    out += std::to_string(a.replica_id);
    out += ",\"seq\":";
    out += std::to_string(a.char_value);
    out += ",\"offset\":0,\"bias\":";
    out += (a.bias == Crdt::Bias::Right) ? "\"right\"" : "\"left\"";
    out += '}';
    return out;
}

static std::string encode_version_summary(const Crdt::Global &g) {
    std::string out = "{";
    bool first = true;
    for (size_t i = 0; i < g.size(); ++i) {
        if (g[i] == 0) continue;
        if (!first) out += ',';
        out += '"';
        out += std::to_string(i);
        out += "\":";
        out += std::to_string(g[i]);
        first = false;
    }
    out += '}';
    return out;
}

// ============================================================================
// JSON writer implementations
// ============================================================================

std::string to_json(const Identity &id) {
    std::string out = "{";
    out += "\"identity_id\":" + escape_json_string(id.identity_id);
    out += ",\"display_name\":" + escape_json_string(id.display_name);
    out += ",\"status\":" + escape_json_string(id.status);
    out += ",\"bio\":" + escape_json_string(id.bio);
    out += ",\"color\":" + escape_json_string(id.color);
    out += ",\"public_key\":" + escape_json_string(id.public_key);
    out += ",\"updated\":" + escape_json_string(id.updated);
    out += '}';
    return out;
}

std::string to_json(const Presence &p) {
    std::string out = "{";
    out += "\"replica_id\":" + escape_json_string(p.replica_id);
    out += ",\"identity_id\":" + escape_json_string(p.identity_id);
    out += ",\"device_name\":" + escape_json_string(p.device_name);
    out += ",\"active\":";
    out += p.active ? "true" : "false";
    out += ",\"last_heartbeat\":" + escape_json_string(p.last_heartbeat);
    out += ",\"session_started\":" + escape_json_string(p.session_started);
    out += ",\"version_summary\":" + encode_version_summary(p.version_summary);
    out += '}';
    return out;
}

static std::string encode_cursor_pair(const CursorPair &cp) {
    std::string out = "{\"anchor\":";
    out += encode_anchor(cp.anchor);
    out += ",\"head\":";
    out += encode_anchor(cp.head);
    out += '}';
    return out;
}

static std::string encode_cursor_array(const std::vector<CursorPair> &v) {
    std::string out = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) out += ',';
        out += encode_cursor_pair(v[i]);
    }
    out += ']';
    return out;
}

std::string to_json(const EphemeralState &es) {
    std::string out = "{";
    out += "\"seq\":";
    out += std::to_string(es.seq);
    out += ",\"timestamp\":" + escape_json_string(es.timestamp);
    out += ",\"cursors\":" + encode_cursor_array(es.cursors);
    out += ",\"selections\":" + encode_cursor_array(es.selections);
    if (es.viewport_top.has_value()) {
        out += ",\"viewport_top\":" + encode_anchor(*es.viewport_top);
    } else {
        out += ",\"viewport_top\":null";
    }
    if (es.viewport_bottom.has_value()) {
        out += ",\"viewport_bottom\":" + encode_anchor(*es.viewport_bottom);
    } else {
        out += ",\"viewport_bottom\":null";
    }
    out += ",\"activity\":" + escape_json_string(es.activity);
    out += '}';
    return out;
}

// ============================================================================
// JSON reader helper
// ============================================================================

namespace {

struct JsonReader {
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

    std::optional<std::string> read_string() {
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

    std::optional<uint64_t> read_uint64() {
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

    std::optional<uint32_t> read_uint32() {
        auto v = read_uint64();
        if (!v || *v > UINT32_MAX) return std::nullopt;
        return static_cast<uint32_t>(*v);
    }

    std::optional<uint16_t> read_uint16() {
        auto v = read_uint64();
        if (!v || *v > UINT16_MAX) return std::nullopt;
        return static_cast<uint16_t>(*v);
    }

    std::optional<bool> read_bool() {
        skip_ws();
        if (pos + 4 <= src.size() && src.substr(pos, 4) == "true") {
            pos += 4; return true;
        }
        if (pos + 5 <= src.size() && src.substr(pos, 5) == "false") {
            pos += 5; return false;
        }
        return std::nullopt;
    }

    // Skip a JSON value without parsing
    bool skip_value() {
        skip_ws();
        char c = peek();
        if (c == '"') { return read_string().has_value(); }
        if (c == 'n') {
            // null
            if (pos + 4 <= src.size() && src.substr(pos, 4) == "null") {
                pos += 4; return true;
            }
            return false;
        }
        if (c == 't') {
            if (pos + 4 <= src.size() && src.substr(pos, 4) == "true") {
                pos += 4; return true;
            }
            return false;
        }
        if (c == 'f') {
            if (pos + 5 <= src.size() && src.substr(pos, 5) == "false") {
                pos += 5; return true;
            }
            return false;
        }
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
        // Number or other literal
        while (!at_end() && peek() != ',' && peek() != '}' && peek() != ']')
            advance();
        return true;
    }

    // Advance past comma and read next key in object; returns nullopt at '}'
    std::optional<std::string> next_key() {
        skip_ws();
        if (peek() == '}') return std::nullopt;
        if (peek() == ',') advance();
        auto key = read_string();
        if (!key) return std::nullopt;
        skip_ws();
        if (!expect(':')) return std::nullopt;
        return key;
    }

    // Read anchor object: {"replica":N,"seq":N,"offset":0,"bias":"left"|"right"}
    std::optional<Crdt::Anchor> read_anchor() {
        skip_ws();
        if (!expect('{')) return std::nullopt;
        Crdt::Anchor a;
        while (auto key = next_key()) {
            if (*key == "replica") {
                auto v = read_uint16();
                if (!v) return std::nullopt;
                a.replica_id = *v;
            } else if (*key == "seq") {
                auto v = read_uint32();
                if (!v) return std::nullopt;
                a.char_value = *v;
            } else if (*key == "bias") {
                auto v = read_string();
                if (!v) return std::nullopt;
                if (*v == "right") a.bias = Crdt::Bias::Right;
                else a.bias = Crdt::Bias::Left;
            } else {
                if (!skip_value()) return std::nullopt;
            }
        }
        if (!expect('}')) return std::nullopt;
        return a;
    }

    // Read version_summary object: {"0": val, "1": val, ...}
    std::optional<Crdt::Global> read_version_summary() {
        skip_ws();
        if (!expect('{')) return std::nullopt;
        Crdt::Global g;
        while (auto key = next_key()) {
            // Key is the replica index as a decimal string
            uint16_t idx = 0;
            for (char ch : *key) {
                if (ch < '0' || ch > '9') return std::nullopt;
                idx = static_cast<uint16_t>(idx * 10 + (ch - '0'));
            }
            auto v = read_uint32();
            if (!v) return std::nullopt;
            if (*v > 0) g.observe(Crdt::Lamport(idx, *v));
        }
        if (!expect('}')) return std::nullopt;
        return g;
    }

    // Read a CursorPair object: {"anchor":{...},"head":{...}}
    std::optional<CursorPair> read_cursor_pair() {
        skip_ws();
        if (!expect('{')) return std::nullopt;
        CursorPair cp;
        while (auto key = next_key()) {
            if (*key == "anchor") {
                auto v = read_anchor();
                if (!v) return std::nullopt;
                cp.anchor = *v;
            } else if (*key == "head") {
                auto v = read_anchor();
                if (!v) return std::nullopt;
                cp.head = *v;
            } else {
                if (!skip_value()) return std::nullopt;
            }
        }
        if (!expect('}')) return std::nullopt;
        return cp;
    }

    // Read array of CursorPair
    std::optional<std::vector<CursorPair>> read_cursor_array() {
        skip_ws();
        if (!expect('[')) return std::nullopt;
        std::vector<CursorPair> result;
        skip_ws();
        if (peek() != ']') {
            do {
                auto cp = read_cursor_pair();
                if (!cp) return std::nullopt;
                result.push_back(*cp);
                skip_ws();
            } while (expect(','));
        }
        if (!expect(']')) return std::nullopt;
        return result;
    }

    // Read an optional anchor: either an anchor object or null
    std::optional<std::optional<Crdt::Anchor>> read_optional_anchor() {
        skip_ws();
        if (peek() == 'n') {
            if (pos + 4 <= src.size() && src.substr(pos, 4) == "null") {
                pos += 4;
                return std::optional<Crdt::Anchor>(std::nullopt);
            }
            return std::nullopt; // parse error
        }
        auto a = read_anchor();
        if (!a) return std::nullopt;
        return std::optional<Crdt::Anchor>(*a);
    }
};

} // anonymous namespace

// ============================================================================
// JSON parser implementations
// ============================================================================

std::optional<Identity> identity_from_json(const std::string &json) {
    if (json.empty()) return std::nullopt;
    JsonReader r{json};
    r.skip_ws();
    if (!r.expect('{')) return std::nullopt;

    Identity id;
    bool has_identity_id = false;

    while (auto key = r.next_key()) {
        if (*key == "identity_id") {
            auto v = r.read_string();
            if (!v) return std::nullopt;
            id.identity_id = *v;
            has_identity_id = true;
        } else if (*key == "display_name") {
            auto v = r.read_string();
            if (!v) return std::nullopt;
            id.display_name = *v;
        } else if (*key == "status") {
            auto v = r.read_string();
            if (!v) return std::nullopt;
            id.status = *v;
        } else if (*key == "bio") {
            auto v = r.read_string();
            if (!v) return std::nullopt;
            id.bio = *v;
        } else if (*key == "color") {
            auto v = r.read_string();
            if (!v) return std::nullopt;
            id.color = *v;
        } else if (*key == "public_key") {
            auto v = r.read_string();
            if (!v) return std::nullopt;
            id.public_key = *v;
        } else if (*key == "updated") {
            auto v = r.read_string();
            if (!v) return std::nullopt;
            id.updated = *v;
        } else {
            if (!r.skip_value()) return std::nullopt;
        }
    }
    if (!r.expect('}')) return std::nullopt;
    if (!has_identity_id) return std::nullopt;
    return id;
}

std::optional<Presence> presence_from_json(const std::string &json) {
    if (json.empty()) return std::nullopt;
    JsonReader r{json};
    r.skip_ws();
    if (!r.expect('{')) return std::nullopt;

    Presence p;
    bool has_replica_id = false;

    while (auto key = r.next_key()) {
        if (*key == "replica_id") {
            auto v = r.read_string();
            if (!v) return std::nullopt;
            p.replica_id = *v;
            has_replica_id = true;
        } else if (*key == "identity_id") {
            auto v = r.read_string();
            if (!v) return std::nullopt;
            p.identity_id = *v;
        } else if (*key == "device_name") {
            auto v = r.read_string();
            if (!v) return std::nullopt;
            p.device_name = *v;
        } else if (*key == "active") {
            auto v = r.read_bool();
            if (!v) return std::nullopt;
            p.active = *v;
        } else if (*key == "last_heartbeat") {
            auto v = r.read_string();
            if (!v) return std::nullopt;
            p.last_heartbeat = *v;
        } else if (*key == "session_started") {
            auto v = r.read_string();
            if (!v) return std::nullopt;
            p.session_started = *v;
        } else if (*key == "version_summary") {
            auto v = r.read_version_summary();
            if (!v) return std::nullopt;
            p.version_summary = *v;
        } else {
            if (!r.skip_value()) return std::nullopt;
        }
    }
    if (!r.expect('}')) return std::nullopt;
    if (!has_replica_id) return std::nullopt;
    return p;
}

std::optional<EphemeralState> ephemeral_from_json(const std::string &json) {
    if (json.empty()) return std::nullopt;
    JsonReader r{json};
    r.skip_ws();
    if (!r.expect('{')) return std::nullopt;

    EphemeralState es;
    bool has_seq = false;

    while (auto key = r.next_key()) {
        if (*key == "seq") {
            auto v = r.read_uint64();
            if (!v) return std::nullopt;
            es.seq = *v;
            has_seq = true;
        } else if (*key == "timestamp") {
            auto v = r.read_string();
            if (!v) return std::nullopt;
            es.timestamp = *v;
        } else if (*key == "cursors") {
            auto v = r.read_cursor_array();
            if (!v) return std::nullopt;
            es.cursors = std::move(*v);
        } else if (*key == "selections") {
            auto v = r.read_cursor_array();
            if (!v) return std::nullopt;
            es.selections = std::move(*v);
        } else if (*key == "viewport_top") {
            auto v = r.read_optional_anchor();
            if (!v) return std::nullopt;
            es.viewport_top = *v;
        } else if (*key == "viewport_bottom") {
            auto v = r.read_optional_anchor();
            if (!v) return std::nullopt;
            es.viewport_bottom = *v;
        } else if (*key == "activity") {
            auto v = r.read_string();
            if (!v) return std::nullopt;
            es.activity = *v;
        } else {
            if (!r.skip_value()) return std::nullopt;
        }
    }
    if (!r.expect('}')) return std::nullopt;
    if (!has_seq) return std::nullopt;
    return es;
}

} // namespace CollabText::Identity
