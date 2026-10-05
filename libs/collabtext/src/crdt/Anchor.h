#pragma once

#include "crdt/Clock.h"
#include "crdt/SumTree.h"
#include <cstdint>

namespace CollabText::Crdt {

// ============================================================================
// Anchor — a stable reference to a position in the document
// ============================================================================

/// An Anchor identifies a position by the identity of a specific item.
///
/// Unlike byte offsets, Anchors do not change when text is inserted or deleted
/// elsewhere in the document.
///
/// The `bias` field determines behavior when text is inserted exactly at the
/// anchor's position:
///   - Left: anchor stays before the new text
///   - Right: anchor moves after the new text
///
/// `at_end` is an explicit past-the-end position, not a magic identity. The
/// legacy type spelled it as the largest representable (replica_id, value) pair,
/// which was only a maximum because the replica space was a bounded integer
/// range. With a 128-bit actor there is no largest actor, so a sentinel built
/// out of one would be an ordinary identity that happens to compare high —
/// which is precisely the kind of value a caller could not tell apart from a
/// real position. Start-of-document keeps its identity-based spelling, because
/// `Identity::min()` is a true minimum.
struct Anchor {
    Identity position;              ///< Identity of the item this names
    Bias bias = Bias::Left;        ///< Left or Right bias
    bool at_end = false;           ///< Resolves past the last item

    Anchor() = default;
    Anchor(Identity id, Bias b = Bias::Left, bool end = false)
        : position(std::move(id)), bias(b), at_end(end) {}

    /// Sentinel: resolves to document start (offset 0).
    static Anchor min() { return {Identity::min(), Bias::Left}; }

    /// Sentinel: resolves to document end.
    static Anchor end() { return {Identity::min(), Bias::Right, true}; }

    bool is_min() const { return !at_end && position == Identity::min(); }
    bool is_end() const { return at_end; }

    /// True when this anchor names a real item identity rather than a
    /// sentinel. An anchor carrying an invalid actor is a sentinel, never an
    /// ordinary position: there is no such thing as actor 0 (P02).
    [[nodiscard]] bool is_item() const { return !at_end && position.actor.valid(); }

    bool operator==(const Anchor&) const = default;
};

} // namespace CollabText::Crdt
