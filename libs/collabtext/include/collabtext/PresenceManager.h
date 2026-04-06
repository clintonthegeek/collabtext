#pragma once

#include "collabtext/Identity.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace CollabText::Identity {

// ============================================================================
// PresenceManager — writes local presence/ephemeral state, reads remote state
// ============================================================================
//
// No internal timer or event loop. The caller drives cadence (e.g. a QTimer).
//
// File layout under shared_folder:
//   replicas/<replica_id>/presence.json
//   replicas/<replica_id>/ephemeral.json

class PresenceManager {
public:
    PresenceManager(std::filesystem::path shared_folder,
                    std::string replica_id,
                    std::string identity_id);

    // ---- writes ----

    /// Atomically write presence.json for our replica.
    void write_presence(const Presence& presence);

    /// Atomically write ephemeral.json for our replica.
    void write_ephemeral(const EphemeralState& state);

    // ---- reads ----

    /// Enumerate replicas/*/presence.json, skipping our own replica_id.
    /// Returns (replica_id, Presence) pairs; unparseable files are skipped.
    std::vector<std::pair<std::string, Presence>> read_remote_presences() const;

    /// Enumerate replicas/*/ephemeral.json, skipping our own replica_id.
    /// Returns (replica_id, EphemeralState) pairs; unparseable files are skipped.
    std::vector<std::pair<std::string, EphemeralState>> read_remote_ephemerals() const;

    // ---- liveness helpers (static, no I/O) ----

    /// True if active == true AND heartbeat is within the last 30 seconds.
    static bool is_live(const Presence& p);

    /// True if active == true AND heartbeat is older than 30 seconds.
    static bool is_stale(const Presence& p);

    /// True if active == false (regardless of heartbeat age).
    static bool is_departed(const Presence& p);

    // ---- lifecycle ----

    /// Read presence.json, set active=false, write it back.
    void depart();

    // ---- accessors ----

    const std::string& replica_id() const;
    const std::string& identity_id() const;

private:
    std::filesystem::path shared_folder_;
    std::string replica_id_;
    std::string identity_id_;

    std::filesystem::path own_dir() const;

    /// Atomically write content to path (write to .tmp then rename).
    static void atomic_write(const std::filesystem::path& path,
                             const std::string& content);

    /// Parse an ISO 8601 UTC timestamp ("YYYY-MM-DDTHH:MM:SSZ") to time_t.
    /// Returns -1 on parse failure.
    static std::time_t parse_iso8601(const std::string& ts);
};

} // namespace CollabText::Identity
