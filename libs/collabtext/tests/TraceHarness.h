// tests/TraceHarness.h — versioned event traces, deterministic replay, and
// delta reduction for the randomized suites.
//
// Roadmap task V04 ("Make randomized failures replayable and reducible").
//
// What a trace is
// ---------------
// A trace records *decisions*, never entropy and never derived operation
// identity. Each event names what a scenario chose to do (which replica, which
// byte range, which delivered operation, whether a peer was offline), so a
// replay re-executes the same choices. Operation Identity identifiers are
// re-derived by the engine exactly as they were during the recorded run: the
// trace never carries an identity it does not have to.
//
// A recorded run and a replayed run therefore agree byte-for-byte on replica
// text, which is what makes a CI failure a replayable artifact instead of a
// one-shot sample. See tests/TestSeed.h for the seed and artifact environment
// contract.
//
// Wire format (version 1)
// -----------------------
//   #collabtext-trace 1
//   #model buffer|idlist|networksim
//   #label <free text>
//   #seed <uint64>
//   #replicas <n>
//   #net <min_latency_ms> <max_latency_ms> <duplicate_ppm> <allow_reorder 0|1>
//   edit <replica> <start> <end> <text>
//   insert <replica> <index> <id> <l|r>
//   remove <replica> <index> <l|r>       (a remove carries no payload id)
//   deliver <replica> <op_index> [x]
//   undo <replica>
//   redo <replica>
//   disconnect <replica>
//   reconnect <replica>
//   block <from> <to>
//   unblock <from> <to>
//   restart <replica>
//   tick <ms>
//   drain
//   gc <replica>
//   compact
//
//   <text> is "-" for an empty replacement or lowercase hex otherwise, so a
//   recorded UTF-8 payload survives whitespace and newlines. The optional "x"
//   marker on a delivery is *injected fault injection* for the V04 proof: the
//   transport loses that delivery. Ordinary recorded scenarios never contain
//   it; a trace that does contain one reproduces the failure on its own.
//
// Model semantics
// ---------------
// buffer      Replica index r maps to Buffer(r + 1). Edits are byte ranges in
//             the replica's own visible text. A delivery names one produced
//             operation by index, so arbitrary reordering, duplication, and
//             omission are all expressible. A delivery to a disconnected
//             replica is blocked (recorded, no effect), which is the trace's
//             model of a partition; queueing until reconnect belongs to
//             NetworkSim and V05.
// idlist      Replica index r maps to IdList(r + 1). An insert/remove names a
//             *visible index* resolved against the replica's current state.
// networksim  Re-executes the events against NetworkSim using the recorded
//             seed and configuration. Delivery order, latency, duplication,
//             symmetric disconnects, and asymmetric blocked links are
//             NetworkSim's own deterministic evolution; the trace still records
//             the seed and every partition decision, so nothing is rerolled.
//
// restart
//   Modelled as replay-based recovery: the replica's engine is rebuilt from
//   scratch by re-executing the journal of decisions that concerned it, in
//   their original order. That reproduces its clock, its undo *and* redo
//   stacks, and its document. It is a harness model, not a store
//   implementation: durable segment/checkpoint recovery is L03/L05's task, and
//   a scenario that seeds a replica out of band must record that seeding as
//   trace events or the replay will not match.
//
// Reduction
// ---------
// reduce_trace() removes events one contiguous chunk at a time and keeps a
// removal only when the candidate still (a) replays to completion — every
// remaining event is applicable, every delivery reference resolves, every
// edit range is still a valid character range, every IdList visible index still
// exists — (b) still delivers to every replica the failing trace delivered to,
// and (c) still fails the chosen failure predicate. Validity is what preserves identity and causality:
// removing an edit invalidates the byte ranges and produced-operation
// references that depended on it, so such a candidate is rejected instead of
// silently clamped.
//
// What reduction preserves is the *predicate*, not the mechanism. A smaller
// failing trace may diverge for a different reason than the original did, and a
// degenerate reproduction remains possible when the original trace never
// finished syncing. That is why the task requires the unreduced trace to be
// saved first: the full artifact is the report, the reduced trace is the
// starting point for investigation.

#pragma once

#include "TestActor.h"
#include "TestSeed.h"

#include "crdt/Buffer.h"
#include "crdt/IdList.h"
#include "crdt/NetworkSim.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace CollabText::Test {

inline constexpr int kTraceFormatVersion = 1;

/// Number of empty apply_ops() passes a `settle` event performs. A scenario
/// that converges with a different pass count needs a different format
/// version, not a private constant.
inline constexpr int kSettlePasses = 20;
inline constexpr std::string_view kTraceMagic = "#collabtext-trace";

enum class TraceModel { Buffer, IdList, NetworkSim };

enum class EventKind {
    Edit,
    Insert,
    Remove,
    Deliver,
    Undo,
    Redo,
    Disconnect,
    Reconnect,
    BlockLink,
    UnblockLink,
    Restart,
    Tick,
    Drain,
    Settle,
    CollectGarbage,
    Compact,
};

struct Event {
    EventKind kind = EventKind::Edit;
    int replica = -1;        ///< acting or target replica index
    int peer = -1;           ///< second replica for asymmetric link events
    uint32_t start = 0;      ///< Edit: byte range start
    uint32_t end = 0;        ///< Edit: byte range end
    std::string text;        ///< Edit: replacement text
    uint32_t index = 0;      ///< Insert/Remove: visible index
    uint64_t id = 0;         ///< Insert: payload id
    Crdt::Bias bias = Crdt::Bias::Left;  ///< Insert/Remove: anchor bias
    int op_index = -1;       ///< Deliver: index into the produced-operation log
    bool lose = false;       ///< Deliver: injected transport loss
    uint64_t ms = 0;         ///< Tick

    bool involves(int r) const {
        switch (kind) {
        case EventKind::Tick:
        case EventKind::Drain:
        case EventKind::Settle:
        case EventKind::Compact:
            return false;
        default:
            return replica == r;
        }
    }
};

struct Trace {
    TraceModel model = TraceModel::Buffer;
    std::string label;
    uint64_t seed = 0;
    int replicas = 1;
    Crdt::NetworkConfig net;   ///< only meaningful for TraceModel::NetworkSim
    std::vector<Event> events;

    std::string to_text() const;
    bool save(const std::string& path) const;
    static bool load(const std::string& path, Trace& out, std::string& error);
};

namespace detail {

inline std::string hex_encode(std::string_view raw) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (const unsigned char c : raw) {
        out += digits[c >> 4];
        out += digits[c & 0x0f];
    }
    return out;
}

inline bool hex_value(char c, unsigned& out) {
    if (c >= '0' && c <= '9') { out = static_cast<unsigned>(c - '0'); return true; }
    if (c >= 'a' && c <= 'f') { out = static_cast<unsigned>(c - 'a' + 10); return true; }
    if (c >= 'A' && c <= 'F') { out = static_cast<unsigned>(c - 'A' + 10); return true; }
    return false;
}

inline std::string encode_text(const std::string& text) {
    if (text.empty()) return "-";
    return hex_encode(text);
}

inline bool decode_text(std::string_view token, std::string& out) {
    if (token == "-") { out.clear(); return true; }
    if (token.size() % 2 != 0) return false;
    std::string raw;
    raw.reserve(token.size() / 2);
    for (size_t i = 0; i < token.size(); i += 2) {
        unsigned hi = 0, lo = 0;
        if (!hex_value(token[i], hi) || !hex_value(token[i + 1], lo)) return false;
        raw += static_cast<char>((hi << 4) | lo);
    }
    out = raw;
    return true;
}

