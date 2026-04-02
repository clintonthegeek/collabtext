#include "crdt/Buffer.h"
#include "crdt/OperationQueue.h"
#include <algorithm>
#include <cassert>
#include <numeric>

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

void Buffer::set_fragments(std::vector<Fragment>&& frags) {
    // Set cached visibility flags based on current undo map
    for (auto& f : frags) {
        f.visible = f.compute_visible(m_undo_map);
    }
    rebuild_insertion_index(frags);
    FragmentTree tree;
    for (auto& f : frags) tree.push_item(std::move(f));
    m_fragment_tree = std::move(tree);
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
            static_cast<uint32_t>(f.content.size())
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
    m_fragment_tree.for_each([&](const Fragment& f) {
        if (f.visible)
            result += f.content;
    });
    return result;
}

uint32_t Buffer::visible_length() const {
    return m_fragment_tree.summary().visible_bytes;
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

        uint32_t frag_bytes = static_cast<uint32_t>(f.content.size());
        if (accumulated + frag_bytes > byte_offset) {
            // Target is within this fragment
            uint32_t offset_in_frag = byte_offset - accumulated;
            uint32_t char_offset = count_chars(f.content, offset_in_frag);
            result = Anchor(f.origin.replica_id, f.origin.value + char_offset, bias);
            found = true;
            return;
        }
        accumulated += frag_bytes;
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

        // Check if this fragment contains the target character
        if (f.origin.replica_id == anchor.replica_id &&
            anchor.char_value >= f.origin.value &&
            anchor.char_value < f.origin.value + f.length) {

            if (f.visible) {
                uint32_t char_offset = anchor.char_value - f.origin.value;
                uint32_t byte_offset = chars_to_bytes(f.content, char_offset);
                result = accumulated + byte_offset;
            } else {
                // Fragment is invisible — resolve to its position
                result = accumulated;
            }
            found = true;
            return;
        }

        if (f.visible) {
            accumulated += static_cast<uint32_t>(f.content.size());
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
        uint32_t frag_bytes = static_cast<uint32_t>(frags[i].content.size());
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
    assert(offset_in_frag < frags[frag_idx].content.size());

    Fragment &orig = frags[frag_idx];
    uint32_t char_count = count_utf8_chars(orig.content, offset_in_frag);

    Fragment second;
    second.origin = Lamport(orig.origin.replica_id, orig.origin.value + char_count);
    second.locator = orig.locator;
    second.content = orig.content.substr(offset_in_frag);
    second.length = orig.length - char_count;
    second.delete_count = orig.delete_count;

    orig.content = orig.content.substr(0, offset_in_frag);
    orig.length = char_count;

    Lamport saved_origin = orig.origin;
    Locator saved_locator = orig.locator;

    Lamport second_origin(saved_origin.replica_id, saved_origin.value + char_count);
    insert_fragment(frags, std::move(second));

    for (size_t i = frag_idx + 1; i < frags.size(); ++i) {
        if (frags[i].origin == second_origin &&
            frags[i].locator == saved_locator) {
            return i;
        }
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

void Buffer::insert_fragment_into_tree(Fragment frag) {
    // Set visibility before inserting
    frag.visible = frag.compute_visible(m_undo_map);

    if (m_fragment_tree.empty()) {
        m_fragment_tree.push_item(std::move(frag));
        return;
    }

    // Use FragmentOrderDim to find the insertion point in O(log n)
    FragmentOrderDim target{frag.locator, frag.origin};

    auto cursor = m_fragment_tree.cursor<FragmentOrderDim>();
    cursor.seek(FragmentOrderDim::zero(), Bias::Left);

    FragmentTree new_tree;
    new_tree.push_tree(cursor.slice(target));
    new_tree.push_item(std::move(frag));
    new_tree.push_tree(cursor.suffix());
    m_fragment_tree = std::move(new_tree);
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
                    uint32_t byte_pos = 0;
                    for (uint32_t c = 0; c < f.length; ++c) {
                        uint32_t char_bytes = 1;
                        unsigned char ch = static_cast<unsigned char>(f.content[byte_pos]);
                        if (ch >= 0xF0) char_bytes = 4;
                        else if (ch >= 0xE0) char_bytes = 3;
                        else if (ch >= 0xC0) char_bytes = 2;

                        Fragment single;
                        single.origin = Lamport(f.origin.replica_id, f.origin.value + c);
                        single.locator = f.locator;
                        single.content = f.content.substr(byte_pos, char_bytes);
                        single.length = 1;
                        single.delete_count = f.delete_count;
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

    auto frags = get_fragments();

    EditOperation op;
    op.ranges = ranges;
    op.new_text = new_text;
    op.version = m_version;

    UndoEntry undo_entry;

    // Process ranges from right to left so that byte offsets remain valid.
    std::vector<size_t> order(ranges.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return ranges[a].first > ranges[b].first;
    });

    for (size_t idx : order) {
        uint32_t start = ranges[idx].first;
        uint32_t end = ranges[idx].second;
        const std::string &replacement = new_text[idx];

        // --- Delete phase: mark [start, end) as deleted ---
        if (end > start) {
            auto [start_frag, start_off] = resolve_visible_offset(frags, start);

            if (start_frag < frags.size() && start_off > 0) {
                start_frag = split_fragment_at(frags, start_frag, start_off);
            }

            uint32_t remaining = end - start;
            size_t fi = start_frag;
            while (remaining > 0 && fi < frags.size()) {
                if (!frags[fi].is_visible(m_undo_map)) {
                    fi++;
                    continue;
                }

                uint32_t frag_bytes = static_cast<uint32_t>(frags[fi].content.size());

                if (frag_bytes <= remaining) {
                    frags[fi].delete_count++;
                    for (uint32_t c = 0; c < frags[fi].length; ++c) {
                        Lamport ts = frags[fi].timestamp_at(c);
                        op.deleted_timestamps.push_back(ts);
                        undo_entry.deleted_keys.push_back(UndoMapKey(ts));
                    }
                    remaining -= frag_bytes;
                    fi++;
                } else {
                    split_fragment_at(frags, fi, remaining);
                    frags[fi].delete_count++;
                    for (uint32_t c = 0; c < frags[fi].length; ++c) {
                        Lamport ts = frags[fi].timestamp_at(c);
                        op.deleted_timestamps.push_back(ts);
                        undo_entry.deleted_keys.push_back(UndoMapKey(ts));
                    }
                    remaining = 0;
                }
            }
        }

        // --- Insert phase ---
        if (!replacement.empty()) {
            auto [ins_frag, ins_off] = resolve_visible_offset(frags, start);

            if (ins_frag < frags.size() && ins_off > 0) {
                Lamport split_origin = frags[ins_frag].origin;
                uint32_t split_frag_length = frags[ins_frag].length;
                uint32_t split_char_offset = count_utf8_chars(
                    frags[ins_frag].content, ins_off);

                size_t second_idx = split_fragment_at(frags, ins_frag, ins_off);

                Locator orig_loc = frags[ins_frag].locator;
                Locator next_hi = Locator::max();
                for (size_t i = second_idx + 1; i < frags.size(); ++i) {
                    if (frags[i].locator > orig_loc) {
                        next_hi = frags[i].locator;
                        break;
                    }
                }
                Locator second_loc = Locator::between(orig_loc, next_hi);

                Lamport second_origin = frags[second_idx].origin;
                Fragment moved = std::move(frags[second_idx]);
                frags.erase(frags.begin() + static_cast<ptrdiff_t>(second_idx));
                moved.locator = second_loc;
                insert_fragment(frags, std::move(moved));

                size_t new_second_idx = 0;
                for (size_t i = 0; i < frags.size(); ++i) {
                    if (frags[i].origin == second_origin &&
                        frags[i].locator == second_loc) {
                        new_second_idx = i;
                        break;
                    }
                }

                EditOperation::SplitRelocation reloc;
                reloc.fragment_origin = split_origin;
                reloc.split_offset = split_char_offset;
                reloc.fragment_length = split_frag_length;
                reloc.new_locator = second_loc;
                op.split_relocations.push_back(std::move(reloc));

                ins_frag = new_second_idx;
            }

            Locator new_loc = locator_between(frags, ins_frag);

            uint32_t char_count = count_utf8_chars(replacement, static_cast<uint32_t>(replacement.size()));

            Lamport frag_origin = m_clock.tick();
            for (uint32_t c = 1; c < char_count; ++c) {
                m_clock.tick();
            }

            Fragment frag(frag_origin, new_loc, replacement, char_count);
            insert_fragment(frags, frag);

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
    }

    // Every edit operation gets its own unique timestamp.
    if (op.inserted_fragments.empty()) {
        op.timestamp = m_clock.tick();
    } else {
        op.timestamp = Lamport(m_replica_id, m_clock.value - 1);
    }

    // Update version vector
    m_version.observe(op.timestamp);

    // Push undo entry
    m_undo_stack.resize(m_undo_cursor);
    m_undo_stack.push_back(std::move(undo_entry));
    m_undo_cursor = m_undo_stack.size();

    normalize_fragments(frags);
    set_fragments(std::move(frags));
    return op;
}

// ---------------------------------------------------------------------------
// Remote edit application
// ---------------------------------------------------------------------------

bool Buffer::apply_remote_edit(const EditOperation &op) {
    if (m_version.observed(op.timestamp))
        return true;

    if (!m_version.observed_all(op.version))
        return false;

    auto frags = get_fragments();

    // Apply deletions by matching timestamps.
    for (auto &ts : op.deleted_timestamps) {
        for (size_t fi = 0; fi < frags.size(); ++fi) {
            auto &f = frags[fi];
            if (f.origin.replica_id != ts.replica_id) continue;
            if (ts.value < f.origin.value || ts.value >= f.origin.value + f.length)
                continue;

            if (f.length == 1) {
                f.delete_count++;
            } else {
                uint32_t offset = ts.value - f.origin.value;
                uint32_t byte_off = char_to_byte_offset(frags[fi].content, offset);

                if (offset > 0) {
                    fi = split_fragment_at(frags, fi, byte_off);
                }

                if (frags[fi].length > 1) {
                    uint32_t cb = first_char_bytes(frags[fi].content);
                    split_fragment_at(frags, fi, cb);
                }
                frags[fi].delete_count++;
            }
            break;
        }
    }

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
                uint32_t byte_off = char_to_byte_offset(f.content, char_off);
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
        Fragment frag(ins.origin, ins.locator, ins.content, ins.length);
        insert_fragment(frags, frag);
    }

    // Update clock and version
    m_clock.observe(op.timestamp);
    m_version.observe(op.timestamp);

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

    // Handle undo_keys (hide/show via undo map)
    for (auto &key : op.undo_keys) {
        if (op.is_redo) {
            m_undo_map.redo(key);
        } else {
            m_undo_map.undo(key);
        }
    }

    // Handle undelete_keys (adjust delete counter)
    auto frags = get_fragments();
    for (auto &key : op.undelete_keys) {
        for (auto &f : frags) {
            if (f.origin.replica_id == key.replica_id &&
                key.lamport_value >= f.origin.value &&
                key.lamport_value < f.origin.value + f.length) {
                if (op.is_redo)
                    f.delete_count++;   // re-delete
                else if (f.delete_count > 0)
                    f.delete_count--;   // un-delete
                break;
            }
        }
    }

    m_clock.observe(op.timestamp);
    m_version.observe(op.timestamp);
    m_version.join(op.version);

    normalize_fragments(frags);
    set_fragments(std::move(frags));
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

std::optional<Operation> Buffer::undo() {
    if (m_undo_cursor == 0)
        return std::nullopt;

    m_undo_cursor--;
    auto &entry = m_undo_stack[m_undo_cursor];

    UndoOperation op;
    op.version = m_version;
    op.is_redo = false;

    // Undo inserted characters (hide them via undo map)
    for (auto &key : entry.inserted_keys) {
        m_undo_map.undo(key);
        op.undo_keys.push_back(key);
    }

    // Undo deleted characters (decrement delete counter)
    auto frags = get_fragments();
    for (auto &key : entry.deleted_keys) {
        for (auto &f : frags) {
            if (f.origin.replica_id == key.replica_id &&
                key.lamport_value >= f.origin.value &&
                key.lamport_value < f.origin.value + f.length &&
                f.delete_count > 0) {
                f.delete_count--;
                break;
            }
        }
        op.undelete_keys.push_back(key);
    }
    set_fragments(std::move(frags));

    op.timestamp = m_clock.tick();
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
    op.is_redo = true;

    // Redo: re-hide inserted characters
    for (auto &key : entry.inserted_keys) {
        m_undo_map.redo(key);
        op.undo_keys.push_back(key);
    }

    // Redo: re-delete deleted characters (increment delete counter)
    auto frags = get_fragments();
    for (auto &key : entry.deleted_keys) {
        for (auto &f : frags) {
            if (f.origin.replica_id == key.replica_id &&
                key.lamport_value >= f.origin.value &&
                key.lamport_value < f.origin.value + f.length) {
                f.delete_count++;
                break;
            }
        }
        op.undelete_keys.push_back(key);
    }
    set_fragments(std::move(frags));

    op.timestamp = m_clock.tick();
    m_version.observe(op.timestamp);

    return op;
}

} // namespace CollabText::Crdt
