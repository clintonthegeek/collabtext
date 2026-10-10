#include "transport/StreamSync.h"
#include <collabtext/Serialization.h>
#include <collabtext/Operations.h>
#include "crdt/StreamSerialization.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <limits>
#include <array>
#include <cstring>

namespace fs = std::filesystem;

namespace CollabText::Crdt {

namespace {
std::string scope_key(const StreamScope& s) { return s.stream + "\n" + std::to_string(s.epoch); }
bool valid_scope(const StreamScope& s) {
    if (s.stream.empty() || s.stream == "." || s.stream == ".." || s.stream.size() > 128 || s.epoch == 0) return false;
    for (unsigned char c : s.stream)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
              || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ':')) return false;
    return true;
}
std::string stream_dir_name(std::string_view stream) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out; out.reserve(stream.size() * 2);
    for (unsigned char c : stream) { out.push_back(hex[c >> 4]); out.push_back(hex[c & 15]); }
    return out;
}
void put_u64(std::string& out, uint64_t v) {
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<char>(v >> (i * 8)));
}
bool take_u64(std::string_view& in, uint64_t& v) {
    if (in.size() < 8) return false;
    v = 0; for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<uint8_t>(in[i]);
    in.remove_prefix(8); return true;
}
std::string encode_delivery(const OutboundDelivery& d) {
    std::string out("CTDL\1", 5);
    put_u64(out, d.token.stream.size()); out += d.token.stream;
    put_u64(out, d.token.epoch);
    const auto actor = d.token.producer.bytes();
    out.append(reinterpret_cast<const char*>(actor.data()), actor.size());
    put_u64(out, d.token.sequence); put_u64(out, d.payload.size()); out += d.payload;
    return out;
}
std::optional<InboundDelivery> decode_delivery(std::string_view in) {
    if (in.size() < 5 || in.substr(0, 5) != std::string_view("CTDL\1", 5)) return {};
    in.remove_prefix(5); uint64_t n = 0;
    if (!take_u64(in, n) || n == 0 || n > 128 || in.size() < n) return {};
    InboundDelivery d; d.token.stream.assign(in.substr(0, n)); in.remove_prefix(n);
    if (!take_u64(in, d.token.epoch) || in.size() < ActorId::kBytes) return {};
    std::array<uint8_t, ActorId::kBytes> bytes{};
    std::memcpy(bytes.data(), in.data(), bytes.size()); in.remove_prefix(bytes.size());
    d.token.producer = ActorId::from_bytes(bytes);
    uint64_t payload_size = 0;
    if (!take_u64(in, d.token.sequence) || !take_u64(in, payload_size)
        || payload_size != in.size() || d.token.epoch == 0 || d.token.sequence == 0
        || !d.token.producer.valid()) return {};
    d.payload.assign(in); return d;
}
std::string encode_receipt_value(uint64_t seq, const RecoveryPoint& point) {
    std::string out("CTRC\1", 5); put_u64(out, seq);
    out.push_back(point.kind() == RecoveryPoint::Kind::checkpoint ? 1 : 0);
    put_u64(out, point.checkpoint_id().size()); out.append(point.checkpoint_id()); return out;
}
struct ReceiptValue { uint64_t sequence = 0; RecoveryPoint point = RecoveryPoint::replay_from_beginning(); };
std::optional<ReceiptValue> decode_receipt_value(std::string_view in) {
    if (in.size() < 5 || in.substr(0, 5) != std::string_view("CTRC\1", 5)) return {};
    in.remove_prefix(5); ReceiptValue v; uint64_t n = 0;
    if (!take_u64(in, v.sequence) || in.empty()) return {};
    bool checkpoint = in.front() != 0; in.remove_prefix(1);
    if (!take_u64(in, n) || n != in.size() || n > 1024 || (checkpoint && n == 0)) return {};
    if (checkpoint) v.point = RecoveryPoint::checkpoint(std::string(in));
    if (v.sequence == 0) return {}; return v;
}
std::optional<std::string> read_bytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary); if (!f) return {};
    return std::string(std::istreambuf_iterator<char>(f), {});
}
}

struct FileOpStream::ScopeState {
    StreamScope scope;
    InboundHandler handler;
    std::unordered_map<std::string, ReceiptValue> frontiers;
    std::unordered_map<std::string, ReceiptValue> remote_receipts;
};
struct FileOpStream::WriterState {
    StreamScope scope;
    std::unique_ptr<SegmentWriter> writer;
    uint64_t next = 1;
    std::unordered_map<uint64_t, std::string> payloads;
};

FileOpStream::FileOpStream(fs::path folder, ActorId actor, WriterConfig cfg)
    : m_shared_folder(std::move(folder)), m_local_actor(actor), m_writer_cfg(cfg) {}
FileOpStream::~FileOpStream() = default;

Result<void> FileOpStream::start() {
    if (m_started) return {};
    if (!m_local_actor.valid()) return Error(ErrorCode::invalid_identity, "file transport requires a valid actor");
    auto local = m_shared_folder / "local" / m_local_actor.to_string();
    fs::create_directories(local);
    auto lease = Storage::ProcessLease::acquire(local / "writer.lock", m_writer_cfg.file_ops);
    if (!lease.ok()) return lease.error();
    m_lease.emplace(std::move(lease.value())); m_started = true;
    return {};
}

