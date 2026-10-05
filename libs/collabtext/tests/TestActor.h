/// TestActor.h — deterministic actors and identities for tests.
///
/// Roadmap task P06a widened the engine's identity from a 16-bit replica id to a
/// persistent 128-bit ActorId, which turns every `Buffer(actor(1))` / `Identity(actor(1), 5)`
/// in the suite into an actor-keyed value. Tests need an actor, and they need it
/// to be *the same actor on every run* — a real `ActorId::generate()` per case
/// would make a failure depend on entropy and a test that minted one actor per
/// replica would need to thread it through every construction site.
///
/// These helpers are deliberately synthetic and obviously so. Production actors
/// come from 128 bits of OS randomness (P02) and are never derived; a test that
/// quietly started deriving real identities would be testing the wrong thing.
///
/// `actor(n)` is big-endian in the low bytes, so `actor(1) < actor(2) <
/// actor(255)` under the byte-wise comparison the engine uses, and
/// `actor(0)` is the invalid all-zero id — which is what makes it useful for
/// asserting that an invalid actor is rejected rather than treated as actor 0.

#pragma once

#include <collabtext/ActorId.h>

#include "crdt/Clock.h"

#include <array>
#include <cstdint>

namespace CollabText::Test {

/// The nth synthetic actor. `actor(0)` is invalid (all zeroes) by construction.
inline ActorId actor(uint8_t n) {
    std::array<uint8_t, ActorId::kBytes> bytes{};
    bytes[ActorId::kBytes - 1] = n;
    return ActorId::from_bytes(bytes);
}

/// A synthetic actor built from a full 64-bit value, for cases that need more
/// than 255 distinct actors or a specific ordering between them.
inline ActorId actor64(uint64_t n) {
    std::array<uint8_t, ActorId::kBytes> bytes{};
    for (size_t i = 0; i < 8; ++i)
        bytes[ActorId::kBytes - 1 - i] = static_cast<uint8_t>(n >> (8 * i));
    return ActorId::from_bytes(bytes);
}

/// An identity for convenience: `id(2, 10)` is actor 2 at counter 10.
inline Crdt::Identity id(uint8_t actor_index, uint64_t counter) {
    return Crdt::Identity{actor(actor_index), counter};
}

} // namespace CollabText::Test