inline std::string_view model_name(TraceModel model) {
    switch (model) {
    case TraceModel::Buffer: return "buffer";
    case TraceModel::IdList: return "idlist";
    case TraceModel::NetworkSim: return "networksim";
    }
    return "buffer";
}

inline bool parse_model(std::string_view text, TraceModel& out) {
    if (text == "buffer") { out = TraceModel::Buffer; return true; }
    if (text == "idlist") { out = TraceModel::IdList; return true; }
    if (text == "networksim") { out = TraceModel::NetworkSim; return true; }
    return false;
}

inline std::string_view event_name(EventKind kind) {
    switch (kind) {
    case EventKind::Edit: return "edit";
    case EventKind::Insert: return "insert";
    case EventKind::Remove: return "remove";
    case EventKind::Deliver: return "deliver";
    case EventKind::Undo: return "undo";
    case EventKind::Redo: return "redo";
    case EventKind::Disconnect: return "disconnect";
    case EventKind::Reconnect: return "reconnect";
    case EventKind::BlockLink: return "block";
    case EventKind::UnblockLink: return "unblock";
    case EventKind::Restart: return "restart";
    case EventKind::Tick: return "tick";
    case EventKind::Drain: return "drain";
    case EventKind::Settle: return "settle";
    case EventKind::CollectGarbage: return "gc";
    case EventKind::Compact: return "compact";
    }
    return "edit";
}

template <typename T>
inline bool parse_uint(std::string_view token, T& out) {
    if (token.empty()) return false;
    uint64_t value = 0;
    for (const char c : token) {
        if (c < '0' || c > '9') return false;
        if (value > (UINT64_MAX - static_cast<uint64_t>(c - '0')) / 10) return false;
        value = value * 10 + static_cast<uint64_t>(c - '0');
    }
    out = static_cast<T>(value);
    return true;
}

inline bool parse_int(std::string_view token, int& out) {
    if (token.empty() || token[0] == '-') return false;
    uint64_t value = 0;
    if (!parse_uint(token, value)) return false;
    out = static_cast<int>(value);
    return true;
}

inline std::vector<std::string> split_ws(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream stream(line);
    std::string token;
    while (stream >> token) tokens.push_back(token);
    return tokens;
}

}  // namespace detail

// ============================================================================
// Trace serialization
// ============================================================================

inline std::string Trace::to_text() const {
    std::ostringstream out;
    out << kTraceMagic << ' ' << kTraceFormatVersion << '\n';
    out << "#model " << detail::model_name(model) << '\n';
    out << "#label " << (label.empty() ? std::string("-") : label) << '\n';
    out << "#seed " << seed << '\n';
    out << "#replicas " << replicas << '\n';
    out << "#net " << net.min_latency_ms << ' ' << net.max_latency_ms << ' '
        << static_cast<uint32_t>(net.duplicate_probability * 10000.0 + 0.5)
        << ' ' << (net.allow_reorder ? 1 : 0) << '\n';
    for (const Event& e : events) {
        switch (e.kind) {
        case EventKind::Edit:
            out << "edit " << e.replica << ' ' << e.start << ' ' << e.end << ' '
                << detail::encode_text(e.text) << '\n';
            break;
        case EventKind::Insert:
        case EventKind::Remove:
            out << detail::event_name(e.kind) << ' ' << e.replica << ' ' << e.index
                << ' ' << e.id << ' ' << (e.bias == Crdt::Bias::Right ? 'r' : 'l') << '\n';
            break;
        case EventKind::Deliver:
            out << "deliver " << e.replica << ' ' << e.op_index;
            if (e.lose) out << " x";
            out << '\n';
            break;
        case EventKind::BlockLink:
        case EventKind::UnblockLink:
            out << detail::event_name(e.kind) << ' ' << e.replica << ' ' << e.peer
                << '\n';
            break;
        case EventKind::Undo:
        case EventKind::Redo:
        case EventKind::Disconnect:
        case EventKind::Reconnect:
        case EventKind::Restart:
        case EventKind::CollectGarbage:
            out << detail::event_name(e.kind) << ' ' << e.replica << '\n';
            break;
        case EventKind::Tick:
            out << "tick " << e.ms << '\n';
            break;
        case EventKind::Drain:
        case EventKind::Settle:
        case EventKind::Compact:
            out << detail::event_name(e.kind) << '\n';
            break;
        }
    }
    return out.str();
}

inline bool Trace::save(const std::string& path) const {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    const std::string text = to_text();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(file);
}

