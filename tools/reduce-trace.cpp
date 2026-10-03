// tools/reduce-trace.cpp — delta reduction for collabtext event traces.
//
// Roadmap task V04 ("Make randomized failures replayable and reducible").
//
//   reduce-trace <trace-file> [--predicate convergence|invariant|any]
//                              [--out FILE] [--replay] [--max-passes N]
//
// A randomized suite run with COLLABTEXT_TRACE_OUTPUT set leaves a versioned
// event trace behind. This tool replays it, checks that it still reproduces the
// reported failure, then removes events for as long as the trace both stays
// valid and keeps failing. Validity is the load-bearing rule: a candidate whose
// remaining edit ranges, visible indices, or delivery references no longer
// apply is rejected instead of being clamped, so identity and causality are
// never faked. The reduced trace is what a developer pastes into a bug report
// and replays locally.
//
//   --replay  Report the trace's replay result and exit without reducing.
//
// Exit status: 0 success, 1 usage error, 2 unreadable/unsupported trace,
// 3 the trace does not fail the predicate (nothing to reduce), 4 the reduced
// trace failed to reproduce the failure.

#include "TestSeed.h"
#include "TraceHarness.h"

#include <iostream>
#include <string>
#include <string_view>

namespace {

using namespace CollabText::Test;

int usage(const char* program) {
    std::cerr << "Usage: " << program
              << " <trace-file> [--predicate convergence|invariant|any]"
                 " [--out FILE] [--replay] [--max-passes N]\n";
    return 1;
}

void report(const char* label, const ReplayResult& result) {
    std::cout << label << ": status="
              << (result.status == ReplayStatus::Completed ? "completed" : "not-applicable")
              << " converged=" << (result.converged ? "yes" : "no");
    if (!result.converged) std::cout << " (" << result.divergence << ")";
    if (!result.invariant_error.empty())
        std::cout << " invariant=" << result.invariant_error << " replica="
                  << result.invariant_replica;
    std::cout << "\n  events applied=" << result.applied
              << " blocked=" << result.blocked << " lost=" << result.lost
              << " produced_ops=" << result.produced_ops << "\n";
    for (size_t r = 0; r < result.replica_state.size(); ++r) {
        const std::string& state = result.replica_state[r];
        const std::string shown = state.size() > 96 ? state.substr(0, 96) + "..." : state;
        std::cout << "  replica " << r << " (" << state.size() << "): " << shown << "\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage(argv[0]);

    std::string path;
    std::string out_path;
    FailurePredicate predicate = FailurePredicate::Any;
    bool replay_only = false;
    int max_passes = 8;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--replay") {
            replay_only = true;
        } else if (arg == "--predicate" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            if (value == "convergence") predicate = FailurePredicate::Convergence;
            else if (value == "invariant") predicate = FailurePredicate::Invariant;
            else if (value == "any") predicate = FailurePredicate::Any;
            else return usage(argv[0]);
        } else if (arg == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (arg == "--max-passes" && i + 1 < argc) {
            max_passes = std::stoi(argv[++i]);
            if (max_passes < 1) return usage(argv[0]);
        } else if (arg.rfind("--", 0) == 0) {
            return usage(argv[0]);
        } else if (path.empty()) {
            path = arg;
        } else {
            return usage(argv[0]);
        }
    }

    Trace trace;
    std::string error;
    if (!Trace::load(path, trace, error)) {
        std::cerr << "cannot read trace: " << error << "\n";
        return 2;
    }

    std::cout << "trace: " << path << "\n"
              << "  label=" << (trace.label.empty() ? "-" : trace.label)
              << " seed=" << trace.seed << " replicas=" << trace.replicas
              << " model=" << detail::model_name(trace.model)
              << " events=" << trace.events.size() << "\n"
              << "  predicate=" << predicate_name(predicate) << "\n";

    const ReplayResult before = replay(trace);
    report("original", before);
    if (before.status != ReplayStatus::Completed) {
        std::cerr << "the supplied trace does not replay: " << before.error
                  << " (event " << before.error_event << ")\n";
        return 2;
    }
    if (replay_only) return 0;
    if (!predicate_fails(before, predicate)) {
        std::cerr << "the trace does not fail the " << predicate_name(predicate)
                  << " predicate; there is nothing to reduce\n";
        return 3;
    }

    const ReduceOutcome reduced = reduce_trace(trace, predicate, max_passes);
    if (!reduced.ok) {
        std::cerr << "reduction refused: " << reduced.note << "\n";
        return 3;
    }

    const ReplayResult after = replay(reduced.trace);
    report("reduced", after);
    if (after.status != ReplayStatus::Completed || !predicate_fails(after, predicate)) {
        std::cerr << "the reduced trace does not reproduce the failure\n";
        return 4;
    }

    if (out_path.empty()) out_path = path + ".reduced";
    if (!reduced.trace.save(out_path)) {
        std::cerr << "cannot write " << out_path << "\n";
        return 2;
    }
    std::cout << "failure preserved: " << reduced.failure << "\n"
              << "removed " << reduced.removed << " of " << trace.events.size()
              << " events after " << reduced.candidates_tried << " candidates\n"
              << "reduced trace: " << out_path << "\n";
    return 0;
}