Result<void> FileOpStream::replay_all(
    const StreamScope& scope,
    const std::function<Result<void>(const InboundDelivery&)>& visitor) const {
    if (!m_scopes.contains(scope_key(scope)))
        return Error(ErrorCode::unknown_stream, "stream scope is not registered");
    if (!visitor) return Error(ErrorCode::malformed, "full replay visitor is empty");
    const auto replicas = m_shared_folder / "replicas";
    if (!fs::exists(replicas)) return {};
    for (const auto& peer : fs::directory_iterator(replicas)) {
        if (!peer.is_directory()) continue;
        auto actor = ActorId::parse(peer.path().filename().string());
        if (!actor.ok()) continue;
        const auto dir = peer.path() / "log" / "streams" / stream_dir_name(scope.stream)
                       / std::to_string(scope.epoch);
        if (!fs::exists(dir)) continue;
        auto cursor = m_shared_folder / "local" / m_local_actor.to_string() / "read-cursors"
                    / "replay" / (stream_dir_name(scope.stream) + "-" + std::to_string(scope.epoch)
                                   + "-" + actor.value().to_string() + ".bin");
        SegmentReader reader(dir, cursor);
        reader.start();
        uint64_t expected = 1;
        std::string last_payload;
        for (;;) {
            auto batch = reader.read_new();
            if (batch.gap) return batch.gap->with_context("full session replay");
            if (batch.records.empty()) break;
            for (const auto& raw : batch.records) {
                auto delivery = decode_delivery(raw);
                if (!delivery || delivery->token.stream != scope.stream
                    || delivery->token.epoch != scope.epoch || delivery->token.producer != actor.value())
                    return Error(ErrorCode::corrupt_record, "invalid or misplaced delivery in full replay",
                                 peer.path().string());
                const auto sequence = delivery->token.sequence;
                if (sequence == expected) {
                    ++expected; last_payload = delivery->payload;
                    auto visited = visitor(*delivery);
                    if (!visited.ok()) return visited.error();
                } else if (sequence == expected - 1 && delivery->payload == last_payload) {
                    // A retry after an uncertain append can leave an identical frame.
                } else {
                    return Error(ErrorCode::counter_regression,
                                 "producer delivery log is not a contiguous idempotent sequence",
                                 peer.path().string());
                }
            }
            auto committed = reader.commit(batch.token);
            if (!committed.ok()) return committed.error();
        }
    }
    return {};
}

Result<void> FileOpStream::replay_producer(
    const StreamScope& scope, const ActorId& producer,
    const std::function<Result<void>(const InboundDelivery&)>& visitor) const {
    if (!m_scopes.contains(scope_key(scope)))
        return Error(ErrorCode::unknown_stream, "stream scope is not registered");
    if (!producer.valid() || !visitor)
        return Error(ErrorCode::malformed, "producer replay requires a valid actor and visitor");

    const auto peer = m_shared_folder / "replicas" / producer.to_string();
    const auto dir = peer / "log" / "streams" / stream_dir_name(scope.stream)
                   / std::to_string(scope.epoch);
    if (!fs::exists(dir)) return {};

    // No checkpoint is supplied, and commit() below only advances this reader
    // in memory. Export scans therefore never consume a receiver cursor.
    SegmentReader reader(dir, {});
    reader.start();
    uint64_t expected = 1;
    std::string last_payload;
    for (;;) {
        auto batch = reader.read_new();
        if (batch.gap) return batch.gap->with_context("producer branch export");
        for (const auto& raw : batch.records) {
            auto delivery = decode_delivery(raw);
            if (!delivery || delivery->token.stream != scope.stream
                || delivery->token.epoch != scope.epoch
                || delivery->token.producer != producer)
                return Error(ErrorCode::corrupt_record,
                             "invalid or misplaced producer delivery in branch export",
                             peer.string());
            const auto sequence = delivery->token.sequence;
            if (sequence == UINT64_MAX)
                return Error(ErrorCode::counter_exhausted,
                             "producer branch delivery sequence is exhausted",
                             peer.string());
            if (sequence == expected) {
                ++expected;
                last_payload = delivery->payload;
                auto visited = visitor(*delivery);
                if (!visited.ok()) return visited.error();
            } else if (sequence == expected - 1 && delivery->payload == last_payload) {
                // An uncertain append retry may leave an identical frame.
            } else {
                return Error(ErrorCode::counter_regression,
                             "producer branch sequence is not contiguous and idempotent",
                             peer.string());
            }
        }
        if (batch.records.empty()) break;
        auto committed = reader.commit(batch.token);
        if (!committed.ok()) return committed.error();
    }
    return {};
}

Result<void> FileOpStream::flush() {
    for (auto& [key, state] : m_writers) {
        (void)key;
        auto result = state->writer->flush();
        if (!result.ok()) return result.error();
    }
    return {};
}

Result<void> FileOpStream::close() {
    for (auto& [key, state] : m_writers) {
        (void)key;
        auto result = state->writer->close();
        if (!result.ok()) return result.error();
    }
    m_writers.clear();
    auto released = m_lease ? m_lease->release() : Result<void>::success();
    if (!released.ok()) return released.error();
    m_lease.reset(); m_started = false;
    return {};
}

Result<void> FileOpStream::register_stream(const StreamScope& scope) {
    if (!valid_scope(scope)) return Error(ErrorCode::malformed, "invalid stream scope");
    auto key = scope_key(scope); auto found = m_scopes.find(key);
    if (found != m_scopes.end()) return {};
    if (!m_started) { auto r = start(); if (!r.ok()) return r.error(); }
    auto state = std::make_unique<ScopeState>(); state->scope = scope;
    auto receipt_dir = m_shared_folder / "local" / m_local_actor.to_string() / "receipts"
                    / stream_dir_name(scope.stream) / std::to_string(scope.epoch);
    fs::create_directories(receipt_dir);
    for (const auto& e : fs::directory_iterator(receipt_dir)) {
        if (!e.is_regular_file()) continue;
        auto actor = ActorId::parse(e.path().filename().string()); if (!actor.ok()) continue;
        auto bytes = read_bytes(e.path()); if (!bytes) continue;
        auto value = decode_receipt_value(*bytes); if (value) state->frontiers.emplace(actor.value().to_string(), *value);
    }
    m_scopes.emplace(key, std::move(state)); return {};
}