inline bool Trace::load(const std::string& path, Trace& out, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) { error = "cannot open " + path; return false; }
    std::string line;
    bool saw_magic = false;
    out = Trace{};
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.rfind("#", 0) == 0) {
            const auto tokens = detail::split_ws(line);
            if (tokens.empty()) continue;
            if (tokens[0] == kTraceMagic) {
                int version = 0;
                if (tokens.size() < 2 || !detail::parse_int(tokens[1], version)) {
                    error = "missing trace version"; return false;
                }
                if (version != kTraceFormatVersion) {
                    error = "unsupported trace version " + std::to_string(version);
                    return false;
                }
                saw_magic = true;
            } else if (tokens[0] == "#model") {
                if (tokens.size() < 2 || !detail::parse_model(tokens[1], out.model)) {
                    error = "unknown trace model"; return false;
                }
            } else if (tokens[0] == "#label") {
                out.label = (tokens.size() > 1 && tokens[1] != "-") ? tokens[1] : "";
            } else if (tokens[0] == "#seed") {
                if (tokens.size() < 2 || !detail::parse_uint(tokens[1], out.seed)) {
                    error = "invalid #seed"; return false;
                }
            } else if (tokens[0] == "#replicas") {
                if (tokens.size() < 2 || !detail::parse_int(tokens[1], out.replicas) ||
                    out.replicas < 1) {
                    error = "invalid #replicas"; return false;
                }
            } else if (tokens[0] == "#net") {
                int min_lat = 0, max_lat = 0, dup = 0, reorder = 0;
                if (tokens.size() < 5 || !detail::parse_int(tokens[1], min_lat) ||
                    !detail::parse_int(tokens[2], max_lat) ||
                    !detail::parse_int(tokens[3], dup) || !detail::parse_int(tokens[4], reorder)) {
                    error = "invalid #net"; return false;
                }
                out.net.min_latency_ms = static_cast<uint32_t>(min_lat);
                out.net.max_latency_ms = static_cast<uint32_t>(max_lat);
                out.net.duplicate_probability = static_cast<double>(dup) / 10000.0;
                out.net.allow_reorder = reorder != 0;
            }
            continue;
        }

        const auto tokens = detail::split_ws(line);
        if (tokens.empty()) continue;
        Event e;
        const std::string& verb = tokens[0];
        auto need = [&](size_t count) {
            if (tokens.size() != count) {
                error = "malformed event '" + line + "'";
                return false;
            }
            return true;
        };
        if (verb == "edit") {
            if (!need(5) || !detail::parse_int(tokens[1], e.replica) ||
                !detail::parse_uint(tokens[2], e.start) ||
                !detail::parse_uint(tokens[3], e.end) ||
                !detail::decode_text(tokens[4], e.text)) {
                if (error.empty()) error = "malformed edit event";
                return false;
            }
            e.kind = EventKind::Edit;
        } else if (verb == "insert" || verb == "remove") {
            // An insert carries a payload id; a remove does not, so its bias is
            // the fourth field. Both forms are accepted so a hand-written trace
            // does not have to invent an id for a deletion.
            const bool with_id = (tokens.size() == 5);
            const bool without_id = (verb == "remove" && tokens.size() == 4);
            if ((!with_id && !without_id) ||
                !detail::parse_int(tokens[1], e.replica) ||
                !detail::parse_uint(tokens[2], e.index) ||
                (with_id && !detail::parse_uint(tokens[3], e.id))) {
                if (error.empty()) error = "malformed " + verb + " event";
                return false;
            }
            const std::string& bias_token = tokens[with_id ? 4 : 3];
            if (bias_token != "l" && bias_token != "r") {
                error = "malformed bias in " + verb + " event";
                return false;
            }
            e.kind = (verb == "insert") ? EventKind::Insert : EventKind::Remove;
            e.bias = (bias_token == "r") ? Crdt::Bias::Right : Crdt::Bias::Left;
        } else if (verb == "deliver") {
            if (tokens.size() < 3 || tokens.size() > 4 ||
                !detail::parse_int(tokens[1], e.replica) ||
                !detail::parse_int(tokens[2], e.op_index)) {
                if (error.empty()) error = "malformed deliver event";
                return false;
            }
            if (tokens.size() == 4) {
                if (tokens[3] != "x") { error = "unknown deliver flag"; return false; }
                e.lose = true;
            }
            e.kind = EventKind::Deliver;
        } else if (verb == "block" || verb == "unblock") {
            if (!need(3) || !detail::parse_int(tokens[1], e.replica) ||
                !detail::parse_int(tokens[2], e.peer)) {
                if (error.empty()) error = "malformed " + verb + " event";
                return false;
            }
            e.kind = (verb == "block") ? EventKind::BlockLink : EventKind::UnblockLink;
        } else if (verb == "undo" || verb == "redo" || verb == "disconnect" ||
                   verb == "reconnect" || verb == "restart" || verb == "gc") {
            if (!need(2) || !detail::parse_int(tokens[1], e.replica)) {
                if (error.empty()) error = "malformed " + verb + " event";
                return false;
            }
            if (verb == "undo") e.kind = EventKind::Undo;
            else if (verb == "redo") e.kind = EventKind::Redo;
            else if (verb == "disconnect") e.kind = EventKind::Disconnect;
            else if (verb == "reconnect") e.kind = EventKind::Reconnect;
            else if (verb == "restart") e.kind = EventKind::Restart;
            else e.kind = EventKind::CollectGarbage;
        } else if (verb == "tick") {
            if (!need(2) || !detail::parse_uint(tokens[1], e.ms)) {
                if (error.empty()) error = "malformed tick event";
                return false;
            }
            e.kind = EventKind::Tick;
        } else if (verb == "drain" || verb == "settle") {
            if (!need(1)) {
                if (error.empty()) error = "malformed " + verb + " event";
                return false;
            }
            e.kind = (verb == "drain") ? EventKind::Drain : EventKind::Settle;
        } else if (verb == "compact") {
            if (!need(1)) { if (error.empty()) error = "malformed compact event"; return false; }
            e.kind = EventKind::Compact;
        } else {
            error = "unknown event verb '" + verb + "'";
            return false;
        }
        out.events.push_back(e);
    }
    if (!saw_magic) { error = "missing " + std::string(kTraceMagic) + " header"; return false; }
    return true;
}

// ============================================================================
// Invariant checkers — non-fatal, so the reducer can use them
// ============================================================================

/// Structural check for a Buffer replica. Returns an empty string when the
/// invariants hold, otherwise a description of the first violation.
inline std::string buffer_invariant_error(const Crdt::Buffer& buf) {
    const std::string text = buf.text();
    if (buf.visible_length() != static_cast<uint32_t>(text.size()))
        return "visible_length != text().size()";
    if (buf.visible_rope_len() != buf.text().size())
        return "visible_rope_len != text size";
    uint32_t visible_sum = 0;
    uint32_t deleted_sum = 0;
    const auto fragments = buf.fragments();
    for (const auto& f : fragments) {
        if (f.visible) visible_sum += f.byte_length;
        else deleted_sum += f.byte_length;
        if (f.byte_length == 0 || f.length == 0)
            return "empty fragment";
        if (f.byte_length < f.length)
            return "fragment byte_length < length";
    }
    if (visible_sum != buf.visible_rope_len())
        return "visible fragment byte sum != visible_rope_len";
    if (deleted_sum != buf.deleted_rope_len())
        return "deleted fragment byte sum != deleted_rope_len";
    for (size_t i = 1; i < fragments.size(); ++i) {
        const auto cmp = fragments[i].locator <=> fragments[i - 1].locator;
        if (cmp < 0) return "fragment locator ordering violated";
        if (cmp == 0 && !(fragments[i].origin > fragments[i - 1].origin))
            return "fragment origin ordering violated at a shared locator";
    }
    return {};
}

inline std::string idlist_invariant_error(const Crdt::IdList& list) {
    const auto entries = list.entries();
    size_t visible = 0;
    for (const auto& e : entries) if (e.visible) ++visible;
    if (list.size() != visible) return "size() != visible entry count";
    if (entries.size() != list.entry_count())
        return "entries().size() != entry_count()";
    for (size_t i = 1; i < entries.size(); ++i) {
        const auto& a = entries[i - 1];
        const auto& b = entries[i];
        if (!(a.locator < b.locator) && !(a.locator == b.locator && a.origin < b.origin))
            return "entries not strictly ordered by (locator, origin)";
    }
    const auto& version = list.version();
    for (const auto& e : entries)
        if (!version.observed(e.origin))
            return "version does not observe an entry origin";
    return {};
}

// ============================================================================
// Replay
// ============================================================================

enum class ReplayStatus {
    Completed,     ///< every event applied (or was blocked/lost by design)
    NotApplicable, ///< an event cannot be applied in this reduced trace
};

struct ReplayResult {
    ReplayStatus status = ReplayStatus::Completed;
    std::string error;              ///< reason for NotApplicable
    size_t error_event = 0;         ///< index of the offending event
    bool converged = true;
    std::string divergence;         ///< which replicas disagreed
    std::string invariant_error;    ///< first invariant violation, if any
    int invariant_replica = -1;
    std::vector<std::string> replica_state;
    size_t applied = 0;
    size_t blocked = 0;             ///< deliveries blocked by a disconnect
    size_t lost = 0;                ///< deliveries dropped by injected faults
    size_t produced_ops = 0;
    size_t idlists = 0;
    /// Bitmask of replicas that actually received at least one applied
    /// delivery. Reduction requires a candidate to keep covering the same
    /// delivery targets, so a reproduction cannot degenerate into "one replica
    /// received nothing".
    uint64_t delivery_targets = 0;
};

namespace detail {

/// True when `offset` is 0, the text size, or the start of a UTF-8 sequence.
inline bool is_char_boundary(std::string_view text, uint32_t offset) {
    if (offset > text.size()) return false;
    if (offset == text.size()) return true;
    const unsigned char c = static_cast<unsigned char>(text[offset]);
    return (c & 0xC0) != 0x80;
}

/// Anchor for "insert at visible index `index`". IdList::insert_after() places
/// an entry at the front for Anchor::min() and immediately after a named entry
/// otherwise, so a visible index maps to the *predecessor* entry's anchor.
inline Crdt::Anchor insert_anchor(const Crdt::IdList& list, uint32_t index,
                                  Crdt::Bias bias) {
    if (index == 0) return Crdt::Anchor::min();
    return list.anchor_at_index(index - 1, bias);
}

struct BufferWorld {
    std::vector<Crdt::Buffer> replicas;
    std::vector<Crdt::Operation> produced;
    std::vector<bool> offline;

