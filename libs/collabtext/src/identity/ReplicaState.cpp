/// ReplicaState.cpp — the private, per-device identity and counter state.
///
/// Roadmap task P02.
///
/// The properties this file exists to guarantee, in the order they matter:
///
///   1. An operation id is durable before it is handed out. `next_operation()`
///      persists the reservation and returns it only afterwards, so a crash
///      cannot reissue an id (F4).
///   2. A corrupt state file is reported, never replaced. Replacing it would
///      mint a new actor while old operations still claim the old identity —
///      the exact "silent fallback" failure the acceptance criteria forbid.
///   3. Counters never wrap and never go backwards. Exhaustion is an error that
///      mutates nothing.
///   4. Two local writers cannot both hold a stream. The lease is created
///      exclusively, and a conflict names the holder instead of stealing.
///
/// The persistence format is intentionally boring and strictly parsed:
///
///   collabtext-replica-state 1
///   actor <32 hex characters>
///   stream <name> <next counter>
///   ...
///
/// An unknown version is unsupported_version rather than a best-effort parse,
/// and an unknown directive is malformed rather than skipped. A skipped line is
/// a silently lost counter.

#include "collabtext/ActorId.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>

namespace CollabText {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kStateMagic = "collabtext-replica-state";
constexpr uint32_t kStateVersion = 1;
constexpr std::string_view kLeaseSuffix = ".lease";

std::string_view hex_digits = "0123456789abcdef";

std::string to_hex(const std::array<uint8_t, ActorId::kBytes>& bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const uint8_t byte : bytes) {
        out += hex_digits[byte >> 4];
        out += hex_digits[byte & 0x0f];
    }
    return out;
}

/// Strict hex parse: exact length, no case folding surprises beyond lower/upper,
/// no truncation of a longer string.
Result<std::array<uint8_t, ActorId::kBytes>> parse_hex_exact(std::string_view text) {
    std::array<uint8_t, ActorId::kBytes> bytes{};
    if (text.size() != bytes.size() * 2)
        return Error(ErrorCode::malformed,
                     "actor id must be 32 hex characters, got " +
                         std::to_string(text.size()));
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < bytes.size(); ++i) {
        const int hi = nibble(text[i * 2]);
        const int lo = nibble(text[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return Error(ErrorCode::malformed, "actor id has a non-hex character");
        bytes[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return bytes;
}

/// The platform randomness adapter: the only place the project asks the
/// operating system for entropy. Narrow on purpose.
Result<std::array<uint8_t, ActorId::kBytes>> platform_random(std::string_view purpose) {
    (void)purpose;
    std::array<uint8_t, ActorId::kBytes> bytes{};
    std::random_device device;
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(device());
    // std::random_device is allowed to be deterministic on some platforms. If it
    // produced an all-zero id, refuse rather than mint an invalid actor.
    bool all_zero = true;
    for (const uint8_t byte : bytes) all_zero = all_zero && byte == 0;
    if (all_zero)
        return Error(ErrorCode::random_source_unavailable,
                     "randomness source produced an all-zero actor id");
    return bytes;
}

bool fsync_path(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    const int rc = ::fsync(fd);
    ::close(fd);
    return rc == 0;
}

bool fsync_directory(const std::filesystem::path& dir) {
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    const int rc = ::fsync(fd);
    ::close(fd);
    return rc == 0;
}

/// Write `contents` to `path` atomically: temp file, fsync, rename, fsync dir.
Result<void> write_atomic(const std::filesystem::path& path, std::string_view contents) {
    std::error_code ec;
    const std::filesystem::path tmp = std::filesystem::path(path.string() + ".tmp");
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file)
            return Error(ErrorCode::io_error, "cannot create temp file", tmp.string());
        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        file.flush();
        if (!file)
            return Error(ErrorCode::io_error, "short write to temp file", tmp.string());
    }
    if (!fsync_path(tmp))
        return Error(ErrorCode::io_error, "fsync of temp file failed", tmp.string());
    fs::rename(tmp, path, ec);
    if (ec)
        return Error(ErrorCode::io_error, "rename failed: " + ec.message(), path.string());
    if (!fsync_directory(path.parent_path()))
        return Error(ErrorCode::io_error, "directory fsync failed", path.parent_path().string());
    return {};
}

Result<uint64_t> parse_u64(std::string_view text, std::string_view what) {
    uint64_t value = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last || text.empty())
        return Error(ErrorCode::malformed,
                     std::string("invalid integer for ") + std::string(what));
    return value;
}