Result<void> FileOpStream::push(const OutboundDelivery& d, PushDurability durability) {
    if (!m_started || !m_lease) return Error(ErrorCode::writer_conflict, "file transport is not started");
    if (!d.token.producer.valid() || d.token.producer != m_local_actor || d.token.sequence == 0)
        return Error(ErrorCode::invalid_identity, "outbound token producer or sequence is invalid");
    if (d.token.sequence == UINT64_MAX) return Error(ErrorCode::counter_exhausted, "delivery sequence exhausted");
    StreamScope scope{d.token.stream, d.token.epoch}; auto si = m_scopes.find(scope_key(scope));
    if (si == m_scopes.end()) return Error(ErrorCode::unknown_stream, "stream scope is not registered", d.token.stream);
    auto key = scope_key(scope); auto wi = m_writers.find(key);
    if (wi == m_writers.end()) {
        auto dir = m_shared_folder / "replicas" / m_local_actor.to_string() / "log" / "streams"
                 / stream_dir_name(scope.stream) / std::to_string(scope.epoch);
        fs::create_directories(dir);
        auto ws = std::make_unique<WriterState>(); ws->scope = scope;
        ws->writer = std::make_unique<SegmentWriter>(dir, SegmentKind::Stream, m_writer_cfg);
        auto started = ws->writer->start(); if (!started.ok()) return started.error();
        SegmentReader scan(dir, m_shared_folder / "local" / m_local_actor.to_string() / "read-cursors"
                          / (stream_dir_name(scope.stream) + "-" + std::to_string(scope.epoch) + ".scan"));
        scan.start();
        uint64_t expected = 1;
        for (;;) {
            auto batch = scan.read_new(); if (batch.gap) return *batch.gap;
            for (const auto& raw : batch.records) {
                auto old = decode_delivery(raw); if (!old) return Error(ErrorCode::corrupt_record, "invalid delivery in producer log");
                if (old->token.stream != scope.stream || old->token.epoch != scope.epoch || old->token.producer != m_local_actor)
                    return Error(ErrorCode::corrupt_record, "producer log contains mismatched token");
                if (old->token.sequence == UINT64_MAX)
                    return Error(ErrorCode::counter_exhausted, "producer delivery sequence exhausted");
                auto [at, inserted] = ws->payloads.emplace(old->token.sequence, old->payload);
                if (!inserted && at->second != old->payload)
                    return Error(ErrorCode::actor_conflict, "producer log reuses a delivery token");
                if (old->token.sequence == expected) ++expected;
                else if (old->token.sequence != expected - 1)
                    return Error(old->token.sequence < expected ? ErrorCode::counter_regression : ErrorCode::counter_regression,
                                 "producer log sequence is not contiguous");
            }
            if (batch.records.empty()) break;
            auto committed = scan.commit(batch.token); if (!committed.ok()) return committed.error();
        }
        ws->next = expected;
        wi = m_writers.emplace(key, std::move(ws)).first;
    }
    auto& w = *wi->second; const auto existing = w.payloads.find(d.token.sequence);
    if (existing != w.payloads.end()) {
        if (existing->second != d.payload)
            return Error(ErrorCode::actor_conflict, "delivery token was reused with different bytes");
        if (durability == PushDurability::durable) {
            auto reconciled = w.writer->flush();
            if (!reconciled.ok()) return reconciled.error();
        }
        return {};
    }
    if (d.token.sequence != w.next) return Error(d.token.sequence < w.next ? ErrorCode::counter_regression : ErrorCode::counter_regression,
                                                   "delivery sequence must be contiguous");
    auto a = w.writer->append(encode_delivery(d), d.token.sequence); if (!a.ok()) return a.error();
    if (durability == PushDurability::durable) { auto f = w.writer->flush(); if (!f.ok()) return f.error(); }
    w.payloads.emplace(d.token.sequence, d.payload);
    ++w.next; return {};
}

Result<SubscriptionId> FileOpStream::subscribe(const StreamScope& scope, InboundHandler handler) {
    auto r = register_stream(scope); if (!r.ok()) return r.error();
    if (!handler) return Error(ErrorCode::malformed, "inbound handler is empty");
    auto& state = *m_scopes.at(scope_key(scope)); if (state.handler) return Error(ErrorCode::writer_conflict, "scope already has an inbound handler");
    auto id = m_next_subscription++; state.handler = std::move(handler); m_subscriptions.emplace(id, scope); return id;
}
Result<SubscriptionId> FileOpStream::subscribe_receipts(ReceiptHandler handler) {
    if (!handler) return Error(ErrorCode::malformed, "receipt handler is empty");
    auto id = m_next_subscription++; m_receipt_subscriptions.emplace(id, std::move(handler)); return id;
}
Result<void> FileOpStream::unsubscribe(SubscriptionId id) {
    if (auto it = m_subscriptions.find(id); it != m_subscriptions.end()) {
        m_scopes.at(scope_key(it->second))->handler = {}; m_subscriptions.erase(it);
    }
    m_receipt_subscriptions.erase(id); return {};
}
Result<std::vector<StreamReceipt>> FileOpStream::receipts(const StreamScope& scope) const {
    auto si = m_scopes.find(scope_key(scope)); if (si == m_scopes.end()) return Error(ErrorCode::unknown_stream, "stream scope is not registered");
    std::vector<StreamReceipt> out;
    for (const auto& [actor_hex, v] : si->second->remote_receipts) {
        auto actor = ActorId::parse(actor_hex); if (!actor.ok()) continue;
        out.push_back({actor.value(), DeliveryToken{scope.stream, scope.epoch, m_local_actor, v.sequence}});
    }
    return out;
}