    explicit BufferWorld(int count) : replicas(), offline(count, false) {
        replicas.reserve(static_cast<size_t>(count));
        for (int r = 0; r < count; ++r)
            replicas.emplace_back(CollabText::Test::actor(static_cast<uint8_t>(r + 1)));
    }
};

struct IdListWorld {
    std::vector<Crdt::IdList> replicas;
    std::vector<Crdt::IdListOperation> produced;
    std::vector<bool> offline;

    explicit IdListWorld(int count) : replicas(), offline(count, false) {
        replicas.reserve(static_cast<size_t>(count));
        for (int r = 0; r < count; ++r)
            replicas.emplace_back(CollabText::Test::actor(static_cast<uint8_t>(r + 1)));
    }
};

inline void snapshot_buffers(const BufferWorld& world, ReplayResult& result) {
    result.replica_state.clear();
    for (const auto& buf : world.replicas) result.replica_state.push_back(buf.text());
}

inline void snapshot_idlists(const IdListWorld& world, ReplayResult& result) {
    result.replica_state.clear();
    result.idlists = world.replicas.size();
    for (const auto& list : world.replicas) {
        std::ostringstream out;
        for (const uint64_t id : list.ids()) out << id << ',';
        result.replica_state.push_back(out.str());
    }
}

inline bool check_convergence(const std::vector<std::string>& state, ReplayResult& result) {
    result.converged = true;
    for (size_t r = 1; r < state.size(); ++r) {
        if (state[r] != state[0]) {
            result.converged = false;
            std::ostringstream out;
            out << "replicas 0 and " << r << " differ";
            result.divergence = out.str();
            return false;
        }
    }
    return true;
}

}  // namespace detail

/// Re-execute the recorded decisions of a buffer-model trace.
inline ReplayResult replay_buffer(const Trace& trace) {
    using namespace detail;
    ReplayResult result;
    BufferWorld world(trace.replicas);
    result.idlists = 0;

    for (size_t i = 0; i < trace.events.size(); ++i) {
        const Event& e = trace.events[i];
        auto reject = [&](const std::string& why) {
            result.status = ReplayStatus::NotApplicable;
            result.error_event = i;
            result.error = why;
            return result;
        };
        if (e.replica < 0 || e.replica >= trace.replicas) {
            if (e.kind != EventKind::Tick && e.kind != EventKind::Drain &&
                e.kind != EventKind::Settle && e.kind != EventKind::Compact)
                return reject("replica index out of range");
        }
        switch (e.kind) {
        case EventKind::Edit: {
            const std::string text = world.replicas[e.replica].text();
            if (e.start > e.end || e.end > text.size())
                return reject("edit range outside the replica text");
            if (!is_char_boundary(text, e.start) || !is_char_boundary(text, e.end))
                return reject("edit range splits a UTF-8 sequence");
            Crdt::Operation op = world.replicas[e.replica].apply_local_edit(
                {{e.start, e.end}}, {e.text});
            world.produced.push_back(op);
            ++result.applied;
            break;
        }
        case EventKind::Deliver: {
            if (e.lose) { ++result.lost; break; }
            if (world.offline[e.replica]) { ++result.blocked; break; }
            if (e.op_index < 0 ||
                static_cast<size_t>(e.op_index) >= world.produced.size())
                return reject("delivery references an operation that was never produced");
            world.replicas[e.replica].apply_ops({world.produced[e.op_index]});
            if (e.replica < 64) result.delivery_targets |= (uint64_t{1} << e.replica);
            ++result.applied;
            break;
        }
        case EventKind::Undo: {
            if (auto op = world.replicas[e.replica].undo()) {
                world.produced.push_back(*op);
            }
            ++result.applied;
            break;
        }
        case EventKind::Redo: {
            if (auto op = world.replicas[e.replica].redo()) {
                world.produced.push_back(*op);
            }
            ++result.applied;
            break;
        }
        case EventKind::Disconnect:
            world.offline[e.replica] = true;
            ++result.applied;
            break;
        case EventKind::Reconnect:
            world.offline[e.replica] = false;
            ++result.applied;
            break;
        case EventKind::Restart: {
            // Replay-based recovery: rebuild the replica from the journal of
            // decisions that concerned it, in order.
            Crdt::Buffer rebuilt(CollabText::Test::actor(static_cast<uint8_t>(e.replica + 1)));
            for (size_t j = 0; j < i; ++j) {
                const Event& p = trace.events[j];
                if (!p.involves(e.replica)) continue;
                switch (p.kind) {
                case EventKind::Edit:
                    rebuilt.apply_local_edit({{p.start, p.end}}, {p.text});
                    break;
                case EventKind::Deliver:
                    if (p.lose || p.op_index < 0 ||
                        static_cast<size_t>(p.op_index) >= world.produced.size())
                        break;
                    rebuilt.apply_ops({world.produced[p.op_index]});
                    break;
                case EventKind::Undo: {
                    if (auto op = rebuilt.undo())
                        world.produced.push_back(*op);
                    break;
                }
                case EventKind::Redo: {
                    if (auto op = rebuilt.redo())
                        world.produced.push_back(*op);
                    break;
                }
                case EventKind::Disconnect:
                    world.offline[e.replica] = true;
                    break;
                case EventKind::Reconnect:
                    world.offline[e.replica] = false;
                    break;
                default:
                    break;
                }
            }
            world.replicas[e.replica] = std::move(rebuilt);
            ++result.applied;
            break;
        }
        case EventKind::Settle:
            // Retry pass: Buffer defers operations whose causal dependencies
            // have not arrived yet, and an empty apply_ops() is how a scenario
            // flushes them. The pass count is part of trace format v1.
            for (auto& buf : world.replicas)
                for (int pass = 0; pass < kSettlePasses; ++pass) buf.apply_ops({});
            ++result.applied;
            break;
        case EventKind::Tick:
        case EventKind::Drain:
            // The buffer model is synchronous: timing and draining are
            // expressed as explicit delivery events. Accepted for format
            // compatibility with a NetworkSim trace.
            ++result.applied;
            break;
        case EventKind::CollectGarbage:
            world.replicas[e.replica].collect_garbage();
            ++result.applied;
            break;
        case EventKind::Compact: {
            Crdt::Global watermark = world.replicas[0].version();
            for (size_t r = 1; r < world.replicas.size(); ++r)
                watermark.meet(world.replicas[r].version());
            for (auto& buf : world.replicas) buf.compact(watermark);
            ++result.applied;
            break;
        }
        default:
            return reject("event is not valid in the buffer model");
        }
    }

    result.produced_ops = world.produced.size();
    for (size_t r = 0; r < world.replicas.size(); ++r) {
        const std::string problem = buffer_invariant_error(world.replicas[r]);
        if (!problem.empty()) {
            result.invariant_error = problem;
            result.invariant_replica = static_cast<int>(r);
            break;
        }
    }
    snapshot_buffers(world, result);
    check_convergence(result.replica_state, result);
    return result;
}

