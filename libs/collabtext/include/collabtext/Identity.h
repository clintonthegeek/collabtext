#pragma once

#include "crdt/Anchor.h"
#include "crdt/Clock.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace CollabText::Identity {

// ============================================================================
// Identity — who you are (persistent, published)
// ============================================================================

struct Identity {
    std::string identity_id;
    std::string display_name;
    std::string status;
    std::string bio;
    std::string color;
    std::string public_key;
    std::string updated;
};

// ============================================================================
// Presence — which device/replica you're on right now
// ============================================================================

struct Presence {
    std::string replica_id;
    std::string identity_id;
    std::string device_name;
    bool active = false;
    std::string last_heartbeat;
    std::string session_started;
    Crdt::Global version_summary;
};

// ============================================================================
// EphemeralState — cursor, selection, viewport (high-frequency, not persisted)
// ============================================================================

struct CursorPair {
    Crdt::Anchor anchor;
    Crdt::Anchor head;
};

struct EphemeralState {
    uint64_t seq = 0;
    std::string timestamp;
    std::vector<CursorPair> cursors;
    std::vector<CursorPair> selections;
    std::optional<Crdt::Anchor> viewport_top;
    std::optional<Crdt::Anchor> viewport_bottom;
    std::string activity;
};

// ============================================================================
// Serialization
// ============================================================================

std::string to_json(const Identity &id);
std::optional<Identity> identity_from_json(const std::string &json);

std::string to_json(const Presence &p);
std::optional<Presence> presence_from_json(const std::string &json);

std::string to_json(const EphemeralState &es);
std::optional<EphemeralState> ephemeral_from_json(const std::string &json);

// ============================================================================
// CombinedState — what `replicas/<id>/state.json` actually holds
// ============================================================================

struct CombinedState {
    Presence presence;
    EphemeralState ephemeral;
};

std::string to_json(const CombinedState &s);
std::optional<CombinedState> combined_state_from_json(const std::string &json);

} // namespace CollabText::Identity