void FileOpStream::refresh_receipts_(ScopeState& state) {
    const auto local_dir = m_shared_folder / "local";
    if (!fs::exists(local_dir)) return;
    const auto receipt_rel = fs::path("receipts") / stream_dir_name(state.scope.stream)
                           / std::to_string(state.scope.epoch) / m_local_actor.to_string();
    for (const auto& peer : fs::directory_iterator(local_dir)) {
        if (!peer.is_directory() || peer.path().filename() == m_local_actor.to_string()) continue;
        auto actor = ActorId::parse(peer.path().filename().string()); if (!actor.ok()) continue;
        auto bytes = read_bytes(peer.path() / receipt_rel); if (!bytes) continue;
        auto value = decode_receipt_value(*bytes); if (!value) continue;
        auto key = actor.value().to_string(); auto existing = state.remote_receipts.find(key);
        if (existing != state.remote_receipts.end() && existing->second.sequence >= value->sequence) continue;
        state.remote_receipts[key] = *value;
        StreamReceipt receipt{actor.value(), DeliveryToken{state.scope.stream, state.scope.epoch,
                                                            m_local_actor, value->sequence}};
        std::vector<ReceiptHandler> handlers;
        handlers.reserve(m_receipt_subscriptions.size());
        for (const auto& [id, cb] : m_receipt_subscriptions) { (void)id; handlers.push_back(cb); }
        for (auto& cb : handlers) try { cb(receipt); } catch (...) { /* observation cannot roll back a remote receipt */ }
    }
}

Result<PollReport> FileOpStream::poll(const StreamScope& scope, PollBudget budget) {
    auto si = m_scopes.find(scope_key(scope)); if (si == m_scopes.end()) return Error(ErrorCode::unknown_stream, "stream scope is not registered");
    auto& state = *si->second; refresh_receipts_(state);
    PollReport report; if (!state.handler) return report;
    const auto handler = state.handler;
    auto replicas = m_shared_folder / "replicas"; if (!fs::exists(replicas)) return report;
    bool stop_all = false;
    for (const auto& peer : fs::directory_iterator(replicas)) {
        if (!peer.is_directory() || peer.path().filename() == m_local_actor.to_string()) continue;
        auto actor = ActorId::parse(peer.path().filename().string()); if (!actor.ok()) continue;
        auto dir = peer.path() / "log" / "streams" / stream_dir_name(scope.stream) / std::to_string(scope.epoch);
        if (!fs::exists(dir)) continue;
        SegmentReader reader(dir, m_shared_folder / "local" / m_local_actor.to_string() / "read-cursors"
                            / (stream_dir_name(scope.stream) + "-" + std::to_string(scope.epoch) + "-" + actor.value().to_string() + ".poll"));
        reader.start();
        bool stop_peer = false;
        for (;;) {
            auto batch = reader.read_new();
            if (batch.gap) return *batch.gap;
            if (batch.records.empty()) break;
            for (const auto& raw : batch.records) {
                auto d = decode_delivery(raw); if (!d) return Error(ErrorCode::corrupt_record, "invalid delivery envelope");
                if (d->token.stream != scope.stream || d->token.epoch != scope.epoch || d->token.producer != actor.value())
                    return Error(ErrorCode::corrupt_record, "delivery token does not match its log scope");
                const auto front = state.frontiers.find(actor.value().to_string());
                const uint64_t accepted = front == state.frontiers.end() ? 0 : front->second.sequence;
                if (d->token.sequence <= accepted) continue;
                if (report.offered >= budget.max_records || d->payload.size() > budget.max_payload_bytes - std::min(budget.max_payload_bytes, report.payload_bytes)) {
                    report.budget_limited = true; stop_peer = true; stop_all = true; break;
                }
                if (d->token.sequence != accepted + 1) return Error(ErrorCode::counter_regression, "delivery log has a sequence gap");
                ++report.offered; report.payload_bytes += d->payload.size();
                auto decision = [&] {
                    try { return handler(*d); }
                    catch (...) { return DeliveryDecision::retry(); }
                }();
                if (decision.kind() == DeliveryDecision::Kind::retry) { ++report.retried; stop_peer = true; break; }
                if (decision.kind() == DeliveryDecision::Kind::quarantine) { ++report.quarantined; stop_peer = true; break; }
                if (!decision.recovery_point() || !decision.recovery_point()->valid()
                    || decision.recovery_point()->checkpoint_id().size() > 1024)
                    return Error(ErrorCode::malformed, "commit decision has no valid recovery point");
                auto receipt_dir = m_shared_folder / "local" / m_local_actor.to_string() / "receipts" / stream_dir_name(scope.stream) / std::to_string(scope.epoch);
                fs::create_directories(receipt_dir);
                auto encoded = encode_receipt_value(d->token.sequence, *decision.recovery_point());
                auto persisted = Storage::replace_atomically(receipt_dir / actor.value().to_string(), encoded, m_writer_cfg.step_hook, m_writer_cfg.file_ops);
                if (!persisted.ok()) return persisted.error();
                state.frontiers[actor.value().to_string()] = {d->token.sequence, *decision.recovery_point()};
                ++report.committed;
            }
            if (stop_peer) break;
            auto c = reader.commit(batch.token); if (!c.ok()) return c.error();
        }
        if (stop_all) break;
    }
    return report;
}