std::filesystem::path state_file(const std::filesystem::path& dir) {
    return dir / "replica.state";
}

}  // namespace

// ============================================================================
// ErrorCode
// ============================================================================

std::string_view to_string(ErrorCode code) {
    switch (code) {
    case ErrorCode::malformed: return "malformed";
    case ErrorCode::unknown_format: return "unknown_format";
    case ErrorCode::unsupported_version: return "unsupported_version";
    case ErrorCode::invalid_unicode: return "invalid_unicode";
    case ErrorCode::invalid_identity: return "invalid_identity";
    case ErrorCode::actor_conflict: return "actor_conflict";
    case ErrorCode::writer_conflict: return "writer_conflict";
    case ErrorCode::unknown_stream: return "unknown_stream";
    case ErrorCode::wrong_epoch: return "wrong_epoch";
    case ErrorCode::counter_regression: return "counter_regression";
    case ErrorCode::counter_exhausted: return "counter_exhausted";
    case ErrorCode::unresolvable_anchor: return "unresolvable_anchor";
    case ErrorCode::corrupt_record: return "corrupt_record";
    case ErrorCode::io_error: return "io_error";
    case ErrorCode::unsupported_operation: return "unsupported_operation";
    case ErrorCode::random_source_unavailable: return "random_source_unavailable";
    case ErrorCode::deferred_queue_full: return "deferred_queue_full";
    }
    return "unknown";
}

// ============================================================================
// ActorId
// ============================================================================

Result<ActorId> ActorId::generate() {
    return generate(&platform_random);
}

Result<ActorId> ActorId::generate(const Source& source) {
    if (!source)
        return Error(ErrorCode::random_source_unavailable,
                     "no randomness source was provided for actor generation");
    auto bytes = source("actor identity");
    if (!bytes.ok()) return bytes.error().with_context("generating actor identity");
    if (!bytes.value().size())  // unreachable for a fixed array, kept explicit
        return Error(ErrorCode::random_source_unavailable,
                     "randomness source returned no bytes");
    ActorId id = from_bytes(bytes.value());
    if (!id.valid())
        return Error(ErrorCode::invalid_identity,
                     "randomness source produced an all-zero actor id");
    return id;
}

Result<ActorId> ActorId::parse(std::string_view text) {
    auto bytes = parse_hex_exact(text);
    if (!bytes.ok()) return bytes.error().with_context("parsing actor id");
    ActorId id = from_bytes(bytes.value());
    if (!id.valid())
        return Error(ErrorCode::invalid_identity, "actor id is all zero");
    return id;
}

bool ActorId::valid() const noexcept {
    for (const uint8_t byte : m_bytes)
        if (byte != 0) return true;
    return false;
}

std::string ActorId::to_string() const { return to_hex(m_bytes); }

int ActorId::compare(const ActorId& other) const noexcept {
    return std::lexicographical_compare(
        m_bytes.begin(), m_bytes.end(), other.m_bytes.begin(), other.m_bytes.end()) ? -1
        : (m_bytes == other.m_bytes ? 0 : 1);
}

// ============================================================================
// CheckedCounter
// ============================================================================

Result<uint64_t> CheckedCounter::reserve(std::string_view context) {
    if (m_next == kExhausted)
        return Error(ErrorCode::counter_exhausted,
                     "counter for this stream is exhausted; a new epoch is required",
                     std::string(context));
    const uint64_t issued = m_next;
    m_next = m_next + 1;
    return issued;
}

