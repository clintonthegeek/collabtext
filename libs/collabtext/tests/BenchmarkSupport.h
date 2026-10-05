// tests/BenchmarkSupport.h — measurement primitives for tst_benchmark.
//
// Roadmap task V06 ("Repair benchmark timing, units, correctness, and memory
// labels").
//
// The rules this header exists to enforce, because the benchmarks used to break
// all of them:
//
//  * One clock, and it is a steady clock. A duration measured with a
//    non-monotonic clock can be negative or wildly wrong across a clock
//    adjustment.
//  * Every number carries its unit and the work count it came from. "ns/op" is
//    meaningless next to a total; both are reported separately, and the op count
//    is named.
//  * A metric that was not measured is absent, never zero. A fabricated "0 KB"
//    memory delta reads like a result.
//  * Process RSS is process RSS. It is labelled `process_rss_kb` and is never
//    presented as engine memory, because it includes the allocator, Qt, and
//    everything else in the address space. Engine size is reported
//    *structurally*: fragment/entry counts and visible/deleted bytes, which is
//    what the engine actually holds.
//  * A timed region contains the work it claims to measure. Callers start the
//    stopwatch before reconnect/tick and stop it after the deferred retry, not
//    after the work already happened.

#pragma once

#include "TestSeed.h"
#include "TestActor.h"
#include "TraceHarness.h"

#include "crdt/Buffer.h"
#include "crdt/IdList.h"
#include "crdt/NetworkSim.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace CollabText::Bench {

using Crdt::Buffer;
using Crdt::IdList;
using Crdt::NetworkSim;
using Crdt::Operation;
using Test::Event;
using Test::EventKind;
using Test::Trace;

using Clock = std::chrono::steady_clock;

/// Wall duration of a region. Construct before the work, read elapsed_ns() after.
class Stopwatch {
public:
    Stopwatch() : m_start(Clock::now()) {}

    void restart() { m_start = Clock::now(); }

    uint64_t elapsed_ns() const {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - m_start)
                .count());
    }

    double elapsed_ms() const { return elapsed_ns() / 1e6; }

private:
    Clock::time_point m_start;
};

/// Process resident set size in KiB, or absent when the platform does not
/// expose it. This is process memory, not engine memory.
inline std::optional<int64_t> process_rss_kb() {
    std::ifstream file("/proc/self/statm");
    if (!file.is_open()) return std::nullopt;
    long total_pages = 0;
    long resident_pages = 0;
    file >> total_pages >> resident_pages;
    if (!file) return std::nullopt;
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return std::nullopt;
    return static_cast<int64_t>(resident_pages) * page_size / 1024;
}

/// What the engine structurally holds. These are counts, not byte estimates of
/// allocation: a fragment costs more than its text, and this does not pretend to
/// price it.
struct EngineSize {
    uint64_t units = 0;           ///< fragments (Buffer) or entries (IdList)
    uint64_t tombstones = 0;
    uint64_t visible_bytes = 0;
    uint64_t deleted_bytes = 0;
    std::string unit_name = "fragments";
};

inline EngineSize engine_size(const Crdt::Buffer& buffer) {
    EngineSize size;
    size.unit_name = "fragments";
    size.units = buffer.fragment_count();
    size.tombstones = buffer.tombstone_count();
    size.visible_bytes = buffer.visible_length();
    size.deleted_bytes = buffer.deleted_rope_len();
    return size;
}

inline EngineSize engine_size(const Crdt::IdList& list) {
    EngineSize size;
    size.unit_name = "entries";
    size.units = list.entry_count();
    size.tombstones = list.tombstone_count();
    size.visible_bytes = list.size();
    return size;
}

/// One reported benchmark result. Every field is either a measured value with a
/// stated unit, or absent.
struct Measurement {
    std::string name;

    /// Wall duration of the timed region, in nanoseconds. Always measured.
    uint64_t wall_ns = 0;
    /// What the timed region did, named. e.g. "remote operations applied".
    std::string work_label = "operations";
    /// How many of those. The denominator for any per-item figure below.
    uint64_t work_items = 0;

    /// Operations produced locally in this measurement (edit/undo/redo).
    uint64_t source_edits = 0;
    /// Remote operations applied in this measurement.
    uint64_t remote_applications = 0;

    /// Process RSS sampled at the end, if available. Never engine memory.
    std::optional<int64_t> rss_kb;
    /// When true, rss_kb is a delta from a sampled baseline rather than an
    /// absolute, and is printed as `process_rss_delta`. A delta of zero means
    /// "no change", which is not the same claim as "0 KB of memory".
    bool rss_is_delta = false;
    /// Structural engine size at the end, if the benchmark has an engine.
    std::optional<EngineSize> engine;

    /// Nanoseconds per unit of work, from wall_ns and work_items.
    std::optional<double> ns_per_work_item() const {
        if (work_items == 0) return std::nullopt;
        return static_cast<double>(wall_ns) / static_cast<double>(work_items);
    }

    std::optional<double> work_per_sec() const {
        if (wall_ns == 0) return std::nullopt;
        return static_cast<double>(work_items) * 1e9 / static_cast<double>(wall_ns);
    }

    /// Human-readable line with units spelled out and absent metrics omitted.
    std::string to_text() const {
        std::ostringstream out;
        out << "  " << name
            << ": wall=" << (static_cast<double>(wall_ns) / 1e6) << " ms"
            << " (" << wall_ns << " ns)"
            << ", work=" << work_items << " " << work_label;
        if (const auto per = ns_per_work_item())
            out << ", " << *per << " ns/" << work_label;
        if (const auto rate = work_per_sec())
            out << ", " << *rate << " " << work_label << "/s";
        out << ", source_edits=" << source_edits
            << ", remote_applications=" << remote_applications;
        if (engine) {
            out << ", " << engine->unit_name << "=" << engine->units
                << ", tombstones=" << engine->tombstones
                << ", visible_bytes=" << engine->visible_bytes;
        }
        if (rss_kb)
            out << (rss_is_delta ? ", process_rss_delta=" : ", process_rss=")
                << *rss_kb << " KB";
        else
            out << ", process_rss=absent";
        return out.str();
    }

