#include "crdt/Serialization.h"

#include <charconv>
#include <sstream>

namespace CollabText::Crdt {

// ---- JSON Encoder (fixed schema, no general-purpose library needed) ----

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

static std::string encode_lamport(const Lamport& l) {
    return "[" + std::to_string(l.replica_id) + "," + std::to_string(l.value) + "]";
}

static std::string encode_locator(const Locator& loc) {
    std::string out = "[";
    for (size_t i = 0; i < loc.digits().size(); ++i) {
        if (i > 0) out += ',';
        out += std::to_string(loc.digits()[i]);
    }
    out += ']';
    return out;
}

std::string encode_global(const Global& g) {
    std::string out = "[";
    for (size_t i = 0; i < g.size(); ++i) {
        if (i > 0) out += ',';
        out += std::to_string(g[i]);
    }
    out += ']';
    return out;
}

static std::string encode_edit(const EditOperation& op) {
    std::string out = "{\"t\":\"e\"";

    out += ",\"ts\":" + encode_lamport(op.timestamp);
    out += ",\"v\":" + encode_global(op.version);
    out += ",\"did\":" + encode_lamport(op.deletion_id);

    // Ranges
    out += ",\"r\":[";
    for (size_t i = 0; i < op.ranges.size(); ++i) {
        if (i > 0) out += ',';
        out += "[" + std::to_string(op.ranges[i].first) + ","
             + std::to_string(op.ranges[i].second) + "]";
    }
    out += ']';

    // New text
    out += ",\"nt\":[";
    for (size_t i = 0; i < op.new_text.size(); ++i) {
        if (i > 0) out += ',';
        out += escape_json_string(op.new_text[i]);
    }
    out += ']';

    // Inserted fragments
    if (!op.inserted_fragments.empty()) {
        out += ",\"if\":[";
        for (size_t i = 0; i < op.inserted_fragments.size(); ++i) {
            auto& f = op.inserted_fragments[i];
            if (i > 0) out += ',';
            out += "{\"o\":" + encode_lamport(f.origin)
                 + ",\"l\":" + encode_locator(f.locator)
                 + ",\"c\":" + escape_json_string(f.content)
                 + ",\"n\":" + std::to_string(f.length) + "}";
        }
        out += ']';
    }

    // Deletion runs
    if (!op.deletion_runs.empty()) {
        out += ",\"dr\":[";
        for (size_t i = 0; i < op.deletion_runs.size(); ++i) {
            auto& d = op.deletion_runs[i];
            if (i > 0) out += ',';
            out += "[" + std::to_string(d.replica_id) + ","
                 + std::to_string(d.start_value) + ","
                 + std::to_string(d.count) + "]";
        }
        out += ']';
    }

    // Split relocations
    if (!op.split_relocations.empty()) {
        out += ",\"sr\":[";
        for (size_t i = 0; i < op.split_relocations.size(); ++i) {
            auto& s = op.split_relocations[i];
            if (i > 0) out += ',';
            out += "{\"o\":" + encode_lamport(s.fragment_origin)
                 + ",\"s\":" + std::to_string(s.split_offset)
                 + ",\"n\":" + std::to_string(s.fragment_length)
                 + ",\"l\":" + encode_locator(s.new_locator) + "}";
        }
        out += ']';
    }

    out += '}';
    return out;
}

static std::string encode_undo(const UndoOperation& op) {
    std::string out = "{\"t\":\"u\"";
    out += ",\"ts\":" + encode_lamport(op.timestamp);
    out += ",\"v\":" + encode_global(op.version);

    out += ",\"c\":[";
    for (size_t i = 0; i < op.counts.size(); ++i) {
        if (i > 0) out += ',';
        out += "[" + encode_lamport(op.counts[i].first) + ","
             + std::to_string(op.counts[i].second) + "]";
    }
    out += "]}";
    return out;
}

std::string encode_operation(const Operation& op) {
    if (auto* e = std::get_if<EditOperation>(&op))
        return encode_edit(*e);
    if (auto* u = std::get_if<UndoOperation>(&op))
        return encode_undo(*u);
    return "{}";
}

std::string encode_idlist_operation(const IdListOperation& op) {
    return std::visit([](const auto& o) -> std::string {
        using T = std::decay_t<decltype(o)>;
        if constexpr (std::is_same_v<T, IdListInsertOp>) {
            std::string out = "{\"t\":\"il-i\"";
            out += ",\"ts\":" + encode_lamport(o.timestamp);
            out += ",\"v\":" + encode_global(o.version);
            out += ",\"id\":" + std::to_string(o.id);
            out += ",\"loc\":" + encode_locator(o.locator);
            out += '}';
            return out;
        } else if constexpr (std::is_same_v<T, IdListRemoveOp>) {
            std::string out = "{\"t\":\"il-r\"";
            out += ",\"ts\":" + encode_lamport(o.timestamp);
            out += ",\"v\":" + encode_global(o.version);
            out += ",\"to\":" + encode_lamport(o.target_origin);
            out += '}';
            return out;
        } else {
            // IdListUndoOpVariant
            std::string out = "{\"t\":\"il-u\"";
            out += ",\"ts\":" + encode_lamport(o.timestamp);
            out += ",\"v\":" + encode_global(o.version);
            out += ",\"c\":[";
            for (size_t i = 0; i < o.counts.size(); ++i) {
                if (i > 0) out += ',';
                out += '[';
                out += encode_lamport(o.counts[i].first);
                out += ',';
                out += std::to_string(o.counts[i].second);
                out += ']';
            }
            out += "]}";
            return out;
        }
    }, op);
}

// ---- JSON Decoder (minimal recursive-descent for our fixed schema) ----

namespace {

struct Parser {
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