Result<void> CheckedCounter::observe(uint64_t observed, std::string_view context) {
    // Being behind an observed value is ordinary: the peer simply has not seen
    // everything we have produced. Only the end of the range is a failure.
    if (observed == kExhausted)
        return Error(ErrorCode::counter_exhausted,
                     "cannot raise a counter past its last value",
                     std::string(context));
    if (observed + 1 > m_next) m_next = observed + 1;
    return Result<void>::success();
}

Result<void> CheckedCounter::restore(uint64_t next_value, std::string_view context) {
    if (next_value < m_next)
        return Error(ErrorCode::counter_regression,
                     "restored counter " + std::to_string(next_value) +
                         " is behind the current high-water mark " +
                         std::to_string(m_next),
                     std::string(context));
    m_next = next_value;
    return Result<void>::success();
}

// ============================================================================
// WriterLease
// ============================================================================

std::string WriterLease::describe() const {
    return holder.valid() ? holder.to_string() + std::string(" (pid ") +
                                std::to_string(pid) + ")"
                          : std::string("unknown holder");
}

// ============================================================================
// ReplicaState
// ============================================================================

Result<ReplicaState> ReplicaState::open(std::filesystem::path state_dir) {
    return open(std::move(state_dir), &platform_random);
}

Result<ReplicaState> ReplicaState::open(std::filesystem::path state_dir,
                                        const ActorId::Source& source) {
    if (state_dir.empty())
        return Error(ErrorCode::io_error, "state directory path is empty");
    std::error_code ec;
    fs::create_directories(state_dir, ec);
    if (ec)
        return Error(ErrorCode::io_error,
                     "cannot create state directory: " + ec.message(),
                     state_dir.string());

    ReplicaState state;
    state.m_state_dir = state_dir;

    const auto path = state_file(state_dir);
    if (fs::exists(path, ec)) {
        auto loaded = state.load();
        if (!loaded.ok()) return loaded.error().with_context("loading " + path.string());
        return state;
    }

    // First run: mint an identity and persist it *before* returning, so a crash
    // after this point cannot mint a second actor for the same device.
    auto actor = ActorId::generate(source);
    if (!actor.ok()) return actor.error().with_context("creating " + path.string());
    state.m_actor = std::move(actor).value();
    auto persisted = state.persist();
    if (!persisted.ok()) return persisted.error().with_context("creating " + path.string());
    return state;
}

