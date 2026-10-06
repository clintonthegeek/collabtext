#pragma once

#include "crdt/Clock.h"
#include "crdt/Placement.h"
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
/// It holds only the two quantities a seek actually reads: the visible and
/// deleted byte counts. P03 removed three version-vector fields that were written
/// here and read by nobody, and their removal is not only cheaper -- one of them
/// aggregated with Global::meet(), which is a nonzero-minimum fold and *not*
/// coverage intersection (finding F3). S03 removed the (locator, origin) maximum
/// too: with placement by tree relation the fragment tree is a plain sequence in
/// document order, and nothing seeks by a position key any more.
///
/// The laws this aggregate must satisfy: identity is `zero()`, composition is
/// associative, and the byte counts are commutative. See tst_summary_laws.cpp,
/// which checks them against randomized splice patterns.
struct FragmentSummary {
    uint32_t visible_bytes = 0;    ///< Total visible bytes in subtree
    uint32_t deleted_bytes = 0;    ///< Total deleted (invisible) bytes in subtree

    static FragmentSummary zero() { return {}; }

    void add_summary(const FragmentSummary& other) {
        visible_bytes += other.visible_bytes;
        deleted_bytes += other.deleted_bytes;
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

// ============================================================================
// Fragment
// ============================================================================

/// A Fragment represents a contiguous run of characters inserted by the
/// same actor in a single burst. Each character in the fragment has:
///   - Identity: (actor, origin.counter + offset)
///   - Placement: the first character carries `link`; every later character is
///     a right child of the one before it with the same right origin, so a run
///     is a chain in the tree and `link_at()` derives any item's link.
///
/// Fragments can be split when characters are deleted or when another
/// replica inserts between them. The fragment tree is a sequence in document
/// order; its order IS the document order, so there is no position key.
struct Fragment {
    using Summary = FragmentSummary;

    Identity origin;         ///< Identity of the first character
    Link link;               ///< Placement of the FIRST item (see Placement.h)
    uint32_t byte_length = 0; ///< Byte length of text content
    uint32_t length = 0;     ///< Number of characters (may differ from byte_length for multi-byte)
    std::vector<Identity> deletions;  ///< Identities of the deletion operations
    bool visible = true;     ///< Cached visibility (set during tree construction)
    std::string text;        ///< Inline text content owned by this fragment

    Fragment() = default;
    Fragment(Identity orig, Link l, uint32_t byte_len, uint32_t char_len)
        : origin(std::move(orig)), link(std::move(l)), byte_length(byte_len), length(char_len) {}
    Fragment(Identity orig, Link l, uint32_t byte_len, uint32_t char_len,
             std::string txt)
        : origin(orig), link(std::move(l)), byte_length(byte_len), length(char_len),
          text(std::move(txt)) {}

    /// Identity of the item at `offset` within this fragment.
    ///
    /// Derived, never allocated: the fragment's items are consecutive
    /// positions of the owning operation's reserved interval
    /// (docs/specs/vnext-identity.md §3.3), so a split partitions them.
    Identity item_at(uint32_t offset) const {
        return Identity(origin.actor, origin.counter + offset);
    }

    /// Placement of the item at `offset`: the first item carries the stored link,
    /// every later one is a right child of its predecessor with the same right
    /// origin. This is what makes a split free of new data: the tail's first
    /// item is `link_at(k)`.
    Link link_at(uint32_t offset) const {
        if (offset == 0) return link;
        return Link{item_at(offset - 1), Side::Right, link.right_origin};
    }

    /// Cut this fragment before item `k` (0 < k < length) and return the tail.
    ///
    /// Splitting is a presentation artifact, not an edit: the tail's first item is
    /// `link_at(k)`, which is what it already was, so no tree relation changes.
    Fragment split_off(uint32_t k) {
        uint32_t byte = 0;
        for (uint32_t c = 0; c < k; ++c) {
            const auto lead = static_cast<unsigned char>(text[byte]);
            byte += lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : 4;
        }
        Fragment tail;
        tail.origin = item_at(k);
        tail.link = link_at(k);
        tail.text = text.substr(byte);
        tail.byte_length = static_cast<uint32_t>(tail.text.size());
        tail.length = length - k;
        tail.deletions = deletions;
        tail.visible = visible;
        text.resize(byte);
        byte_length = byte;
        length = k;
        return tail;
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
        return s;
    }
};

} // namespace CollabText::Crdt
