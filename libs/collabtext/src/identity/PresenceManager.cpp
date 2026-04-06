#include "collabtext/PresenceManager.h"

#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace CollabText::Identity {

// ============================================================================
// Constructor
// ============================================================================

PresenceManager::PresenceManager(std::filesystem::path shared_folder,
                                 std::string replica_id,
                                 std::string identity_id)
    : shared_folder_(std::move(shared_folder))
    , replica_id_(std::move(replica_id))
    , identity_id_(std::move(identity_id))
{}

// ============================================================================
// Internal helpers
// ============================================================================

std::filesystem::path PresenceManager::own_dir() const {
    return shared_folder_ / "replicas" / replica_id_;
}

void PresenceManager::atomic_write(const std::filesystem::path& path,
                                   const std::string& content) {
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f)
            throw std::runtime_error("PresenceManager: cannot open tmp file: " + tmp.string());
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!f)
            throw std::runtime_error("PresenceManager: write error: " + tmp.string());
    }
    std::filesystem::rename(tmp, path);
}

std::time_t PresenceManager::parse_iso8601(const std::string& ts) {
    // Accept "YYYY-MM-DDTHH:MM:SSZ" (and optionally fractional seconds before Z)
    if (ts.empty()) return -1;

    std::tm tm{};
    const char* p = ts.c_str();
    char* end = nullptr;

    // Try the basic format first
    end = strptime(p, "%Y-%m-%dT%H:%M:%S", &tm);
    if (!end) return -1;

    // Accept optional fractional seconds before 'Z'
    if (*end == '.') {
        ++end;
        while (*end >= '0' && *end <= '9') ++end;
    }

    // Must end with 'Z' for UTC
    if (*end != 'Z') return -1;

    return timegm(&tm);
}

// ============================================================================
// Write operations
// ============================================================================

void PresenceManager::write_presence(const Presence& presence) {
    auto path = own_dir() / "presence.json";
    atomic_write(path, to_json(presence));
}

void PresenceManager::write_ephemeral(const EphemeralState& state) {
    auto path = own_dir() / "ephemeral.json";
    atomic_write(path, to_json(state));
}

// ============================================================================
// Read operations
// ============================================================================

std::vector<std::pair<std::string, Presence>>
PresenceManager::read_remote_presences() const {
    std::vector<std::pair<std::string, Presence>> result;

    auto replicas_dir = shared_folder_ / "replicas";
    std::error_code ec;
    for (auto const& entry : std::filesystem::directory_iterator(replicas_dir, ec)) {
        if (!entry.is_directory()) continue;
        std::string rid = entry.path().filename().string();
        if (rid == replica_id_) continue;

        auto presence_path = entry.path() / "presence.json";
        if (!std::filesystem::exists(presence_path)) continue;

        std::ifstream f(presence_path, std::ios::binary);
        if (!f) continue;

        std::string json((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());

        auto parsed = presence_from_json(json);
        if (!parsed.has_value()) continue;

        result.emplace_back(std::move(rid), std::move(*parsed));
    }

    return result;
}

std::vector<std::pair<std::string, EphemeralState>>
PresenceManager::read_remote_ephemerals() const {
    std::vector<std::pair<std::string, EphemeralState>> result;

    auto replicas_dir = shared_folder_ / "replicas";
    std::error_code ec;
    for (auto const& entry : std::filesystem::directory_iterator(replicas_dir, ec)) {
        if (!entry.is_directory()) continue;
        std::string rid = entry.path().filename().string();
        if (rid == replica_id_) continue;

        auto ephemeral_path = entry.path() / "ephemeral.json";
        if (!std::filesystem::exists(ephemeral_path)) continue;

        std::ifstream f(ephemeral_path, std::ios::binary);
        if (!f) continue;

        std::string json((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());

        auto parsed = ephemeral_from_json(json);
        if (!parsed.has_value()) continue;

        result.emplace_back(std::move(rid), std::move(*parsed));
    }

    return result;
}

// ============================================================================
// Liveness helpers
// ============================================================================

bool PresenceManager::is_live(const Presence& p) {
    if (!p.active) return false;
    auto t = parse_iso8601(p.last_heartbeat);
    if (t == -1) return false;
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    return std::difftime(now, t) < 30.0;
}

bool PresenceManager::is_stale(const Presence& p) {
    if (!p.active) return false;
    auto t = parse_iso8601(p.last_heartbeat);
    if (t == -1) return true; // unparseable timestamp → treat as stale
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    return std::difftime(now, t) >= 30.0;
}

bool PresenceManager::is_departed(const Presence& p) {
    return !p.active;
}

// ============================================================================
// Depart
// ============================================================================

void PresenceManager::depart() {
    auto path = own_dir() / "presence.json";

    std::ifstream f(path, std::ios::binary);
    if (!f) return; // nothing to do if not yet written

    std::string json((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    f.close();

    auto parsed = presence_from_json(json);
    if (!parsed.has_value()) return;

    parsed->active = false;
    atomic_write(path, to_json(*parsed));
}

// ============================================================================
// Accessors
// ============================================================================

const std::string& PresenceManager::replica_id() const {
    return replica_id_;
}

const std::string& PresenceManager::identity_id() const {
    return identity_id_;
}

} // namespace CollabText::Identity
