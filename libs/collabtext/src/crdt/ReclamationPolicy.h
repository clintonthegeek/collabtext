#pragma once

#include "crdt/Clock.h"

#include <cstdint>
#include <limits>
#include <map>
#include <vector>

namespace CollabText::Crdt {

/// The per-stream operation frontier selected by a GC certificate. This is
/// intentionally the actor-counter map for one stream; delivery sequence
/// numbers and another stream's counters are not interchangeable here.
using ReclamationFrontier = std::map<ActorId, uint64_t>;

/// Whether an identity is within the certified operation frontier for its
/// stream. Missing actors, invalid identities and zero counters fail closed.
[[nodiscard]] inline bool operation_is_covered(
    const ReclamationFrontier &frontier, const Identity &identity) {
  if (!identity.actor.valid() || identity.counter == 0)
    return false;
  const auto actor = frontier.find(identity.actor);
  return actor != frontier.end() && identity.counter <= actor->second;
}

/// Whether every identity in an item's reserved interval is covered. This is
/// used for text fragments, whose item identities are consecutive even when
/// the fragment's UTF-8 byte length differs from its character count.
[[nodiscard]] inline bool item_interval_is_covered(
    const ReclamationFrontier &frontier, const Identity &first,
    uint32_t item_count) {
  if (item_count == 0 || !first.actor.valid() || first.counter == 0 ||
      first.counter > std::numeric_limits<uint64_t>::max() - (item_count - 1))
    return false;
  const auto actor = frontier.find(first.actor);
  return actor != frontier.end() &&
         first.counter + item_count - 1 <= actor->second;
}

/// Shared necessary condition for deleting a tombstone: its complete item
/// interval and every deletion operation must be present at the certified
/// frontier. Undo, pins, translations and anchor stubs are independent
/// retention conditions and must also pass before a caller physically deletes.
[[nodiscard]] inline bool tombstone_is_covered(
    const ReclamationFrontier &frontier, const Identity &first,
    uint32_t item_count, const std::vector<Identity> &deletions) {
  if (deletions.empty() ||
      !item_interval_is_covered(frontier, first, item_count))
    return false;
  for (const auto &deletion : deletions) {
    if (!operation_is_covered(frontier, deletion))
      return false;
  }
  return true;
}

} // namespace CollabText::Crdt
