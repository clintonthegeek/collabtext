#include "collabtext/PresenceManager.h"

#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

namespace CollabText::Identity {

PresenceManager::PresenceManager(std::filesystem::path shared_folder,
                                 std::string replica_id,
                                 std::string identity_id)
    : shared_folder_(std::move(shared_folder))
    , replica_id_(std::move(replica_id))
    , identity_id_(std::move(identity_id)) {}

std::filesystem::path PresenceManager::own_dir_() const {
    return shared_folder_ / "replicas" / replica_id_;
}

void PresenceManager::start() {
    std::error_code ec;
    std::filesystem::create_directories(own_dir_(), ec);
    auto path = own_dir_() / "state.json";
    if (std::filesystem::exists(path)) {
        std::ifstream f(path, std::ios::binary);
        std::string json((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
        if (auto cs = combined_state_from_json(json)) {
            staged_presence_ = cs->presence;
            staged_ephemeral_ = cs->ephemeral;
        }
    }
}

void PresenceManager::update_presence(const Presence& p) {
    staged_presence_ = p;
    dirty_ = true;
}

void PresenceManager::update_ephemeral(const EphemeralState& e) {
    staged_ephemeral_ = e;
    dirty_ = true;
}

void PresenceManager::atomic_write_(const std::filesystem::path& path,
                                    const std::string& content) {
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("PresenceManager: open tmp failed");
        f.write(content.data(), std::streamsize(content.size()));
        if (!f) throw std::runtime_error("PresenceManager: write failed");
    }
    std::filesystem::rename(tmp, path);
}

void PresenceManager::write_now_(std::chrono::steady_clock::time_point now) {
    CombinedState cs;
    cs.presence = staged_presence_;
    cs.ephemeral = staged_ephemeral_;
    atomic_write_(own_dir_() / "state.json", to_json(cs));
    last_write_ = now;
    dirty_ = false;
    ++write_count_;
}

void PresenceManager::tick(std::chrono::steady_clock::time_point now) {
    if (dirty_) {
        if (!last_write_ || (now - *last_write_) >= kFloor)
            write_now_(now);
        return;
    }
    if (last_write_ && (now - *last_write_) >= kCeiling)
        write_now_(now);
}

void PresenceManager::flush_state() {
    write_now_(std::chrono::steady_clock::now());
}

std::vector<std::pair<std::string, Presence>>
PresenceManager::read_remote_presences() const {
    std::vector<std::pair<std::string, Presence>> result;
    auto replicas_dir = shared_folder_ / "replicas";
    std::error_code ec;
    for (auto const& entry : std::filesystem::directory_iterator(replicas_dir, ec)) {
        if (!entry.is_directory()) continue;
        std::string rid = entry.path().filename().string();
        if (rid == replica_id_) continue;
        auto path = entry.path() / "state.json";
        if (!std::filesystem::exists(path)) continue;
        std::ifstream f(path, std::ios::binary);
        std::string json((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
        auto cs = combined_state_from_json(json);
        if (!cs) continue;
        result.emplace_back(std::move(rid), cs->presence);
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
        auto path = entry.path() / "state.json";
        if (!std::filesystem::exists(path)) continue;
        std::ifstream f(path, std::ios::binary);
        std::string json((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
        auto cs = combined_state_from_json(json);
        if (!cs) continue;
        result.emplace_back(std::move(rid), cs->ephemeral);
    }
    return result;
}

std::time_t PresenceManager::parse_iso8601_(const std::string& ts) {
    if (ts.empty()) return -1;
    std::tm tm{};
    const char* p = ts.c_str();
    char* end = strptime(p, "%Y-%m-%dT%H:%M:%S", &tm);
    if (!end) return -1;
    if (*end == '.') {
        ++end;
        while (*end >= '0' && *end <= '9') ++end;
    }
    if (*end != 'Z') return -1;
    return timegm(&tm);
}

bool PresenceManager::is_live(const Presence& p) {
    if (!p.active) return false;
    auto t = parse_iso8601_(p.last_heartbeat);
    if (t == -1) return false;
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    return std::difftime(now, t) < 30.0;
}
bool PresenceManager::is_stale(const Presence& p) {
    if (!p.active) return false;
    auto t = parse_iso8601_(p.last_heartbeat);
    if (t == -1) return true;
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    return std::difftime(now, t) >= 30.0;
}
bool PresenceManager::is_departed(const Presence& p) { return !p.active; }

void PresenceManager::depart() {
    staged_presence_.active = false;
    dirty_ = true;
    flush_state();
}

} // namespace CollabText::Identity
