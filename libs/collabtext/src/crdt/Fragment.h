#pragma once

#include "crdt/Clock.h"
#include "crdt/Locator.h"
#include "crdt/UndoMap.h"
#include <cstdint>
#include <vector>

namespace CollabText::Crdt {

// ============================================================================
// Fragment Summary — aggregate data cached in SumTree nodes
// ============================================================================

struct FragmentSummary {
    uint32_t visible_bytes = 0;    ///< Total visible bytes in subtree
    uint32_t deleted_bytes = 0;    ///< Total deleted (invisible) bytes in subtree
    Locator max_locator;           ///< Maximum fragment Locator in subtree
    Lamport max_origin = Lamport::min(); ///< Origin of fragment with max (locator, origin)
    Global max_version;            ///< Latest timestamp in subtree
    Global min_insertion_version;  ///< Earliest insertion version in subtree
    Global max_insertion_version;  ///< Latest insertion version in subtree

    static FragmentSummary zero() { return {}; }

    void add_summary(const FragmentSummary& other) {
        visible_bytes += other.visible_bytes;
        deleted_bytes += other.deleted_bytes;
        // Track the max (locator, origin) pair for ordering
        if (other.max_locator > max_locator ||
            (other.max_locator == max_locator && other.max_origin > max_origin)) {
            max_locator = other.max_locator;
            max_origin = other.max_origin;
        }
        max_version.join(other.max_version);
        min_insertion_version.meet(other.min_insertion_version);
        max_insertion_version.join(other.max_insertion_version);
    }
};

// ============================================================================
// Dimensions — different ways to seek through a SumTree<Fragment>
// ============================================================================

/// Seek by visible byte offset (the user-facing document offset).
struct VisibleOffset {
    uint32_t value = 0;

    static VisibleOffset zero() { return {0}; }
    void add_summary(const FragmentSummary& s) { value += s.visible_bytes; }

    auto operator<=>(const VisibleOffset&) const = default;
    bool operator==(const VisibleOffset&) const = default;
};

/// Seek by full offset (visible + deleted bytes).
struct FullOffset {
    uint32_t value = 0;

    static FullOffset zero() { return {0}; }
    void add_summary(const FragmentSummary& s) {
        value += s.visible_bytes + s.deleted_bytes;
    }

    auto operator<=>(const FullOffset&) const = default;
    bool operator==(const FullOffset&) const = default;
};

/// Seek by Locator (fragment position identifier).
struct LocatorDim {
    Locator value;

    static LocatorDim zero() { return {Locator::min()}; }
    void add_summary(const FragmentSummary& s) {
        if (s.max_locator > value)
            value = s.max_locator;
    }

    auto operator<=>(const LocatorDim& other) const { return value <=> other.value; }
    bool operator==(const LocatorDim& other) const { return value == other.value; }
};

/// Seek by (locator, origin) for sorted insertion point finding.
struct FragmentOrderDim {
    Locator locator;
    Lamport origin = Lamport::min();

    static FragmentOrderDim zero() { return {Locator::min(), Lamport::min()}; }
    void add_summary(const FragmentSummary& s) {
        locator = s.max_locator;
        origin = s.max_origin;
    }

    auto operator<=>(const FragmentOrderDim& other) const {
        if (auto cmp = locator <=> other.locator; cmp != 0) return cmp;
        return origin <=> other.origin;
    }
    bool operator==(const FragmentOrderDim&) const = default;
};

// ============================================================================
// Fragment
// ============================================================================

/// A Fragment represents a contiguous run of characters inserted by the
/// same replica in a single burst. Each character in the fragment has:
///   - Lamport timestamp: (replica_id, origin_lamport + offset)
///   - Position: locator (same for all chars in the fragment since they
///     were inserted contiguously)
///
/// Fragments can be split when characters are deleted or when another
/// replica inserts between them.
struct Fragment {
    using Summary = FragmentSummary;

    Lamport origin;          ///< Timestamp of the first character
    Locator locator;         ///< Fractional position in the document
    uint32_t byte_length = 0; ///< Byte length of content in the rope
    uint32_t length = 0;     ///< Number of characters (may differ from byte_length for multi-byte)
    std::vector<Lamport> deletions;  ///< Lamport timestamps of deletion operations
    bool visible = true;     ///< Cached visibility (set during tree construction)

    Fragment() = default;
    Fragment(Lamport orig, Locator loc, uint32_t byte_len, uint32_t char_len)
        : origin(orig), locator(loc), byte_length(byte_len), length(char_len) {}

    /// Lamport timestamp of the character at `offset` within this fragment.
    Lamport timestamp_at(uint32_t offset) const {
        return Lamport(origin.replica_id, origin.value + offset);
    }

    /// True if this fragment has been deleted (by any replica).
    bool deleted() const { return !deletions.empty(); }

    /// Compute visibility from deletions and undo_map.
    /// Use this when building the tree to set the `visible` flag.
    bool compute_visible(const UndoMap &undo_map) const {
        if (undo_map.is_undone(origin)) return false;
        for (auto &del : deletions) {
            if (!undo_map.is_undone(del)) return false;
        }
        return true;
    }

    /// True if this fragment is visible in the document.
    /// For legacy compatibility, accepts an undo_map parameter.
    bool is_visible(const UndoMap &undo_map) const {
        return compute_visible(undo_map);
    }

    /// True if this fragment was ever visible (inserted but not deleted).
    /// Ignores undo state.
    bool was_visible() const {
        return deletions.empty();
    }

    /// Produce a FragmentSummary for this fragment.
    /// Uses the cached `visible` flag for byte accounting.
    FragmentSummary summary() const {
        FragmentSummary s;
        uint32_t bytes = byte_length;
        if (visible) {
            s.visible_bytes = bytes;
        } else {
            s.deleted_bytes = bytes;
        }
        s.max_locator = locator;
        s.max_origin = origin;
        if (length > 0) {
            s.max_version.observe(Lamport(origin.replica_id, origin.value + length - 1));
            Lamport ins_ts(origin.replica_id, origin.value);
            s.min_insertion_version.observe(ins_ts);
            s.max_insertion_version.observe(ins_ts);
        }
        return s;
    }
};

} // namespace CollabText::Crdt
