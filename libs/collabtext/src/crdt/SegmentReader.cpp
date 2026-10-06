#include "crdt/SegmentReader.h"
#include "crdt/SegmentFormat.h"
#include "crdt/ZstdUtil.h"
#include "storage/DurableFile.h"
#include <collabtext/OpStream.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

namespace CollabText::Crdt {

namespace {

constexpr size_t kCursorBytes = 64;
constexpr size_t kCursorMaxBytes = kCursorBytes + 2 + 1024;
constexpr size_t kMaxSegmentFileBytes = 80 * 1024 * 1024;
constexpr size_t kMaxBatchRecords = 4096;
constexpr size_t kMaxBatchBytes = 16 * 1024 * 1024;
std::atomic<uint64_t> g_reader_id{1};

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
        value = value * 10 + uint64_t(c - '0');
    }
    return value;
}

uint64_t read_u64_le(const char* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= uint64_t(uint8_t(p[i])) << (i * 8);
    return v;
}
void write_u64_le(std::string& s, uint64_t v) {
    for (int i = 0; i < 8; ++i) s += char((v >> (i * 8)) & 0xFF);
}

std::string encode_cursor(const SegmentCursor& cursor) {
    std::string bytes("CTCR", 4);
    bytes.append(1, '\x01');
    bytes.append(3, '\0');
    write_u64_le(bytes, cursor.last_sealed);
    write_u64_le(bytes, cursor.open_id);
    write_u64_le(bytes, cursor.open_bytes);
    bytes.append(reinterpret_cast<const char*>(cursor.open_prefix_hash.data()),
                 cursor.open_prefix_hash.size());
    const uint16_t id_size = static_cast<uint16_t>(cursor.recovery_point_id.size());
    bytes += char(id_size & 0xFF);
    bytes += char((id_size >> 8) & 0xFF);
    bytes.append(cursor.recovery_point_id);
    return bytes;
}

std::optional<std::string> read_bounded(const fs::path& path, size_t maximum) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec || size > maximum) return std::nullopt;
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    std::string bytes(static_cast<size_t>(size), '\0');
    if (!bytes.empty()) file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!file && !bytes.empty()) return std::nullopt;
    return bytes;
}

bool all_zero(const std::array<uint8_t, 32>& hash) {
    return std::all_of(hash.begin(), hash.end(), [](uint8_t b) { return b == 0; });
}

} // namespace

std::optional<SegmentCursor> decode_segment_cursor(std::string_view bytes) {
    if (bytes.size() < kCursorBytes + 2 || bytes.size() > kCursorMaxBytes
        || bytes.substr(0, 4) != "CTCR"
        || bytes[4] != '\x01' || bytes[5] != '\0' || bytes[6] != '\0'
        || bytes[7] != '\0') return std::nullopt;
    SegmentCursor cursor{read_u64_le(bytes.data() + 8),
                         read_u64_le(bytes.data() + 16),
                         read_u64_le(bytes.data() + 24), {}};
    std::copy_n(reinterpret_cast<const uint8_t*>(bytes.data() + 32),
                cursor.open_prefix_hash.size(), cursor.open_prefix_hash.begin());
    const size_t id_size = uint8_t(bytes[64]) | (size_t(uint8_t(bytes[65])) << 8);
    if (id_size > 1024 || bytes.size() != kCursorBytes + 2 + id_size) return std::nullopt;
    cursor.recovery_point_id.assign(bytes.substr(kCursorBytes + 2));
    if (cursor.open_id == 0)
        return cursor.open_bytes == 0 && all_zero(cursor.open_prefix_hash)
            ? std::optional<SegmentCursor>(cursor) : std::nullopt;
    if (cursor.last_sealed == UINT64_MAX || cursor.open_id != cursor.last_sealed + 1
        || cursor.open_bytes == 0 || all_zero(cursor.open_prefix_hash)) return std::nullopt;
    return cursor;
}

SegmentReader::SegmentReader(fs::path peer_stream_dir, fs::path cursor_path)
    : m_dir(std::move(peer_stream_dir)), m_cursor_path(std::move(cursor_path)),
      m_reader_id(g_reader_id.fetch_add(1, std::memory_order_relaxed)) {
    if (m_reader_id == 0)
        m_reader_id = g_reader_id.fetch_add(1, std::memory_order_relaxed);
}

void SegmentReader::start(const CollabText::RecoveryPoint* recovered_point) {
    m_committed = {};
    m_issued_cursor.reset();
    load_cursor_(recovered_point);
}

