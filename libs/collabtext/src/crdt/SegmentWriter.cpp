#include "crdt/SegmentWriter.h"
#include "crdt/ZstdUtil.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

namespace CollabText::Crdt {

namespace {

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

} // namespace

std::string SegmentWriter::segment_filename_(uint64_t id, std::string_view suffix) {
    std::ostringstream os;
    os << std::setw(10) << std::setfill('0') << id;
    std::string out = os.str();
    out.append(suffix.data(), suffix.size());
    return out;
}

SegmentWriter::SegmentWriter(fs::path stream_dir, SegmentKind kind, WriterConfig cfg)
    : m_dir(std::move(stream_dir))
    , m_kind(kind)
    , m_cfg(cfg) {}

void SegmentWriter::start() {
    fs::create_directories(m_dir);
    recover_();
    m_started = true;
}

void SegmentWriter::recover_() {
    uint64_t highest_sealed = 0;
    std::optional<uint64_t> open_id;
    std::vector<fs::path> tmps;

    for (auto& e : fs::directory_iterator(m_dir)) {
        std::string name = e.path().filename().string();
        if (ends_with(name, ".seg.zst.tmp")) {
            tmps.push_back(e.path());
            continue;
        }
        if (ends_with(name, ".seg.zst")) {
            auto id = parse_segment_id(name);
            if (id) highest_sealed = std::max(highest_sealed, *id);
        } else if (ends_with(name, ".open")) {
            auto id = parse_segment_id(name);
            if (id) {
                if (open_id) open_id = std::max(*open_id, *id);
                else open_id = id;
            }
        }
    }
    for (auto& p : tmps) { std::error_code ec; fs::remove(p, ec); }

    if (open_id && *open_id <= highest_sealed) {
        std::error_code ec;
        fs::remove(m_dir / segment_filename_(*open_id, ".open"), ec);
        open_id.reset();
    }

    if (!open_id) {
        m_open_id = 0;
        return;
    }

    fs::path path = m_dir / segment_filename_(*open_id, ".open");
    std::string content;
    {
        std::ifstream f(path, std::ios::binary);
        content.assign(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
    }
    size_t last_nl = content.find_last_of('\n');
    size_t valid_size = (last_nl == std::string::npos) ? 0 : last_nl + 1;
    if (valid_size != content.size()) {
        std::ofstream trunc(path, std::ios::binary | std::ios::trunc);
        trunc.write(content.data(), std::streamsize(valid_size));
    }

    m_open_id = *open_id;
    m_open_size = valid_size;
    m_open_records_b64.clear();
    m_open_has_records = false;

    size_t i = 0;
    while (i < valid_size) {
        size_t nl = content.find('\n', i);
        if (nl == std::string::npos) break;
        std::string line = content.substr(i, nl - i);
        m_open_records_b64.push_back(line);
        i = nl + 1;
    }
    if (!m_open_records_b64.empty()) {
        m_open_has_records = true;
    }

    m_open_file.open(path, std::ios::binary | std::ios::app);
}

void SegmentWriter::open_new_segment_(uint64_t id) {
    m_open_id = id;
    m_open_size = 0;
    m_open_records_b64.clear();
    m_open_first_lamport = 0;
    m_open_last_lamport = 0;
    m_open_has_records = false;
    m_open_first_record_time.reset();
    fs::path path = m_dir / segment_filename_(id, ".open");
    m_open_file.open(path, std::ios::binary | std::ios::app);
    ++m_stats.segments_opened;
}

void SegmentWriter::append(std::string_view payload, uint64_t lamport) {
    Pending p;
    p.payload.assign(payload);
    p.lamport = lamport;
    m_pending_bytes += p.payload.size();
    m_pending.push_back(std::move(p));
    if (!m_first_pending) m_first_pending = std::chrono::steady_clock::now();
}

void SegmentWriter::tick(std::chrono::steady_clock::time_point now) {
    bool should_flush = false;
    if (m_pending_bytes >= m_cfg.flush_bytes) should_flush = true;
    if (m_first_pending && now - *m_first_pending >= m_cfg.flush_idle)
        should_flush = true;
    if (should_flush) flush_pending_to_open_(now);

    bool should_seal = false;
    if (m_open_has_records) {
        if (m_open_size >= m_cfg.seal_bytes) should_seal = true;
        if (m_open_first_record_time
            && now - *m_open_first_record_time >= m_cfg.seal_idle) {
            should_seal = true;
        }
    }
    if (should_seal) seal_open_();
}

void SegmentWriter::flush_pending_to_open_(std::chrono::steady_clock::time_point now) {
    if (m_pending.empty()) return;
    if (m_open_id == 0) {
        uint64_t next = 1;
        for (auto& e : fs::directory_iterator(m_dir)) {
            std::string name = e.path().filename().string();
            auto id = parse_segment_id(name);
            if (id) next = std::max(next, *id + 1);
        }
        open_new_segment_(next);
    }
    for (auto& p : m_pending) {
        std::string b64 = base64_encode(p.payload);
        m_open_file.write(b64.data(), std::streamsize(b64.size()));
        m_open_file.put('\n');
        m_open_records_b64.push_back(b64);
        m_open_size += b64.size() + 1;
        m_stats.bytes_written_open += b64.size() + 1;
        if (!m_open_has_records) {
            m_open_first_lamport = p.lamport;
            m_open_first_record_time = now;
            m_open_has_records = true;
        }
        m_open_last_lamport = p.lamport;
    }
    m_open_file.flush();
    m_pending.clear();
    m_pending_bytes = 0;
    m_first_pending.reset();
}

void SegmentWriter::flush() {
    flush_pending_to_open_(std::chrono::steady_clock::now());
    if (m_open_file.is_open()) {
        m_open_file.flush();
        ++m_stats.fsync_count;
    }
}

void SegmentWriter::close() {
    flush_pending_to_open_(std::chrono::steady_clock::now());
    if (m_open_has_records) seal_open_();
}

void SegmentWriter::seal_open_() {
    if (!m_open_has_records) return;

    std::vector<std::string> raw_records;
    raw_records.reserve(m_open_records_b64.size());
    for (auto& b64 : m_open_records_b64) {
        auto decoded = base64_decode(b64);
        if (!decoded) continue;
        raw_records.push_back(std::move(*decoded));
    }

    SegmentHeader hdr;
    hdr.format_version = 1;
    hdr.kind = m_kind;
    hdr.flags = 0;
    hdr.first_lamport = m_open_first_lamport;
    hdr.last_lamport = m_open_last_lamport;

    std::string sealed_bytes = encode_sealed_segment(hdr, raw_records, m_cfg.zstd_level);

    fs::path tmp = m_dir / segment_filename_(m_open_id, ".seg.zst.tmp");
    fs::path final_path = m_dir / segment_filename_(m_open_id, ".seg.zst");

    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(sealed_bytes.data(), std::streamsize(sealed_bytes.size()));
        f.flush();
    }
    fs::rename(tmp, final_path);

    if (m_open_file.is_open()) m_open_file.close();
    std::error_code ec;
    fs::remove(m_dir / segment_filename_(m_open_id, ".open"), ec);

    m_stats.bytes_written_sealed += sealed_bytes.size();
    ++m_stats.segments_sealed;
    ++m_stats.fsync_count;

    m_open_id = 0;
    m_open_size = 0;
    m_open_records_b64.clear();
    m_open_has_records = false;
    m_open_first_record_time.reset();
}

} // namespace CollabText::Crdt
