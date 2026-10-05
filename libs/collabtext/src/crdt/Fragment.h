#pragma once

#include "crdt/Clock.h"
#include "crdt/Locator.h"
#include "crdt/UndoMap.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace CollabText::Crdt {

// ============================================================================
// Fragment Summary — aggregate data cached in SumTree nodes
// ============================================================================

/// The aggregate SumTree nodes cache, and nothing else.
///
/// It holds only the four quantities a seek actually reads: two byte counts and
/// the maximum (locator, origin) pair. P03 removed three version-vector fields
/// that were written here and read by nobody, and their removal is not only
/// cheaper — one of them aggregated with Global::meet(), which is a
/// nonzero-minimum fold and *not* coverage intersection (finding F3). Carrying
/// it inside a tree summary made a "maybe everyone has seen it" quantity look
/// like a certainty, and no SumTree dimension consumed it.
///
/// The laws this aggregate must satisfy: identity is `zero()`, composition is
/// associative, and the byte counts and the maximum pair are commutative. See
/// tst_summary_laws.cpp, which checks them against randomized splice patterns.
struct FragmentSummary {
    uint32_t visible_bytes = 0;    ///< Total visible bytes in subtree
    uint32_t deleted_bytes = 0;    ///< Total deleted (invisible) bytes in subtree
    Locator max_locator;           ///< Maximum fragment Locator in subtree
    Identity max_origin = Identity::min(); ///< Origin of fragment with max (locator, origin)

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
///
/// `origin` is optional because there is no greatest identity to ask for
/// "everything at this locator": a 128-bit actor space has no maximum. A
/// disengaged `origin` therefore means *past every origin at this locator*,
/// which is what an insertion between same-locator fragments needs when it has
/// to find the next distinct locator above. Spelled as a magic all-ones
/// identity it would be an ordinary value a caller could not distinguish from a
/// real position, and a real actor could in principle exceed it.
struct FragmentOrderDim {
    Locator locator;
    std::optional<Identity> origin;

    static FragmentOrderDim zero() { return {Locator::min(), Identity::min()}; }
    /// Every origin at `locator`, in order.
    static FragmentOrderDim past_origin(Locator at) { return {at, std::nullopt}; }

    void add_summary(const FragmentSummary& s) {
        locator = s.max_locator;
        origin = s.max_origin;
    }

    auto operator<=>(const FragmentOrderDim& other) const {
        if (auto cmp = locator <=> other.locator; cmp != 0) return cmp;
        if (!origin.has_value() && !other.origin.has_value())
            return std::strong_ordering::equal;
        if (!origin.has_value()) return std::strong_ordering::greater;
        if (!other.origin.has_value()) return std::strong_ordering::less;
        return *origin <=> *other.origin;
    }
    bool operator==(const FragmentOrderDim& other) const {
        return (*this <=> other) == std::strong_ordering::equal;
    }
};

// ============================================================================
// Fragment
// ============================================================================

/// A Fragment represents a contiguous run of characters inserted by the
/// same actor in a single burst. Each character in the fragment has:
///   - Identity: (actor, origin.counter + offset)
///   - Position: locator (same for all chars in the fragment since they
///     were inserted contiguously)
///
/// Fragments can be split when characters are deleted or when another
/// replica inserts between them.
struct Fragment {
    using Summary = FragmentSummary;

    Identity origin;         ///< Identity of the first character
    Locator locator;         ///< Fractional position in the document
    uint32_t byte_length = 0; ///< Byte length of text content
    uint32_t length = 0;     ///< Number of characters (may differ from byte_length for multi-byte)
    std::vector<Identity> deletions;  ///< Identities of the deletion operations
    bool visible = true;     ///< Cached visibility (set during tree construction)
    std::string text;        ///< Inline text content owned by this fragment

    Fragment() = default;
    Fragment(Identity orig, Locator loc, uint32_t byte_len, uint32_t char_len)
        : origin(std::move(orig)), locator(loc), byte_length(byte_len), length(char_len) {}
    Fragment(Identity orig, Locator loc, uint32_t byte_len, uint32_t char_len,
             std::string txt)
        : origin(orig), locator(loc), byte_length(byte_len), length(char_len),
          text(std::move(txt)) {}

    /// Identity of the item at `offset` within this fragment.
    ///
    /// Derived, never allocated: the fragment's items are consecutive
    /// positions of the owning operation's reserved interval
    /// (docs/specs/vnext-identity.md §3.3), so a split partitions them.
    Identity item_at(uint32_t offset) const {
        return Identity(origin.actor, origin.counter + offset);
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

    /// Compute visibility at a specific version. Returns true if this
    /// fragment existed and was visible as of that version.
    bool was_visible_at(const Global &version, const UndoMap &undo_map) const {
        if (!version.observed(origin)) return false;
        if (undo_map.was_undone(origin, version)) return false;
        for (auto &del : deletions) {
            if (version.observed(del) && !undo_map.was_undone(del, version))
                return false;
        }
        return true;
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
        return s;
    }
};

} // namespace CollabText::Crdt
