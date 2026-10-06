#include "crdt/SegmentWriter.h"
#include "crdt/ZstdUtil.h"
#include "crdt/StreamSerialization.h"
#include <collabtext/Serialization.h>

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>

namespace fs = std::filesystem;

namespace CollabText::Crdt {
namespace {

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size()
        && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::optional<uint64_t> parse_segment_id(std::string_view filename) {
    if (filename.size() < 10) return std::nullopt;
    uint64_t value = 0;
    for (size_t i = 0; i < 10; ++i) {
        const char c = filename[i];
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + static_cast<uint64_t>(c - '0');
    }
    return value;
}

bool is_seal_temporary(std::string_view filename) {
    return parse_segment_id(filename).has_value()
        && filename.substr(10).starts_with(".seg.zst.tmp");
}

Result<std::string> read_bounded(const fs::path& path, size_t maximum) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) return Error(ErrorCode::io_error, "cannot stat segment", path.string());
    if (size > maximum) return Error(ErrorCode::limit_exceeded, "segment exceeds recovery limit", path.string());
    std::ifstream f(path, std::ios::binary);
    if (!f) return Error(ErrorCode::io_error, "cannot open segment", path.string());
    std::string data(static_cast<size_t>(size), '\0');
    if (!data.empty()) f.read(data.data(), static_cast<std::streamsize>(data.size()));
    if (!f && !data.empty()) return Error(ErrorCode::io_error, "cannot read segment", path.string());
    return data;
}

Result<void> remove_durable(const fs::path& path, const WriterConfig& cfg,
                            SegmentStats* stats) {
    const auto& ops = cfg.file_ops ? *cfg.file_ops : Storage::FileOps::real();
    auto removed = ops.remove(path);
    if (!removed.ok()) return removed.error();
    Storage::FileOpCounts counts;
    auto synced = Storage::sync_directory(path.parent_path(), cfg.step_hook,
                                           cfg.file_ops, &counts);
    if (!synced.ok()) return synced.error();
    stats->fsync_count += counts.file_syncs + counts.dir_syncs;
    return Result<void>::success();
}