Result<void> ReplicaState::load() {
    const auto path = state_file(m_state_dir);
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return Error(ErrorCode::corrupt_record, "state file cannot be read", path.string());

    std::string line;
    bool saw_magic = false;
    ActorId actor;
    std::vector<std::pair<StreamId, StreamCounters>> streams;
    size_t line_number = 0;

    while (std::getline(file, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::istringstream stream(line);
        std::string keyword;
        stream >> keyword;

        if (!saw_magic) {
            if (keyword != kStateMagic)
                return Error(ErrorCode::unknown_format,
                             "not a replica state file", path.string());
            std::string version_text;
            stream >> version_text;
            std::string extra;
            if (stream >> extra)
                return Error(ErrorCode::malformed, "trailing data after the version",
                             path.string() + ":" + std::to_string(line_number));
            auto version = parse_u64(version_text, "state version");
            if (!version.ok())
                return version.error().with_context(path.string());
            if (version.value() != kStateVersion)
                return Error(ErrorCode::unsupported_version,
                             "replica state version " + std::to_string(version.value()) +
                                 " is not supported by this build",
                             path.string());
            saw_magic = true;
            continue;
        }

        // Complete consumption per line: trailing junk is malformed, not
        // ignored. An ignored field here would be a lost counter.
        std::string trailing;
        if (keyword == "actor") {
            std::string text;
            stream >> text;
            if (stream >> trailing)
                return Error(ErrorCode::malformed,
                             "trailing data after the actor directive",
                             path.string() + ":" + std::to_string(line_number));
            auto parsed = ActorId::parse(text);
            if (!parsed.ok())
                return parsed.error().with_context(
                    path.string() + ":" + std::to_string(line_number));
            actor = std::move(parsed).value();
            continue;
        }
        if (keyword == "stream") {
            std::string name;
            std::string next_text;
            stream >> name >> next_text;
            if (stream >> trailing)
                return Error(ErrorCode::malformed,
                             "trailing data after the stream directive",
                             path.string() + ":" + std::to_string(line_number));
            if (name.empty() || next_text.empty())
                return Error(ErrorCode::malformed,
                             "stream directive needs a name and a counter",
                             path.string() + ":" + std::to_string(line_number));
            auto next = parse_u64(next_text, "stream counter");
            if (!next.ok())
                return next.error().with_context(
                    path.string() + ":" + std::to_string(line_number));
            StreamCounters counters;
            auto restored = counters.counter.restore(next.value(), name);
            if (!restored.ok()) return restored.error();
            streams.emplace_back(std::move(name), counters);
            continue;
        }
        // An unknown directive is malformed, not skipped: skipping it would drop
        // a counter and reissue identifiers.
        return Error(ErrorCode::malformed,
                     "unknown directive '" + keyword + "'",
                     path.string() + ":" + std::to_string(line_number));
    }

    if (!saw_magic)
        return Error(ErrorCode::unknown_format, "empty or truncated state file",
                     path.string());
    if (!actor.valid())
        return Error(ErrorCode::corrupt_record,
                     "state file has no valid actor identity", path.string());

    m_actor = actor;
    m_streams = std::move(streams);
    return Result<void>::success();
}

Result<void> ReplicaState::persist() const {
    std::ostringstream out;
    out << kStateMagic << ' ' << kStateVersion << '\n';
    out << "actor " << m_actor.to_string() << '\n';
    for (const auto& [name, counters] : m_streams)
        out << "stream " << name << ' ' << counters.counter.peek() << '\n';
    return write_atomic(state_file(m_state_dir), out.str());
}

Result<ReplicaState::StreamCounters*> ReplicaState::stream(const StreamId& name) {
    for (auto& [existing, counters] : m_streams)
        if (existing == name) return &counters;
    m_streams.emplace_back(name, StreamCounters{});
    return &m_streams.back().second;
}

Result<OperationId> ReplicaState::next_operation(const StreamId& stream_name,
                                                  const DocumentId& document,
                                                  uint64_t epoch) {
    if (stream_name.empty())
        return Error(ErrorCode::unknown_stream, "stream name is empty");
    if (document.empty())
        return Error(ErrorCode::malformed, "document id is empty");
    if (!m_actor.valid())
        return Error(ErrorCode::invalid_identity,
                     "replica state has no actor identity");

    auto counters_result = this->stream(stream_name);
    if (!counters_result.ok())
        return counters_result.error().with_context("creating stream " + stream_name);
    StreamCounters* counters = counters_result.value();

    auto reserved = counters->counter.reserve(stream_name);
    if (!reserved.ok()) return reserved.error().with_context("reserving operation id");

    // Persist the reservation *before* handing the id out. An id that exists only
    // in memory is an id a crash will hand out twice — finding F4.
    //
    // If persisting fails the reservation is deliberately NOT rolled back: the
    // value stays consumed so it can never be reissued. A skipped id is a gap in
    // numbering; a reissued id is a duplicated identity.
    if (auto persisted = persist(); !persisted.ok())
        return persisted.error().with_context("persisting operation reservation for " +
                                               stream_name);

    OperationId id;
    id.actor = m_actor;
    id.counter = reserved.value();
    (void)document;
    (void)epoch;
    return id;
}

Result<uint64_t> ReplicaState::peek_stream(const StreamId& stream_name) const {
    for (const auto& [name, counters] : m_streams)
        if (name == stream_name) return counters.counter.peek();
    return Error(ErrorCode::unknown_stream, "no counter for stream '" + stream_name + "'");
}

uint64_t ReplicaState::high_water(const StreamId& stream_name) const {
    for (const auto& [name, counters] : m_streams)
        if (name == stream_name)
            return counters.counter.peek() ? counters.counter.peek() - 1 : 0;
    return 0;
}

std::filesystem::path ReplicaState::lease_path(const DocumentId& document,
                                               const StreamId& stream_name) const {
    std::string safe_doc = document;
    for (char& c : safe_doc)
        if (c == '/' || c == '\\') c = '_';
    return m_state_dir /
           ("writer-" + safe_doc + "." + stream_name + std::string(kLeaseSuffix));
}

Result<WriterLease> ReplicaState::acquire_writer(const DocumentId& document,
                                                  const StreamId& stream_name) {
    const auto path = lease_path(document, stream_name);
    std::error_code ec;
    fs::create_directories(m_state_dir, ec);

    // O_CREAT|O_EXCL is the whole mechanism: the create either wins or it does
    // not, with no window in between.
    const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) {
        std::string holder = "unknown";
        std::ifstream existing(path, std::ios::binary);
        std::string line;
        if (existing && std::getline(existing, line))
            holder = line;
        return Error(ErrorCode::writer_conflict,
                     "another local writer holds this stream (holder: " + holder + ")",
                     path.string());
    }

    WriterLease lease;
    lease.holder = m_actor;
    lease.pid = static_cast<uint64_t>(::getpid());
    lease.path = path;
    const std::string record = m_actor.to_string() + " " + std::to_string(lease.pid) + "\n";
    const ssize_t written = ::write(fd, record.data(), record.size());
    const bool synced = ::fsync(fd) == 0;
    ::close(fd);
    if (written != static_cast<ssize_t>(record.size()) || !synced) {
        fs::remove(path, ec);
        return Error(ErrorCode::io_error, "cannot record writer lease", path.string());
    }
    fsync_directory(m_state_dir);
    m_leases.push_back(lease);
    return lease;
}