/// Re-execute the recorded decisions of an idlist-model trace.
inline ReplayResult replay_idlist(const Trace& trace) {
    using namespace detail;
    ReplayResult result;
    IdListWorld world(trace.replicas);

    for (size_t i = 0; i < trace.events.size(); ++i) {
        const Event& e = trace.events[i];
        auto reject = [&](const std::string& why) {
            result.status = ReplayStatus::NotApplicable;
            result.error_event = i;
            result.error = why;
            return result;
        };
        if (e.replica < 0 || e.replica >= trace.replicas) {
            if (e.kind != EventKind::Tick && e.kind != EventKind::Drain &&
                e.kind != EventKind::Settle && e.kind != EventKind::Compact)
                return reject("replica index out of range");
        }
        switch (e.kind) {
        case EventKind::Insert:
        case EventKind::Remove: {
            Crdt::IdList& list = world.replicas[e.replica];
            if (e.index > list.size())
                return reject("visible index beyond the replica's list size");
            Crdt::IdListOperation op;
            if (e.kind == EventKind::Insert) {
                op = list.insert_after(insert_anchor(list, e.index, e.bias), e.id);
            } else {
                op = list.remove_at(list.anchor_at_index(e.index, e.bias));
            }
            world.produced.push_back(op);
            ++result.applied;
            break;
        }
        case EventKind::Deliver: {
            if (e.lose) { ++result.lost; break; }
            if (world.offline[e.replica]) { ++result.blocked; break; }
            if (e.op_index < 0 ||
                static_cast<size_t>(e.op_index) >= world.produced.size())
                return reject("delivery references an operation that was never produced");
            world.replicas[e.replica].apply_ops({world.produced[e.op_index]});
            if (e.replica < 64) result.delivery_targets |= (uint64_t{1} << e.replica);
            ++result.applied;
            break;
        }
        case EventKind::Undo: {
            if (auto op = world.replicas[e.replica].undo()) world.produced.push_back(*op);
            ++result.applied;
            break;
        }
        case EventKind::Redo: {
            if (auto op = world.replicas[e.replica].redo()) world.produced.push_back(*op);
            ++result.applied;
            break;
        }
        case EventKind::Disconnect:
            world.offline[e.replica] = true;
            ++result.applied;
            break;
        case EventKind::Reconnect:
            world.offline[e.replica] = false;
            ++result.applied;
            break;
        case EventKind::Restart: {
            Crdt::IdList rebuilt(CollabText::Test::actor(static_cast<uint8_t>(e.replica + 1)));
            for (size_t j = 0; j < i; ++j) {
                const Event& p = trace.events[j];
                if (!p.involves(e.replica)) continue;
                switch (p.kind) {
                case EventKind::Insert:
                case EventKind::Remove: {
                    Crdt::IdList& list = rebuilt;
                    if (p.index > list.size()) break;
                    Crdt::IdListOperation op;
                    if (p.kind == EventKind::Insert) {
                        op = list.insert_after(insert_anchor(list, p.index, p.bias), p.id);
                    } else {
                        op = list.remove_at(list.anchor_at_index(p.index, p.bias));
                    }
                    world.produced.push_back(op);
                    break;
                }
                case EventKind::Deliver:
                    if (p.lose || p.op_index < 0 ||
                        static_cast<size_t>(p.op_index) >= world.produced.size())
                        break;
                    rebuilt.apply_ops({world.produced[p.op_index]});
                    break;
                case EventKind::Undo: {
                    if (auto op = rebuilt.undo()) world.produced.push_back(*op);
                    break;
                }
                case EventKind::Redo: {
                    if (auto op = rebuilt.redo()) world.produced.push_back(*op);
                    break;
                }
                case EventKind::Disconnect:
                    world.offline[e.replica] = true;
                    break;
                case EventKind::Reconnect:
                    world.offline[e.replica] = false;
                    break;
                default:
                    break;
                }
            }
            world.replicas[e.replica] = std::move(rebuilt);
            ++result.applied;
            break;
        }
        case EventKind::Settle:
            for (auto& list : world.replicas)
                for (int pass = 0; pass < kSettlePasses; ++pass) list.apply_ops({});
            ++result.applied;
            break;
        case EventKind::Tick:
        case EventKind::Drain:
            ++result.applied;
            break;
        case EventKind::CollectGarbage:
            world.replicas[e.replica].collect_garbage();
            ++result.applied;
            break;
        case EventKind::Compact: {
            Crdt::Global watermark = world.replicas[0].version();
            for (size_t r = 1; r < world.replicas.size(); ++r)
                watermark.meet(world.replicas[r].version());
            for (auto& list : world.replicas) list.compact(watermark);
            ++result.applied;
            break;
        }
        default:
            return reject("event is not valid in the idlist model");
        }
    }

    result.produced_ops = world.produced.size();
    for (size_t r = 0; r < world.replicas.size(); ++r) {
        const std::string problem = idlist_invariant_error(world.replicas[r]);
        if (!problem.empty()) {
            result.invariant_error = problem;
            result.invariant_replica = static_cast<int>(r);
            break;
        }
    }
    snapshot_idlists(world, result);
    check_convergence(result.replica_state, result);
    return result;
}

/// Re-execute a NetworkSim-model trace with its recorded seed and config.
inline ReplayResult replay_network(const Trace& trace) {
    ReplayResult result;
    Crdt::NetworkSim net(trace.replicas, trace.net, trace.seed);
    for (size_t i = 0; i < trace.events.size(); ++i) {
        const Event& e = trace.events[i];
        auto reject = [&](const std::string& why) {
            result.status = ReplayStatus::NotApplicable;
            result.error_event = i;
            result.error = why;
            return result;
        };
        if (e.replica < 0 || e.replica >= trace.replicas) {
            if (e.kind != EventKind::Drain && e.kind != EventKind::Settle &&
                e.kind != EventKind::Compact && e.kind != EventKind::Tick)
                return reject("replica index out of range");
        }
        switch (e.kind) {
        case EventKind::Edit:
            net.edit(e.replica, {{e.start, e.end}}, {e.text});
            ++result.applied;
            break;
        case EventKind::Undo:
            net.undo(e.replica);
            ++result.applied;
            break;
        case EventKind::Redo:
            net.redo(e.replica);
            ++result.applied;
            break;
        case EventKind::Disconnect:
            net.disconnect(e.replica);
            ++result.applied;
            break;
        case EventKind::Reconnect:
            net.reconnect(e.replica);
            ++result.applied;
            break;
        case EventKind::BlockLink:
        case EventKind::UnblockLink:
            if (e.peer < 0 || e.peer >= trace.replicas)
                return reject("link event names a replica out of range");
            if (e.kind == EventKind::BlockLink) net.block_link(e.replica, e.peer);
            else net.unblock_link(e.replica, e.peer);
            ++result.applied;
            break;
        case EventKind::Tick:
            net.tick(e.ms);
            ++result.applied;
            break;
        case EventKind::Drain:
            net.drain();
            ++result.applied;
            break;
        case EventKind::Settle:
            return reject("settle is meaningless in the networksim model; use drain");
        case EventKind::CollectGarbage:
            net.collect_garbage(e.replica);
            ++result.applied;
            break;
        case EventKind::Compact:
            net.compact_all();
            ++result.applied;
            break;
        case EventKind::Deliver:
        case EventKind::Insert:
        case EventKind::Remove:
        case EventKind::Restart:
            // NetworkSim owns its delivery order: latency, duplication, and
            // partition state are its own deterministic evolution, so an
            // explicit per-operation delivery has no meaning in this model.
            return reject("event is not valid in the networksim model");
        }
    }

    for (size_t r = 0; r < net.num_replicas(); ++r) {
        const std::string problem = buffer_invariant_error(net.buffer(static_cast<int>(r)));
        if (!problem.empty()) {
            result.invariant_error = problem;
            result.invariant_replica = static_cast<int>(r);
            break;
        }
    }
    result.replica_state.clear();
    for (int r = 0; r < net.num_replicas(); ++r)
        result.replica_state.push_back(net.buffer(r).text());
    detail::check_convergence(result.replica_state, result);
    return result;
}

