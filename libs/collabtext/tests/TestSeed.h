// tests/TestSeed.h — deterministic seed and trace-artifact configuration for
// the randomized suites.
//
// Roadmap task V04 ("Make randomized failures replayable and reducible").
//
// Two environment variables control every migrated randomized suite:
//
//   COLLABTEXT_TEST_SEED    uint64 seed (decimal, no sign/whitespace) or the
//                           literal word "random". Unset, malformed, or out of
//                           range values fall back to the documented fixed
//                           default kDefaultTestSeed, and a malformed value is
//                           reported once per process. The default makes a
//                           plain `./tst_fuzz` run a deterministic CI sample
//                           rather than an unrepeatable campaign.
//   COLLABTEXT_TRACE_OUTPUT Directory for trace artifacts. Unset means the
//                           suite records nothing to disk; suites still print
//                           their effective seed and keep the in-memory trace.
//
// Every migrated suite prints its effective seed, so a failure report always
// names the campaign that produced it:
//
//   COLLABTEXT_TEST_SEED=random COLLABTEXT_TRACE_OUTPUT=/tmp/ct-trace \
//     ./build-roadmap-debug/libs/collabtext/tst_fuzz convergence_utf8_stress
//
// A random campaign is opt-in precisely because it is not replayable on its
// own: what makes it replayable is the versioned event trace (TraceHarness.h),
// which records the decisions rather than the entropy.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <random>
#include <iostream>
#include <string>
#include <string_view>

namespace CollabText::Test {

/// Documented fixed default seed. Ordinary verification and CI use this
/// value; it is also the seed named by the roadmap's fixed-seed runs.
inline constexpr uint64_t kDefaultTestSeed = 20261003ULL;

struct SeedConfig {
    uint64_t seed = kDefaultTestSeed;      ///< effective campaign seed
    bool random_campaign = false;          ///< COLLABTEXT_TEST_SEED=random
    bool seed_rejected = false;            ///< COLLABTEXT_TEST_SEED was malformed
    std::string seed_raw;                  ///< value as read, for diagnostics
    std::string trace_output_dir;          ///< empty when trace output is off
};

namespace detail {

/// Strict uint64 parse: digits only, no sign, no whitespace, no overflow.
/// std::stoull would accept " 42", "-1" (wrapping), and "42abc" (prefix).
inline bool parse_u64_strict(std::string_view text, uint64_t& out) {
    if (text.empty()) return false;
    uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;  // overflow
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

inline uint64_t entropy_seed() {
    std::random_device device;
    uint64_t high = device();
    uint64_t low = device();
    return (high << 32) ^ low;
}

/// FNV-1a over a scenario label, used to derive a per-scenario seed from the
/// campaign seed. Deriving instead of drawing keeps every scenario
/// independent of the order in which the runner executes them.
inline uint64_t mix_label(uint64_t seed, std::string_view label) {
    uint64_t hash = 1469598103934665603ULL;
    for (const char c : label) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    return seed ^ (hash * 0x9E3779B97F4A7C15ULL);
}

}  // namespace detail

/// Process-wide seed configuration, parsed once from the environment.
inline const SeedConfig& seed_config() {
    static const SeedConfig config = [] {
        SeedConfig cfg;
        if (const char* raw = std::getenv("COLLABTEXT_TEST_SEED")) {
            cfg.seed_raw = raw;
            if (cfg.seed_raw == "random") {
                cfg.random_campaign = true;
                cfg.seed = detail::entropy_seed();
            } else if (!detail::parse_u64_strict(cfg.seed_raw, cfg.seed)) {
                std::cerr << "COLLABTEXT_TEST_SEED=\"" << cfg.seed_raw
                          << "\" is not a uint64 (or \"random\"); using default seed "
                          << kDefaultTestSeed << "\n";
                cfg.seed = kDefaultTestSeed;
                cfg.seed_rejected = true;
            }
        }
        if (const char* dir = std::getenv("COLLABTEXT_TRACE_OUTPUT")) {
            if (*dir != '\0') cfg.trace_output_dir = dir;
        }
        return cfg;
    }();
    return config;
}

/// Seed for one named scenario, derived from the campaign seed.
inline uint64_t effective_seed(std::string_view label) {
    return detail::mix_label(seed_config().seed, label);
}

/// Path for a trace artifact, or an empty string when COLLABTEXT_TRACE_OUTPUT
/// is unset. The caller supplies the file name only.
inline std::string trace_artifact_path(std::string_view file_name) {
    const std::string& dir = seed_config().trace_output_dir;
    if (dir.empty()) return {};
    std::string path = dir;
    if (!path.empty() && path.back() != '/') path += '/';
    path += file_name;
    return path;
}

/// Announce the campaign seed for a suite/scenario pair. Every migrated suite
/// calls this so its output always names the effective seed.
inline void announce_seed(std::string_view suite, std::string_view scenario,
                          std::string_view kind) {
    std::cout << "[seed] suite=" << suite << " scenario=" << scenario
              << " campaign_seed=" << seed_config().seed
              << " effective_seed=" << effective_seed(scenario)
              << " source="
              << (seed_config().random_campaign ? "random" : "fixed")
              << " kind=" << kind << "\n";
}

}  // namespace CollabText::Test
