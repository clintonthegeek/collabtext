#pragma once

#include "crdt/Operations.h"

#include <string>

namespace CollabText::Crdt {

/// Build a deterministic CRDT operation that inserts the given seed
/// content into an empty buffer. The op uses a synthetic seed replica
/// (replica_id = 0) so two peers computing it from the same input
/// produce byte-identical operations — applying them on either side
/// converges to the same buffer state.
///
/// Usage on join:
///   Buffer buf(my_replica_id);
///   buf.apply_ops({op_for_seed(seed)});
///   // ... continue with normal sync ...
///
/// The seed op is never broadcast via FileSync; each replica
/// reconstructs it locally from `seed.txt`.
Operation op_for_seed(const std::string& content);

} // namespace CollabText::Crdt
