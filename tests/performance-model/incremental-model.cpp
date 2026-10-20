#include "incremental-model.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <vector>
#include <random>

namespace IncrementalReference {
namespace {

struct Id {
    uint16_t actor = 0;
    uint64_t counter = 0;
    auto operator<=>(const Id&) const = default;
};

struct Item {
    Id id;
    uint8_t bytes = 1;
    uint8_t utf16 = 1;
    bool marker = false;
    bool visible = true;
    std::vector<uint64_t> deletions;
};

struct Run {
    uint64_t handle = 0;
    std::vector<Item> items;
};

struct Summary {
    uint64_t visible_bytes = 0;
    uint64_t hidden_bytes = 0;
    uint64_t retained_items = 0;
    uint64_t visible_items = 0;
    uint64_t visible_scalars = 0;
    uint64_t visible_utf16 = 0;
    uint64_t retained_runs = 0;
    auto operator<=>(const Summary&) const = default;
};

Summary plus(Summary a, const Summary& b) {
    a.visible_bytes += b.visible_bytes;
    a.hidden_bytes += b.hidden_bytes;
    a.retained_items += b.retained_items;
    a.visible_items += b.visible_items;
    a.visible_scalars += b.visible_scalars;
    a.visible_utf16 += b.visible_utf16;
    a.retained_runs += b.retained_runs;
    return a;
}

using OriginKey = std::pair<uint16_t, uint64_t>;
struct OriginValue { uint64_t end = 0; uint64_t handle = 0; uint32_t slot = 0; auto operator<=>(const OriginValue&) const = default; };
using DeletionKey = std::tuple<uint64_t, uint16_t, uint64_t>;
struct DeletionValue { uint64_t end = 0; uint64_t handle = 0; auto operator<=>(const DeletionValue&) const = default; };

struct State {
    std::vector<Run> runs;
    std::map<OriginKey, OriginValue> origins;
    std::map<DeletionKey, DeletionValue> deletion_incidence;
    std::map<Id, uint32_t> insertion_undo;
    std::map<uint64_t, uint32_t> deletion_undo;
    Summary summary;
    uint64_t next_handle = 1;
    uint64_t next_actor3_counter = 1;
    uint64_t next_actor4_counter = 1;
    uint64_t version = 0;
    uint64_t callbacks = 0;
};

enum class Kind : uint8_t { insert, insert_marker, split, erase, toggle_deletion, toggle_insertion, collect };
struct Event { Kind kind; };

bool same_run_properties(const Item& a, const Item& b) {
    return !a.marker && !b.marker && a.visible == b.visible &&
           a.deletions == b.deletions && a.id.actor == b.id.actor &&
           a.id.counter != UINT64_MAX && a.id.counter + 1 == b.id.counter;
}

bool visible_now(const State& state, const Item& item) {
    const auto insertion = state.insertion_undo.find(item.id);
    if (insertion != state.insertion_undo.end() && (insertion->second & 1u)) return false;
    for (const auto deletion : item.deletions) {
        const auto it = state.deletion_undo.find(deletion);
        if (it == state.deletion_undo.end() || (it->second & 1u) == 0) return false;
    }
    return true;
}

void normalize(State& state) {
    std::vector<Run> normalized;
    normalized.reserve(state.runs.size() + 4);
    for (auto& old : state.runs) {
        if (old.items.empty()) continue;
        Run current{old.handle, {}};
        for (auto item : old.items) {
            item.visible = visible_now(state, item);
            if (!current.items.empty() && !same_run_properties(current.items.back(), item)) {
                normalized.push_back(std::move(current));
                current = Run{state.next_handle++, {}};
            }
            current.items.push_back(std::move(item));
        }
        normalized.push_back(std::move(current));
    }
    state.runs = std::move(normalized);
}

Summary summarize_item(const Item& item) {
    Summary s;
    s.retained_items = 1;
    if (item.marker) {
        s.visible_items = item.visible ? 1 : 0;
    } else if (item.visible) {
        s.visible_bytes = item.bytes;
        s.visible_items = 1;
        s.visible_scalars = 1;
        s.visible_utf16 = item.utf16;
    } else {
        s.hidden_bytes = item.bytes;
    }
    return s;
}

void build_origins(State& state) {
    state.origins.clear();
    for (uint32_t slot = 0; slot < state.runs.size(); ++slot) {
        const auto& run = state.runs[slot];
        if (run.items.empty()) throw std::runtime_error("empty run");
        const auto& first = run.items.front();
        uint64_t expected = first.id.counter;
        for (const auto& item : run.items) {
            if (item.id.actor != first.id.actor || item.id.counter != expected ||
                item.deletions != first.deletions || item.visible != first.visible ||
                item.marker != first.marker)
                throw std::runtime_error("run is not homogeneous");
            if (expected == UINT64_MAX) throw std::runtime_error("identity interval overflow");
            ++expected;
        }
        if (!state.origins.emplace(OriginKey{first.id.actor, first.id.counter},
                OriginValue{expected, run.handle, slot}).second)
            throw std::runtime_error("overlapping origin interval");
    }
    for (auto it = state.origins.begin(); it != state.origins.end(); ++it) {
        auto next = std::next(it);
        if (next != state.origins.end() && next->first.first == it->first.first &&
            next->first.second < it->second.end)
            throw std::runtime_error("overlapping origin intervals");
    }
}

void build_deletion_incidence(State& state) {
    state.deletion_incidence.clear();
    for (const auto& run : state.runs) {
        if (run.items.empty()) throw std::runtime_error("empty run");
        const auto& first = run.items.front();
        const uint64_t end = first.id.counter + run.items.size();
        const std::set<uint64_t> deletions(first.deletions.begin(), first.deletions.end());
        for (const auto deletion : deletions) {
            if (!state.deletion_incidence.emplace(
                    DeletionKey{deletion, first.id.actor, first.id.counter},
                    DeletionValue{end, run.handle}).second)
                throw std::runtime_error("duplicate deletion incidence interval");
        }
    }
}

void recompute_summary(State& state) {
    state.summary = {};
    for (const auto& run : state.runs) {
        for (const auto& item : run.items)
            state.summary = plus(state.summary, summarize_item(item));
        if (!run.items.empty()) ++state.summary.retained_runs;
    }
}

void build_derived(State& state) {
    build_origins(state);
    build_deletion_incidence(state);
    recompute_summary(state);
}

State initial_state() {
    State s;
    s.runs.push_back({s.next_handle++, {
        Item{{1,1},1,1,false,true,{}}, Item{{1,2},2,1,false,true,{}},
        Item{{1,3},4,2,false,true,{}}, Item{{1,4},1,1,false,true,{}}}});
    s.runs.push_back({s.next_handle++, {Item{{1,5},0,0,true,true,{}}}});
    s.runs.push_back({s.next_handle++, {
        Item{{2,1},1,1,false,true,{}}, Item{{2,2},4,2,false,true,{}},
        Item{{2,3},1,1,false,true,{}}}});
    build_derived(s);
    return s;
}

std::optional<std::pair<size_t, size_t>> scan_find(const State& state, Id id) {
    for (size_t r = 0; r < state.runs.size(); ++r)
        for (size_t i = 0; i < state.runs[r].items.size(); ++i)
            if (state.runs[r].items[i].id == id) return std::pair{r, i};
    return std::nullopt;
}

std::optional<std::pair<size_t, size_t>> indexed_find(const State& state, Id id) {
    auto it = state.origins.upper_bound({id.actor, id.counter});
    if (it == state.origins.begin()) return std::nullopt;
    --it;
    if (it->first.first != id.actor || id.counter < it->first.second ||
        id.counter >= it->second.end || it->second.slot >= state.runs.size())
        return std::nullopt;
    const auto& run = state.runs[it->second.slot];
    if (run.handle != it->second.handle) return std::nullopt;
    return std::pair{static_cast<size_t>(it->second.slot),
                     static_cast<size_t>(id.counter - it->first.second)};
}

std::string fingerprint(const State& state) {
    std::ostringstream out;
    out << state.version << ':' << state.next_handle << ':' << state.next_actor3_counter
        << ':' << state.next_actor4_counter
        << ':' << state.callbacks << '|';
    for (const auto& run : state.runs) {
        out << 'h' << run.handle << '[';
        for (const auto& item : run.items) {
            out << item.id.actor << '/' << item.id.counter << '/' << unsigned(item.bytes)
                << '/' << unsigned(item.utf16) << '/' << item.marker << '/' << item.visible;
            for (auto deletion : item.deletions) out << 'd' << deletion;
            out << ';';
        }
        out << ']';
    }
    for (const auto& [key, value] : state.origins)
        out << "o" << key.first << ':' << key.second << ':' << value.end << ':'
            << value.handle << ':' << value.slot;
    for (const auto& [key, value] : state.deletion_incidence)
        out << "d" << std::get<0>(key) << ':' << std::get<1>(key) << ':'
            << std::get<2>(key) << ':' << value.end << ':' << value.handle;
    out << "s" << state.summary.visible_bytes << ':' << state.summary.hidden_bytes << ':'
        << state.summary.retained_items << ':' << state.summary.visible_items << ':'
        << state.summary.visible_scalars << ':' << state.summary.visible_utf16 << ':'
        << state.summary.retained_runs;
    for (const auto& [id, count] : state.insertion_undo)
        out << "u" << id.actor << ':' << id.counter << ':' << count;
    for (const auto& [id, count] : state.deletion_undo) out << "v" << id << ':' << count;
    return out.str();
}

bool check_state(const State& state, uint64_t& checks) {
    State oracle = state;
    build_derived(oracle);
    if (oracle.origins != state.origins ||
        oracle.deletion_incidence != state.deletion_incidence ||
        oracle.summary != state.summary) return false;
    std::set<Id> ids;
    for (const auto& run : state.runs) {
        if (run.handle == 0 || run.items.empty()) return false;
        for (const auto& item : run.items) {
            if (!ids.insert(item.id).second || item.visible != visible_now(state, item)) return false;
            if ((item.marker && (item.bytes != 0 || item.utf16 != 0)) ||
                (!item.marker && (item.bytes < 1 || item.bytes > 4 ||
                                  item.utf16 != (item.bytes == 4 ? 2 : 1))))
                return false;
            ++checks;
            if (indexed_find(state, item.id) != scan_find(state, item.id)) return false;
        }
    }
    for (const auto& [key, value] : state.deletion_incidence) {
        (void)value;
        bool seen = false;
        for (const auto& run : state.runs)
            if (run.handle == value.handle)
                for (const auto& item : run.items)
                    if (item.id.actor == std::get<1>(key) && item.id.counter >= std::get<2>(key) &&
                        item.id.counter < value.end &&
                        std::find(item.deletions.begin(), item.deletions.end(), std::get<0>(key)) != item.deletions.end())
                        seen = true;
        if (!seen) return false;
        ++checks;
    }
    return true;
}

std::vector<Id> all_ids(const State& state) {
    std::vector<Id> out;
    for (const auto& run : state.runs)
        for (const auto& item : run.items) out.push_back(item.id);
    return out;
}

bool mutate_candidate(State& s, Event event) {
    switch (event.kind) {
    case Kind::insert: {
        if (s.summary.retained_items > 128) return false;
        const uint64_t next = s.next_actor3_counter;
        if (next >= UINT64_MAX - 1) return false;
        s.next_actor3_counter += 2;
        Run added{s.next_handle++, {Item{{3,next},1,1,false,true,{}},
                                    Item{{3,next+1},2,1,false,true,{}}}};
        s.runs.insert(s.runs.begin() + static_cast<ptrdiff_t>(s.runs.size()/2), std::move(added));
        break;
    }
    case Kind::insert_marker: {
        if (s.next_actor4_counter == UINT64_MAX) return false;
        const size_t rank = s.runs.size()/2;
        Run marker{s.next_handle++, {Item{{4,s.next_actor4_counter++},0,0,true,true,{}}}};
        s.runs.insert(s.runs.begin()+static_cast<ptrdiff_t>(rank), std::move(marker));
        break;
    }
    case Kind::split: {
        auto it = std::find_if(s.runs.begin(), s.runs.end(), [](const Run& r){ return r.items.size() > 1; });
        if (it == s.runs.end()) return false;
        const size_t rank = static_cast<size_t>(std::distance(s.runs.begin(), it));
        const size_t cut = it->items.size()/2;
        Run tail{s.next_handle++, {}};
        tail.items.assign(it->items.begin()+static_cast<ptrdiff_t>(cut), it->items.end());
        it->items.erase(it->items.begin()+static_cast<ptrdiff_t>(cut), it->items.end());
        s.runs.insert(s.runs.begin()+static_cast<ptrdiff_t>(rank+1), std::move(tail));
        break;
    }
    case Kind::erase: {
        const auto ids = all_ids(s);
        if (ids.empty()) return false;
        const Id target = ids[ids.size()/2];
        const auto found = indexed_find(s, target);
        if (!found) return false;
        auto& item = s.runs[found->first].items[found->second];
        const uint64_t deletion = 1000 + s.version;
        if (std::find(item.deletions.begin(), item.deletions.end(), deletion) == item.deletions.end())
            item.deletions.push_back(deletion);
        std::sort(item.deletions.begin(), item.deletions.end());
        break;
    }
    case Kind::toggle_deletion: {
        if (s.deletion_incidence.empty()) return false;
        const auto id = std::get<0>(s.deletion_incidence.begin()->first);
        ++s.deletion_undo[id];
        break;
    }
    case Kind::toggle_insertion: {
        const auto ids = all_ids(s);
        if (ids.empty()) return false;
        ++s.insertion_undo[ids.back()];
        break;
    }
    case Kind::collect: {
        bool removed = false;
        for (auto r = s.runs.begin(); r != s.runs.end();) {
            std::erase_if(r->items, [](const Item& item){ return !item.visible; });
            if (r->items.empty()) { r = s.runs.erase(r); removed = true; }
            else ++r;
        }
        if (!removed) return false;
        break;
    }
    }
    normalize(s);
    return true;
}

// Return true only when the entire candidate is published. A nonnegative
// fail_after injects failure after the named preparation stage and discards it.
bool transact(State& published, Event event, int fail_after, uint64_t& checks) {
    const auto before = fingerprint(published);
    State candidate = published;
    if (!mutate_candidate(candidate, event)) return false;
    if (fail_after == 0) return false;             // tree path
    build_origins(candidate);                       // origin intervals + locators
    if (fail_after == 1) return false;
    build_deletion_incidence(candidate);
    if (fail_after == 2) return false;
    recompute_summary(candidate);
    if (!check_state(candidate, checks)) throw std::runtime_error("candidate invariant failure");
    if (fail_after == 3) return false;             // summaries
    ++candidate.version;                           // undo/causal state is staged
    if (fail_after == 4) return false;
    ++candidate.callbacks;                         // notification intent is staged
    if (fail_after == 5) return false;
    if (!check_state(candidate, checks)) throw std::runtime_error("prepublish invariant failure");
    published = std::move(candidate);              // single publication point
    if (fingerprint(published) == before) throw std::runtime_error("accepted event made no state change");
    return true;
}

Event event_from(unsigned choice) { return {static_cast<Kind>(choice % 7)}; }

void hash_append(uint64_t& hash, const std::string& text) {
    for (unsigned char c : text) { hash ^= c; hash *= 1099511628211ULL; }
}

} // namespace

Report enumerate_short_traces() {
    Report report;
    uint64_t digest = 1469598103934665603ULL;
    constexpr unsigned alphabet = 7;
    for (unsigned encoded = 0; encoded < alphabet*alphabet*alphabet*alphabet; ++encoded) {
        State state = initial_state();
        unsigned value = encoded;
        for (unsigned step = 0; step < 4; ++step) {
            const Event event = event_from(value % alphabet);
            value /= alphabet;
            const auto before = fingerprint(state);
            uint64_t checks = 0;
            State failed = state;
            for (int stage = 0; stage <= 5; ++stage) {
                const auto before_failure = fingerprint(failed);
                const bool accepted = transact(failed, event, stage, checks);
                if (accepted || fingerprint(failed) != before_failure)
                    throw std::runtime_error("injected failure published partial state");
                ++report.injected_failures;
            }
            const bool accepted = transact(state, event, -1, checks);
            if (!accepted) ++report.refusals;
            if (accepted && !check_state(state, checks))
                throw std::runtime_error("short trace invariant failure");
            if (!accepted && fingerprint(state) != before)
                throw std::runtime_error("refusal changed published state");
            report.events++;
            report.checks += checks;
            hash_append(digest, fingerprint(state));
        }
        ++report.traces;
    }
    std::ostringstream out; out << std::hex << digest; report.digest = out.str();
    return report;
}

Report run_seeded_traces(uint64_t seed, unsigned traces, unsigned steps) {
    Report report;
    uint64_t digest = 1469598103934665603ULL;
    std::mt19937_64 random(seed);
    for (unsigned trace = 0; trace < traces; ++trace) {
        State state = initial_state();
        for (unsigned step = 0; step < steps; ++step) {
            const Event event = event_from(static_cast<unsigned>(random()));
            const auto before = fingerprint(state);
            uint64_t checks = 0;
            State failed = state;
            const int stage = static_cast<int>(random() % 6);
            const auto failed_before = fingerprint(failed);
            const bool failed_accepted = transact(failed, event, stage, checks);
            if (failed_accepted || fingerprint(failed) != failed_before)
                throw std::runtime_error("seeded injected failure published partial state");
            ++report.injected_failures;
            const bool accepted = transact(state, event, -1, checks);
            if (!accepted) ++report.refusals;
            if (accepted && !check_state(state, checks))
                throw std::runtime_error("seeded trace invariant failure");
            if (!accepted && fingerprint(state) != before)
                throw std::runtime_error("seeded refusal changed published state");
            ++report.events;
            report.checks += checks;
            hash_append(digest, fingerprint(state));
        }
        ++report.traces;
    }
    std::ostringstream out; out << std::hex << digest; report.digest = out.str();
    return report;
}

void check_summary_laws(uint64_t seed, unsigned trials) {
    std::mt19937_64 random(seed);
    for (unsigned i = 0; i < trials; ++i) {
        const auto gen = [&]() {
            return Summary{random()%100, random()%100, random()%100,
                           random()%100, random()%100, random()%100, random()%100};
        };
        const Summary a = gen(), b = gen(), c = gen(), zero{};
        if (plus(zero,a) != a || plus(a,zero) != a ||
            plus(plus(a,b),c) != plus(a,plus(b,c)))
            throw std::runtime_error("summary monoid law failed");
    }
}

} // namespace IncrementalReference
