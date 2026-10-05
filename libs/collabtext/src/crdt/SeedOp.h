#pragma once

#include "crdt/Operations.h"

#include <string>

namespace CollabText::Crdt {

/// Build a deterministic CRDT operation that inserts the given seed content
/// into an empty buffer.
///
/// The seed operation must be reproducible, which means its identity cannot be
/// a device identity: two peers that read the same `seed.txt` have to compute
/// byte-identical operations or they will not converge, and neither of them owns
/// the identity. So the actor is derived from the content itself — see
/// `seed_actor()` — rather than minted.
///
/// That derivation is not a convenience. The previous scheme used replica 0,
/// which was also the *default* replica of every engine that did not configure
/// one, so a seed operation and an ordinary local edit could claim the same
/// identity; and two peers seeding different content into one document both
/// claimed replica 0's counter range, which is precisely the aliasing
/// docs/specs/vnext-identity.md §3.2 exists to prevent. Deriving the actor from
/// the content keeps determinism and removes both.
///
/// Usage on join:
///   Buffer buf(my_actor);
///   buf.apply_ops({op_for_seed(seed)});
///   // ... continue with normal sync ...
///
/// The seed op is never broadcast via FileSync; each replica reconstructs it
/// locally from `seed.txt`.
Operation op_for_seed(const std::string& content);

/// The reserved actor a seed operation is minted under.
///
/// Domain-separated from any device identity: the bytes are
/// SHA-256("collabtext/seed-actor/v1\0" || content), so the same content always
/// yields the same actor on every machine, and different content never shares
/// one. It is not a device identity and must never be used for anything but a
/// seed operation.
ActorId seed_actor(const std::string& content);

} // namespace CollabText::Crdt
