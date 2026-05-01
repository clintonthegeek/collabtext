#include "crdt/SegmentReader.h"
#include "crdt/SegmentFormat.h"
#include "crdt/ZstdUtil.h"

#include <algorithm>
#include <fstream>

namespace fs = std::filesystem;

namespace CollabText::Crdt {

namespace {

constexpr size_t kCursorBytes = 24;

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size()
        && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::optional<uint64_t> parse_segment_id(const std::string& filename) {
    if (filename.size() < 10) return std::nullopt;
    for (int i = 0; i < 10; ++i)
        if (filename[i] < '0' || filename[i] > '9') return std::nullopt;
    return std::stoull(filename.substr(0, 10));
}

uint64_t read_u64_le(const char* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= uint64_t(uint8_t(p[i])) << (i * 8);
    return v;
}
void write_u64_le(std::string& s, uint64_t v) {
    for (int i = 0; i < 8; ++i) s += char((v >> (i * 8)) & 0xFF);
}

} // namespace

SegmentReader::SegmentReader(fs::path peer_stream_dir, fs::path cursor_path)
    : m_dir(std::move(peer_stream_dir))
    , m_cursor_path(std::move(cursor_path)) {}

void SegmentReader::start() { load_cursor_(); }

void SegmentReader::load_cursor_() {
    std::ifstream f(m_cursor_path, std::ios::binary);
    if (!f) return;
    char buf[kCursorBytes];
    f.read(buf, kCursorBytes);
    if (f.gcount() != std::streamsize(kCursorBytes)) return;
    m_last_sealed = read_u64_le(buf);
    m_open_id     = read_u64_le(buf + 8);
    m_open_bytes  = read_u64_le(buf + 16);
}

void SegmentReader::save_cursor_() const {
    std::error_code ec;
    fs::create_directories(m_cursor_path.parent_path(), ec);
    std::string buf;
    write_u64_le(buf, m_last_sealed);
    write_u64_le(buf, m_open_id);
    write_u64_le(buf, m_open_bytes);
    fs::path tmp = m_cursor_path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(buf.data(), std::streamsize(buf.size()));
    }
    fs::rename(tmp, m_cursor_path);
}

void SegmentReader::commit() { save_cursor_(); }

std::vector<std::string> SegmentReader::read_new() {
    std::vector<std::string> out;
    if (!fs::exists(m_dir)) return out;

    std::vector<std::pair<uint64_t, fs::path>> sealed;
    std::optional<std::pair<uint64_t, fs::path>> open_seg;

    for (auto& e : fs::directory_iterator(m_dir)) {
        std::string name = e.path().filename().string();
        if (ends_with(name, ".seg.zst")) {
            auto id = parse_segment_id(name);
            if (id) sealed.emplace_back(*id, e.path());
        } else if (ends_with(name, ".open")) {
            auto id = parse_segment_id(name);
            if (id) {
                if (!open_seg || open_seg->first < *id) open_seg = {*id, e.path()};
            }
        }
    }

    std::sort(sealed.begin(), sealed.end(),
        [](auto& a, auto& b) { return a.first < b.first; });

    for (auto& [id, path] : sealed) {
        if (id <= m_last_sealed) continue;
        std::string raw;
        {
            std::ifstream f(path, std::ios::binary);
            raw.assign(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
        }
        auto seg = decode_sealed_segment(raw);
        if (!seg) continue;
        for (auto& r : seg->records) out.push_back(std::move(r));
        m_last_sealed = id;
    }

    if (open_seg) {
        auto [id, path] = *open_seg;
        if (id <= m_last_sealed) {
            m_open_id = 0;
            m_open_bytes = 0;
        } else {
            if (m_open_id != id) {
                m_open_id = id;
                m_open_bytes = 0;
            }
            std::ifstream f(path, std::ios::binary | std::ios::ate);
            uint64_t file_size = uint64_t(f.tellg());
            if (file_size > m_open_bytes) {
                f.seekg(std::streamoff(m_open_bytes));
                std::string tail;
                tail.resize(file_size - m_open_bytes);
                f.read(tail.data(), std::streamsize(tail.size()));
                size_t i = 0;
                size_t consumed = 0;
                while (i < tail.size()) {
                    size_t nl = tail.find('\n', i);
                    if (nl == std::string::npos) break;
                    auto line = std::string(tail.data() + i, nl - i);
                    if (auto decoded = base64_decode(line))
                        out.push_back(std::move(*decoded));
                    consumed = nl + 1;
                    i = nl + 1;
                }
                m_open_bytes += consumed;
            }
        }
    }

    return out;
}

} // namespace CollabText::Crdt
