#pragma once

#include "collabtext/Identity.h"

#include <optional>
#include <string>

namespace CollabText::Identity {

struct SigningKeyPair {
    std::string public_key;   // "ed25519:base64..."
    std::string private_key;  // raw bytes, never synced
};

/// Generate an Ed25519 keypair. Currently returns nullopt (stub).
std::optional<SigningKeyPair> generate_keypair();

/// Sign profile fields. Currently returns empty string (stub).
std::string sign_profile(const Identity& identity,
                         const std::string& private_key);

/// Verify profile signature. Currently returns true (stub — trust everything).
bool verify_profile(const Identity& identity,
                    const std::string& signature);

} // namespace CollabText::Identity