inline ReplayResult replay(const Trace& trace) {
    switch (trace.model) {
    case TraceModel::Buffer: return replay_buffer(trace);
    case TraceModel::IdList: return replay_idlist(trace);
    case TraceModel::NetworkSim: return replay_network(trace);
    }
    ReplayResult result;
    result.status = ReplayStatus::NotApplicable;
    result.error = "unknown trace model";
    return result;
}

// ============================================================================
// Failure predicates and reduction
// ============================================================================

enum class FailurePredicate {
    Convergence,   ///< replicas must converge on identical text/ids
    Invariant,     ///< structural invariants must hold
    Any,           ///< either
};

inline bool predicate_fails(const ReplayResult& result, FailurePredicate predicate) {
    switch (predicate) {
    case FailurePredicate::Convergence: return !result.converged;
    case FailurePredicate::Invariant: return !result.invariant_error.empty();
    case FailurePredicate::Any:
        return !result.converged || !result.invariant_error.empty();
    }
    return false;
}

inline std::string_view predicate_name(FailurePredicate predicate) {
    switch (predicate) {
    case FailurePredicate::Convergence: return "convergence";
    case FailurePredicate::Invariant: return "invariant";
    case FailurePredicate::Any: return "any";
    }
    return "any";
}

struct ReduceOutcome {
    bool ok = false;
    Trace trace;                  ///< smallest trace that still fails
    size_t removed = 0;           ///< events removed
    size_t candidates_tried = 0;
    std::string note;             ///< why reduction did not happen
    std::string failure;          ///< the surviving failure description
};

/// Greedy delta reduction. Removes contiguous chunks of events, largest first,
/// keeping a removal only when the candidate still replays to completion and
/// still fails `predicate`. Returns ok=false (and an explanatory note) when the
/// supplied trace does not itself fail the predicate — reducing a passing trace
/// would be meaningless.
inline ReduceOutcome reduce_trace(const Trace& failing, FailurePredicate predicate,
                                  int max_passes = 8) {
    ReduceOutcome outcome;
    const ReplayResult original = replay(failing);
    if (original.status != ReplayStatus::Completed) {
        outcome.note = "supplied trace is not replayable: " + original.error;
        return outcome;
    }
    if (!predicate_fails(original, predicate)) {
        outcome.note = "supplied trace does not fail the requested predicate";
        return outcome;
    }
    outcome.failure = original.converged
        ? ("invariant: " + original.invariant_error)
        : ("convergence: " + original.divergence);
    const uint64_t required_targets = original.delivery_targets;

    Trace best = failing;
    bool progressed = true;
    for (int pass = 0; pass < max_passes && progressed; ++pass) {
        progressed = false;
        for (size_t chunk = 16; chunk >= 1; chunk /= 2) {
            size_t start = 0;
            while (start < best.events.size()) {
                if (start + chunk > best.events.size()) break;
                Trace candidate = best;
                candidate.events.erase(candidate.events.begin() +
                                          static_cast<std::ptrdiff_t>(start),
                                      candidate.events.begin() +
                                          static_cast<std::ptrdiff_t>(start + chunk));
                ++outcome.candidates_tried;
                const ReplayResult reduced = replay(candidate);
                if (reduced.status == ReplayStatus::Completed &&
                    (reduced.delivery_targets & required_targets) == required_targets &&
                    predicate_fails(reduced, predicate)) {
                    best = std::move(candidate);
                    outcome.removed += chunk;
                    progressed = true;
                    // Re-scan from the same position in the shorter trace.
                } else {
                    ++start;
                }
            }
        }
    }
    outcome.ok = true;
    outcome.trace = std::move(best);
    return outcome;
}

}  // namespace CollabText::Test

// ============================================================================
// Recorders — drive the real engines while recording the decisions
// ============================================================================
//
// A scenario uses a recorder instead of touching the engine directly: the
// recorder performs the action, appends the decision to the trace, and returns
// the produced-operation index so the scenario can schedule deliveries. The
// scenario keeps its own control flow; nothing about its strategy is rerolled
// on replay.
//
//   TraceHarness::BufferRecorder rec(3, Test::effective_seed("chaos"), "chaos");
//   int op = rec.edit(0, 0, 0, "hi");
//   rec.deliver(1, op);
//   rec.deliver(2, op);
//   ...
//   QCOMPARE(rec.buffer(0).text(), rec.buffer(1).text());
//
// save() writes a trace only when COLLABTEXT_TRACE_OUTPUT is set, so recording
// costs nothing by default while any failing campaign can be told to keep its
// evidence.

namespace CollabText::Test {

inline std::string save_trace(const Trace& trace, std::string_view suffix) {
    const std::string name = (trace.label.empty() ? std::string("trace")
                                                  : trace.label) +
                             "-" + std::string(suffix) + ".trace";
    const std::string path = trace_artifact_path(name);
    if (path.empty()) return {};
    std::error_code code;
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, code);
    if (code) return {};
    if (!trace.save(path)) return {};
    return path;
}

/// Records a Buffer-model scenario.
class BufferRecorder {
public:
    BufferRecorder(int replicas, uint64_t seed, std::string label)
        : m_trace{}, m_world(replicas) {
        m_trace.model = TraceModel::Buffer;
        m_trace.label = std::move(label);
        m_trace.seed = seed;
        m_trace.replicas = replicas;
    }

    Trace& trace() { return m_trace; }
    const Trace& trace() const { return m_trace; }

    const Crdt::Buffer& buffer(int r) const { return m_world.replicas[r]; }
    Crdt::Buffer& buffer(int r) { return m_world.replicas[r]; }
    int replicas() const { return m_trace.replicas; }

    /// Local edit by byte range in the replica's own visible text.
    /// Returns the produced operation's index, or -1 if the edit was rejected.
    int edit(int r, uint32_t start, uint32_t end, std::string text) {
        const std::string current = m_world.replicas[r].text();
        if (start > end || end > current.size()) return -1;
        if (!detail::is_char_boundary(current, start) ||
            !detail::is_char_boundary(current, end))
            return -1;
        Event e;
        e.kind = EventKind::Edit;
        e.replica = r;
        e.start = start;
        e.end = end;
        e.text = std::move(text);
        m_trace.events.push_back(e);
        m_world.produced.push_back(
            m_world.replicas[r].apply_local_edit({{start, end}}, {m_trace.events.back().text}));
        return static_cast<int>(m_world.produced.size()) - 1;
    }

    /// Deliver one produced operation to a replica. `lose` records an injected
    /// transport loss (the V04 injected-failure proof); blocked deliveries to a
    /// disconnected replica are recorded and have no effect.
    void deliver(int r, int op_index, bool lose = false) {
        Event e;
        e.kind = EventKind::Deliver;
        e.replica = r;
        e.op_index = op_index;
        e.lose = lose;
        m_trace.events.push_back(e);
        if (lose) return;
        if (m_world.offline[r]) return;
        if (op_index < 0 || static_cast<size_t>(op_index) >= m_world.produced.size()) return;
        m_world.replicas[r].apply_ops({m_world.produced[op_index]});
    }

