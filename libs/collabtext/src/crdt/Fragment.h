#pragma once

#include "crdt/Clock.h"
#include "crdt/Locator.h"
#include "crdt/UndoMap.h"
#include <string>

namespace CollabText::Crdt {

/// A Fragment represents a contiguous run of characters inserted by the
/// same replica in a single burst. Each character in the fragment has:
///   - Lamport timestamp: (replica_id, origin_lamport + offset)
///   - Position: locator (same for all chars in the fragment since they
///     were inserted contiguously)
///
/// Fragments can be split when characters are deleted or when another
/// replica inserts between them.
struct Fragment {
    Lamport origin;          ///< Timestamp of the first character
    Locator locator;         ///< Fractional position in the document
    std::string content;     ///< UTF-8 content
    uint32_t length = 0;     ///< Number of characters (may differ from content.size() for multi-byte)
    uint32_t delete_count = 0;  ///< Number of active delete votes (deleted when > 0)

    Fragment() = default;
    Fragment(Lamport orig, Locator loc, std::string text, uint32_t len)
        : origin(orig), locator(loc), content(std::move(text)), length(len) {}

    /// Lamport timestamp of the character at `offset` within this fragment.
    Lamport timestamp_at(uint32_t offset) const {
        return Lamport(origin.replica_id, origin.value + offset);
    }

    /// True if this fragment has been deleted (by any replica).
    bool deleted() const { return delete_count > 0; }

    /// True if this fragment is visible in the document
    /// (not deleted and not undone).
    bool is_visible(const UndoMap &undo_map) const {
        if (delete_count > 0) return false;

        // Check if the first character is undone. For single-char
        // fragments (the common case after splitting) this is exact.
        return !undo_map.is_undone(UndoMapKey(origin));
    }

    /// True if this fragment was ever visible (inserted but not deleted).
    /// Ignores undo state.
    bool was_visible() const {
        return delete_count == 0;
    }
};

} // namespace CollabText::Crdt
