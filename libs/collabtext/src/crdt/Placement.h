#pragma once
// How an element hangs in the FugueMax tree (roadmap task S03).
//
// A document is a list of elements. Each element records, at the moment it is
// minted, where it attaches: a `parent`, a `side` of that parent, and its `right
// origin`. The list is the in-order traversal of the resulting tree, and the
// tree depends only on the set of operations, never on arrival order. That is
// the whole convergence argument, and it is why a position here is a *relation to
// other elements* rather than a coordinate: a flat fractional position cannot say
// "immediately after the thing I was typed after", which is exactly what keeps one
// author's run intact when a peer concurrently edits inside it.
//
// The algorithm is Fugue/FugueMax from Weidner and Kleppmann, "The Art of the
// Fugue" (arXiv 2305.00583); the reference transcription it is tested against is
// tests/reference/FugueMaxModel.h. Docs: docs/specs/vnext-sequence-algorithm.md.
//
// Only the FIRST element of a run is stored. Every later element of a run is a
// right child of the one before it, carrying the same right origin, so a split
// of a run derives the tail's link and nothing else is recorded.

#include <collabtext/Engine.h>

#include <cstdint>
#include <optional>

namespace CollabText::Crdt {

enum class Side : uint8_t { Left = 0, Right = 1 };

struct Link {
    /// The element this one is a child of. Empty means the virtual root, i.e. the
    /// start of the document.
    std::optional<Identity> parent;
    Side side = Side::Right;
    /// The element immediately after this one's insertion point when it was
    /// minted, tombstones included. Empty means the end of the document. For a
    /// left child it equals `parent`.
    std::optional<Identity> right_origin;

    bool operator==(const Link&) const = default;
};

}  // namespace CollabText::Crdt
