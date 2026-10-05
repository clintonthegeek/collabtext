#pragma once

#include "core/Sha256.h"

#include <collabtext/ActorId.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace CollabText::Crdt {

/// A deterministic actor derived from a domain string and some bytes.
///
/// Two uses, both of which need an identity that is *reproducible* rather than
/// minted: a seed operation every peer reconstructs from the same file, and a
/// simulated replica that must be the same actor on every run of a scenario.
///
/// Deriving from the content is what keeps that safe. A single hard-coded actor
/// would give every seed and every simulated replica the same identity, and two
/// distinct operations claiming one identity is exactly the aliasing
/// docs/specs/vnext-identity.md §3.2 exists to prevent. Domain separation keeps
/// a seed actor from ever colliding with a simulator's actor for the same bytes.
///
/// This is not a device identity. Real actors are 128 bits of OS randomness
/// (P02) and are never derived from anything.
inline ActorId derived_actor(std::string_view domain,
                            std::string_view material) {
    // A NUL between the two so ("ab", "c") and ("a", "bc") cannot collide.
    std::string input;
    input.reserve(domain.size() + material.size() + 1);
    input.append(domain);
    input.push_back('\0');
    input.append(material);

    const std::string digest = sha256_hex(input);
    std::array<uint8_t, ActorId::kBytes> bytes{};
    const auto nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
        return 0;
    };
    for (size_t i = 0; i < ActorId::kBytes; ++i) {
        // The digest is lowercase hex, so two characters per byte.
        bytes[i] = static_cast<uint8_t>(
            static_cast<uint8_t>(nibble(digest[2 * i]) << 4) |
            nibble(digest[2 * i + 1]));
    }
    return ActorId::from_bytes(bytes);
}

} // namespace CollabText::Crdt