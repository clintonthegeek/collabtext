#include "crdt/Buffer.h"
#include "crdt/OperationQueue.h"
#include "crdt/SequenceOrder.h"
#include "codec/Utf.h"
#include <algorithm>
#include <cassert>
#include <numeric>

namespace CollabText::Crdt {

Buffer::Buffer(SharedIdentityAllocator allocator)
    : m_allocator(std::move(allocator))
{
    // A null allocator is a programming error: nothing can be minted and there
    // is no state to recover to, so it fails loudly here.
    assert(m_allocator && "an engine without an allocator cannot mint identity");
    // An *invalid actor* is not asserted on, because it is a state rather than a
    // bug: an engine may exist before its private state has an identity. Minting
    // is refused at the reservation site instead (check_allocator), where the
    // refusal can be reported to a caller instead of vanishing in Release.
}

Result<std::vector<CollabText::detail::PositionItem>> Buffer::position_items() const {
    std::vector<CollabText::detail::PositionItem> items;
    for (const auto& fragment : get_fragments()) {
        size_t scalars = 0;
        if (!Codec::utf8_length(fragment.text, scalars).ok())
            return Error(ErrorCode::invalid_unicode, "invalid retained fragment text");
        if (scalars != fragment.length || fragment.text.size() != fragment.byte_length)
            return Error(ErrorCode::history_unavailable, "retained fragment length mismatch");
        if (fragment.length && (!fragment.origin.actor.valid() || !fragment.origin.counter ||
            fragment.length > UINT64_MAX - fragment.origin.counter))
            return Error(ErrorCode::history_unavailable, "retained item interval is invalid");
        size_t byte = 0;
        for (uint32_t scalar = 0; scalar < fragment.length; ++scalar) {
            const auto lead = static_cast<unsigned char>(fragment.text[byte]);
            const uint32_t width = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
            items.push_back({fragment.item_at(scalar), width, fragment.visible, std::nullopt});
            byte += width;
        }
    }
    return items;
}

void Buffer::local_clear() noexcept {
    auto allocator = m_allocator;
    auto version = m_version;
    const auto undo_limit = max_undo_depth();
    auto changed = std::move(m_on_change);
    *this = Buffer(std::move(allocator));
    m_version = std::move(version);
    m_on_change = std::move(changed);
    set_max_undo_depth(undo_limit);
}

// ---------------------------------------------------------------------------
// Fragment tree helpers
// ---------------------------------------------------------------------------

std::vector<Fragment> Buffer::get_fragments() const {
    return m_fragment_tree.items();
}

void Buffer::set_fragments(std::vector<Fragment>&& frags) {
    for (auto& f : frags) {
        f.visible = f.compute_visible(m_undo_map);
        assert(f.byte_length == f.text.size());
    }
    FragmentTree tree;
    for (auto& f : frags) tree.push_item(std::move(f));
    m_fragment_tree = std::move(tree);
}

std::vector<Fragment> Buffer::fragments() const {
    return get_fragments();
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

std::string Buffer::text() const {
    std::string result;
    result.reserve(m_fragment_tree.summary().visible_bytes);
    m_fragment_tree.for_each([&](const Fragment& f) {
        if (f.visible) result += f.text;
    });
    return result;
}

uint32_t Buffer::visible_length() const {
    return m_fragment_tree.summary().visible_bytes;
}

uint32_t Buffer::visible_rope_len() const {
    return m_fragment_tree.summary().visible_bytes;
}

uint32_t Buffer::deleted_rope_len() const {
    return m_fragment_tree.summary().deleted_bytes;
}

size_t Buffer::tombstone_count() const {
    size_t count = 0;
    m_fragment_tree.for_each([&](const Fragment& f) {
        if (!f.visible) ++count;
    });
    return count;
}

size_t Buffer::fragment_count() const {
    size_t count = 0;
    m_fragment_tree.for_each([&](const Fragment&) { ++count; });
    return count;
}

size_t Buffer::max_undo_depth() const {
    return m_max_undo_depth;
}

void Buffer::set_max_undo_depth(size_t depth) {
    m_max_undo_depth = depth;
    trim_undo_stack();
}

void Buffer::trim_undo_stack() {
    if (m_undo_stack.size() <= m_max_undo_depth) return;
    size_t excess = m_undo_stack.size() - m_max_undo_depth;
    m_undo_stack.erase(m_undo_stack.begin(),
                       m_undo_stack.begin() + static_cast<ptrdiff_t>(excess));
    if (excess > m_undo_cursor)
        m_undo_cursor = 0;
    else
        m_undo_cursor -= excess;
}

size_t Buffer::collect_garbage() {
    // Local history cannot prove that remote redo or anchors have expired.
    // Retain all restoration state until certified reclamation is available.
    return 0;
}

size_t Buffer::compact(const Global& /*watermark*/) {
    // Observing a deletion does not release a replica's undo/redo obligations.
    // A raw clock is not a reclamation certificate; retain identities and data.
    return 0;
}

// ---------------------------------------------------------------------------
// edits_since — compute visible text changes between two versions
// ---------------------------------------------------------------------------

std::vector<TextEdit> Buffer::edits_since(const Global &since) const {
    std::vector<TextEdit> edits;

    // Walk every fragment. Track two running byte offsets:
    // old_pos: position in the visible text as of `since`
    // new_pos: position in the current visible text
    uint32_t old_pos = 0;
    uint32_t new_pos = 0;

    // Accumulator for merging adjacent edits into single TextEdits
    // (e.g., a deleted fragment immediately followed by an inserted one
    // is one replacement, not two separate edits)
    bool in_edit = false;
    uint32_t edit_old_start = 0;
    uint32_t edit_old_end = 0;
    uint32_t edit_new_start = 0;

    auto flush_edit = [&]() {
        if (!in_edit) return;
        uint32_t edit_new_end = new_pos;
        if (edit_old_start != edit_old_end || edit_new_start != edit_new_end) {
            TextEdit e;
            e.old_start = edit_old_start;
            e.old_end = edit_old_end;
            // Extract the new text from the current visible text
            // by walking the tree again... OR we can accumulate it.
            // For efficiency, we'll collect new text during the walk.
        }
        in_edit = false;
    };

    // Better approach: two-pass is complex. Single pass that accumulates
    // new_text as we go.
    edits.clear();
    old_pos = 0;
    new_pos = 0;
    in_edit = false;
    std::string edit_new_text;

    auto flush = [&]() {
        if (!in_edit) return;
        if (edit_old_start != edit_old_end || !edit_new_text.empty()) {
            edits.push_back({edit_old_start, edit_old_end, std::move(edit_new_text)});
            edit_new_text.clear();
        }
        in_edit = false;
    };

    m_fragment_tree.for_each([&](const Fragment& f) {
        bool was_vis = f.was_visible_at(since, m_undo_map);
        bool is_vis = f.visible;

        if (was_vis && is_vis) {
            // Unchanged visible fragment — flush any pending edit, advance both
            flush();
            old_pos += f.byte_length;
            new_pos += f.byte_length;
        } else if (was_vis && !is_vis) {
            // Was visible, now invisible — DELETION
            if (!in_edit) {
                in_edit = true;
                edit_old_start = old_pos;
                edit_old_end = old_pos;
                edit_new_text.clear();
            }
            edit_old_end += f.byte_length;
            old_pos += f.byte_length;
        } else if (!was_vis && is_vis) {
            // Was invisible (or didn't exist), now visible — INSERTION
            if (!in_edit) {
                in_edit = true;
                edit_old_start = old_pos;
                edit_old_end = old_pos;
                edit_new_text.clear();
            }
            edit_new_text += f.text;
            new_pos += f.byte_length;
        } else {
            // Invisible in both versions — skip, but don't flush
            // (could be a tombstone between two changed fragments
            // that form one logical edit)
        }
    });

    flush();
    return edits;
}

// ---------------------------------------------------------------------------
// Anchors
// ---------------------------------------------------------------------------

/// Count UTF-8 characters in the first `byte_count` bytes (local helper).
static uint32_t count_chars(const std::string &s, uint32_t byte_count) {
    uint32_t chars = 0;
    for (uint32_t b = 0; b < byte_count; ) {
        unsigned char c = static_cast<unsigned char>(s[b]);
        if (c < 0x80) b += 1;
        else if ((c & 0xE0) == 0xC0) b += 2;
        else if ((c & 0xF0) == 0xE0) b += 3;
        else b += 4;
        ++chars;
    }
    return chars;
}

/// Get the byte offset of the `char_offset`-th character (local helper).
static uint32_t chars_to_bytes(const std::string &s, uint32_t char_offset) {
    uint32_t b = 0;
    for (uint32_t c = 0; c < char_offset; ++c) {
        unsigned char ch = static_cast<unsigned char>(s[b]);
        if (ch < 0x80) b += 1;
        else if ((ch & 0xE0) == 0xC0) b += 2;
        else if ((ch & 0xF0) == 0xE0) b += 3;
        else b += 4;
    }
    return b;
}

Anchor Buffer::anchor_at(uint32_t byte_offset, Bias bias) const {
    if (m_fragment_tree.empty()) return Anchor::min();
    uint32_t accumulated = 0;
    Anchor result = Anchor::end();
    bool found = false;
    m_fragment_tree.for_each([&](const Fragment& f) {
        if (found) return;
        if (!f.visible) return;
        if (accumulated + f.byte_length > byte_offset) {
            uint32_t offset_in_frag = byte_offset - accumulated;
            uint32_t char_offset = count_chars(f.text, offset_in_frag);
            result = Anchor(f.item_at(char_offset), bias);
            found = true;
            return;
        }
        accumulated += f.byte_length;
    });
    return result;
}

uint32_t Buffer::resolve_anchor(const Anchor& anchor) const {
    if (anchor.is_min()) return 0;
    if (anchor.is_end()) return visible_length();
    uint32_t accumulated = 0;
    uint32_t result = visible_length();
    bool found = false;
    m_fragment_tree.for_each([&](const Fragment& f) {
        if (found) return;
        if (f.origin.actor == anchor.position.actor &&
            anchor.position.counter >= f.origin.counter &&
            anchor.position.counter < f.origin.counter + f.length) {
            if (f.visible) {
                uint32_t char_offset =
                    static_cast<uint32_t>(anchor.position.counter - f.origin.counter);
                uint32_t byte_off = chars_to_bytes(f.text, char_offset);
                result = accumulated + byte_off;
            } else {
                result = accumulated;
            }
            found = true;
            return;
        }
        if (f.visible) {
            accumulated += f.byte_length;
        }
    });
    return result;
}

int Buffer::compare_anchors(const Anchor& a, const Anchor& b) const {
    uint32_t pos_a = resolve_anchor(a);
    uint32_t pos_b = resolve_anchor(b);
    if (pos_a < pos_b) return -1;
    if (pos_a > pos_b) return 1;
    if (a.bias != b.bias)
        return (a.bias == Bias::Left) ? -1 : 1;
    return 0;
}

const Global &Buffer::version() const {
    return m_version;
}

const ActorId &Buffer::actor_id() const {
    return m_allocator->actor();
}

// ---------------------------------------------------------------------------
// Fragment management helpers (operate on vectors)
// ---------------------------------------------------------------------------

/// Count UTF-8 characters in the first `byte_count` bytes of `s`.
static uint32_t count_utf8_chars(const std::string &s, uint32_t byte_count) {
    uint32_t chars = 0;
    for (uint32_t b = 0; b < byte_count; ) {
        unsigned char c = static_cast<unsigned char>(s[b]);
        if (c < 0x80) b += 1;
        else if ((c & 0xE0) == 0xC0) b += 2;
        else if ((c & 0xF0) == 0xE0) b += 3;
        else b += 4;
        ++chars;
    }
    return chars;
}

/// Get the byte length of the first UTF-8 character in `s`.
static uint32_t first_char_bytes(const std::string &s) {
    unsigned char c = static_cast<unsigned char>(s[0]);
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    return 4;
}

/// Get the byte offset of the `char_offset`-th character in `s`.
static uint32_t char_to_byte_offset(const std::string &s, uint32_t char_offset) {
    uint32_t b = 0;
    for (uint32_t c = 0; c < char_offset; ++c) {
        unsigned char ch = static_cast<unsigned char>(s[b]);
        if (ch < 0x80) b += 1;
        else if ((ch & 0xE0) == 0xC0) b += 2;
        else if ((ch & 0xF0) == 0xE0) b += 3;
        else b += 4;
    }
    return b;
}

std::pair<size_t, uint32_t> Buffer::resolve_visible_offset(
    const std::vector<Fragment>& frags, uint32_t byte_offset) const
{
    uint32_t accumulated = 0;
    for (size_t i = 0; i < frags.size(); ++i) {
        if (!frags[i].is_visible(m_undo_map))
            continue;
        uint32_t frag_bytes = frags[i].byte_length;
        if (accumulated + frag_bytes > byte_offset) {
            return {i, byte_offset - accumulated};
        }
        accumulated += frag_bytes;
    }
    return {frags.size(), 0};
}

size_t Buffer::split_fragment_at(std::vector<Fragment>& frags,
                                  size_t frag_idx, uint32_t offset_in_frag) const
{
    assert(frag_idx < frags.size());
    assert(offset_in_frag > 0);
    assert(offset_in_frag < frags[frag_idx].byte_length);

    // A split does not change any tree relation: the tail's first item is the
    // fragment's own `link_at(k)`. It stays adjacent, so the tail goes right after.
    const uint32_t chars = count_utf8_chars(frags[frag_idx].text, offset_in_frag);
    Fragment tail = frags[frag_idx].split_off(chars);
    frags.insert(frags.begin() + static_cast<ptrdiff_t>(frag_idx) + 1, std::move(tail));
    return frag_idx + 1;
}

/// The element a new insertion at visible byte offset `start` goes immediately
/// after: the visible scalar that ends exactly at `start`, or nothing at the
/// document start. `start` is a validated scalar boundary within the text.
static std::optional<Identity> visible_element_before(const std::vector<Fragment>& frags,
                                                      uint32_t start) {
    if (start == 0) return std::nullopt;
    uint32_t seen = 0;
    for (const auto& f : frags) {
        if (!f.visible) continue;
        if (seen + f.byte_length >= start) {
            const uint32_t scalars = count_utf8_chars(f.text, start - seen);
            return f.item_at(scalars - 1);
        }
        seen += f.byte_length;
    }
    return std::nullopt;
}

/// Split the visible fragment that straddles visible byte offset `off`, if any, so
/// that `off` falls on a fragment boundary.
static void split_at_visible_offset(std::vector<Fragment>& frags, uint32_t off) {
    uint32_t seen = 0;
    for (size_t i = 0; i < frags.size(); ++i) {
        if (!frags[i].visible) continue;
        const uint32_t length = frags[i].byte_length;
        if (off > seen && off < seen + length) {
            Fragment tail = frags[i].split_off(count_utf8_chars(frags[i].text, off - seen));
            frags.insert(frags.begin() + static_cast<ptrdiff_t>(i) + 1, std::move(tail));
            return;
        }
        seen += length;
    }
}

// ---------------------------------------------------------------------------
// Local edit
// ---------------------------------------------------------------------------

Operation Buffer::apply_local_edit(
    const std::vector<std::pair<uint32_t, uint32_t>> &ranges,
    const std::vector<std::string> &new_text)
{
    // Kept for callers that cannot handle a failure. A refused edit returns an
    // empty operation and mutates nothing; it does not assert, because an
    // assertion is not a behaviour a Release build is allowed to have.
    auto result = try_apply_local_edit(ranges, new_text);
    if (!result.ok()) return Operation{};
    return std::move(result).value();
}

Result<Operation> Buffer::try_apply_local_edit(
    const std::vector<std::pair<uint32_t, uint32_t>> &ranges,
    const std::vector<std::string> &new_text)
{
    // Validate against the *current* visible length before any mutation: an
    // offset outside the document, an inverted range, or a pair of vectors that
    // disagree in length would otherwise be consumed by the edit walk below,
    // which assumes all three hold.
    if (auto valid = validate_local_edit(ranges, new_text, visible_length());
        !valid.ok()) {
        return valid.error();
    }
    // Byte offsets must be Unicode scalar boundaries. Seek the containing run
    // rather than copying the entire document for every edit.
    const auto boundary = [&](uint32_t offset) {
        if (offset == 0 || offset == visible_length()) return true;
        auto cursor = m_fragment_tree.cursor<VisibleOffset>();
        // SumTree's Left seek selects the first item whose end is strictly
        // beyond offset, skipping prior runs and zero-width tombstones.
        if (!cursor.seek({offset}, Bias::Left) || !cursor.item()) return false;
        const auto& fragment = *cursor.item();
        const auto start = cursor.position().value;
        if (offset < start || offset - start >= fragment.text.size()) return false;
        return (static_cast<unsigned char>(fragment.text[offset - start]) & 0xc0) != 0x80;
    };
    for (const auto& [start, end] : ranges) {
        if (!boundary(start) || !boundary(end))
            return Error(ErrorCode::invalid_unicode,
                         "local edit range splits a UTF-8 scalar");
    }
    return try_apply_validated_local_edit(ranges, new_text);
}

Result<Operation> Buffer::try_apply_validated_local_edit(
    const std::vector<std::pair<uint32_t, uint32_t>> &ranges,
    const std::vector<std::string> &new_text)
{
    assert(ranges.size() == new_text.size());

    if (auto usable = check_allocator(); !usable.ok()) return usable.error();

    // One reservation for the whole operation, before anything is mutated
    // (identity spec §3.2): `n` item identities followed by the operation's own.
    //
    // The item count has to be known before the walk, not discovered during it,
    // because the operation's identity is the *last* value of the reservation
    // and the deletion identity is that same value (§3.3) — the fragments being
    // tombstoned need it before the first one is visited.
    uint64_t inserted_scalars = 0;
    std::vector<uint32_t> scalar_counts(ranges.size());
    for (size_t i = 0; i < ranges.size(); ++i) {
        scalar_counts[i] = count_utf8_chars(
            new_text[i], static_cast<uint32_t>(new_text[i].size()));
        inserted_scalars += scalar_counts[i];
    }

    auto reserved = m_allocator->reserve_run(inserted_scalars + 1);
    if (!reserved.ok()) {
        // Exhaustion, or a durable write that failed. Either way nothing has
        // been touched: the reservation is all-or-nothing and this is the first
        // statement that mutates anything.
        return reserved.error().with_context("allocating identities for a local edit");
    }

    uint64_t next_item = reserved.value();
    const Identity op_identity(m_allocator->actor(), next_item + inserted_scalars);

    EditOperation op;
    op.ranges = ranges;
    op.new_text = new_text;
    op.version = m_version;
    UndoEntry undo_entry;
    bool had_deletions = false;

    // Track actually-deleted character timestamps for building deletion_runs.
    std::vector<Identity> deleted_timestamps;

    // Ascending, left to right. Every offset is in the OLD visible text.
    std::vector<size_t> order(ranges.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return ranges[a].first < ranges[b].first;
    });

    // The edit is applied to a flat copy of the document. Placement is the slow,
    // whole-vector path on purpose (S03): one code path, SequenceOrder::integrate,
    // decides where an element goes for the originator and for every peer.
    auto frags = get_fragments();

    // ---- Mint every insertion against the OLD state ----
    //
    // An insertion is "immediately after the visible element before `start`".
    // Minting every range against the same old state keeps one range's items from
    // depending on another's, so the operation's fragments can be integrated in any
    // order by a receiver. The paper's definition is the same: the left origin is
    // the (i-1)th VISIBLE element, and the new element sits right after it, ahead
    // of any tombstones that follow.
    std::vector<Fragment> inserts;
    for (const size_t idx : order) {
        const std::string &replacement = new_text[idx];
        if (replacement.empty()) continue;

        const std::optional<Identity> left = visible_element_before(frags, ranges[idx].first);
        const Link link = SequenceOrder::mint_link(frags, left);

        // The scalar count was computed before the reservation, so the item
        // identities are consecutive positions of the one interval this
        // operation owns — run by run, in document order.
        const uint32_t char_count = scalar_counts[idx];
        const Identity frag_origin(m_allocator->actor(), next_item);
        next_item += char_count;

        Fragment frag(frag_origin, link, static_cast<uint32_t>(replacement.size()),
                      char_count, replacement);
        frag.visible = true;
        inserts.push_back(std::move(frag));

        EditOperation::InsertedFragment ins_rec;
        ins_rec.origin = frag_origin;
        ins_rec.link = link;
        ins_rec.content = replacement;
        ins_rec.length = char_count;
        op.inserted_fragments.push_back(std::move(ins_rec));

        for (uint32_t c = 0; c < char_count; ++c) {
            undo_entry.inserted_keys.push_back(
                UndoMapKey(Identity(frag_origin.actor, frag_origin.counter + c)));
        }
    }

    // ---- Deletions ----
    // Right to left, so the old-text offsets of the ranges still to be processed
    // stay valid while earlier ones are tombstoned.
    for (auto at = order.rbegin(); at != order.rend(); ++at) {
        const auto [start, end] = ranges[*at];
        if (end <= start) continue;
        split_at_visible_offset(frags, start);
        split_at_visible_offset(frags, end);
        uint32_t seen = 0;
        for (auto &f : frags) {
            if (!f.visible) continue;
            const uint32_t from = seen;
            seen += f.byte_length;
            if (from < start || seen > end) continue;
            f.deletions.push_back(op_identity);
            f.visible = false;
            had_deletions = true;
            for (uint32_t c = 0; c < f.length; ++c)
                deleted_timestamps.push_back(f.item_at(c));
        }
    }

    // ---- Placement, then commit ----
    for (auto &frag : inserts) {
        const bool placed = SequenceOrder::integrate(frags, std::move(frag));
        assert(placed && "a link minted from this very state names a present element");
        (void)placed;
    }
    set_fragments(std::move(frags));

    // ---- Identity ----
    // The operation's identity is the last value of the interval reserved above,
    // so it is strictly above every item identity it created and strictly below
    // whatever the next reservation returns. An insert operation and one of its
    // own characters no longer share an identity, and "which operation inserted
    // this character" is a range-containment question.
    //
    // A deletion-only operation reserves exactly one value, and `next_item` was
    // never advanced, so `op_identity` is that single value.
    assert(next_item == op_identity.counter);
    op.timestamp = op_identity;
    // The deleting operation's own identity marks the tombstone (§3.3). Before
    // the domains were separated this needed a second counter tick
    // (`deletion_ts`) because the operation's timestamp could not serve twice.
    op.deletion_id = op_identity;
    m_version.observe(op.timestamp);

    // ---- Undo entry ----
    if (had_deletions)
        undo_entry.deletion_ids.push_back(op_identity);
    m_undo_stack.resize(m_undo_cursor);
    m_undo_stack.push_back(std::move(undo_entry));
    m_undo_cursor = m_undo_stack.size();
    trim_undo_stack();

    // Build run-length encoded deletion descriptors from the deleted identities.
    // Sort by (actor, counter) then group consecutive runs.
    {
        std::sort(deleted_timestamps.begin(), deleted_timestamps.end());
        for (size_t i = 0; i < deleted_timestamps.size(); ) {
            const ActorId who = deleted_timestamps[i].actor;
            const uint64_t start = deleted_timestamps[i].counter;
            uint32_t count = 1;
            while (i + count < deleted_timestamps.size() &&
                   deleted_timestamps[i + count].actor == who &&
                   deleted_timestamps[i + count].counter == start + count) {
                ++count;
            }
            op.deletion_runs.push_back({Identity(who, start), count});
            i += count;
        }
    }

    if (m_on_change) m_on_change();

    return Result<Operation>(std::move(op));
}

// ---------------------------------------------------------------------------
// Remote edit application
// ---------------------------------------------------------------------------

void Buffer::apply_deletion_runs(
    std::vector<Fragment>& frags,
    const std::vector<EditOperation::DeletionRun>& runs,
    Identity deletion_id)
{
    // Process each deletion run. Each run targets consecutive item identities
    // from a single actor. Find the fragment(s) containing those items, split
    // at boundaries, and mark deleted.
    for (auto& run : runs) {
        uint32_t remaining = run.count;
        uint64_t next_counter = run.first.counter;

        while (remaining > 0) {
            // Find the fragment containing item (run.first.actor, next_counter)
            bool found = false;
            for (size_t fi = 0; fi < frags.size(); ++fi) {
                auto &f = frags[fi];
                if (f.origin.actor != run.first.actor) continue;
                if (next_counter < f.origin.counter ||
                    next_counter >= f.origin.counter + f.length) continue;

                found = true;
                uint32_t char_off =
                    static_cast<uint32_t>(next_counter - f.origin.counter);
                uint32_t avail = f.length - char_off;
                uint32_t to_del = std::min(remaining, avail);

                if (char_off == 0 && to_del == f.length) {
                    // Delete entire fragment
                    f.deletions.push_back(deletion_id);
                } else {
                    // Need to split
                    if (char_off > 0) {
                        uint32_t byte_off = char_to_byte_offset(frags[fi].text, char_off);
                        fi = split_fragment_at(frags, fi, byte_off);
                        // fi now points to the second half
                        to_del = std::min(remaining, frags[fi].length);
                    }

                    if (to_del < frags[fi].length) {
                        uint32_t byte_off = char_to_byte_offset(frags[fi].text, to_del);
                        split_fragment_at(frags, fi, byte_off);
                        // fi still points to the first part (to be deleted)
                    }

                    frags[fi].deletions.push_back(deletion_id);
                }

                next_counter += to_del;
                remaining -= to_del;
                break;
            }
            if (!found) break; // Item not found, skip remaining
        }
    }
}

Buffer::ApplyStep Buffer::apply_remote_edit(const EditOperation &op) {
    if (m_version.observed(op.timestamp))
        return ApplyStep::applied;

    if (!m_version.observed_all(op.version))
        return ApplyStep::not_ready;

    // Every remote edit takes this one path: copy the document, apply the
    // deletions, integrate each inserted run by its tree relation, rebuild. It is
    // O(n) per operation by design (S03 disabled the locator fast paths rather than
    // adapt them); the only thing it must be is the same rule the originator ran.
    auto frags = get_fragments();

    apply_deletion_runs(frags, op.deletion_runs, op.deletion_id);

    for (auto &ins : op.inserted_fragments) {
        Fragment frag(ins.origin, ins.link, static_cast<uint32_t>(ins.content.size()),
                      ins.length, ins.content);
        if (!SequenceOrder::integrate(frags, std::move(frag))) {
            // The run hangs off an element this replica does not hold even though the
            // operation's frontier says it was observed: no honest replica produces
            // that, and waiting will not make it true. Nothing has been committed.
            m_apply_error = Error(ErrorCode::malformed,
                                  "inserted run attaches to an element that is not in the document",
                                  "run origin " + ins.origin.actor.to_string() + "/" +
                                      std::to_string(ins.origin.counter));
            return ApplyStep::malformed;
        }
    }

    // The raise is derived state, not durable allocation (§3.4): applying a
    // remote operation lifts the local counter above everything that operation
    // used, so a later local identity sorts after it. Under §3.2 the operation
    // identity is the highest value in its interval, so observing it covers the
    // whole run; the per-run observations below are retained because they are
    // what makes this correct for an operation minted by a peer that has not
    // adopted interval allocation.
    observe_remote(op.timestamp);
    m_version.observe(op.timestamp);
    m_version.observe(op.deletion_id);

    for (auto &ins : op.inserted_fragments) {
        Identity last_ts(ins.origin.actor, ins.origin.counter + ins.length - 1);
        observe_remote(last_ts);
        m_version.observe(last_ts);
    }

    m_version.join(op.version);

    set_fragments(std::move(frags));
    return ApplyStep::applied;
}

// ---------------------------------------------------------------------------
// Remote undo application
// ---------------------------------------------------------------------------

Buffer::ApplyStep Buffer::apply_remote_undo(const UndoOperation &op) {
    if (m_version.observed(op.timestamp))
        return ApplyStep::applied;

    if (!m_version.observed_all(op.version))
        return ApplyStep::not_ready;

    for (auto &[edit_id, count] : op.counts) {
        m_undo_map.insert(UndoMapEntry{{edit_id, op.timestamp}, count});
    }

    observe_remote(op.timestamp);
    m_version.observe(op.timestamp);
    m_version.join(op.version);

    m_fragment_tree.for_each_mut([this](Fragment& f) {
        f.visible = f.compute_visible(m_undo_map);
    });
    return ApplyStep::applied;
}

// ---------------------------------------------------------------------------
// apply_ops
// ---------------------------------------------------------------------------

Buffer::ApplyStep Buffer::try_apply(const Operation& op) {
    m_apply_error.reset();
    return visit_operation([this](const auto &o) -> ApplyStep {
        using T = std::decay_t<decltype(o)>;
        if constexpr (std::is_same_v<T, EditOperation>) {
            return apply_remote_edit(o);
        } else {
            return apply_remote_undo(o);
        }
    }, op);
}

ApplyOutcome Buffer::defer_or_quarantine(const Operation& op, Identity awaiting) {
    const size_t bytes = estimate_operation_bytes(op);

    if (deferred_budget_exhausted(m_deferred_budget, m_deferred_count,
                                   m_deferred_bytes, bytes)) {
        // Backpressure is explicit. Diagnostic copies are separately bounded;
        // the caller retains ownership of every refused operation.
        const Error reason(
            ErrorCode::deferred_queue_full,
            "deferral budget exhausted: " + std::to_string(m_deferred_count) +
                "/" + std::to_string(m_deferred_budget.max_operations) +
                " operations and " + std::to_string(m_deferred_bytes) + "/" +
                std::to_string(m_deferred_budget.max_bytes) + " bytes held",
            "waiting on actor " + awaiting.actor.to_string() +
                " counter " + std::to_string(awaiting.counter));
        if (!deferred_budget_exhausted(m_quarantine_budget, m_quarantined.size(),
                                       m_quarantined_bytes, bytes)) {
            m_quarantined.push_back(QuarantinedBufferOp{op, reason});
            m_quarantined_bytes += bytes;
        }
        // The offered operation is a const reference: ownership never transfers
        // on refusal. The store/transport must retain it until accepted.

        return ApplyOutcome::rejected(reason);
    }

    enqueue_deferred({get_op_timestamp(op), op});
    m_deferred_count += 1;
    m_deferred_bytes += bytes;
    return ApplyOutcome::deferred(awaiting);
}

ApplyOutcome Buffer::apply_one(const Operation& op) {
    const Identity ts = get_op_timestamp(op);

    // Identity first, and deliberately before the duplicate check.
    // `observed()` is vacuously true for counter 0, so asking "have I seen
    // this?" of an operation with no usable identity reports a duplicate
    // forever and the malformed record is never refused.
    if (const ValidationResult identity = validate_operation_identity(op);
        !identity.ok()) {
        return ApplyOutcome::rejected(identity.error);
    }

    // Duplicate. Idempotence has to hold even for an operation we could not
    // fully validate, because the commonest cause of an unexpected record is a
    // transport re-delivering one we already applied.
    if (m_version.observed(ts)) return ApplyOutcome::duplicate();

    // Validity before causality: a record that can never be applied is refused
    // now rather than parked in the queue behind a dependency that will not
    // make it valid.
    const ValidationResult verdict = validate_operation(op, m_version);
    switch (verdict.verdict) {
    case ValidationVerdict::rejected:
        return ApplyOutcome::rejected(verdict.error);
    case ValidationVerdict::deferred:
        m_deferred_actors.insert(ts.actor);
        return defer_or_quarantine(op, *verdict.awaiting);
    case ValidationVerdict::valid:
        break;
    }

    // Head-of-line blocking within a replica. Operations from one replica are
    // causally ordered, so if an earlier one is waiting, this one is too.
    if (m_deferred_actors.count(ts.actor)) {
        // The identity this one is ultimately waiting for is the earliest thing
        // its actor is still missing; the blocked actor's own pending
        // operation is the honest answer when it is not otherwise derivable.
        const Identity awaiting = ts.counter > 0
                                      ? Identity(ts.actor, ts.counter - 1)
                                      : ts;
        m_deferred_actors.insert(ts.actor);
        return defer_or_quarantine(op, awaiting);
    }

    switch (try_apply(op)) {
    case ApplyStep::applied:
        break;
    case ApplyStep::malformed:
        // Not a causal wait: this operation can never apply. Say so now instead of
        // parking it behind a dependency that will not make it valid.
        return ApplyOutcome::rejected(*m_apply_error);
    case ApplyStep::not_ready:
        // Validation said this was applicable, so a refusal here means the
        // engine's own causal gate disagrees. Treat it as a deferral rather
        // than pretending it applied: the operation is still owed.
        m_deferred_actors.insert(ts.actor);
        return defer_or_quarantine(op, ts);
    }

    if (m_on_change) m_on_change();
    return ApplyOutcome::applied();
}

std::vector<ApplyOutcome> Buffer::apply_ops(const std::vector<Operation> &ops) {
    std::vector<ApplyOutcome> outcomes;
    outcomes.reserve(ops.size());
    for (const auto &op : ops) {
        outcomes.push_back(apply_one(op));
    }
    retry_deferred();
    return outcomes;
}

void Buffer::retry_deferred() {
    bool progress = true;
    while (progress) {
        progress = false;
        m_deferred_actors.clear();

        OperationQueue remaining;
        size_t remaining_count = 0;
        size_t remaining_bytes = 0;
        m_deferred_queue.for_each([&](const OperationEntry& entry) {
            if (m_deferred_actors.count(entry.timestamp.actor)) {
                remaining.push_item(entry);
                ++remaining_count;
                remaining_bytes += estimate_operation_bytes(entry.op);
                return;
            }
            const ApplyStep step = try_apply(entry.op);
            if (step == ApplyStep::malformed) {
                // It waited for its prerequisites and then turned out to be
                // unappliable. Quarantine it with the reason rather than let it
                // block its actor's queue forever.
                progress = true;
                const size_t bytes = estimate_operation_bytes(entry.op);
                if (!deferred_budget_exhausted(m_quarantine_budget, m_quarantined.size(),
                                               m_quarantined_bytes, bytes)) {
                    m_quarantined.push_back(QuarantinedBufferOp{entry.op, *m_apply_error});
                    m_quarantined_bytes += bytes;
                }
            } else if (step == ApplyStep::applied) {
                progress = true;
                if (m_on_change) m_on_change();
            } else {
                m_deferred_actors.insert(entry.timestamp.actor);
                remaining.push_item(entry);
                ++remaining_count;
                remaining_bytes += estimate_operation_bytes(entry.op);
            }
        });
        m_deferred_queue = std::move(remaining);
        m_deferred_count = remaining_count;
        m_deferred_bytes = remaining_bytes;
    }
}

void Buffer::enqueue_deferred(OperationEntry entry) {
    // Maintain timestamp order so retry_deferred visits lower-timestamp
    // (earlier) ops first per replica, making per-replica blocking correct.
    if (m_deferred_queue.empty()) {
        m_deferred_queue.push_item(std::move(entry));
        return;
    }
    TimestampDim target{entry.timestamp};
    auto cursor = m_deferred_queue.cursor<TimestampDim>();
    cursor.seek(TimestampDim::zero(), Bias::Left);
    OperationQueue new_queue;
    new_queue.push_tree(cursor.slice(target));
    new_queue.push_item(std::move(entry));
    new_queue.push_tree(cursor.suffix());
    m_deferred_queue = std::move(new_queue);
}

// ---------------------------------------------------------------------------
// Undo / Redo
// ---------------------------------------------------------------------------

bool Buffer::coalesce_last_undo() {
    if (m_undo_cursor < 2) return false;

    auto& latest = m_undo_stack[m_undo_cursor - 1];
    auto& previous = m_undo_stack[m_undo_cursor - 2];

    previous.inserted_keys.insert(previous.inserted_keys.end(),
                                  latest.inserted_keys.begin(),
                                  latest.inserted_keys.end());
    previous.deletion_ids.insert(previous.deletion_ids.end(),
                                 latest.deletion_ids.begin(),
                                 latest.deletion_ids.end());

    m_undo_stack.erase(m_undo_stack.begin()
                       + static_cast<ptrdiff_t>(m_undo_cursor - 1));
    m_undo_cursor--;
    return true;
}

LocalHistorySnapshot Buffer::local_history_snapshot() const {
    LocalHistorySnapshot snapshot;
    snapshot.cursor = m_undo_cursor;
    snapshot.entries.reserve(m_undo_stack.size());
    for (const auto &entry : m_undo_stack) {
        LocalUndoEntrySnapshot saved;
        saved.inserted_items.reserve(entry.inserted_keys.size());
        for (const auto &key : entry.inserted_keys)
            saved.inserted_items.push_back(key.position);
        saved.deletion_operations = entry.deletion_ids;
        snapshot.entries.push_back(std::move(saved));
    }
    return snapshot;
}

Result<void>
Buffer::restore_local_history(const LocalHistorySnapshot &snapshot) {
    if (snapshot.cursor > snapshot.entries.size() ||
        snapshot.entries.size() > m_max_undo_depth)
        return Error(ErrorCode::malformed,
                     "local history cursor or depth is outside configured bounds");

    const auto &owner = actor_id();
    std::vector<UndoEntry> restored;
    restored.reserve(snapshot.entries.size());
    auto valid_local_identity = [&](const Identity &id) {
        return id.actor == owner && id.counter != 0 &&
               id.counter <= m_version.get(owner);
    };
    for (const auto &saved : snapshot.entries) {
        UndoEntry entry;
        entry.inserted_keys.reserve(saved.inserted_items.size());
        for (const auto &id : saved.inserted_items) {
            if (!valid_local_identity(id))
                return Error(ErrorCode::history_unavailable,
                             "local undo item is absent from replayed owner history");
            entry.inserted_keys.emplace_back(id);
        }
        for (const auto &id : saved.deletion_operations) {
            if (!valid_local_identity(id))
                return Error(ErrorCode::history_unavailable,
                             "local undo deletion is absent from replayed owner history");
            entry.deletion_ids.push_back(id);
        }
        restored.push_back(std::move(entry));
    }
    m_undo_stack = std::move(restored);
    m_undo_cursor = snapshot.cursor;
    return Result<void>::success();
}

std::optional<Operation> Buffer::undo() {
    if (m_undo_cursor == 0)
        return std::nullopt;

    // An undo inserts no items, so it reserves exactly one value: its own
    // identity (§3.2 with n == 0). The identities it targets are item
    // identities from a different domain, which is why no undo-map entry can be
    // confused with the operation that produced it (§3.3).
    //
    if (auto usable = check_allocator(); !usable.ok()) return std::nullopt;

    // Reserved *before* the cursor moves: a refused undo that had already
    // decremented m_undo_cursor would silently drop an entry from the undo
    // history.
    auto reserved = m_allocator->reserve_run(1);
    if (!reserved.ok()) return std::nullopt;

    m_undo_cursor--;
    auto &entry = m_undo_stack[m_undo_cursor];

    UndoOperation op;
    op.version = m_version;
    op.timestamp = Identity(m_allocator->actor(), reserved.value());

    for (auto &key : entry.inserted_keys) {
        const Identity edit_id = key.position;
        uint32_t current = m_undo_map.undo_count(edit_id);
        m_undo_map.insert(UndoMapEntry{{edit_id, op.timestamp}, current + 1});
        op.counts.push_back({edit_id, current + 1});
    }

    for (auto &deletion_id : entry.deletion_ids) {
        uint32_t current = m_undo_map.undo_count(deletion_id);
        m_undo_map.insert(
            UndoMapEntry{{deletion_id, op.timestamp}, current + 1});
        op.counts.push_back({deletion_id, current + 1});
    }

    m_fragment_tree.for_each_mut([this](Fragment& f) {
        f.visible = f.compute_visible(m_undo_map);
    });

    m_version.observe(op.timestamp);
    return op;
}

std::optional<Operation> Buffer::redo() {
    if (m_undo_cursor >= m_undo_stack.size())
        return std::nullopt;

    auto &entry = m_undo_stack[m_undo_cursor];
    m_undo_cursor++;

    // An undo inserts no items, so it reserves exactly one value: its own
    // identity (§3.2 with n == 0). The identities it targets are item
    // identities from a different domain, which is why no undo-map entry can be
    // confused with the operation that produced it (§3.3).
    auto reserved = m_allocator->reserve_run(1);
    if (!reserved.ok()) return std::nullopt;

    UndoOperation op;
    op.version = m_version;
    op.timestamp = Identity(m_allocator->actor(), reserved.value());

    for (auto &key : entry.inserted_keys) {
        const Identity edit_id = key.position;
        uint32_t current = m_undo_map.undo_count(edit_id);
        m_undo_map.insert(UndoMapEntry{{edit_id, op.timestamp}, current + 1});
        op.counts.push_back({edit_id, current + 1});
    }

    for (auto &deletion_id : entry.deletion_ids) {
        uint32_t current = m_undo_map.undo_count(deletion_id);
        m_undo_map.insert(
            UndoMapEntry{{deletion_id, op.timestamp}, current + 1});
        op.counts.push_back({deletion_id, current + 1});
    }

    m_fragment_tree.for_each_mut([this](Fragment& f) {
        f.visible = f.compute_visible(m_undo_map);
    });

    m_version.observe(op.timestamp);
    return op;
}

} // namespace CollabText::Crdt