void SegmentReader::load_cursor_(const CollabText::RecoveryPoint* recovered_point) {
    if (!recovered_point || !recovered_point->valid()
        || recovered_point->kind() != CollabText::RecoveryPoint::Kind::checkpoint) return;
    auto bytes = read_bounded(m_cursor_path, kCursorMaxBytes);
    if (!bytes) return;
    const auto decoded = decode_segment_cursor(*bytes);
    // A malformed cursor is not coverage. Replay from the beginning and allow
    // operation identity deduplication at the consumer.
    if (decoded && decoded->recovery_point_id == recovered_point->checkpoint_id())
        m_committed = *decoded;
}

SegmentReadBatch SegmentReader::read_new() {
    SegmentReadBatch batch;
    SegmentCursor candidate = m_committed;
    std::map<uint64_t, fs::path> sealed;
    std::map<uint64_t, fs::path> open;
    std::error_code ec;
    if (fs::exists(m_dir, ec) && !ec) {
        fs::directory_iterator it(m_dir, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (ends_with(name, ".seg.zst")) {
                if (auto id = parse_segment_id(name)) sealed.emplace(*id, it->path());
            } else if (ends_with(name, ".open")) {
                if (auto id = parse_segment_id(name)) open.emplace(*id, it->path());
            }
        }
    }
    if (ec) batch.gap = CollabText::Error(CollabText::ErrorCode::io_error,
                                          "cannot scan segment directory", m_dir.string());

    size_t delivered_bytes = 0;

    while (!batch.gap && candidate.last_sealed != UINT64_MAX) {
        const uint64_t next = candidate.last_sealed + 1;
        auto sealed_it = sealed.find(next);
        if (sealed_it != sealed.end()) {
            auto bytes = read_bounded(sealed_it->second, kMaxSegmentFileBytes);
            auto segment = bytes ? decode_sealed_segment(*bytes) : std::nullopt;
            if (segment) {
                size_t skip_records = 0;
                std::string prefix;
                if (m_committed.open_id == next && m_committed.open_bytes > 0) {
                    for (const auto& record : segment->records) {
                        prefix += base64_encode(record);
                        prefix += '\n';
                        if (prefix.size() >= m_committed.open_bytes) break;
                    }
                    if (prefix.size() != m_committed.open_bytes
                        || sha256_bytes(prefix) != m_committed.open_prefix_hash) {
                        prefix.clear();
                    } else {
                        size_t offset = 0;
                        while (skip_records < segment->records.size() && offset < prefix.size())
                            offset += base64_encode(segment->records[skip_records++]).size() + 1;
                    }
                }
                size_t i = skip_records;
                while (i < segment->records.size()) {
                    const auto& record = segment->records[i];
                    if (batch.records.size() >= kMaxBatchRecords
                        || record.size() > kMaxBatchBytes - std::min(kMaxBatchBytes, delivered_bytes)) {
                        if (batch.records.empty())
                            batch.gap = CollabText::Error(CollabText::ErrorCode::limit_exceeded,
                                                          "one sealed record exceeds the read batch bound",
                                                          sealed_it->second.string());
                        break;
                    }
                    prefix += base64_encode(record);
                    prefix += '\n';
                    batch.records.push_back(record);
                    delivered_bytes += record.size();
                    ++i;
                }
                if (i < segment->records.size()) {
                    if (!prefix.empty()) {
                        candidate.open_id = next;
                        candidate.open_bytes = prefix.size();
                        candidate.open_prefix_hash = sha256_bytes(prefix);
                    } else if (batch.records.empty()) {
                        batch.gap = CollabText::Error(CollabText::ErrorCode::limit_exceeded,
                                                      "one sealed record exceeds the read batch bound",
                                                      sealed_it->second.string());
                    }
                    break;
                }
                candidate.last_sealed = next;
                candidate.open_id = 0;
                candidate.open_bytes = 0;
                candidate.open_prefix_hash = {};
                if (batch.records.size() >= kMaxBatchRecords
                    || delivered_bytes >= kMaxBatchBytes) break;
                continue;
            }
            // A valid open tail for this same segment remains recoverable if a
            // prior seal attempt left an invalid or incomplete sealed file.
            if (!open.contains(next)) {
                batch.gap = CollabText::Error(CollabText::ErrorCode::corrupt_record,
                                              "invalid sealed predecessor blocks segment coverage",
                                              sealed_it->second.string());
                break;
            }
        }

        auto open_it = open.find(next);
        if (open_it != open.end()) {
            auto bytes = read_bounded(open_it->second, kMaxSegmentFileBytes);
            if (!bytes) {
                batch.gap = CollabText::Error(CollabText::ErrorCode::io_error,
                                              "cannot read open segment", open_it->second.string());
                break;
            }
            size_t offset = 0;
            if (m_committed.open_id == next && m_committed.open_bytes <= bytes->size()) {
                const std::string_view prefix(bytes->data(),
                    static_cast<size_t>(m_committed.open_bytes));
                if (sha256_bytes(prefix) == m_committed.open_prefix_hash)
                    offset = static_cast<size_t>(m_committed.open_bytes);
            }
            size_t i = offset;
            size_t consumed = offset;
            while (i < bytes->size()) {
                const size_t nl = bytes->find('\n', i);
                if (nl == std::string::npos) break; // provisional partial line
                auto decoded = base64_decode(std::string_view(*bytes).substr(i, nl - i));
                if (!decoded) {
                    batch.gap = CollabText::Error(CollabText::ErrorCode::corrupt_record,
                                                  "invalid complete record blocks open-tail coverage",
                                                  open_it->second.string());
                    break;
                }
                if (batch.records.size() >= kMaxBatchRecords
                    || decoded->size() > kMaxBatchBytes - std::min(kMaxBatchBytes, delivered_bytes)) {
                    batch.gap = CollabText::Error(CollabText::ErrorCode::limit_exceeded,
                                                  "open-tail read batch exceeds configured bound",
                                                  open_it->second.string());
                    break;
                }
                delivered_bytes += decoded->size();
                batch.records.push_back(std::move(*decoded));
                consumed = nl + 1;
                i = consumed;
            }
            if (consumed > 0) {
                candidate.open_id = next;
                candidate.open_bytes = consumed;
                candidate.open_prefix_hash = sha256_bytes(std::string_view(bytes->data(), consumed));
            }
            break; // open tails are provisional; never scan past them
        }

        const bool later_exists = (!sealed.empty() && sealed.rbegin()->first > next)
            || (!open.empty() && open.rbegin()->first > next);
        if (later_exists)
            batch.gap = CollabText::Error(CollabText::ErrorCode::corrupt_record,
                                          "missing segment predecessor blocks coverage",
                                          m_dir.string());
        break;
    }

    ++m_generation;
    if (m_generation == 0) ++m_generation;
    batch.token.owner_id = m_reader_id;
    batch.token.generation = m_generation;
    m_issued_cursor = candidate;
    return batch;
}

