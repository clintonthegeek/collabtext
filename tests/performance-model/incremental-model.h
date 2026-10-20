#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace IncrementalReference {

struct Report {
    uint64_t traces = 0;
    uint64_t events = 0;
    uint64_t checks = 0;
    uint64_t refusals = 0;
    uint64_t injected_failures = 0;
    std::string digest;
};

Report enumerate_short_traces();
Report run_seeded_traces(uint64_t seed, unsigned traces, unsigned steps);
void check_summary_laws(uint64_t seed, unsigned trials);

} // namespace IncrementalReference
