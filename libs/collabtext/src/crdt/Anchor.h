#pragma once

#include "crdt/Clock.h"
#include "crdt/SumTree.h"
#include <cstdint>

namespace CollabText::Crdt {

// ============================================================================
// Anchor — a stable reference to a position in the document
// ============================================================================

/// An Anchor identifies a position by the Lamport timestamp of a specific
/// character. Unlike byte offsets, Anchors do not change when text is
/// inserted or deleted elsewhere in the document.
///
/// The `bias` field determines behavior when text is inserted exactly at
/// the anchor's position:
///   - Left: anchor stays before the new text
///   - Right: anchor moves after the new text
struct Anchor {
    uint16_t replica_id = 0;     ///< Replica that created the character
    uint32_t char_value = 0;     ///< Lamport sequence value of the character
    Bias bias = Bias::Left;      ///< Left or Right bias

    Anchor() = default;
    Anchor(uint16_t rid, uint32_t cv, Bias b = Bias::Left)
        : replica_id(rid), char_value(cv), bias(b) {}

    /// Sentinel: resolves to document start (offset 0).
    static Anchor min() { return {0, 0, Bias::Left}; }

    /// Sentinel: resolves to document end.
    static Anchor max() { return {UINT16_MAX, UINT32_MAX, Bias::Right}; }

    bool is_min() const { return replica_id == 0 && char_value == 0 && bias == Bias::Left; }
    bool is_max() const { return replica_id == UINT16_MAX && char_value == UINT32_MAX; }

    bool operator==(const Anchor&) const = default;
};

} // namespace CollabText::Crdt