StreamSync::StreamSync(const fs::path& shared_folder,
                       const std::string& replica_name,
                       uint16_t replica_id,
                       WriterConfig cfg)
    : m_shared_folder(shared_folder)
    , m_replica_name(replica_name)
    , m_replica_id(replica_id)
    , m_writer_cfg(cfg) {}

void StreamSync::start() {
    if (m_started) return;
    auto base = m_shared_folder / "replicas" / m_replica_name / "log" / "streams";
    fs::create_directories(base);
    auto local = m_shared_folder / "local" / m_replica_name;
    fs::create_directories(local / "read-cursors" / "streams");
    auto lease = Storage::ProcessLease::acquire(local / "writer.lock",
                                                 m_writer_cfg.file_ops);
    if (!lease.ok()) { m_last_error = lease.error(); return; }
    m_writer_lease.emplace(std::move(lease.value()));
    for (auto& [name, state] : m_streams) {
        ensure_started_(name);
        if (m_last_error) return;
    }
    m_started = true;
}

StreamSync::StreamState& StreamSync::ensure_started_(const std::string& name) {
    auto& s = m_streams[name];
    if (s.writer) return s;
    auto stream_dir = m_shared_folder / "replicas" / m_replica_name
                      / "log" / "streams" / name;
    fs::create_directories(stream_dir);
    s.writer = std::make_unique<SegmentWriter>(stream_dir, SegmentKind::Stream, m_writer_cfg);
    auto started = s.writer->start();
    if (!started.ok()) { m_last_error = started.error(); return s; }
    fs::create_directories(m_shared_folder / "local" / m_replica_name
                           / "read-cursors" / "streams" / name);
    if (!recover_next_sequence_(name, s)) {
        s.writer.reset();
        return s;
    }
    return s;
}

bool StreamSync::recover_next_sequence_(const std::string& stream, StreamState& state) {
    const auto peer_dir = m_shared_folder / "replicas" / m_replica_name
                          / "log" / "streams" / stream;
    const auto cursor = m_shared_folder / "local" / m_replica_name
                        / "read-cursors" / "streams" / (stream + ".sequence-scan");
    SegmentReader reader(peer_dir, cursor);
    reader.start();
    uint64_t highest = 0;
    for (;;) {
        auto batch = reader.read_new();
        if (batch.gap) {
            m_last_error = batch.gap->with_context("recovering producer sequence for " + stream);
            return false;
        }
        for (const auto& raw : batch.records) {
            auto entry = decode_stream_entry(raw);
            if (!entry) {
                m_last_error = Error(ErrorCode::corrupt_record,
                    "invalid producer record while recovering envelope sequence", stream);
                return false;
            }
            highest = std::max(highest, entry->seq);
        }
        if (batch.records.empty()) break;
        auto accepted = reader.commit(batch.token);
        if (!accepted.ok()) { m_last_error = accepted.error(); return false; }
    }
    if (highest == std::numeric_limits<uint64_t>::max()) {
        m_last_error = Error(ErrorCode::counter_exhausted,
                             "stream envelope sequence exhausted", stream);
        return false;
    }
    state.next_seq = highest + 1;
    state.sequence_recovered = true;
    return true;
}

void StreamSync::register_stream(const std::string& name, StreamType type) {
    if (m_streams.count(name)) return;
    m_streams[name].type = type;
    if (m_started) ensure_started_(name);
}

void StreamSync::push(const std::string& stream, const StreamEntry& entry) {
    if (!m_started || !m_writer_lease) {
        m_last_error = Error(ErrorCode::io_error, "StreamSync is not started");
        return;
    }
    auto it = m_streams.find(stream);
    if (it == m_streams.end()) return;
    auto& s = ensure_started_(stream);
    if (entry.replica_id == m_replica_id) {
        if (entry.seq == std::numeric_limits<uint64_t>::max()) {
            m_last_error = Error(ErrorCode::counter_exhausted,
                                 "stream envelope sequence exhausted", stream);
            return;
        }
        if (entry.seq != s.next_seq) {
            auto duplicate = s.merged.find(entry.id);
            if (entry.seq < s.next_seq && duplicate != s.merged.end()
                && duplicate->second.replica_id == entry.replica_id
                && duplicate->second.seq == entry.seq
                && duplicate->second.payload == entry.payload)
                return;
            m_last_error = Error(entry.seq < s.next_seq ? ErrorCode::actor_conflict
                                                        : ErrorCode::counter_regression,
                "producer envelope sequence is not contiguous", stream);
            return;
        }
    }
    auto appended = s.writer->append(encode_stream_entry(entry), entry.seq);
    if (!appended.ok()) {
        m_last_error = appended.error();
        return;
    }
    if (entry.replica_id == m_replica_id) {
        s.next_seq = entry.seq + 1;
    }
    if (it->second.type == StreamType::AppendOnly) {
        s.merged.try_emplace(entry.id, entry);
    } else {
        auto existing = s.merged.find(entry.id);
        if (existing == s.merged.end() || existing->second.timestamp < entry.timestamp)
            s.merged[entry.id] = entry;
    }
}

size_t StreamSync::poll() {
    if (!m_started) return 0;
    auto now = std::chrono::steady_clock::now();
    for (auto& [name, s] : m_streams) {
        (void)name;
        if (!s.writer) continue;
        auto ticked = s.writer->tick(now);
        if (!ticked.ok()) { m_last_error = ticked.error(); return 0; }
    }
    size_t total = 0;
    for (auto& [name, s] : m_streams) {
        size_t c = read_remote_stream_(name, s);
        if (c > 0) {
            total += c;
            if (m_on_new_entries) m_on_new_entries(name, c);
        }
    }
    write_acks_();
    recompute_fence_();
    return total;
}

