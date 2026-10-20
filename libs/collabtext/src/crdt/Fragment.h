#pragma once

#include "crdt/Clock.h"
#include "crdt/Diagnostics.h"
#include "crdt/Placement.h"
#include "crdt/UndoMap.h"
#include <cstdint>
#include <algorithm>
#include <algorithm>
#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace CollabText::Crdt {

// ============================================================================
// Fragment Summary — aggregate data cached in SumTree nodes
// ============================================================================

/// The aggregate SumTree nodes cache, and nothing else.
///
/// It holds only the quantities a seek actually reads: visible and deleted byte
/// counts plus visible UTF-16 units. P03 removed three version-vector fields that were written
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
    uint64_t visible_bytes = 0;    ///< Total visible bytes in subtree (overflow checked at API boundary)
    uint32_t deleted_bytes = 0;    ///< Total deleted (invisible) bytes in subtree
    uint64_t retained_runs = 0;    ///< Number of retained Fragment items in subtree
    // All public text coordinates are int/uint32 byte offsets, so the usable
    // visible UTF-16 range also fits this width.
    uint32_t visible_utf16 = 0;    ///< Visible UTF-16 code units

    static FragmentSummary zero() { return {}; }

    void add_summary(const FragmentSummary& other) {
        Diagnostics::record(Diagnostics::Counter::fragment_summary_compositions);
        visible_bytes += other.visible_bytes;
        deleted_bytes += other.deleted_bytes;
        retained_runs += other.retained_runs;
        visible_utf16 += other.visible_utf16;
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

/// Seek by visible UTF-16 offset while carrying the corresponding byte prefix.
struct VisibleUtf16Offset {
    uint64_t units = 0;
    uint64_t bytes = 0;
    static VisibleUtf16Offset zero() { return {}; }
    void add_summary(const FragmentSummary& s) {
        units += s.visible_utf16;
        bytes += s.visible_bytes;
    }
    auto operator<=>(const VisibleUtf16Offset& other) const { return units <=> other.units; }
    bool operator==(const VisibleUtf16Offset& other) const { return units == other.units; }
};

/// Seek by zero-based retained fragment ordinal, including hidden fragments.
struct RetainedRunOffset {
    uint64_t value = 0;

    static RetainedRunOffset zero() { return {}; }
    void add_summary(const FragmentSummary& s) { value += s.retained_runs; }

    auto operator<=>(const RetainedRunOffset&) const = default;
    bool operator==(const RetainedRunOffset&) const = default;
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

    bool marker = false;    ///< One zero-width ordered item; text remains empty.

    struct Utf16Checkpoint { uint32_t byte = 0; uint32_t units = 0; };
    struct Utf16Index {
        std::vector<Utf16Checkpoint> checkpoints;
    };
    std::shared_ptr<const Utf16Index> utf16_index;
    uint32_t utf16_units_cached = 0;
    bool utf16_index_ready = false;

    void rebuild_utf16_index() {
        Diagnostics::record(Diagnostics::Counter::utf16_index_builds);
        utf16_index.reset();
        utf16_index_ready = false;
        const auto non_ascii = std::find_if(text.begin(), text.end(), [](unsigned char byte) {
            return byte >= 0x80;
        });
        Diagnostics::record(Diagnostics::Counter::utf16_index_ascii_probe_bytes,
                            static_cast<uint64_t>(std::distance(text.begin(), non_ascii)) +
                                (non_ascii != text.end() ? 1 : 0));
        if (non_ascii == text.end()) {
            utf16_units_cached = static_cast<uint32_t>(text.size());
            utf16_index_ready = true;
            return;
        }

        uint32_t byte = 0, units = 0, since_checkpoint = 0;
        std::vector<Utf16Checkpoint> points;
        if (text.size() > 128) points.push_back({0, 0});
        while (byte < text.size()) {
            Diagnostics::record(Diagnostics::Counter::utf16_decoder_steps);
            const auto lead = static_cast<unsigned char>(text[byte]);
            const uint32_t byte_width = lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2 :
                                       (lead & 0xf0) == 0xe0 ? 3 : 4;
            const uint32_t unit_width = byte_width == 4 ? 2 : 1;
            byte += byte_width;
            units += unit_width;
            since_checkpoint += unit_width;
            if (!points.empty() && since_checkpoint >= 128 && byte < text.size()) {
                points.push_back({byte, units});
                since_checkpoint = 0;
            }
        }
        utf16_units_cached = units;
        Diagnostics::record(Diagnostics::Counter::utf16_checkpoint_entries_built, points.size());
        if (points.size() > 1) {
            auto index = std::make_shared<Utf16Index>();
            index->checkpoints = std::move(points);
            utf16_index = std::move(index);
        }
        utf16_index_ready = true;
    }

    uint64_t utf16_length() const {
        return utf16_units_cached;
    }

    /// Map a local UTF-16 boundary to UTF-8 bytes. `exact` is false only when
    /// the requested offset falls between the two units of a surrogate pair.
    uint32_t utf16_byte_offset(uint64_t target, bool& exact) const {
        exact = true;
        if (target == 0 || text.empty()) return 0;
        if (target > utf16_units_cached) { exact = false; return 0; }
        if (utf16_units_cached == byte_length) return static_cast<uint32_t>(target);
        uint32_t byte = 0, units = 0;
        if (utf16_index) {
            const auto& points = utf16_index->checkpoints;
            auto it = std::upper_bound(points.begin(), points.end(), target,
                [](uint64_t value, const Utf16Checkpoint& p) { return value < p.units; });
            if (it != points.begin()) --it;
            byte = it->byte;
            units = it->units;
        }
        while (byte < text.size() && units < target) {
            Diagnostics::record(Diagnostics::Counter::utf16_decoder_steps);
            const auto lead = static_cast<unsigned char>(text[byte]);
            const uint32_t width = lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2 :
                                   (lead & 0xf0) == 0xe0 ? 3 : 4;
            if (width == 4 && units + 1 == target) { exact = false; return byte; }
            byte += width;
            units += width == 4 ? 2 : 1;
        }
        if (units != target) exact = false;
        return byte;
    }

    Fragment() = default;
    Fragment(Identity orig, Link l, uint32_t byte_len, uint32_t char_len)
        : origin(std::move(orig)), link(std::move(l)), byte_length(byte_len), length(char_len) {}
    Fragment(Identity orig, Link l, uint32_t byte_len, uint32_t char_len,
             std::string txt)
        : origin(orig), link(std::move(l)), byte_length(byte_len), length(char_len),
          text(std::move(txt)) { rebuild_utf16_index(); }

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
        uint32_t prefix_units = 0;
        for (uint32_t c = 0; c < k; ++c) {
            const auto lead = static_cast<unsigned char>(text[byte]);
            const uint32_t width = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : 4;
            byte += width;
            prefix_units += width == 4 ? 2 : 1;
        }
        const auto old_index = utf16_index;
        const uint32_t old_units = utf16_units_cached;
        Fragment tail;
        tail.origin = item_at(k);
        tail.link = link_at(k);
        tail.text = text.substr(byte);
        Diagnostics::record(Diagnostics::Counter::fragment_splits);
        Diagnostics::record(Diagnostics::Counter::fragment_split_bytes_copied,
                            tail.text.size());
        tail.byte_length = static_cast<uint32_t>(tail.text.size());
        tail.length = length - k;
        tail.deletions = deletions;
        tail.visible = visible;
        text.resize(byte);
        byte_length = byte;
        length = k;
        if (old_index) {
            auto prefix_index = std::make_shared<Utf16Index>();
            auto tail_index = std::make_shared<Utf16Index>();
            prefix_index->checkpoints.push_back({0, 0});
            tail_index->checkpoints.push_back({0, 0});
            for (const auto& point : old_index->checkpoints) {
                if (point.byte && point.byte < byte)
                    prefix_index->checkpoints.push_back(point);
                else if (point.byte > byte)
                    tail_index->checkpoints.push_back({point.byte - byte, point.units - prefix_units});
            }
            utf16_units_cached = prefix_units;
            tail.utf16_units_cached = old_units - prefix_units;
            utf16_index = prefix_index->checkpoints.size() > 1 ? prefix_index : nullptr;
            tail.utf16_index = tail_index->checkpoints.size() > 1 ? tail_index : nullptr;
            utf16_index_ready = true;
            tail.utf16_index_ready = true;
        } else {
            // No source checkpoints means both sides are ASCII or short enough
            // for the bounded local scan; do not rescan either copied string.
            utf16_units_cached = prefix_units;
            tail.utf16_units_cached = old_units - prefix_units;
            utf16_index_ready = true;
            tail.utf16_index_ready = true;
        }
        return tail;
    }

    /// True if this fragment has been deleted (by any replica).
    bool deleted() const { return !deletions.empty(); }

    /// Compute visibility from deletions and undo_map.
    /// Use this when building the tree to set the `visible` flag.
    bool compute_visible(const UndoMap &undo_map) const {
        Diagnostics::record(Diagnostics::Counter::visibility_recomputations);
        if (undo_map.is_undone(origin)) return false;
        for (auto &del : deletions) {
            Diagnostics::record(Diagnostics::Counter::visibility_deletion_checks);
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
        s.retained_runs = 1;
        uint32_t bytes = byte_length;
        if (visible) {
            s.visible_bytes = bytes;
            s.visible_utf16 = utf16_length();
        } else {
            s.deleted_bytes = bytes;
        }
        return s;
    }
};


} // namespace CollabText::Crdt