    /// Machine-readable form. Absent metrics are null, never 0.
    std::string to_json() const {
        std::ostringstream out;
        out << "{"
            << "\"name\":\"" << name << "\""
            << ",\"wall_ns\":" << wall_ns
            << ",\"work_label\":\"" << work_label << "\""
            << ",\"work_items\":" << work_items
            << ",\"source_edits\":" << source_edits
            << ",\"remote_applications\":" << remote_applications;
        if (const auto per = ns_per_work_item())
            out << ",\"ns_per_work_item\":" << *per;
        else
            out << ",\"ns_per_work_item\":null";
        if (const auto rate = work_per_sec())
            out << ",\"work_per_sec\":" << *rate;
        else
            out << ",\"work_per_sec\":null";
        if (rss_kb) {
            out << ",\"" << (rss_is_delta ? "process_rss_delta_kb"
                                           : "process_rss_kb")
                << "\":" << *rss_kb;
            if (rss_is_delta) out << ",\"process_rss_kb\":null";
        } else {
            out << ",\"process_rss_kb\":null";
        }
        if (engine) {
            out << ",\"" << engine->unit_name << "\":" << engine->units
                << ",\"tombstones\":" << engine->tombstones
                << ",\"visible_bytes\":" << engine->visible_bytes
                << ",\"deleted_bytes\":" << engine->deleted_bytes;
        }
        out << "}";
        return out.str();
    }
};

inline void print(const Measurement& measurement) {
    std::printf("%s\n", measurement.to_text().c_str());
    std::printf("%s\n", measurement.to_json().c_str());
}

// ============================================================================
// Recreated workloads
// ============================================================================
//
// A before/after comparison must compare two *independent executions of the
// same work*, not one buffer before and after a call: the first execution warms
// the allocator, grows SumTree nodes, and leaves its own history behind, which
// is exactly the effect being measured. Recording the decisions and rebuilding
// from them removes that confound, and it is the same discipline V04 applies to
// failing tests.

/// A single-replica edit workload that can be rebuilt identically.
class Workload {
public:
    Workload(const ActorId &actor, std::string label)
        : m_buffer(actor), m_label(std::move(label)) {}

    Buffer& buffer() { return m_buffer; }
    const Buffer& buffer() const { return m_buffer; }
    const std::string& label() const { return m_label; }
    const std::vector<Event>& decisions() const { return m_decisions; }

    /// A local edit, recorded. Returns the operation to broadcast.
    Operation edit(uint32_t start, uint32_t end, std::string text) {
        Event event;
        event.kind = EventKind::Edit;
        event.start = start;
        event.end = end;
        event.text = text;
        m_decisions.push_back(event);
        m_produced.push_back(m_buffer.apply_local_edit({{start, end}}, {text}));
        return m_produced.back();
    }

    /// An undo, recorded. Returns the produced operation, if any.
    std::optional<Operation> undo() {
        Event event;
        event.kind = EventKind::Undo;
        m_decisions.push_back(event);
        auto op = m_buffer.undo();
        if (op) m_produced.push_back(*op);
        return op;
    }

    /// Rebuild this workload's replica from its recorded decisions on a fresh
    /// engine. The result is state-identical to the original, with a different
    /// allocation history.
    void rebuild() {
        Buffer fresh(m_buffer.actor_id());
        std::vector<Operation> produced;
        for (const Event& decision : m_decisions) {
            switch (decision.kind) {
            case EventKind::Edit:
                produced.push_back(
                    fresh.apply_local_edit({{decision.start, decision.end}},
                                           {decision.text}));
                break;
            case EventKind::Undo: {
                auto op = fresh.undo();
                if (op) produced.push_back(*op);
                break;
            }
            default:
                break;
            }
        }
        m_buffer = std::move(fresh);
        m_produced = std::move(produced);
    }

    /// Operations this workload produced, in order.
    const std::vector<Operation>& produced() const { return m_produced; }

private:
    Buffer m_buffer;
    std::string m_label;
    std::vector<Event> m_decisions;
    std::vector<Operation> m_produced;
};

/// Re-execute a recorded NetworkSim trace into `net`, one event at a time.
///
/// Used to recreate an identical simulated network for a second, independently
/// timed run. Partition and ordering decisions come from the trace, so the
/// recreated run is the same run, not a similar one.
inline void reexecute(const Trace& trace, Crdt::NetworkSim& net) {
    for (const Event& event : trace.events) {
        switch (event.kind) {
        case EventKind::Edit:
            net.edit(event.replica, {{event.start, event.end}}, {event.text});
            break;
        case EventKind::Undo: net.undo(event.replica); break;
        case EventKind::Redo: net.redo(event.replica); break;
        case EventKind::Disconnect: net.disconnect(event.replica); break;
        case EventKind::Reconnect: net.reconnect(event.replica); break;
        case EventKind::BlockLink: net.block_link(event.replica, event.peer); break;
        case EventKind::UnblockLink: net.unblock_link(event.replica, event.peer); break;
        case EventKind::Tick: net.tick(event.ms); break;
        case EventKind::Drain: net.drain(); break;
        default:
            // Events with no NetworkSim meaning (per-operation deliveries,
            // GC requests) do not affect the simulated network's evolution.
            break;
        }
    }
}

}  // namespace CollabText::Bench