void StreamSync::flush() {
    for (auto& [name, s] : m_streams) {
        (void)name;
        if (!s.writer) continue;
        auto flushed = s.writer->flush();
        if (!flushed.ok()) { m_last_error = flushed.error(); return; }
    }
}

SegmentReader& StreamSync::reader_for_(StreamState& s, const std::string& stream,
                                       const std::string& peer) {
    auto it = s.readers.find(peer);
    if (it != s.readers.end()) return *it->second;
    auto peer_stream_dir = m_shared_folder / "replicas" / peer / "log" / "streams" / stream;
    auto cursor_path = m_shared_folder / "local" / m_replica_name
                       / "read-cursors" / "streams" / stream / (peer + ".bin");
    auto r = std::make_unique<SegmentReader>(peer_stream_dir, cursor_path);
    r->start();
    auto [ins, _] = s.readers.emplace(peer, std::move(r));
    return *ins->second;
}

size_t StreamSync::read_remote_stream_(const std::string& name, StreamState& s) {
    auto replicas_dir = m_shared_folder / "replicas";
    if (!fs::exists(replicas_dir)) return 0;
    size_t count = 0;
    for (auto& dir_entry : fs::directory_iterator(replicas_dir)) {
        if (!dir_entry.is_directory()) continue;
        std::string peer = dir_entry.path().filename().string();
        if (peer == m_replica_name) continue;
        auto peer_stream_dir = dir_entry.path() / "log" / "streams" / name;
        if (!fs::exists(peer_stream_dir)) continue;
        if (s.blocked.contains(peer)) continue;
        auto& reader = reader_for_(s, name, peer);
        auto batch = reader.read_new();
        auto& records = batch.records;
        if (records.empty() && batch.gap) {
            m_last_error = batch.gap->with_context("StreamSync " + name + " peer " + peer);
            continue;
        }
        if (records.empty()) {
            auto committed = reader.commit(batch.token);
            if (!committed.ok())
                m_last_error = committed.error().with_context("StreamSync " + name + " peer " + peer);
            continue;
        }
        // Validate this batch before callbacks/merged state can see any entry.
        // Generic streams may carry arbitrary opaque payloads. Only recognized
        // operation families are interpreted here; P07 supplies stream binding.
        for (const auto& record : records) {
            auto entry = decode_stream_entry(record);
            Error error;
            if (!entry) error = Error(ErrorCode::corrupt_record, "invalid stream entry");
            else if (entry->payload.starts_with("CTBO")) {
                auto op = decode_operation(entry->payload);
                if (!op.ok()) error = op.error();
            } else if (entry->payload.starts_with("CTLO")) {
                auto op = decode_idlist_operation(entry->payload);
                if (!op.ok()) error = op.error();
            }
            if (error) {
                m_last_error = error.with_context("StreamSync " + name + " peer " + peer);
                s.blocked.emplace(peer, std::move(batch.records));
                break;
            }
        }
        if (s.blocked.contains(peer)) continue;
        const bool callback_missing = !m_on_inbound;
        if (callback_missing)
            m_last_error = Error(ErrorCode::writer_conflict,
                                 "inbound callback is not registered; records remain uncommitted", name);
        bool callback_failed = false;
        for (auto& r : records) {
            auto e = decode_stream_entry(r);
            if (!e) continue;
            auto existing = s.merged.find(e->id);
            const bool newer_anchor = s.type == StreamType::AnchorKeyed
                && (existing == s.merged.end() || e->timestamp > existing->second.timestamp);
            const bool needs_apply = callback_missing
                || !s.callback_completed.contains(e->id) || newer_anchor;
            if (needs_apply) {
                if (!callback_missing) {
                    try {
                        m_on_inbound(name, e->replica_id, e->payload);
                        s.callback_completed.insert(e->id);
                    } catch (...) {
                        m_last_error = Error(ErrorCode::io_error,
                                             "inbound callback threw; record retained", name);
                        callback_failed = true;
                        break;
                    }
                }
            }
            if (!callback_missing && !callback_failed) {
                if (auto op = decode_operation(e->payload)) {
                    const uint64_t lc = op_identity(op.value()).counter;
                    auto& ack = m_ack_state[e->replica_id];
                    if (lc > ack.max_lamport) {
                        ack.max_lamport = lc;
                        ack.last_observed_at = current_iso_utc_();
                    }
                }
            }
            if (s.type == StreamType::AppendOnly) {
                if (s.merged.try_emplace(e->id, *e).second) {
                    ++count;
                }
            } else {
                if (existing == s.merged.end()) {
                    s.merged[e->id] = *e;
                    ++count;
                } else if (e->timestamp > existing->second.timestamp) {
                    existing->second = *e;
                    ++count;
                }
            }
        }
        if (callback_failed || callback_missing) continue;
        auto committed = reader.commit(batch.token);
        if (!committed.ok()) {
            m_last_error = committed.error().with_context("StreamSync " + name + " peer " + peer);
            continue;
        }
        if (batch.gap) m_last_error = batch.gap->with_context("StreamSync " + name + " peer " + peer);
    }
    return count;
}

const std::vector<std::string>& StreamSync::blocked_records(const std::string& stream,
                                                           const std::string& peer) const {
    static const std::vector<std::string> empty;
    auto s = m_streams.find(stream);
    if (s == m_streams.end()) return empty;
    auto batch = s->second.blocked.find(peer);
    return batch == s->second.blocked.end() ? empty : batch->second;
}