    void deliver_many(int r, const std::vector<int>& op_indices, bool lose = false) {
        for (const int index : op_indices) deliver(r, index, lose);
    }

    /// Undo/redo on a replica. Returns the produced operation index, or -1.
    int undo(int r) {
        Event e;
        e.kind = EventKind::Undo;
        e.replica = r;
        m_trace.events.push_back(e);
        auto op = m_world.replicas[r].undo();
        if (!op) return -1;
        m_world.produced.push_back(*op);
        return static_cast<int>(m_world.produced.size()) - 1;
    }

    int redo(int r) {
        Event e;
        e.kind = EventKind::Redo;
        e.replica = r;
        m_trace.events.push_back(e);
        auto op = m_world.replicas[r].redo();
        if (!op) return -1;
        m_world.produced.push_back(*op);
        return static_cast<int>(m_world.produced.size()) - 1;
    }

    void disconnect(int r) {
        Event e;
        e.kind = EventKind::Disconnect;
        e.replica = r;
        m_trace.events.push_back(e);
        m_world.offline[r] = true;
    }

    void reconnect(int r) {
        Event e;
        e.kind = EventKind::Reconnect;
        e.replica = r;
        m_trace.events.push_back(e);
        m_world.offline[r] = false;
    }

    /// Replay-based restart: rebuild replica r from its own decision journal.
    void restart(int r) {
        Event e;
        e.kind = EventKind::Restart;
        e.replica = r;
        m_trace.events.push_back(e);
        Crdt::Buffer rebuilt(CollabText::Test::actor(static_cast<uint8_t>(r + 1)));
        for (size_t j = 0; j + 1 < m_trace.events.size(); ++j) {
            const Event& p = m_trace.events[j];
            if (!p.involves(r)) continue;
            switch (p.kind) {
            case EventKind::Edit:
                rebuilt.apply_local_edit({{p.start, p.end}}, {p.text});
                break;
            case EventKind::Deliver:
                if (p.lose || p.op_index < 0 ||
                    static_cast<size_t>(p.op_index) >= m_world.produced.size())
                    break;
                rebuilt.apply_ops({m_world.produced[p.op_index]});
                break;
            case EventKind::Undo: {
                if (auto op = rebuilt.undo()) m_world.produced.push_back(*op);
                break;
            }
            case EventKind::Redo: {
                if (auto op = rebuilt.redo()) m_world.produced.push_back(*op);
                break;
            }
            case EventKind::Disconnect: m_world.offline[r] = true; break;
            case EventKind::Reconnect: m_world.offline[r] = false; break;
            default: break;
            }
        }
        m_world.replicas[r] = std::move(rebuilt);
    }

    /// Flush deferred operations with empty apply_ops() passes.
    void settle() {
        Event e;
        e.kind = EventKind::Settle;
        m_trace.events.push_back(e);
        for (auto& buf : m_world.replicas)
            for (int pass = 0; pass < kSettlePasses; ++pass) buf.apply_ops({});
    }

    void collect_garbage(int r) {
        Event e;
        e.kind = EventKind::CollectGarbage;
        e.replica = r;
        m_trace.events.push_back(e);
        m_world.replicas[r].collect_garbage();
    }

    void compact_all() {
        Event e;
        e.kind = EventKind::Compact;
        m_trace.events.push_back(e);
        Crdt::Global watermark = m_world.replicas[0].version();
        for (size_t r = 1; r < m_world.replicas.size(); ++r)
            watermark.meet(m_world.replicas[r].version());
        for (auto& buf : m_world.replicas) buf.compact(watermark);
    }

    /// Deliver an operation to every replica except its producer.
    void broadcast(int source, int op_index, bool lose_to_one = false, int lost_peer = -1) {
        for (int r = 0; r < m_trace.replicas; ++r) {
            if (r == source) continue;
            deliver(r, op_index, lose_to_one && r == lost_peer);
        }
    }

    std::string save(std::string_view suffix) const { return save_trace(m_trace, suffix); }

    /// Emit the scenario's trace when COLLABTEXT_TRACE_OUTPUT is set, so an
    /// artifact campaign leaves one replayable file per scenario and not only
    /// a file for the scenarios that failed. Returns the path, or empty.
    std::string finish() const { return save("trace"); }

private:
    Trace m_trace;
    detail::BufferWorld m_world;
};

/// Records an IdList-model scenario.
class IdListRecorder {
public:
    IdListRecorder(int replicas, uint64_t seed, std::string label)
        : m_trace{}, m_world(replicas) {
        m_trace.model = TraceModel::IdList;
        m_trace.label = std::move(label);
        m_trace.seed = seed;
        m_trace.replicas = replicas;
    }

    Trace& trace() { return m_trace; }
    const Trace& trace() const { return m_trace; }

    const Crdt::IdList& list(int r) const { return m_world.replicas[r]; }
    Crdt::IdList& list(int r) { return m_world.replicas[r]; }
    int replicas() const { return m_trace.replicas; }

    int insert_after(int r, uint32_t index, uint64_t id, Crdt::Bias bias = Crdt::Bias::Left) {
        Crdt::IdList& target = m_world.replicas[r];
        if (index > target.size()) return -1;
        Event e;
        e.kind = EventKind::Insert;
        e.replica = r;
        e.index = index;
        e.id = id;
        e.bias = bias;
        m_trace.events.push_back(e);
        m_world.produced.push_back(
            target.insert_after(detail::insert_anchor(target, index, bias), id));
        return static_cast<int>(m_world.produced.size()) - 1;
    }

    int remove_at(int r, uint32_t index, Crdt::Bias bias = Crdt::Bias::Left) {
        Crdt::IdList& target = m_world.replicas[r];
        if (index >= target.size()) return -1;
        Event e;
        e.kind = EventKind::Remove;
        e.replica = r;
        e.index = index;
        e.bias = bias;
        m_trace.events.push_back(e);
        m_world.produced.push_back(
            target.remove_at(target.anchor_at_index(index, bias)));
        return static_cast<int>(m_world.produced.size()) - 1;
    }

    void deliver(int r, int op_index, bool lose = false) {
        Event e;
        e.kind = EventKind::Deliver;
        e.replica = r;
        e.op_index = op_index;
        e.lose = lose;
        m_trace.events.push_back(e);
        if (lose || m_world.offline[r]) return;
        if (op_index < 0 || static_cast<size_t>(op_index) >= m_world.produced.size()) return;
        m_world.replicas[r].apply_ops({m_world.produced[op_index]});
    }

    void broadcast(int source, int op_index) {
        for (int r = 0; r < m_trace.replicas; ++r) {
            if (r != source) deliver(r, op_index);
        }
    }

    int undo(int r) {
        Event e;
        e.kind = EventKind::Undo;
        e.replica = r;
        m_trace.events.push_back(e);
        auto op = m_world.replicas[r].undo();
        if (!op) return -1;
        m_world.produced.push_back(*op);
        return static_cast<int>(m_world.produced.size()) - 1;
    }

    int redo(int r) {
        Event e;
        e.kind = EventKind::Redo;
        e.replica = r;
        m_trace.events.push_back(e);
        auto op = m_world.replicas[r].redo();
        if (!op) return -1;
        m_world.produced.push_back(*op);
        return static_cast<int>(m_world.produced.size()) - 1;
    }

