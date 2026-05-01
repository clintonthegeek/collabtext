#pragma once

#include "collabtext/Identity.h"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace CollabText::Identity {

// ============================================================================
// PresenceManager — writes combined state.json (presence+ephemeral, LWW),
// throttled. Reads remote state.json files. No internal timer; the caller
// drives cadence via tick(now).
//
// File layout under shared_folder:
//   replicas/<replica_id>/state.json
// ============================================================================

class PresenceManager {
public:
    PresenceManager(std::filesystem::path shared_folder,
                    std::string replica_id,
                    std::string identity_id);

    /// Read existing state.json on disk (if any) so we can preserve
    /// continuity across restarts. Always safe to call.
    void start();

    /// Stage updates in memory. Disk write is gated by tick()/flush_state().
    void update_presence(const Presence& presence);
    void update_ephemeral(const EphemeralState& state);

    /// Drive throttler:
    ///   - dirty + ≥250 ms since last write → write now
    ///   - clean + ≥25 s since last write → keepalive write
    void tick(std::chrono::steady_clock::time_point now);

    /// Force-write the staged combined state, ignoring the floor.
    void flush_state();

    std::vector<std::pair<std::string, Presence>> read_remote_presences() const;
    std::vector<std::pair<std::string, EphemeralState>> read_remote_ephemerals() const;

    /// True if active == true AND heartbeat is within the last 30 seconds.
    static bool is_live(const Presence& p);
    /// True if active == true AND heartbeat is older than 30 seconds.
    static bool is_stale(const Presence& p);
    /// True if active == false (regardless of heartbeat age).
    static bool is_departed(const Presence& p);

    /// Mark active=false in staged state and force-flush.
    void depart();

    const std::string& replica_id() const { return replica_id_; }
    const std::string& identity_id() const { return identity_id_; }

    /// Test-only: number of state.json rewrites since start().
    uint64_t write_count_for_test() const { return write_count_; }

private:
    std::filesystem::path shared_folder_;
    std::string replica_id_;
    std::string identity_id_;

    Presence staged_presence_;
    EphemeralState staged_ephemeral_;
    bool dirty_ = false;
    std::optional<std::chrono::steady_clock::time_point> last_write_;
    uint64_t write_count_ = 0;

    static constexpr std::chrono::milliseconds kFloor{250};
    static constexpr std::chrono::seconds kCeiling{25};

    std::filesystem::path own_dir_() const;
    void write_now_(std::chrono::steady_clock::time_point now);
    static void atomic_write_(const std::filesystem::path& path,
                              const std::string& content);
    static std::time_t parse_iso8601_(const std::string& ts);
};

} // namespace CollabText::Identity