std::vector<StreamEntry> StreamSync::entries(const std::string& stream) const {
    auto it = m_streams.find(stream);
    if (it == m_streams.end()) return {};
    std::vector<StreamEntry> result;
    result.reserve(it->second.merged.size());
    if (it->second.type == StreamType::AppendOnly) {
        for (auto& [id, entry] : it->second.merged) result.push_back(entry);
        std::sort(result.begin(), result.end(),
            [](const StreamEntry& a, const StreamEntry& b) {
                if (a.seq != b.seq) return a.seq < b.seq;
                return a.replica_id < b.replica_id;
            });
    } else {
        for (auto& [id, entry] : it->second.merged)
            if (!entry.tombstone) result.push_back(entry);
    }
    return result;
}

void StreamSync::set_on_new_entries(NewEntriesCallback cb) {
    m_on_new_entries = std::move(cb);
}

void StreamSync::set_on_inbound(InboundCallback cb) {
    m_on_inbound = std::move(cb);
}

std::string StreamSync::current_iso_utc_() const {
    auto now = std::chrono::system_clock::now();
    auto now_t = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&now_t, &utc);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buf;
}

void StreamSync::push(const std::string& stream_name, const std::string& payload) {
    if (!m_streams.count(stream_name)) return;
    auto& s = ensure_started_(stream_name);
    if (m_last_error || !s.sequence_recovered) return;
    if (s.next_seq == 0 || s.next_seq == std::numeric_limits<uint64_t>::max()) {
        m_last_error = Error(ErrorCode::counter_exhausted,
                             "stream envelope sequence exhausted", stream_name);
        return;
    }
    uint64_t seq = s.next_seq;
    StreamEntry entry;
    entry.replica_id = m_replica_id;
    entry.seq = seq;
    // next_seq resets on restart; Phase 4 will persist seq to avoid id collisions
    entry.id = m_replica_name + "-" + std::to_string(seq);
    entry.timestamp = current_iso_utc_();
    entry.payload = payload;
    push(stream_name, entry);
}

void StreamSync::write_acks_() {
    if (m_ack_state.empty()) return;

    // Build fresh map, skip peers with no observations
    std::unordered_map<uint16_t, AckState> merged = m_ack_state;
    for (auto it = merged.begin(); it != merged.end(); ) {
        if (it->second.max_lamport == 0) it = merged.erase(it);
        else ++it;
    }
    if (merged.empty()) return;

    auto acks_path = m_shared_folder / "replicas" / m_replica_name / "acks.json";

    // Read existing acks.json and enforce monotonicity
    if (fs::exists(acks_path)) {
        std::ifstream f(acks_path, std::ios::binary | std::ios::ate);
        if (!f) return;
        const auto file_size = f.tellg();
        if (file_size < 0 || file_size > 1024 * 1024) return;
        std::string content(static_cast<size_t>(file_size), '\0');
        f.seekg(0);
        f.read(content.data(), static_cast<std::streamsize>(content.size()));
        if (!f && !content.empty()) return;
        // Simple scan for "\"<id>\": { ... "max_lamport_observed": <N> ..." patterns
        // We look for each peer key and extract max_lamport_observed + last_observed_at
        size_t search_pos = 0;
        while (search_pos < content.size()) {
            // Find a quoted decimal key
            auto q1 = content.find('"', search_pos);
            if (q1 == std::string::npos) break;
            auto q2 = content.find('"', q1 + 1);
            if (q2 == std::string::npos) break;
            std::string key_str = content.substr(q1 + 1, q2 - q1 - 1);
            search_pos = q2 + 1;

            // Check if key_str is a decimal number (replica id)
            bool is_decimal = !key_str.empty();
            for (char c : key_str) if (c < '0' || c > '9') { is_decimal = false; break; }
            if (!is_decimal) continue;

            // Parse replica_id
            uint64_t rid_val = 0;
            for (char c : key_str) {
                rid_val = rid_val * 10 + (c - '0');
                if (rid_val > UINT16_MAX) { rid_val = UINT16_MAX + 1; break; }
            }
            if (rid_val > UINT16_MAX) continue;
            auto rid = static_cast<uint16_t>(rid_val);

            // Find max_lamport_observed value in the object following this key
            auto ml_tag = content.find("\"max_lamport_observed\"", search_pos);
            auto next_key_q = content.find('"', search_pos);
            // Only look ahead if max_lamport_observed comes before the next top-level key
            if (ml_tag == std::string::npos) continue;
            if (next_key_q != std::string::npos && next_key_q < ml_tag) continue;

            auto colon = content.find(':', ml_tag + 22);
            if (colon == std::string::npos) continue;
            size_t num_start = content.find_first_not_of(" \t\n\r", colon + 1);
            if (num_start == std::string::npos) continue;
            size_t num_end = num_start;
            while (num_end < content.size() && content[num_end] >= '0' && content[num_end] <= '9')
                ++num_end;
            if (num_end == num_start) continue;
            uint64_t stored_lc = 0;
            bool counter_overflow = false;
            for (size_t i = num_start; i < num_end; ++i)
            {
                const uint64_t digit = static_cast<uint64_t>(content[i] - '0');
                if (stored_lc > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
                    counter_overflow = true;
                    break;
                }
                stored_lc = stored_lc * 10 + digit;
            }
            if (counter_overflow) continue;

            // Also grab last_observed_at if stored_lc >= fresh (we preserve existing timestamp)
            std::string stored_ts;
            auto ts_tag = content.find("\"last_observed_at\"", search_pos);
            if (ts_tag != std::string::npos) {
                auto ts_colon = content.find(':', ts_tag + 18);
                if (ts_colon != std::string::npos) {
                    auto ts_q1 = content.find('"', ts_colon + 1);
                    if (ts_q1 != std::string::npos) {
                        auto ts_q2 = content.find('"', ts_q1 + 1);
                        if (ts_q2 != std::string::npos)
                            stored_ts = content.substr(ts_q1 + 1, ts_q2 - ts_q1 - 1);
                    }
                }
            }

            auto& ack = merged[rid];
            if (stored_lc > ack.max_lamport) {
                ack.max_lamport = stored_lc;
                ack.last_observed_at = stored_ts;
            }

            search_pos = num_end;
        }
    }

    // Check if anything changed (compare against what was last written by loading acks_path again)
    // Simple approach: always write if merged is non-empty — the caller already skips when
    // m_ack_state is empty. The file comparison would require storing last-written state;
    // instead we rely on the monotonicity read above and just write unconditionally when
    // there are observations. This is safe per the spec ("skip if nothing changed").
    // For now always write — future optimization can diff against a cached snapshot.

    // Build JSON
    std::string json = "{\n  \"schema_version\": 1,\n  \"acks\": {\n";
    bool first = true;
    for (auto& [rid, ack] : merged) {
        if (ack.max_lamport == 0) continue;
        if (!first) json += ",\n";
        first = false;
        json += "    \"" + std::to_string(rid) + "\": {\n";
        json += "      \"max_lamport_observed\": " + std::to_string(ack.max_lamport) + ",\n";
        json += "      \"last_observed_at\": \"" + ack.last_observed_at + "\"\n";
        json += "    }";
    }
    json += "\n  }\n}\n";

    if (fs::exists(acks_path)) {
        std::ifstream existing(acks_path, std::ios::binary | std::ios::ate);
        if (existing) {
            const auto size = existing.tellg();
            if (size >= 0 && static_cast<uint64_t>(size) == json.size()) {
                std::string old(static_cast<size_t>(size), '\0');
                existing.seekg(0);
                existing.read(old.data(), static_cast<std::streamsize>(old.size()));
                if (existing && old == json) return;
            }
        }
    }
    Storage::FileOpCounts counts;
    auto written = Storage::replace_atomically(acks_path, json,
        m_writer_cfg.step_hook, m_writer_cfg.file_ops, &counts);
    if (!written.ok()) m_last_error = written.error();
}