std::optional<uint64_t> record_counter(SegmentKind kind, std::string_view raw) {
    if (kind == SegmentKind::Ops) {
        auto op = decode_operation(raw);
        if (!op) return std::nullopt;
        return op_identity(op.value()).counter;
    }
    auto entry = decode_stream_entry(raw);
    if (!entry) return std::nullopt;
    return entry->seq;
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
    : m_dir(std::move(stream_dir)), m_kind(kind), m_cfg(cfg) {}

SegmentWriter::~SegmentWriter() {
    // Reportable errors require explicit close(); destruction only attempts to
    // persist queued records and release the descriptor. It never seals.
    if (m_started && !m_closed && !m_failure) (void)flush();
    if (m_open_file) (void)m_open_file->close();
}

Result<void> SegmentWriter::start() {
    if (m_started) return Result<void>::success();
    if (m_cfg.max_pending_bytes == 0 || m_cfg.max_tail_bytes == 0
        || m_cfg.max_tail_bytes < 2 || m_cfg.flush_bytes == 0)
        return Error(ErrorCode::limit_exceeded, "invalid segment writer limits", m_dir.string());
    std::error_code ec;
    fs::create_directories(m_dir, ec);
    if (ec) return Error(ErrorCode::io_error, "cannot create segment directory", m_dir.string());
    auto recovered = recover_();
    if (!recovered.ok()) return recovered.error();
    m_started = true;
    return Result<void>::success();
}

Result<void> SegmentWriter::recover_() {
    uint64_t highest_valid_sealed = 0;
    std::optional<uint64_t> open_id;
    std::vector<fs::path> temporaries;
    std::vector<std::pair<uint64_t, fs::path>> sealed_files;
    std::vector<std::pair<uint64_t, fs::path>> open_files;
    for (const auto& e : fs::directory_iterator(m_dir)) {
        const std::string name = e.path().filename().string();
        if (is_seal_temporary(name)) {
            temporaries.push_back(e.path());
        } else if (ends_with(name, ".seg.zst")) {
            if (auto id = parse_segment_id(name)) {
                sealed_files.emplace_back(*id, e.path());
            }
        } else if (ends_with(name, ".open")) {
            if (auto id = parse_segment_id(name)) {
                open_files.emplace_back(*id, e.path());
                if (open_id) open_id = std::max(*open_id, *id);
                else open_id = *id;
            }
        }
    }
    for (const auto& [id, path] : sealed_files) {
        auto bytes = read_bounded(path, kMaxEncodedBytes);
        if (!bytes) continue;
        if (decode_sealed_segment(bytes.value())) highest_valid_sealed = std::max(highest_valid_sealed, id);
    }

    for (const auto& temp : temporaries) {
        auto bytes = read_bounded(temp, kMaxEncodedBytes);
        const auto id = parse_segment_id(temp.filename().string());
        if (bytes && id && decode_sealed_segment(bytes.value())) {
            const fs::path final_path = m_dir / segment_filename_(*id, ".seg.zst");
            Storage::FileOpCounts counts;
            auto published = Storage::replace_atomically(final_path, bytes.value(),
                    m_cfg.step_hook, m_cfg.file_ops, &counts);
            m_stats.fsync_count += counts.file_syncs + counts.dir_syncs;
            if (!published.ok()) return published.error();
            highest_valid_sealed = std::max(highest_valid_sealed, *id);
            auto removed = remove_durable(temp, m_cfg, &m_stats);
            if (!removed.ok()) return removed.error();
        } else {
            const auto temp_id = parse_segment_id(temp.filename().string());
            const bool has_matching_open = temp_id && std::any_of(
                open_files.begin(), open_files.end(), [temp_id](const auto& open) {
                    return open.first == *temp_id;
                });
            if (!has_matching_open)
                return Error(ErrorCode::corrupt_record,
                             "invalid interrupted seal has no matching recoverable tail",
                             temp.string());
            auto removed = remove_durable(temp, m_cfg, &m_stats);
            if (!removed.ok()) return removed.error();
        }
    }

    std::vector<std::pair<uint64_t, fs::path>> uncovered_open;
    for (const auto& open : open_files) {
        if (open.first <= highest_valid_sealed) {
            auto removed = remove_durable(open.second, m_cfg, &m_stats);
            if (!removed.ok()) return removed.error();
        } else {
            uncovered_open.push_back(open);
        }
    }
    if (uncovered_open.size() > 1)
        return Error(ErrorCode::corrupt_record,
                     "multiple uncovered open tails require manual recovery", m_dir.string());
    open_id = uncovered_open.empty()
        ? std::nullopt : std::optional<uint64_t>(uncovered_open.front().first);
    if (!open_id) {
        m_open_id = 0;
        return Result<void>::success();
    }

    const fs::path path = m_dir / segment_filename_(*open_id, ".open");
    auto loaded = read_bounded(path, m_cfg.max_tail_bytes);
    if (!loaded) return loaded.error();
    std::string content = std::move(loaded.value());
    const size_t last_nl = content.find_last_of('\n');
    const size_t valid_size = last_nl == std::string::npos ? 0 : last_nl + 1;
    const std::string_view valid(content.data(), valid_size);
    std::vector<std::string> recovered_records;
    uint64_t first = 0, last = 0;
    bool have_counter = false;
    size_t i = 0;
    while (i < valid.size()) {
        const size_t nl = valid.find('\n', i);
        if (nl == std::string_view::npos) break;
        const auto encoded = valid.substr(i, nl - i);
        auto decoded = base64_decode(encoded);
        if (!decoded) return Error(ErrorCode::corrupt_record, "malformed complete record in open tail", path.string());
        const auto counter = record_counter(m_kind, *decoded);
        if (!counter) return Error(ErrorCode::corrupt_record,
                                   "cannot reconstruct open-tail record metadata", path.string());
        if (!have_counter) first = *counter;
        last = *counter;
        have_counter = true;
        recovered_records.emplace_back(encoded);
        i = nl + 1;
    }
    if (valid_size != content.size()) {
        Storage::FileOpCounts counts;
        auto repaired = Storage::replace_atomically(path, valid, m_cfg.step_hook,
                                                     m_cfg.file_ops, &counts);
        m_stats.fsync_count += counts.file_syncs + counts.dir_syncs;
        if (!repaired.ok()) return repaired.error();
    }
    m_open_id = *open_id;
    m_open_size = valid_size;
    m_open_records_b64 = std::move(recovered_records);
    m_open_has_records = !m_open_records_b64.empty();
    m_open_first_lamport = first;
    m_open_last_lamport = last;
    if (m_open_has_records) m_open_first_record_time = std::chrono::steady_clock::now();
    auto append_file = Storage::DurableAppendFile::open(path, m_cfg.step_hook, m_cfg.file_ops);
    if (!append_file) return append_file.error();
    m_open_file.emplace(std::move(append_file.value()));
    m_stats.segments_opened++;
    return Result<void>::success();
}

Result<void> SegmentWriter::open_new_segment_(uint64_t id) {
    if (id == 0 || id > 9999999999ULL)
        return Error(ErrorCode::counter_exhausted, "segment id space exhausted", m_dir.string());
    m_open_id = id;
    m_open_size = 0;
    m_open_records_b64.clear();
    m_open_first_lamport = 0;
    m_open_last_lamport = 0;
    m_open_has_records = false;
    m_open_first_record_time.reset();
    auto opened = Storage::DurableAppendFile::open(
        m_dir / segment_filename_(id, ".open"), m_cfg.step_hook, m_cfg.file_ops);
    if (!opened) return opened.error();
    m_open_file.emplace(std::move(opened.value()));
    ++m_stats.segments_opened;
    return Result<void>::success();
}

Result<void> SegmentWriter::append(std::string_view payload, uint64_t lamport) {
    if (!m_started || m_closed) return Error(ErrorCode::io_error, "writer is not open", m_dir.string());
    if (m_failure) return *m_failure;
    const size_t max_raw_record = ((m_cfg.max_tail_bytes - 1) / 4) * 3;
    if (payload.size() > max_raw_record)
        return Error(ErrorCode::limit_exceeded, "record exceeds segment tail budget", m_dir.string());
    if (payload.size() > m_cfg.max_pending_bytes
        || m_pending_bytes > m_cfg.max_pending_bytes - payload.size())
        return Error(ErrorCode::deferred_queue_full, "segment writer backpressure limit reached", m_dir.string());
    Pending p{std::string(payload), lamport};
    m_pending_bytes += p.payload.size();
    m_pending.push_back(std::move(p));
    if (!m_first_pending) m_first_pending = std::chrono::steady_clock::now();
    return Result<void>::success();
}

Result<void> SegmentWriter::tick(std::chrono::steady_clock::time_point now) {
    if (!m_started || m_closed) return Error(ErrorCode::io_error, "writer is not open", m_dir.string());
    if (m_failure) return *m_failure;
    if (!m_pending.empty() && (m_pending_bytes >= m_cfg.flush_bytes
        || (m_first_pending && now - *m_first_pending >= m_cfg.flush_idle))) {
        auto flushed = flush_pending_to_open_(now);
        if (!flushed.ok()) return flushed.error();
    }
    if (m_open_has_records && (m_open_size >= m_cfg.seal_bytes
        || (m_open_first_record_time && now - *m_open_first_record_time >= m_cfg.seal_idle)))
        return seal_open_();
    return Result<void>::success();
}

Result<void> SegmentWriter::flush_pending_to_open_(std::chrono::steady_clock::time_point now) {
    while (!m_pending.empty()) {
        const auto& pending = m_pending.front();
        const std::string encoded = base64_encode(pending.payload);
        if (encoded.size() >= m_cfg.max_tail_bytes)
            return Error(ErrorCode::limit_exceeded, "record exceeds segment tail budget", m_dir.string());
        const size_t wire_bytes = encoded.size() + 1;
        if (m_open_has_records && wire_bytes > m_cfg.max_tail_bytes - m_open_size) {
            auto sealed = seal_open_();
            if (!sealed.ok()) return sealed.error();
        }
        if (m_open_id == 0) {
            uint64_t next = m_open_id == 0 ? 1 : m_open_id + 1;
            if (m_open_id == 0) {
                for (const auto& e : fs::directory_iterator(m_dir)) {
                    if (auto id = parse_segment_id(e.path().filename().string())) {
                        if (*id == std::numeric_limits<uint64_t>::max())
                            return Error(ErrorCode::counter_exhausted, "segment id exhausted", m_dir.string());
                        next = std::max(next, *id + 1);
                    }
                }
            }
            auto opened = open_new_segment_(next);
            if (!opened.ok()) return opened.error();
        }
        if (wire_bytes > m_cfg.max_tail_bytes - std::min(m_open_size, m_cfg.max_tail_bytes))
            return Error(ErrorCode::limit_exceeded, "record exceeds segment tail budget", m_dir.string());
        const uint64_t before = m_open_file->sync_count();
        const uint64_t dirs_before = m_open_file->counts().dir_syncs;
        auto written = m_open_file->append(encoded + "\n");
        m_stats.fsync_count += m_open_file->sync_count() - before;
        m_stats.fsync_count += m_open_file->counts().dir_syncs - dirs_before;
        if (!written.ok()) {
            m_failure = written.error();
            return written.error();
        }
        m_open_records_b64.push_back(encoded);
        m_open_size += wire_bytes;
        m_stats.bytes_written_open += wire_bytes;
        if (!m_open_has_records) {
            m_open_first_lamport = pending.lamport;
            m_open_first_record_time = now;
            m_open_has_records = true;
        }
        m_open_last_lamport = pending.lamport;
        m_pending_bytes -= pending.payload.size();
        m_pending.erase(m_pending.begin());
    }
    m_first_pending.reset();
    return Result<void>::success();
}

Result<void> SegmentWriter::flush() {
    if (!m_started || m_closed) return Error(ErrorCode::io_error, "writer is not open", m_dir.string());
    if (m_failure) return *m_failure;
    auto pending = flush_pending_to_open_(std::chrono::steady_clock::now());
    if (!pending.ok()) return pending.error();
    if (m_open_file) {
        const uint64_t before = m_open_file->sync_count();
        auto synced = m_open_file->sync();
        m_stats.fsync_count += m_open_file->sync_count() - before;
        if (!synced.ok()) {
            m_failure = synced.error();
            return synced.error();
        }
    }
    return Result<void>::success();
}

Result<void> SegmentWriter::close() {
    if (m_closed) return Result<void>::success();
    if (!m_started) return Error(ErrorCode::io_error, "writer was not started", m_dir.string());
    auto flushed = flush();
    if (!flushed.ok()) return flushed.error();
    if (m_open_has_records) {
        auto sealed = seal_open_();
        if (!sealed.ok()) return sealed.error();
    }
    m_closed = true;
    return Result<void>::success();
}

Result<void> SegmentWriter::seal_open_() {
    if (!m_open_has_records) return Result<void>::success();
    std::vector<std::string> raw_records;
    raw_records.reserve(m_open_records_b64.size());
    for (const auto& b64 : m_open_records_b64) {
        auto decoded = base64_decode(b64);
        if (!decoded) return Error(ErrorCode::corrupt_record, "open tail contains invalid base64", m_dir.string());
        raw_records.push_back(std::move(*decoded));
    }
    SegmentHeader hdr;
    hdr.format_version = 1;
    hdr.kind = m_kind;
    hdr.first_lamport = m_open_first_lamport;
    hdr.last_lamport = m_open_last_lamport;
    const std::string sealed_bytes = encode_sealed_segment(hdr, raw_records, m_cfg.zstd_level);
    if (sealed_bytes.empty()) return Error(ErrorCode::limit_exceeded, "sealed segment exceeds format limits", m_dir.string());

    const fs::path final_path = m_dir / segment_filename_(m_open_id, ".seg.zst");
    Storage::FileOpCounts counts;
    auto published = Storage::replace_atomically(final_path, sealed_bytes,
            m_cfg.step_hook, m_cfg.file_ops, &counts);
    m_stats.fsync_count += counts.file_syncs + counts.dir_syncs;
    if (!published.ok()) return published.error();

    if (m_open_file) {
        auto closed = m_open_file->close();
        if (!closed.ok()) return closed.error();
        m_open_file.reset();
    }
    const fs::path open_path = m_dir / segment_filename_(m_open_id, ".open");
    auto removed = remove_durable(open_path, m_cfg, &m_stats);
    if (!removed.ok()) return removed.error();
    m_stats.bytes_written_sealed += sealed_bytes.size();
    ++m_stats.segments_sealed;
    m_open_id = 0;
    m_open_size = 0;
    m_open_records_b64.clear();
    m_open_has_records = false;
    m_open_first_record_time.reset();
    return Result<void>::success();
}

} // namespace CollabText::Crdt