    void disconnect(int r) {
        Event e;
        e.kind = EventKind::Disconnect;
        e.replica = r;
        m_trace.events.push_back(e);
        m_world.offline[r] = true;
    }

    void reconnect(int r) {
        Event e;
        e.kind = EventKind::Reconnect;
        e.replica = r;
        m_trace.events.push_back(e);
        m_world.offline[r] = false;
    }

    void restart(int r) {
        Event e;
        e.kind = EventKind::Restart;
        e.replica = r;
        m_trace.events.push_back(e);
        Crdt::IdList rebuilt(CollabText::Test::actor(static_cast<uint8_t>(r + 1)));
        for (size_t j = 0; j + 1 < m_trace.events.size(); ++j) {
            const Event& p = m_trace.events[j];
            if (!p.involves(r)) continue;
            switch (p.kind) {
            case EventKind::Insert:
            case EventKind::Remove: {
                Crdt::IdList& target = rebuilt;
                if (p.kind == EventKind::Insert ? (p.index > target.size())
                                                : (p.index >= target.size()))
                    break;
                Crdt::IdListOperation op;
                if (p.kind == EventKind::Insert) {
                    op = target.insert_after(
                        detail::insert_anchor(target, p.index, p.bias), p.id);
                } else {
                    op = target.remove_at(target.anchor_at_index(p.index, p.bias));
                }
                m_world.produced.push_back(op);
                break;
            }
            case EventKind::Deliver:
                if (p.lose || p.op_index < 0 ||
                    static_cast<size_t>(p.op_index) >= m_world.produced.size())
                    break;
                rebuilt.apply_ops({m_world.produced[p.op_index]});
                break;
            case EventKind::Undo: {
                if (auto op = rebuilt.undo()) m_world.produced.push_back(*op);
                break;
            }
            case EventKind::Redo: {
                if (auto op = rebuilt.redo()) m_world.produced.push_back(*op);
                break;
            }
            case EventKind::Disconnect: m_world.offline[r] = true; break;
            case EventKind::Reconnect: m_world.offline[r] = false; break;
            default: break;
            }
        }
        m_world.replicas[r] = std::move(rebuilt);
    }

    /// Flush deferred operations with empty apply_ops() passes.
    void settle() {
        Event e;
        e.kind = EventKind::Settle;
        m_trace.events.push_back(e);
        for (auto& buf : m_world.replicas)
            for (int pass = 0; pass < kSettlePasses; ++pass) buf.apply_ops({});
    }

    void collect_garbage(int r) {
        Event e;
        e.kind = EventKind::CollectGarbage;
        e.replica = r;
        m_trace.events.push_back(e);
        m_world.replicas[r].collect_garbage();
    }

    void compact_all() {
        Event e;
        e.kind = EventKind::Compact;
        m_trace.events.push_back(e);
        Crdt::Global watermark = m_world.replicas[0].version();
        for (size_t r = 1; r < m_world.replicas.size(); ++r)
            watermark.meet(m_world.replicas[r].version());
        for (auto& list : m_world.replicas) list.compact(watermark);
    }

    std::string save(std::string_view suffix) const { return save_trace(m_trace, suffix); }

    /// Emit the scenario's trace when COLLABTEXT_TRACE_OUTPUT is set, so an
    /// artifact campaign leaves one replayable file per scenario and not only
    /// a file for the scenarios that failed. Returns the path, or empty.
    std::string finish() const { return save("trace"); }

private:
    Trace m_trace;
    detail::IdListWorld m_world;
};

/// Records a NetworkSim-model scenario. NetworkSim owns latency, duplication,
/// and partition behavior; the recorder preserves the decisions and the seed so
/// a replay reproduces the same simulated delivery schedule.
class NetworkRecorder {
public:
    NetworkRecorder(int replicas, Crdt::NetworkConfig config, uint64_t seed,
                    std::string label)
        : m_trace{}, m_net(replicas, config, seed) {
        m_trace.model = TraceModel::NetworkSim;
        m_trace.label = std::move(label);
        m_trace.seed = seed;
        m_trace.replicas = replicas;
        m_trace.net = config;
    }

    Trace& trace() { return m_trace; }
    const Trace& trace() const { return m_trace; }

    Crdt::NetworkSim& net() { return m_net; }
    const Crdt::NetworkSim& net() const { return m_net; }
    Crdt::Buffer& buffer(int r) { return m_net.buffer(r); }
    const Crdt::Buffer& buffer(int r) const { return m_net.buffer(r); }

    void edit(int r, const std::vector<std::pair<uint32_t, uint32_t>>& ranges,
              const std::vector<std::string>& new_text) {
        Event e;
        e.kind = EventKind::Edit;
        e.replica = r;
        // The recorded decision is the resulting single replacement range.
        // NetworkSim scenarios drive one range at a time; a multi-range edit
        // is recorded as one edit per range is not expressible, so reject it.
        if (ranges.size() != 1 || new_text.size() != 1) return;
        e.start = ranges[0].first;
        e.end = ranges[0].second;
        e.text = new_text[0];
        m_trace.events.push_back(e);
        m_net.edit(r, ranges, new_text);
    }

    void undo(int r) {
        Event e;
        e.kind = EventKind::Undo;
        e.replica = r;
        m_trace.events.push_back(e);
        m_net.undo(r);
    }

    void redo(int r) {
        Event e;
        e.kind = EventKind::Redo;
        e.replica = r;
        m_trace.events.push_back(e);
        m_net.redo(r);
    }

    void disconnect(int r) {
        Event e;
        e.kind = EventKind::Disconnect;
        e.replica = r;
        m_trace.events.push_back(e);
        m_net.disconnect(r);
    }

    void reconnect(int r) {
        Event e;
        e.kind = EventKind::Reconnect;
        e.replica = r;
        m_trace.events.push_back(e);
        m_net.reconnect(r);
    }

    void block_link(int from, int to) {
        Event e;
        e.kind = EventKind::BlockLink;
        e.replica = from;
        e.peer = to;
        m_trace.events.push_back(e);
        m_net.block_link(from, to);
    }

    void unblock_link(int from, int to) {
        Event e;
        e.kind = EventKind::UnblockLink;
        e.replica = from;
        e.peer = to;
        m_trace.events.push_back(e);
        m_net.unblock_link(from, to);
    }

    void tick(uint64_t ms) {
        Event e;
        e.kind = EventKind::Tick;
        e.ms = ms;
        m_trace.events.push_back(e);
        m_net.tick(ms);
    }

    void drain() {
        Event e;
        e.kind = EventKind::Drain;
        m_trace.events.push_back(e);
        m_net.drain();
    }

    void collect_garbage(int r) {
        Event e;
        e.kind = EventKind::CollectGarbage;
        e.replica = r;
        m_trace.events.push_back(e);
        m_net.collect_garbage(r);
    }

    void compact_all() {
        Event e;
        e.kind = EventKind::Compact;
        m_trace.events.push_back(e);
        m_net.compact_all();
    }

    std::string save(std::string_view suffix) const { return save_trace(m_trace, suffix); }

    /// Emit the scenario's trace when COLLABTEXT_TRACE_OUTPUT is set, so an
    /// artifact campaign leaves one replayable file per scenario and not only
    /// a file for the scenarios that failed. Returns the path, or empty.
    std::string finish() const { return save("trace"); }

private:
    Trace m_trace;
    Crdt::NetworkSim m_net;
};

}  // namespace CollabText::Test