CollabText::Result<void> SegmentReader::commit(const SegmentDeliveryToken& token) {
    if (!m_issued_cursor || token.owner_id != m_reader_id
        || token.generation == 0 || token.generation != m_generation)
        return CollabText::Error(CollabText::ErrorCode::counter_regression,
                                 "stale or unknown segment delivery token", m_cursor_path.string());
    m_committed = *m_issued_cursor;
    m_committed.recovery_point_id.clear();
    m_issued_cursor.reset();
    return CollabText::Result<void>::success();
}

CollabText::Result<void> SegmentReader::commit_durable(
    const SegmentDeliveryToken& token, const CollabText::RecoveryPoint& point) {
    if (!m_issued_cursor || token.owner_id != m_reader_id
        || token.generation == 0 || token.generation != m_generation)
        return CollabText::Error(CollabText::ErrorCode::counter_regression,
                                 "stale or unknown segment delivery token", m_cursor_path.string());
    if (!point.valid())
        return CollabText::Error(CollabText::ErrorCode::invalid_identity,
                                 "durable segment commit requires a valid recovery point",
                                 m_cursor_path.string());
    if (point.kind() != CollabText::RecoveryPoint::Kind::checkpoint
        || point.checkpoint_id().size() > 1024)
        return CollabText::Error(CollabText::ErrorCode::invalid_identity,
                                 "durable segment cursor requires a bounded checkpoint ID",
                                 m_cursor_path.string());
    SegmentCursor durable_cursor = *m_issued_cursor;
    durable_cursor.recovery_point_id = std::string(point.checkpoint_id());
    const std::string bytes = encode_cursor(durable_cursor);
    auto published = CollabText::Storage::replace_atomically(m_cursor_path, bytes);
    if (!published.ok()) return published.error();
    m_committed = std::move(durable_cursor);
    m_issued_cursor.reset();
    return CollabText::Result<void>::success();
}

} // namespace CollabText::Crdt