void ReplicaState::release_writer() {
    std::error_code ec;
    for (const auto& lease : m_leases) fs::remove(lease.path, ec);
    m_leases.clear();
}

bool ReplicaState::holds_writer(const DocumentId& document,
                                const StreamId& stream_name) const {
    const auto path = lease_path(document, stream_name);
    for (const auto& lease : m_leases)
        if (lease.path == path) return true;
    return false;
}

Result<ActorId> ReplicaState::fork_identity(std::string_view reason) {
    auto actor = ActorId::generate();
    if (!actor.ok()) return actor.error().with_context("forking identity");
    // A fork starts a new numbering space: the old actor's counters belong to
    // the old identity, and reusing them under a new id would be fine — but
    // keeping them would make the two histories indistinguishable in a log.
    m_actor = std::move(actor).value();
    m_streams.clear();
    if (auto persisted = persist(); !persisted.ok())
        return persisted.error().with_context("forking identity");
    return m_actor;
}

Result<void> ReplicaState::check_claim(const StreamId& stream_name,
                                       const ActorId& claimant, uint64_t their_counter,
                                       std::string_view context) const {
    if (!claimant.valid())
        return Error(ErrorCode::invalid_identity, "operation claims an invalid actor",
                     std::string(context));
    if (claimant != m_actor) return Result<void>::success();

    const uint64_t ours = high_water(stream_name);
    if (their_counter <= ours)
        return Error(ErrorCode::actor_conflict,
                     "another writer is producing operations as " +
                         m_actor.to_string() + " on stream " + stream_name +
                         " at counter " + std::to_string(their_counter) +
                         ", which this device already issued",
                     std::string(context));
    // They are ahead of our persisted high-water mark: our own state is
    // truncated or corrupted, which is a local problem and must not be papered
    // over by adopting their number.
    return Error(ErrorCode::corrupt_record,
                 "operation from this actor at counter " +
                     std::to_string(their_counter) + " is ahead of the local "
                     "high-water mark " + std::to_string(ours) +
                     " on stream " + stream_name,
                 std::string(context));
}

Result<void> ReplicaState::flush() { return persist(); }

}  // namespace CollabText