    bool match_string(std::string_view s) {
        skip_ws();
        if (pos + s.size() > src.size()) return false;
        if (src.substr(pos, s.size()) == s) { pos += s.size(); return true; }
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

    std::optional<uint32_t> parse_uint32() {
        auto v = parse_uint64();
        if (!v || *v > UINT32_MAX) return std::nullopt;
        return static_cast<uint32_t>(*v);
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

    std::optional<Lamport> parse_lamport() {
        skip_ws();
        if (!expect('[')) return std::nullopt;
        auto rid = parse_uint16();
        if (!rid || !expect(',')) return std::nullopt;
        auto val = parse_uint32();
        if (!val || !expect(']')) return std::nullopt;
        return Lamport(*rid, *val);
    }

    std::optional<Locator> parse_locator() {
        skip_ws();
        if (!expect('[')) return std::nullopt;
        std::vector<uint64_t> digits;
        skip_ws();
        if (peek() != ']') {
            auto d = parse_uint64();
            if (!d) return std::nullopt;
            digits.push_back(*d);
            while (expect(',')) {
                d = parse_uint64();
                if (!d) return std::nullopt;
                digits.push_back(*d);
            }
        }
        if (!expect(']')) return std::nullopt;
        return Locator(std::move(digits));
    }

    std::optional<Global> parse_global() {
        skip_ws();
        if (!expect('[')) return std::nullopt;
        Global g;
        skip_ws();
        if (peek() != ']') {
            uint16_t idx = 0;
            auto v = parse_uint32();
            if (!v) return std::nullopt;
            if (*v > 0) g.observe(Lamport(idx, *v));
            ++idx;
            while (expect(',')) {
                v = parse_uint32();
                if (!v) return std::nullopt;
                if (*v > 0) g.observe(Lamport(idx, *v));
                ++idx;
            }
        }
        if (!expect(']')) return std::nullopt;
        return g;
    }

    // Skip to next key in an object (after the opening { or previous value)
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

    // Skip a JSON value (for keys we don't recognize)
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
                    // Skip string contents
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

    std::optional<EditOperation> parse_edit_body() {
        EditOperation op;
        while (auto key = next_key()) {
            if (*key == "ts") {
                auto v = parse_lamport();
                if (!v) return std::nullopt;
                op.timestamp = *v;
            } else if (*key == "v") {
                auto v = parse_global();
                if (!v) return std::nullopt;
                op.version = *v;
            } else if (*key == "did") {
                auto v = parse_lamport();
                if (!v) return std::nullopt;
                op.deletion_id = *v;
            } else if (*key == "r") {
                if (!expect('[')) return std::nullopt;
                skip_ws();
                if (peek() != ']') {
                    do {
                        if (!expect('[')) return std::nullopt;
                        auto s = parse_uint32();
                        if (!s || !expect(',')) return std::nullopt;
                        auto e = parse_uint32();
                        if (!e || !expect(']')) return std::nullopt;
                        op.ranges.push_back({*s, *e});
                    } while (expect(','));
                }
                if (!expect(']')) return std::nullopt;
            } else if (*key == "nt") {
                if (!expect('[')) return std::nullopt;
                skip_ws();
                if (peek() != ']') {
                    do {
                        auto s = parse_string();
                        if (!s) return std::nullopt;
                        op.new_text.push_back(std::move(*s));
                    } while (expect(','));
                }
                if (!expect(']')) return std::nullopt;
            } else if (*key == "if") {
                if (!expect('[')) return std::nullopt;
                skip_ws();
                while (peek() != ']') {
                    if (!op.inserted_fragments.empty() && !expect(','))
                        return std::nullopt;
                    if (!expect('{')) return std::nullopt;
                    EditOperation::InsertedFragment frag;
                    while (auto fkey = next_key()) {
                        if (*fkey == "o") {
                            auto v = parse_lamport();
                            if (!v) return std::nullopt;
                            frag.origin = *v;
                        } else if (*fkey == "l") {
                            auto v = parse_locator();
                            if (!v) return std::nullopt;
                            frag.locator = *v;
                        } else if (*fkey == "c") {
                            auto v = parse_string();
                            if (!v) return std::nullopt;
                            frag.content = std::move(*v);
                        } else if (*fkey == "n") {
                            auto v = parse_uint32();
                            if (!v) return std::nullopt;
                            frag.length = *v;
                        } else {
                            if (!skip_value()) return std::nullopt;
                        }
                    }
                    if (!expect('}')) return std::nullopt;
                    op.inserted_fragments.push_back(std::move(frag));
                    skip_ws();
                }
                if (!expect(']')) return std::nullopt;
            } else if (*key == "dr") {
                if (!expect('[')) return std::nullopt;
                skip_ws();
                if (peek() != ']') {
                    do {
                        if (!expect('[')) return std::nullopt;
                        auto rid = parse_uint16();
                        if (!rid || !expect(',')) return std::nullopt;
                        auto sv = parse_uint32();
                        if (!sv || !expect(',')) return std::nullopt;
                        auto cnt = parse_uint32();
                        if (!cnt || !expect(']')) return std::nullopt;
                        op.deletion_runs.push_back({*rid, *sv, *cnt});
                    } while (expect(','));
                }
                if (!expect(']')) return std::nullopt;
            } else if (*key == "sr") {
                if (!expect('[')) return std::nullopt;
                skip_ws();
                while (peek() != ']') {
                    if (!op.split_relocations.empty() && !expect(','))
                        return std::nullopt;
                    if (!expect('{')) return std::nullopt;
                    EditOperation::SplitRelocation sr;
                    while (auto skey = next_key()) {
                        if (*skey == "o") {
                            auto v = parse_lamport();
                            if (!v) return std::nullopt;
                            sr.fragment_origin = *v;
                        } else if (*skey == "s") {
                            auto v = parse_uint32();
                            if (!v) return std::nullopt;
                            sr.split_offset = *v;
                        } else if (*skey == "n") {
                            auto v = parse_uint32();
                            if (!v) return std::nullopt;
                            sr.fragment_length = *v;
                        } else if (*skey == "l") {
                            auto v = parse_locator();
                            if (!v) return std::nullopt;
                            sr.new_locator = *v;
                        } else {
                            if (!skip_value()) return std::nullopt;
                        }
                    }
                    if (!expect('}')) return std::nullopt;
                    op.split_relocations.push_back(std::move(sr));
                    skip_ws();
                }
                if (!expect(']')) return std::nullopt;
            } else {
                if (!skip_value()) return std::nullopt;
            }
        }
        return op;
    }

    std::optional<UndoOperation> parse_undo_body() {
        UndoOperation op;
        while (auto key = next_key()) {
            if (*key == "ts") {
                auto v = parse_lamport();
                if (!v) return std::nullopt;
                op.timestamp = *v;
            } else if (*key == "v") {
                auto v = parse_global();
                if (!v) return std::nullopt;
                op.version = *v;
            } else if (*key == "c") {
                if (!expect('[')) return std::nullopt;
                skip_ws();
                if (peek() != ']') {
                    do {
                        if (!expect('[')) return std::nullopt;
                        auto l = parse_lamport();
                        if (!l || !expect(',')) return std::nullopt;
                        auto cnt = parse_uint32();
                        if (!cnt || !expect(']')) return std::nullopt;
                        op.counts.push_back({*l, *cnt});
                    } while (expect(','));
                }
                if (!expect(']')) return std::nullopt;
            } else {
                if (!skip_value()) return std::nullopt;
            }
        }
        return op;
    }

    std::optional<Operation> parse_operation() {
        skip_ws();
        if (!expect('{')) return std::nullopt;

        // Read "t" key first to determine type
        auto key = next_key();
        if (!key || *key != "t") return std::nullopt;
        auto type_str = parse_string();
        if (!type_str) return std::nullopt;

        if (*type_str == "e") {
            auto op = parse_edit_body();
            if (!op) return std::nullopt;
            if (!expect('}')) return std::nullopt;
            return Operation(*op);
        } else if (*type_str == "u") {
            auto op = parse_undo_body();
            if (!op) return std::nullopt;
            if (!expect('}')) return std::nullopt;
            return Operation(*op);
        }
        return std::nullopt;
    }

    std::optional<IdListInsertOp> parse_idlist_insert_body() {
        IdListInsertOp op;
        while (auto key = next_key()) {
            if (*key == "ts") {
                auto v = parse_lamport(); if (!v) return std::nullopt; op.timestamp = *v;
            } else if (*key == "v") {
                auto v = parse_global(); if (!v) return std::nullopt; op.version = *v;
            } else if (*key == "id") {
                auto v = parse_uint64(); if (!v) return std::nullopt; op.id = *v;
            } else if (*key == "loc") {
                auto v = parse_locator(); if (!v) return std::nullopt; op.locator = *v;
            } else {
                if (!skip_value()) return std::nullopt;
            }
        }
        return op;
    }

    std::optional<IdListRemoveOp> parse_idlist_remove_body() {
        IdListRemoveOp op;
        while (auto key = next_key()) {
            if (*key == "ts") {
                auto v = parse_lamport(); if (!v) return std::nullopt; op.timestamp = *v;
            } else if (*key == "v") {
                auto v = parse_global(); if (!v) return std::nullopt; op.version = *v;
            } else if (*key == "to") {
                auto v = parse_lamport(); if (!v) return std::nullopt; op.target_origin = *v;
            } else {
                if (!skip_value()) return std::nullopt;
            }
        }
        return op;
    }

    std::optional<IdListUndoOpVariant> parse_idlist_undo_body() {
        IdListUndoOpVariant op;
        while (auto key = next_key()) {
            if (*key == "ts") {
                auto v = parse_lamport(); if (!v) return std::nullopt; op.timestamp = *v;
            } else if (*key == "v") {
                auto v = parse_global(); if (!v) return std::nullopt; op.version = *v;
            } else if (*key == "c") {
                if (!expect('[')) return std::nullopt;
                skip_ws();
                if (peek() != ']') {
                    do {
                        if (!expect('[')) return std::nullopt;
                        auto l = parse_lamport(); if (!l || !expect(',')) return std::nullopt;
                        auto cnt = parse_uint32(); if (!cnt || !expect(']')) return std::nullopt;
                        op.counts.push_back({*l, *cnt});
                    } while (expect(','));
                }
                if (!expect(']')) return std::nullopt;
            } else {
                if (!skip_value()) return std::nullopt;
            }
        }
        return op;
    }

    std::optional<IdListOperation> parse_idlist_operation() {
        skip_ws();
        if (!expect('{')) return std::nullopt;
        auto key = next_key();
        if (!key || *key != "t") return std::nullopt;
        auto type_str = parse_string();
        if (!type_str) return std::nullopt;

        if (*type_str == "il-i") {
            auto op = parse_idlist_insert_body();
            if (!op || !expect('}')) return std::nullopt;
            return IdListOperation{*op};
        } else if (*type_str == "il-r") {
            auto op = parse_idlist_remove_body();
            if (!op || !expect('}')) return std::nullopt;
            return IdListOperation{*op};
        } else if (*type_str == "il-u") {
            auto op = parse_idlist_undo_body();
            if (!op || !expect('}')) return std::nullopt;
            return IdListOperation{*op};
        }
        return std::nullopt;
    }
};

} // anonymous namespace

std::optional<Operation> decode_operation(std::string_view json) {
    Parser p{json};
    return p.parse_operation();
}

std::optional<IdListOperation> decode_idlist_operation(std::string_view json) {
    Parser p{json};
    return p.parse_idlist_operation();
}

std::optional<Global> decode_global(std::string_view json) {
    Parser p{json};
    return p.parse_global();
}

} // namespace CollabText::Crdt
