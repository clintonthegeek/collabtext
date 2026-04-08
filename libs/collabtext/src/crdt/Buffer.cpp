#include "crdt/Buffer.h"
#include "crdt/OperationQueue.h"
#include <algorithm>
#include <cassert>
#include <numeric>
#include <unordered_set>

namespace CollabText::Crdt {

Buffer::Buffer(uint16_t replica_id)
    : m_replica_id(replica_id)
    , m_clock(replica_id, 1)  // Start at 1; value 0 is the "unseen" sentinel
{
}

// ---------------------------------------------------------------------------
// Fragment tree helpers
// ---------------------------------------------------------------------------

std::vector<Fragment> Buffer::get_fragments() const {
    return m_fragment_tree.items();
}

/// Pack (replica_id, value) into a single uint64_t for map lookup.
static uint64_t origin_key(const Lamport& origin) {
    return (static_cast<uint64_t>(origin.replica_id) << 32) | origin.value;
}

void Buffer::set_fragments(std::vector<Fragment>&& frags) {
    for (auto& f : frags) {
        f.visible = f.compute_visible(m_undo_map);
        assert(f.byte_length == f.text.size());
    }
    rebuild_insertion_index(frags);
    FragmentTree tree;
    for (auto& f : frags) tree.push_item(std::move(f));
    m_fragment_tree = std::move(tree);
    rebuild_origin_index();
}

void Buffer::rebuild_origin_index() {
    m_origin_index.clear();
    m_fragment_tree.for_each([this](const Fragment& f) {
        m_origin_index[f.origin.replica_id][f.origin.value] = f.locator;
    });
}

std::optional<Locator> Buffer::origin_index_lookup(
    uint16_t replica_id, uint32_t origin_value) const
{
    auto rep_it = m_origin_index.find(replica_id);
    if (rep_it == m_origin_index.end()) return std::nullopt;
    auto& rep_map = rep_it->second;
    if (rep_map.empty()) return std::nullopt;
    auto it = rep_map.upper_bound(origin_value);
    if (it == rep_map.begin()) return std::nullopt;
    --it;
    return it->second;
}

void Buffer::rebuild_insertion_index(const std::vector<Fragment>& frags) {
    InsertionIndex index;
    for (auto& f : frags) {
        // Each fragment maps from (origin timestamp, byte offset within insertion)
        // to its locator. The split_offset is the character offset from the
        // original insertion's start (origin.value - origin.value = 0 for the
        // first fragment, or the offset for split fragments).
        index.push_item(InsertionFragment(
            f.origin,
            0,  // split_offset: 0 for the first/only fragment of an insertion
            f.locator,
            f.byte_length
        ));
    }
    m_insertion_index = std::move(index);
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

template<typename Pred>
size_t Buffer::sweep_and_coalesce(Pred is_gc_eligible) {
    auto frags = get_fragments();
    size_t original_count = frags.size();

    frags.erase(
        std::remove_if(frags.begin(), frags.end(), is_gc_eligible),
        frags.end());

    size_t removed = original_count - frags.size();

    coalesce_fragments(frags);

    if (removed > 0 || frags.size() < original_count) {
        set_fragments(std::move(frags));
    }
    return removed;
}

size_t Buffer::collect_garbage() {
    // Build protected set: deletion IDs still in the undo stack
    std::unordered_set<uint64_t> protected_ids;
    for (const auto& entry : m_undo_stack) {
        for (const auto& del_id : entry.deletion_ids)
            protected_ids.insert(origin_key(del_id));
    }

    return sweep_and_coalesce([&](const Fragment& f) {
        if (f.visible) return false;
        for (const auto& del : f.deletions) {
            if (del.replica_id != m_replica_id)
                return false;  // remote deletion — needs watermark GC
            if (protected_ids.count(origin_key(del)))
                return false;  // protected by undo stack
        }
        return true;
    });
}

size_t Buffer::compact(const Global& watermark) {
    // Build local undo protection set
    std::unordered_set<uint64_t> protected_ids;
    for (const auto& entry : m_undo_stack) {
        for (const auto& del_id : entry.deletion_ids)
            protected_ids.insert(origin_key(del_id));
    }

    return sweep_and_coalesce([&](const Fragment& f) {
        if (f.visible) return false;
        if (f.deletions.empty()) return false;  // undone insertion, not a deletion tombstone
        for (const auto& del : f.deletions) {
            if (!watermark.observed(del))
                return false;  // not all replicas have seen this deletion
            if (protected_ids.count(origin_key(del)))
                return false;  // locally undo-protected
        }
        return true;
    });
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

void Buffer::coalesce_fragments(std::vector<Fragment>& frags) {
    if (frags.size() < 2) return;
    size_t write = 0;
    for (size_t read = 1; read < frags.size(); ++read) {
        Fragment& prev = frags[write];
        Fragment& curr = frags[read];
        if (prev.visible == curr.visible &&
            prev.locator == curr.locator &&
            prev.origin.replica_id == curr.origin.replica_id &&
            prev.origin.value + prev.length == curr.origin.value &&
            prev.deletions == curr.deletions) {
            prev.text += curr.text;
            prev.byte_length += curr.byte_length;
            prev.length += curr.length;
        } else {
            ++write;
            if (write != read)
                frags[write] = std::move(curr);
        }
    }
    frags.resize(write + 1);
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
    Anchor result = Anchor::max();
    bool found = false;
    m_fragment_tree.for_each([&](const Fragment& f) {
        if (found) return;
        if (!f.visible) return;
        if (accumulated + f.byte_length > byte_offset) {
            uint32_t offset_in_frag = byte_offset - accumulated;
            uint32_t char_offset = count_chars(f.text, offset_in_frag);
            result = Anchor(f.origin.replica_id, f.origin.value + char_offset, bias);
            found = true;
            return;
        }
        accumulated += f.byte_length;
    });
    return result;
}

uint32_t Buffer::resolve_anchor(const Anchor& anchor) const {
    if (anchor.is_min()) return 0;
    if (anchor.is_max()) return visible_length();
    uint32_t accumulated = 0;
    uint32_t result = visible_length();
    bool found = false;
    m_fragment_tree.for_each([&](const Fragment& f) {
        if (found) return;
        if (f.origin.replica_id == anchor.replica_id &&
            anchor.char_value >= f.origin.value &&
            anchor.char_value < f.origin.value + f.length) {
            if (f.visible) {
                uint32_t char_offset = anchor.char_value - f.origin.value;
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

uint16_t Buffer::replica_id() const {
    return m_replica_id;
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

void Buffer::insert_fragment(std::vector<Fragment>& frags, Fragment frag) const {
    auto it = std::lower_bound(frags.begin(), frags.end(), frag,
        [](const Fragment &a, const Fragment &b) {
            auto cmp = a.locator <=> b.locator;
            if (cmp != 0) return cmp < 0;
            return a.origin < b.origin;
        });
    frags.insert(it, std::move(frag));
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

    Fragment &orig = frags[frag_idx];
    uint32_t char_count = count_utf8_chars(orig.text, offset_in_frag);

    Fragment second;
    second.origin = Lamport(orig.origin.replica_id, orig.origin.value + char_count);
    second.locator = orig.locator;
    second.text = orig.text.substr(offset_in_frag);
    second.byte_length = static_cast<uint32_t>(second.text.size());
    second.length = orig.length - char_count;
    second.deletions = orig.deletions;
    second.visible = orig.visible;

    orig.text.resize(offset_in_frag);
    orig.byte_length = offset_in_frag;
    orig.length = char_count;

    Lamport second_origin = second.origin;
    Locator saved_locator = orig.locator;
    insert_fragment(frags, std::move(second));

    for (size_t i = frag_idx + 1; i < frags.size(); ++i) {
        if (frags[i].origin == second_origin && frags[i].locator == saved_locator)
            return i;
    }
    assert(false && "split_fragment_at: could not find second half after insert");
    return frag_idx + 1;
}

Locator Buffer::locator_between(const std::vector<Fragment>& frags,
                                 size_t ins_frag) const
{
    Locator lo = Locator::min();
    if (ins_frag > 0) {
        lo = frags[ins_frag - 1].locator;
    }

    Locator hi = Locator::max();
    for (size_t i = ins_frag; i < frags.size(); ++i) {
        if (frags[i].locator > lo) {
            hi = frags[i].locator;
            break;
        }
    }

    assert(lo < hi);
    return Locator::between(lo, hi);
}

void Buffer::normalize_fragments(std::vector<Fragment>& frags) const {
    size_t i = 0;
    while (i < frags.size()) {
        size_t run_start = i;
        Locator loc = frags[i].locator;
        size_t run_end = i + 1;
        while (run_end < frags.size() && frags[run_end].locator == loc)
            run_end++;

        bool multi_replica = false;
        uint16_t first_replica = frags[run_start].origin.replica_id;
        for (size_t j = run_start + 1; j < run_end; ++j) {
            if (frags[j].origin.replica_id != first_replica) {
                multi_replica = true;
                break;
            }
        }

        if (multi_replica) {
            std::vector<Fragment> extracted;
            for (size_t j = run_start; j < run_end; ++j) {
                auto &f = frags[j];
                if (f.length == 1) {
                    extracted.push_back(std::move(f));
                } else {
                    const std::string& ftext = f.text;
                    uint32_t byte_pos = 0;
                    for (uint32_t c = 0; c < f.length; ++c) {
                        uint32_t char_bytes = 1;
                        unsigned char ch = static_cast<unsigned char>(ftext[byte_pos]);
                        if (ch >= 0xF0) char_bytes = 4;
                        else if (ch >= 0xE0) char_bytes = 3;
                        else if (ch >= 0xC0) char_bytes = 2;

                        Lamport single_origin(f.origin.replica_id, f.origin.value + c);
                        Fragment single;
                        single.origin = single_origin;
                        single.locator = f.locator;
                        single.text = ftext.substr(byte_pos, char_bytes);
                        single.byte_length = char_bytes;
                        single.length = 1;
                        single.deletions = f.deletions;
                        extracted.push_back(std::move(single));
                        byte_pos += char_bytes;
                    }
                }
            }

            std::sort(extracted.begin(), extracted.end(),
                [](const Fragment &a, const Fragment &b) {
                    auto cmp = a.locator <=> b.locator;
                    if (cmp != 0) return cmp < 0;
                    return a.origin < b.origin;
                });

            frags.erase(
                frags.begin() + static_cast<ptrdiff_t>(run_start),
                frags.begin() + static_cast<ptrdiff_t>(run_end));
            frags.insert(
                frags.begin() + static_cast<ptrdiff_t>(run_start),
                std::make_move_iterator(extracted.begin()),
                std::make_move_iterator(extracted.end()));
            i = run_start + extracted.size();
        } else {
            i = run_end;
        }
    }
}

// ---------------------------------------------------------------------------
// Local edit
// ---------------------------------------------------------------------------

Operation Buffer::apply_local_edit(
    const std::vector<std::pair<uint32_t, uint32_t>> &ranges,
    const std::vector<std::string> &new_text)
{
    assert(ranges.size() == new_text.size());

    EditOperation op;
    op.ranges = ranges;
    op.new_text = new_text;
    op.version = m_version;
    UndoEntry undo_entry;
    Lamport deletion_ts = m_clock.tick();
    bool had_deletions = false;

    // Track actually-deleted character timestamps for building deletion_runs.
    std::vector<Lamport> deleted_timestamps;

    // Sort ranges ascending (left-to-right) — offsets are in the OLD tree's
    // visible space and never change during processing.
    std::vector<size_t> order(ranges.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return ranges[a].first < ranges[b].first;
    });

    auto cursor = m_fragment_tree.cursor<VisibleOffset>();
    // Note: do NOT seek({0}) here — that would skip past invisible prefix

    // Deferred relocations: applied during the final sort phase.
    // Each entry: {old_locator, min_origin, new_locator}
    // All fragments with locator == old_locator and origin >= min_origin
    // will have their locator changed to new_locator.
    struct DeferredReloc {
        Locator old_loc;
        Lamport min_origin;
        Locator new_loc;
    };
    std::vector<DeferredReloc> deferred_relocs;
    // fragments. The slice() method handles the !m_did_seek case by starting
    // from the leftmost item.
    FragmentTree new_tree;

    // Pending: the remaining half of a fragment split at a range boundary.
    // Carried across ranges when a single old-tree fragment spans multiple ranges.
    std::optional<Fragment> pending;

    // Helper: mark a fragment as deleted and record timestamps for op/undo.
    auto mark_deleted = [&](Fragment& f) {
        f.deletions.push_back(deletion_ts);
        f.visible = false;
        had_deletions = true;
        for (uint32_t c = 0; c < f.length; ++c) {
            deleted_timestamps.push_back(f.timestamp_at(c));
        }
    };

    // Helper: split a fragment at a byte offset, returning {first_half, second_half}.
    auto split_frag = [](const Fragment& f, uint32_t byte_off)
        -> std::pair<Fragment, Fragment>
    {
        uint32_t char_count = count_utf8_chars(f.text, byte_off);
        Fragment first;
        first.origin = f.origin;
        first.locator = f.locator;
        first.text = f.text.substr(0, byte_off);
        first.byte_length = byte_off;
        first.length = char_count;
        first.deletions = f.deletions;
        first.visible = f.visible;

        Fragment second;
        second.origin = Lamport(f.origin.replica_id, f.origin.value + char_count);
        second.locator = f.locator;
        second.text = f.text.substr(byte_off);
        second.byte_length = f.byte_length - byte_off;
        second.length = f.length - char_count;
        second.deletions = f.deletions;
        second.visible = f.visible;
        return {std::move(first), std::move(second)};
    };

    // Helper: consume visible bytes from pending/cursor, pushing unchanged
    // fragments to new_tree. Used for inter-range gaps.
    auto consume_unchanged = [&](uint32_t vis_bytes) {
        // Consume from pending first
        while (pending && vis_bytes > 0) {
            if (pending->visible) {
                uint32_t pv = pending->byte_length;
                if (pv <= vis_bytes) {
                    new_tree.push_item(std::move(*pending));
                    vis_bytes -= pv;
                    pending.reset();
                } else {
                    auto [first, second] = split_frag(*pending, vis_bytes);
                    new_tree.push_item(std::move(first));
                    pending = std::move(second);
                    vis_bytes = 0;
                }
            } else {
                // Invisible — push, no visible bytes consumed
                new_tree.push_item(std::move(*pending));
                pending.reset();
            }
        }
        // Consume from cursor
        while (vis_bytes > 0 && cursor.item()) {
            if (!cursor.item()->visible) {
                Fragment frag = *cursor.item();
                new_tree.push_item(std::move(frag));
                cursor.next();
                continue;
            }
            Fragment frag = *cursor.item();
            uint32_t fv = frag.byte_length;
            if (fv <= vis_bytes) {
                new_tree.push_item(std::move(frag));
                vis_bytes -= fv;
                cursor.next();
            } else {
                auto [first, second] = split_frag(frag, vis_bytes);
                new_tree.push_item(std::move(first));
                pending = std::move(second);
                cursor.next();
                vis_bytes = 0;
            }
        }
    };

    // Helper: consume and delete visible bytes from pending/cursor.
    auto consume_deleted = [&](uint32_t vis_bytes) {
        // From pending
        while (pending && vis_bytes > 0) {
            if (pending->visible) {
                uint32_t pv = pending->byte_length;
                if (pv <= vis_bytes) {
                    mark_deleted(*pending);
                    new_tree.push_item(std::move(*pending));
                    vis_bytes -= pv;
                    pending.reset();
                } else {
                    auto [first, second] = split_frag(*pending, vis_bytes);
                    mark_deleted(first);
                    new_tree.push_item(std::move(first));
                    pending = std::move(second);
                    vis_bytes = 0;
                }
            } else {
                new_tree.push_item(std::move(*pending));
                pending.reset();
            }
        }
        // From cursor
        while (vis_bytes > 0 && cursor.item()) {
            if (!cursor.item()->visible) {
                Fragment frag = *cursor.item();
                new_tree.push_item(std::move(frag));
                cursor.next();
                continue;
            }
            Fragment frag = *cursor.item();
            uint32_t fv = frag.byte_length;
            if (fv <= vis_bytes) {
                mark_deleted(frag);
                new_tree.push_item(std::move(frag));
                vis_bytes -= fv;
                cursor.next();
            } else {
                auto [first, second] = split_frag(frag, vis_bytes);
                mark_deleted(first);
                new_tree.push_item(std::move(first));
                pending = std::move(second);
                cursor.next();
                vis_bytes = 0;
            }
        }
    };

    // ---- Phase 0: Prefix copy (up to first range) ----
    if (!order.empty()) {
        uint32_t first_start = ranges[order[0]].first;
        // Use cursor.slice for O(log n) bulk copy
        new_tree.push_tree(cursor.slice({first_start}));
        // Handle straddling fragment (cursor.position < first_start means
        // a visible fragment spans the boundary)
        if (cursor.item() && cursor.position().value < first_start) {
            uint32_t split_byte = first_start - cursor.position().value;
            auto [first, second] = split_frag(*cursor.item(), split_byte);
            new_tree.push_item(std::move(first));
            pending = std::move(second);
            cursor.next();
        }
    }

    // ---- Process each range ----
    uint32_t prev_end = order.empty() ? 0 : ranges[order[0]].first;

    for (size_t oi = 0; oi < order.size(); ++oi) {
        size_t idx = order[oi];
        uint32_t start = ranges[idx].first;
        uint32_t end = ranges[idx].second;
        const std::string &replacement = new_text[idx];

        // Inter-range gap: copy unchanged content between previous end and this start
        if (start > prev_end) {
            consume_unchanged(start - prev_end);
        }

        // ---- Delete phase ----
        if (end > start) {
            consume_deleted(end - start);
        }

        // ---- Insert phase ----
        if (!replacement.empty()) {
            // Determine lo/hi locators for the new fragment.
            Locator lo = new_tree.empty() ? Locator::min() : new_tree.last().locator;

            // Check the immediate next fragment's locator.
            Locator imm_loc;
            bool has_imm = false;
            if (pending) {
                imm_loc = pending->locator;
                has_imm = true;
            } else if (cursor.item()) {
                imm_loc = cursor.item()->locator;
                has_imm = true;
            }

            Locator hi = Locator::max();
            bool needs_relocation = false;
            if (has_imm && imm_loc > lo) {
                hi = imm_loc;
            } else if (has_imm) {
                // Same-locator group — need relocation to create space.
                needs_relocation = true;
                // Find the next locator strictly > lo in the original tree.
                {
                    auto loc_cursor = m_fragment_tree.cursor<FragmentOrderDim>();
                    auto _prefix = loc_cursor.slice(
                        FragmentOrderDim{lo, Lamport::max()});
                    if (loc_cursor.item() && loc_cursor.item()->locator > lo) {
                        hi = loc_cursor.item()->locator;
                    }
                }
            }

            // When inserting between same-locator fragments, relocate them
            // to create ordering space. We relocate the pending fragment now,
            // and record a deferred relocation for remaining same-locator
            // cursor fragments — they'll be relocated during the final sort.
            if (needs_relocation) {
                Locator new_next_loc = Locator::between(lo, hi);

                Lamport reloc_origin;
                uint32_t reloc_length = 0;

                if (pending) {
                    reloc_origin = pending->origin;
                    reloc_length = pending->length;
                    pending->locator = new_next_loc;
                } else if (cursor.item()) {
                    Fragment f = *cursor.item();
                    reloc_origin = f.origin;
                    reloc_length = f.length;
                    f.locator = new_next_loc;
                    pending = std::move(f);
                    cursor.next();
                }

                // Record deferred relocation: all fragments at old locator
                // with origin >= reloc_origin will be relocated during sort.
                deferred_relocs.push_back({lo, reloc_origin, new_next_loc});

                // Record SplitRelocation for remote replicas
                EditOperation::SplitRelocation reloc;
                reloc.fragment_origin = reloc_origin;
                reloc.split_offset = 0;
                reloc.fragment_length = reloc_length;
                reloc.new_locator = new_next_loc;
                op.split_relocations.push_back(std::move(reloc));

                hi = new_next_loc;
            }

            Locator new_loc = Locator::between(lo, hi);

            uint32_t char_count = count_utf8_chars(
                replacement, static_cast<uint32_t>(replacement.size()));

            Lamport frag_origin = m_clock.tick();
            for (uint32_t c = 1; c < char_count; ++c) m_clock.tick();

            Fragment frag(frag_origin, new_loc,
                          static_cast<uint32_t>(replacement.size()), char_count,
                          replacement);
            frag.visible = true;
            new_tree.push_item(std::move(frag));

            EditOperation::InsertedFragment ins_rec;
            ins_rec.origin = frag_origin;
            ins_rec.locator = new_loc;
            ins_rec.content = replacement;
            ins_rec.length = char_count;
            op.inserted_fragments.push_back(std::move(ins_rec));

            for (uint32_t c = 0; c < char_count; ++c) {
                undo_entry.inserted_keys.push_back(
                    UndoMapKey(frag_origin.replica_id, frag_origin.value + c));
            }
        }

        prev_end = end;
    }

    // ---- Suffix ----
    if (pending) {
        new_tree.push_item(std::move(*pending));
        pending.reset();
    }
    new_tree.push_tree(cursor.suffix());

    // ---- Timestamp ----
    if (op.inserted_fragments.empty()) {
        op.timestamp = m_clock.tick();
    } else {
        op.timestamp = Lamport(m_replica_id, m_clock.value - 1);
    }
    op.deletion_id = deletion_ts;
    m_version.observe(op.timestamp);

    // ---- Undo entry ----
    if (had_deletions)
        undo_entry.deletion_ids.push_back(deletion_ts);
    m_undo_stack.resize(m_undo_cursor);
    m_undo_stack.push_back(std::move(undo_entry));
    m_undo_cursor = m_undo_stack.size();
    trim_undo_stack();

    // ---- Commit tree: fast path, in-place relocation, or full rebuild ----
    // Check ordering of the cursor-built tree (O(n) walk, tiny constant).
    bool ordering_ok = true;
    {
        Locator prev_loc;
        Lamport prev_origin;
        bool first = true;
        new_tree.for_each([&](const Fragment& f) {
            if (!ordering_ok) return;
            if (!first) {
                if (f.locator < prev_loc ||
                    (f.locator == prev_loc && f.origin < prev_origin)) {
                    ordering_ok = false;
                }
            }
            prev_loc = f.locator;
            prev_origin = f.origin;
            first = false;
        });
    }

    bool used_fast_path = false;
    if (ordering_ok && deferred_relocs.empty()) {
        // Tree is ordered, no relocations needed. Commit directly.
        // Visibility is already correct from the cursor walk:
        // - Unchanged fragments: copied with original visibility (undo_map unchanged)
        // - Deleted fragments: mark_deleted set visible=false
        // - Inserted fragments: set visible=true
        m_fragment_tree = std::move(new_tree);

        // Incremental origin index update for inserted fragments
        for (auto& ins : op.inserted_fragments) {
            m_origin_index[ins.origin.replica_id][ins.origin.value] = ins.locator;
        }
        used_fast_path = true;
    }

    if (!used_fast_path) {
        // Full path: extract, relocate, sort, normalize, rebuild.
        // Only reached when the cursor-built tree has ordering violations
        // (~2.3% of edits in adversarial patterns).
        auto frags = new_tree.items();

        for (auto& dr : deferred_relocs) {
            Lamport next_expected = dr.min_origin;
            uint32_t total_chars = 0;
            for (auto& f : frags) {
                if (f.origin.replica_id != dr.min_origin.replica_id) continue;
                if (f.origin.value != next_expected.value) continue;
                if (f.locator == dr.old_loc) {
                    f.locator = dr.new_loc;
                }
                total_chars += f.length;
                next_expected = Lamport(f.origin.replica_id,
                                        f.origin.value + f.length);
            }
            for (auto& sr : op.split_relocations) {
                if (sr.fragment_origin == dr.min_origin && sr.new_locator == dr.new_loc) {
                    sr.fragment_length = total_chars;
                    break;
                }
            }
        }

        std::sort(frags.begin(), frags.end(), [](const Fragment& a, const Fragment& b) {
            if (auto cmp = a.locator <=> b.locator; cmp != 0) return cmp < 0;
            return a.origin < b.origin;
        });
        normalize_fragments(frags);
        set_fragments(std::move(frags));
    }

    // Build run-length encoded deletion descriptors from the deleted timestamps.
    // Sort by (replica_id, value) then group contiguous runs.
    {
        std::sort(deleted_timestamps.begin(), deleted_timestamps.end());
        for (size_t i = 0; i < deleted_timestamps.size(); ) {
            uint16_t rid = deleted_timestamps[i].replica_id;
            uint32_t start = deleted_timestamps[i].value;
            uint32_t count = 1;
            while (i + count < deleted_timestamps.size() &&
                   deleted_timestamps[i + count].replica_id == rid &&
                   deleted_timestamps[i + count].value == start + count) {
                ++count;
            }
            op.deletion_runs.push_back({rid, start, count});
            i += count;
        }
    }

    return op;
}

// ---------------------------------------------------------------------------
// Remote edit application
// ---------------------------------------------------------------------------

void Buffer::apply_deletion_runs(
    std::vector<Fragment>& frags,
    const std::vector<EditOperation::DeletionRun>& runs,
    Lamport deletion_id)
{
    // Process each deletion run. Each run targets characters from a single
    // replica with contiguous Lamport values. Find the fragment(s) containing
    // those characters, split at boundaries, and mark deleted.
    for (auto& run : runs) {
        uint32_t remaining = run.count;
        uint32_t next_val = run.start_value;

        while (remaining > 0) {
            // Find the fragment containing character (run.replica_id, next_val)
            bool found = false;
            for (size_t fi = 0; fi < frags.size(); ++fi) {
                auto &f = frags[fi];
                if (f.origin.replica_id != run.replica_id) continue;
                if (next_val < f.origin.value ||
                    next_val >= f.origin.value + f.length) continue;

                found = true;
                uint32_t char_off = next_val - f.origin.value;
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

                next_val += to_del;
                remaining -= to_del;
                break;
            }
            if (!found) break; // Character not found, skip remaining
        }
    }
}

bool Buffer::apply_remote_edit_fast(const EditOperation &op) {
    if (!op.split_relocations.empty()) return false;
    for (auto &ins : op.inserted_fragments) {
        if (ins.length != 1) return false;
    }

    // Locator-collision check: if any incoming insert lands on a locator that
    // already has a fragment in the tree, fall back to the slow path so
    // normalize_fragments can atomize multi-character fragments at the shared
    // locator.  Without this, an existing multi-char fragment from one replica
    // would coexist with the incoming single-char fragment from another
    // replica, producing a tree that reads in the wrong order versus replicas
    // that received the multi-char insert via the slow path.
    for (auto &ins : op.inserted_fragments) {
        auto check_cursor = m_fragment_tree.cursor<FragmentOrderDim>();
        check_cursor.seek(
            FragmentOrderDim{ins.locator, Lamport::min()}, Bias::Right);
        if (check_cursor.item() && check_cursor.item()->locator == ins.locator)
            return false;
    }

    // Apply deletion runs via origin index + in-place tree mutations.
    // Handles partial deletions by splitting fragments in place.
    for (auto& run : op.deletion_runs) {
        uint32_t remaining = run.count;
        uint32_t next_val = run.start_value;

        while (remaining > 0) {
            auto loc_opt = origin_index_lookup(run.replica_id, next_val);
            if (!loc_opt) return false;  // Can't find fragment, fall back

            FragmentOrderDim target{*loc_opt, Lamport(run.replica_id, next_val)};

            // Capture fragment info for potential splitting
            struct SplitInfo {
                bool found = false;
                bool needs_split_before = false;  // split at char_off > 0
                bool needs_split_after = false;   // split after to_del < length
                uint32_t char_off = 0;
                uint32_t to_del = 0;
                // Info for building the split-off fragments
                Fragment before_frag;  // chars [0, char_off)
                Fragment after_frag;   // chars [char_off + to_del, length)
            };
            SplitInfo info;

            bool found = m_fragment_tree.edit_item<FragmentOrderDim>(
                target,
                [&](Fragment& f) {
                    if (f.origin.replica_id != run.replica_id) return;
                    if (next_val < f.origin.value ||
                        next_val >= f.origin.value + f.length) return;

                    info.found = true;
                    info.char_off = next_val - f.origin.value;
                    uint32_t avail = f.length - info.char_off;
                    info.to_del = std::min(remaining, avail);

                    if (info.char_off == 0 && info.to_del == f.length) {
                        // Whole-fragment deletion — simple case
                        f.deletions.push_back(op.deletion_id);
                    } else {
                        // Partial deletion — need to split.
                        // Build the split-off fragments BEFORE modifying f.
                        uint32_t byte_off_start = (info.char_off > 0)
                            ? char_to_byte_offset(f.text, info.char_off) : 0;
                        uint32_t byte_off_end = char_to_byte_offset(
                            f.text, info.char_off + info.to_del);

                        if (info.char_off > 0) {
                            // "before" fragment: chars [0, char_off)
                            info.needs_split_before = true;
                            info.before_frag.origin = f.origin;
                            info.before_frag.locator = f.locator;
                            info.before_frag.text = f.text.substr(0, byte_off_start);
                            info.before_frag.byte_length = byte_off_start;
                            info.before_frag.length = info.char_off;
                            info.before_frag.deletions = f.deletions;
                            info.before_frag.visible = f.visible;
                        }

                        if (info.char_off + info.to_del < f.length) {
                            // "after" fragment: chars [char_off + to_del, length)
                            info.needs_split_after = true;
                            uint32_t after_char_start = info.char_off + info.to_del;
                            info.after_frag.origin = Lamport(
                                f.origin.replica_id,
                                f.origin.value + after_char_start);
                            info.after_frag.locator = f.locator;
                            info.after_frag.text = f.text.substr(byte_off_end);
                            info.after_frag.byte_length = static_cast<uint32_t>(
                                info.after_frag.text.size());
                            info.after_frag.length = f.length - after_char_start;
                            info.after_frag.deletions = f.deletions;
                            info.after_frag.visible = f.visible;
                        }

                        // Mutate f in place to become the deleted middle part
                        f.origin = Lamport(f.origin.replica_id,
                                           f.origin.value + info.char_off);
                        f.text = f.text.substr(byte_off_start,
                                               byte_off_end - byte_off_start);
                        f.byte_length = static_cast<uint32_t>(f.text.size());
                        f.length = info.to_del;
                        f.deletions.push_back(op.deletion_id);
                    }

                    next_val += info.to_del;
                    remaining -= info.to_del;
                });

            if (!found || !info.found) return false;

            // Insert split-off fragments outside of edit_item
            if (info.needs_split_before) {
                FragmentOrderDim before_target{
                    info.before_frag.locator, info.before_frag.origin};
                m_fragment_tree.insert_item<FragmentOrderDim>(
                    before_target, std::move(info.before_frag));
                m_origin_index[info.before_frag.origin.replica_id]
                              [info.before_frag.origin.value] =
                    info.before_frag.locator;
            }
            if (info.needs_split_after) {
                FragmentOrderDim after_target{
                    info.after_frag.locator, info.after_frag.origin};
                m_fragment_tree.insert_item<FragmentOrderDim>(
                    after_target, std::move(info.after_frag));
                m_origin_index[info.after_frag.origin.replica_id]
                              [info.after_frag.origin.value] =
                    info.after_frag.locator;
            }
        }
    }

    // Apply insertions
    for (auto &ins : op.inserted_fragments) {
        Fragment frag(ins.origin, ins.locator,
                      static_cast<uint32_t>(ins.content.size()), ins.length,
                      ins.content);
        frag.visible = true;
        FragmentOrderDim target{ins.locator, ins.origin};
        m_fragment_tree.insert_item<FragmentOrderDim>(target, std::move(frag));
        m_origin_index[ins.origin.replica_id][ins.origin.value] = ins.locator;
    }

    m_clock.observe(op.timestamp);
    m_version.observe(op.timestamp);
    m_clock.observe(op.deletion_id);
    m_version.observe(op.deletion_id);
    for (auto &ins : op.inserted_fragments) {
        Lamport last_ts(ins.origin.replica_id, ins.origin.value + ins.length - 1);
        m_clock.observe(last_ts);
        m_version.observe(last_ts);
    }
    m_version.join(op.version);
    return true;
}

bool Buffer::apply_remote_edit(const EditOperation &op) {
    if (m_version.observed(op.timestamp))
        return true;

    if (!m_version.observed_all(op.version))
        return false;

    // Try fast path
    if (apply_remote_edit_fast(op))
        return true;

    // Full path (existing code)
    auto frags = get_fragments();

    // Apply deletion runs
    apply_deletion_runs(frags, op.deletion_runs, op.deletion_id);

    // Apply split relocations.
    for (auto &reloc : op.split_relocations) {
        Lamport target_ts(reloc.fragment_origin.replica_id,
                          reloc.fragment_origin.value + reloc.split_offset);

        for (size_t fi = 0; fi < frags.size(); ++fi) {
            auto &f = frags[fi];
            if (f.origin.replica_id != target_ts.replica_id) continue;
            if (target_ts.value < f.origin.value ||
                target_ts.value >= f.origin.value + f.length) continue;

            uint32_t char_off = target_ts.value - f.origin.value;
            if (char_off > 0) {
                uint32_t byte_off = char_to_byte_offset(f.text, char_off);
                fi = split_fragment_at(frags, fi, byte_off);
            }

            uint32_t end_ts = reloc.fragment_origin.value + reloc.fragment_length;
            uint32_t next_expected = target_ts.value;
            struct RelocEntry { size_t idx; Locator effective; };
            std::vector<RelocEntry> to_relocate;
            for (size_t si = 0; si < frags.size(); ++si) {
                if (next_expected >= end_ts) break;
                auto &sf = frags[si];
                if (sf.origin.replica_id != target_ts.replica_id) continue;
                if (sf.origin.value != next_expected) continue;
                Locator eff = (reloc.new_locator > sf.locator)
                              ? reloc.new_locator : sf.locator;
                if (eff != sf.locator) {
                    to_relocate.push_back({si, eff});
                }
                next_expected = sf.origin.value + sf.length;
            }

            for (auto it = to_relocate.rbegin(); it != to_relocate.rend(); ++it) {
                Fragment moved = std::move(frags[it->idx]);
                frags.erase(frags.begin() + static_cast<ptrdiff_t>(it->idx));
                moved.locator = it->effective;
                insert_fragment(frags, std::move(moved));
            }
            break;
        }
    }

    // Apply insertions
    for (auto &ins : op.inserted_fragments) {
        Fragment frag(ins.origin, ins.locator,
                      static_cast<uint32_t>(ins.content.size()), ins.length,
                      ins.content);
        insert_fragment(frags, std::move(frag));
    }

    // Update clock and version
    m_clock.observe(op.timestamp);
    m_version.observe(op.timestamp);
    m_clock.observe(op.deletion_id);
    m_version.observe(op.deletion_id);

    for (auto &ins : op.inserted_fragments) {
        Lamport last_ts(ins.origin.replica_id, ins.origin.value + ins.length - 1);
        m_clock.observe(last_ts);
        m_version.observe(last_ts);
    }

    m_version.join(op.version);

    normalize_fragments(frags);
    set_fragments(std::move(frags));
    return true;
}

// ---------------------------------------------------------------------------
// Remote undo application
// ---------------------------------------------------------------------------

bool Buffer::apply_remote_undo(const UndoOperation &op) {
    if (m_version.observed(op.timestamp))
        return true;

    if (!m_version.observed_all(op.version))
        return false;

    for (auto &[edit_id, count] : op.counts) {
        m_undo_map.insert(UndoMapEntry{{edit_id, op.timestamp}, count});
    }

    m_clock.observe(op.timestamp);
    m_version.observe(op.timestamp);
    m_version.join(op.version);

    m_fragment_tree.for_each_mut([this](Fragment& f) {
        f.visible = f.compute_visible(m_undo_map);
    });
    return true;
}

// ---------------------------------------------------------------------------
// apply_ops
// ---------------------------------------------------------------------------

bool Buffer::try_apply(const Operation& op) {
    return std::visit([this](const auto &o) -> bool {
        using T = std::decay_t<decltype(o)>;
        if constexpr (std::is_same_v<T, EditOperation>) {
            return apply_remote_edit(o);
        } else {
            return apply_remote_undo(o);
        }
    }, op);
}

void Buffer::apply_ops(const std::vector<Operation> &ops) {
    for (auto &op : ops) {
        Lamport ts = get_op_timestamp(op);
        uint16_t replica = ts.replica_id;

        if (m_deferred_replicas.count(replica)) {
            // This replica already has a deferred op — defer this one too.
            // Since ops from the same replica are causally ordered, if an
            // earlier op couldn't apply, this one can't either.
            enqueue_deferred({ts, op});
            continue;
        }

        bool applied = try_apply(op);
        if (!applied) {
            m_deferred_replicas.insert(replica);
            enqueue_deferred({ts, op});
        }
    }
    retry_deferred();
}

void Buffer::retry_deferred() {
    bool progress = true;
    while (progress) {
        progress = false;
        m_deferred_replicas.clear();

        OperationQueue remaining;
        m_deferred_queue.for_each([&](const OperationEntry& entry) {
            if (m_deferred_replicas.count(entry.timestamp.replica_id)) {
                remaining.push_item(entry);
                return;
            }
            bool applied = try_apply(entry.op);
            if (applied) {
                progress = true;
            } else {
                m_deferred_replicas.insert(entry.timestamp.replica_id);
                remaining.push_item(entry);
            }
        });
        m_deferred_queue = std::move(remaining);
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

std::optional<Operation> Buffer::undo() {
    if (m_undo_cursor == 0)
        return std::nullopt;

    m_undo_cursor--;
    auto &entry = m_undo_stack[m_undo_cursor];

    UndoOperation op;
    op.version = m_version;
    op.timestamp = m_clock.tick();

    for (auto &key : entry.inserted_keys) {
        Lamport edit_id(key.replica_id, key.lamport_value);
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

    UndoOperation op;
    op.version = m_version;
    op.timestamp = m_clock.tick();

    for (auto &key : entry.inserted_keys) {
        Lamport edit_id(key.replica_id, key.lamport_value);
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
