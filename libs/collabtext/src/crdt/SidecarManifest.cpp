#include "crdt/SidecarManifest.h"

#include <QByteArray>
#include <QCryptographicHash>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace fs = std::filesystem;

namespace CollabText::Crdt {

std::string sha256_hex(const std::string& data) {
    QByteArray digest = QCryptographicHash::hash(
        QByteArray::fromStdString(data),
        QCryptographicHash::Sha256);
    return digest.toHex().toStdString();
}

bool doc_id_less(const std::string& a, const std::string& b) {
    return a < b;
}

// ---- minimal JSON helpers (consistent with Comment.cpp style) ----

static std::string escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
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

std::string manifest_to_json(const SidecarManifest& m) {
    std::ostringstream o;
    o << "{"
      << "\"schema_version\":" << m.schema_version
      << ",\"doc_id\":" << escape(m.doc_id)
      << ",\"enrolled_at\":" << escape(m.enrolled_at)
      << ",\"original_filename\":" << escape(m.original_filename)
      << ",\"seed_sha256\":" << escape(m.seed_sha256)
      << "}";
    return o.str();
}

namespace {

struct Parser {
    std::string_view src;
    size_t pos = 0;

    void skip_ws() {
        while (pos < src.size()
               && (src[pos] == ' ' || src[pos] == '\t'
                   || src[pos] == '\n' || src[pos] == '\r'))
            ++pos;
    }

    bool eat(char c) {
        skip_ws();
        if (pos < src.size() && src[pos] == c) { ++pos; return true; }
        return false;
    }

    std::optional<std::string> parse_string() {
        skip_ws();
        if (pos >= src.size() || src[pos] != '"') return std::nullopt;
        ++pos;
        std::string out;
        while (pos < src.size() && src[pos] != '"') {
            char c = src[pos++];
            if (c == '\\' && pos < src.size()) {
                char esc = src[pos++];
                switch (esc) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'u': {
                        if (pos + 4 > src.size()) return std::nullopt;
                        unsigned int u = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = src[pos++];
                            u <<= 4;
                            if (h >= '0' && h <= '9') u |= (h - '0');
                            else if (h >= 'a' && h <= 'f') u |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') u |= (h - 'A' + 10);
                            else return std::nullopt;
                        }
                        if (u < 0x80) {
                            out += static_cast<char>(u);
                        } else {
                            out += '?';
                        }
                        break;
                    }
                    default: return std::nullopt;
                }
            } else {
                out += c;
            }
        }
        if (pos >= src.size()) return std::nullopt;
        ++pos; // closing quote
        return out;
    }

    std::optional<int> parse_int() {
        skip_ws();
        size_t start = pos;
        if (pos < src.size() && (src[pos] == '-' || src[pos] == '+')) ++pos;
        while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') ++pos;
        if (start == pos) return std::nullopt;
        try {
            return std::stoi(std::string(src.substr(start, pos - start)));
        } catch (...) {
            return std::nullopt;
        }
    }
};

} // namespace

std::optional<SidecarManifest> manifest_from_json(const std::string& json) {
    Parser p{json};
    if (!p.eat('{')) return std::nullopt;

    SidecarManifest m;
    m.schema_version = -1;
    bool first = true;

    while (true) {
        p.skip_ws();
        if (p.pos < p.src.size() && p.src[p.pos] == '}') { ++p.pos; break; }
        if (!first && !p.eat(',')) return std::nullopt;
        first = false;

        auto key = p.parse_string();
        if (!key) return std::nullopt;
        if (!p.eat(':')) return std::nullopt;

        if (*key == "schema_version") {
            auto v = p.parse_int();
            if (!v) return std::nullopt;
            m.schema_version = *v;
        } else if (*key == "doc_id") {
            auto v = p.parse_string();
            if (!v) return std::nullopt;
            m.doc_id = *v;
        } else if (*key == "enrolled_at") {
            auto v = p.parse_string();
            if (!v) return std::nullopt;
            m.enrolled_at = *v;
        } else if (*key == "original_filename") {
            auto v = p.parse_string();
            if (!v) return std::nullopt;
            m.original_filename = *v;
        } else if (*key == "seed_sha256") {
            auto v = p.parse_string();
            if (!v) return std::nullopt;
            m.seed_sha256 = *v;
        } else {
            auto v = p.parse_string();
            if (!v) return std::nullopt;
        }
    }

    if (m.schema_version != 1) return std::nullopt;
    return m;
}

void write_manifest(const fs::path& path, const SidecarManifest& m) {
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("write_manifest: open tmp failed: " + tmp.string());
        std::string json = manifest_to_json(m);
        f.write(json.data(), static_cast<std::streamsize>(json.size()));
        if (!f) throw std::runtime_error("write_manifest: write failed: " + tmp.string());
    }
    fs::rename(tmp, path);
}

std::optional<SidecarManifest> read_manifest(const fs::path& path) {
    if (!fs::exists(path)) return std::nullopt;
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    return manifest_from_json(content);
}

} // namespace CollabText::Crdt