uint64_t StreamSync::lowest_peer_acked_lamport() const {
    return m_cached_fence;
}

void StreamSync::set_on_ack_update(std::function<void(uint64_t)> cb) {
    m_ack_update_cb = std::move(cb);
}

std::optional<uint64_t> decode_legacy_peer_ack(std::string_view content,
                                               uint16_t replica_id) {
    constexpr size_t kMaxAckBytes = 1u * 1024u * 1024u;
    if (content.size() > kMaxAckBytes) return std::nullopt;
    const std::string our_key = "\"" + std::to_string(replica_id) + "\"";
    const auto key_pos = content.find(our_key);
    if (key_pos == std::string_view::npos) return std::nullopt;
    const auto ml_tag = content.find("\"max_lamport_observed\"", key_pos + our_key.size());
    const auto object_end = content.find('}', key_pos + our_key.size());
    if (ml_tag == std::string_view::npos || object_end == std::string_view::npos
        || ml_tag > object_end) return std::nullopt;
    const auto colon = content.find(':', ml_tag + 22);
    if (colon == std::string_view::npos) return std::nullopt;
    size_t pos = colon + 1;
    while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t'
                                    || content[pos] == '\n' || content[pos] == '\r')) ++pos;
    const size_t start = pos;
    uint64_t value = 0;
    while (pos < content.size() && content[pos] >= '0' && content[pos] <= '9') {
        const uint64_t digit = static_cast<uint64_t>(content[pos] - '0');
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10)
            return std::nullopt;
        value = value * 10 + digit;
        ++pos;
    }
    if (pos == start) return std::nullopt;
    if (pos < content.size() && content[pos] != ',' && content[pos] != '}'
        && content[pos] != ' ' && content[pos] != '\t' && content[pos] != '\n'
        && content[pos] != '\r') return std::nullopt;
    return value;
}

uint64_t StreamSync::read_peer_ack_(const fs::path& acks_path) const {
    if (!fs::exists(acks_path)) return UINT64_MAX;

    std::ifstream f(acks_path, std::ios::binary | std::ios::ate);
    if (!f) return UINT64_MAX;
    const auto file_size = f.tellg();
    if (file_size < 0 || file_size > 1024 * 1024) return UINT64_MAX;
    std::string content(static_cast<size_t>(file_size), '\0');
    f.seekg(0);
    f.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (!f && !content.empty()) return UINT64_MAX;
    return decode_legacy_peer_ack(content, m_replica_id).value_or(UINT64_MAX);
}

void StreamSync::recompute_fence_() {
    auto replicas_dir = m_shared_folder / "replicas";
    if (!fs::exists(replicas_dir)) return;

    uint64_t new_fence = 0;
    bool any_peer = false;

    for (auto& dir_entry : fs::directory_iterator(replicas_dir)) {
        if (!dir_entry.is_directory()) continue;
        std::string peer = dir_entry.path().filename().string();
        if (peer == m_replica_name) continue;  // skip our own directory

        auto acks_path = dir_entry.path() / "acks.json";
        uint64_t peer_val = read_peer_ack_(acks_path);
        if (peer_val == UINT64_MAX) continue;  // no entry for us — excluded

        if (!any_peer) {
            new_fence = peer_val;
            any_peer = true;
        } else {
            new_fence = std::min(new_fence, peer_val);
        }
    }

    if (!any_peer) return;  // no enrolled peers yet — leave fence at current value

    if (new_fence > m_cached_fence) {
        m_cached_fence = new_fence;
        if (m_ack_update_cb) m_ack_update_cb(m_cached_fence);
    }
}

} // namespace CollabText::Crdt
