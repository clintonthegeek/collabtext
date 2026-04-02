#include "crdt/Buffer.h"
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
// Queries
// ---------------------------------------------------------------------------

std::string Buffer::text() const {
    std::string result;
    for (auto &f : m_fragments) {
        if (f.is_visible(m_undo_map))
            result += f.content;
    }
    return result;
}

uint32_t Buffer::visible_length() const {
    uint32_t len = 0;
    for (auto &f : m_fragments) {
        if (f.is_visible(m_undo_map))
            len += static_cast<uint32_t>(f.content.size());
    }
    return len;
}

const Global &Buffer::version() const {
    return m_version;
}

uint16_t Buffer::replica_id() const {
    return m_replica_id;
}

const std::vector<Fragment> &Buffer::fragments() const {
    return m_fragments;
}

// ---------------------------------------------------------------------------
// Fragment management helpers
// ---------------------------------------------------------------------------

/// Get the byte offset of the `char_offset`-th character in `s`.
static uint32_t char_to_byte_offset(const std::string &s, uint32_t char_offset);

void Buffer::insert_fragment(Fragment frag) {
    // Find insertion point: sorted by (locator, then origin timestamp)
    auto it = std::lower_bound(m_fragments.begin(), m_fragments.end(), frag,
        [](const Fragment &a, const Fragment &b) {
            auto cmp = a.locator <=> b.locator;
            if (cmp != 0) return cmp < 0;
            return a.origin < b.origin;
        });
    m_fragments.insert(it, std::move(frag));
}

/// Ensure that at every locator where multiple replicas have fragments,
/// all fragments are single-character (atomized) and correctly sorted.
/// This guarantees correct character-level interleaving regardless of
/// operation arrival order.
void Buffer::normalize_fragments() {
    size_t i = 0;
    while (i < m_fragments.size()) {
        size_t run_start = i;
        Locator loc = m_fragments[i].locator;

        // Find the end of the run at this locator
        size_t run_end = i + 1;
        while (run_end < m_fragments.size() && m_fragments[run_end].locator == loc)
            run_end++;

        // Check if multiple replica IDs are present
        bool multi_replica = false;
        uint16_t first_replica = m_fragments[run_start].origin.replica_id;
        for (size_t j = run_start + 1; j < run_end; ++j) {
            if (m_fragments[j].origin.replica_id != first_replica) {
                multi_replica = true;
                break;
            }
        }

        if (multi_replica) {
            // Extract all fragments at this locator, atomize, and re-insert
            // in sorted order.
            std::vector<Fragment> extracted;
            for (size_t j = run_start; j < run_end; ++j) {
                auto &f = m_fragments[j];
                if (f.length == 1) {
                    extracted.push_back(std::move(f));
                } else {
                    // Atomize: split into individual characters
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

            // Sort extracted fragments by (locator, origin)
            std::sort(extracted.begin(), extracted.end(),
                [](const Fragment &a, const Fragment &b) {
                    auto cmp = a.locator <=> b.locator;
                    if (cmp != 0) return cmp < 0;
                    return a.origin < b.origin;
                });

            // Erase the old run and insert the sorted atomized fragments
            m_fragments.erase(
                m_fragments.begin() + static_cast<ptrdiff_t>(run_start),
                m_fragments.begin() + static_cast<ptrdiff_t>(run_end));
            m_fragments.insert(
                m_fragments.begin() + static_cast<ptrdiff_t>(run_start),
                std::make_move_iterator(extracted.begin()),
                std::make_move_iterator(extracted.end()));

            i = run_start + extracted.size();
        } else {
            i = run_end;
        }
    }
}

std::pair<size_t, uint32_t> Buffer::resolve_visible_offset(uint32_t byte_offset) const {
    uint32_t accumulated = 0;
    for (size_t i = 0; i < m_fragments.size(); ++i) {
        if (!m_fragments[i].is_visible(m_undo_map))
            continue;
        uint32_t frag_bytes = static_cast<uint32_t>(m_fragments[i].content.size());
        if (accumulated + frag_bytes > byte_offset) {
            return {i, byte_offset - accumulated};
        }
        accumulated += frag_bytes;
    }
    return {m_fragments.size(), 0};
}

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

size_t Buffer::split_fragment_at(size_t frag_idx, uint32_t offset_in_frag) {
    assert(frag_idx < m_fragments.size());
    assert(offset_in_frag > 0);
    assert(offset_in_frag < m_fragments[frag_idx].content.size());

    Fragment &orig = m_fragments[frag_idx];
    uint32_t char_count = count_utf8_chars(orig.content, offset_in_frag);

    Fragment second;
    second.origin = Lamport(orig.origin.replica_id, orig.origin.value + char_count);
    second.locator = orig.locator;
    second.content = orig.content.substr(offset_in_frag);
    second.length = orig.length - char_count;
    second.delete_count = orig.delete_count;

    orig.content = orig.content.substr(0, offset_in_frag);
    orig.length = char_count;

    // Save the origin and locator BEFORE any mutation that might invalidate refs
    Lamport saved_origin = orig.origin;
    Locator saved_locator = orig.locator;

    // Insert the second half in sorted position. It often goes right after
    // the first half, but if other replicas' fragments are interleaved at
    // the same locator, it needs to be placed correctly.
    Lamport second_origin(saved_origin.replica_id, saved_origin.value + char_count);
    insert_fragment(std::move(second));

    // Find the second half's actual position so callers can reference it.
    // Search from frag_idx+1 onwards for the fragment we just inserted.
    for (size_t i = frag_idx + 1; i < m_fragments.size(); ++i) {
        if (m_fragments[i].origin == second_origin &&
            m_fragments[i].locator == saved_locator) {
            return i;
        }
    }

    assert(false && "split_fragment_at: could not find second half after insert");
    return frag_idx + 1;
}

Locator Buffer::locator_between(size_t ins_frag) const {
    // Find the locator of the predecessor (or Locator::min())
    Locator lo = Locator::min();
    if (ins_frag > 0) {
        lo = m_fragments[ins_frag - 1].locator;
    }

    // Find the locator of the first fragment at or after ins_frag with a
    // locator strictly greater than lo.
    Locator hi = Locator::max();
    for (size_t i = ins_frag; i < m_fragments.size(); ++i) {
        if (m_fragments[i].locator > lo) {
            hi = m_fragments[i].locator;
            break;
        }
    }

    assert(lo < hi);
    return Locator::between(lo, hi);
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
            auto [start_frag, start_off] = resolve_visible_offset(start);

            if (start_frag < m_fragments.size() && start_off > 0) {
                start_frag = split_fragment_at(start_frag, start_off);
            }

            uint32_t remaining = end - start;
            size_t fi = start_frag;
            while (remaining > 0 && fi < m_fragments.size()) {
                if (!m_fragments[fi].is_visible(m_undo_map)) {
                    fi++;
                    continue;
                }

                uint32_t frag_bytes = static_cast<uint32_t>(m_fragments[fi].content.size());

                if (frag_bytes <= remaining) {
                    m_fragments[fi].delete_count++;
                    for (uint32_t c = 0; c < m_fragments[fi].length; ++c) {
                        Lamport ts = m_fragments[fi].timestamp_at(c);
                        op.deleted_timestamps.push_back(ts);
                        undo_entry.deleted_keys.push_back(UndoMapKey(ts));
                    }
                    remaining -= frag_bytes;
                    fi++;
                } else {
                    split_fragment_at(fi, remaining);
                    m_fragments[fi].delete_count++;
                    for (uint32_t c = 0; c < m_fragments[fi].length; ++c) {
                        Lamport ts = m_fragments[fi].timestamp_at(c);
                        op.deleted_timestamps.push_back(ts);
                        undo_entry.deleted_keys.push_back(UndoMapKey(ts));
                    }
                    remaining = 0;
                }
            }
        }

        // --- Insert phase ---
        if (!replacement.empty()) {
            auto [ins_frag, ins_off] = resolve_visible_offset(start);

            // If inserting in the middle of a fragment, split it and give
            // the second half a new locator so we can place text between.
            if (ins_frag < m_fragments.size() && ins_off > 0) {
                // Record info for the split relocation before splitting
                Lamport split_origin = m_fragments[ins_frag].origin;
                uint32_t split_frag_length = m_fragments[ins_frag].length;
                uint32_t split_char_offset = count_utf8_chars(
                    m_fragments[ins_frag].content, ins_off);

                // Split: first half keeps original locator. The second half
                // gets a new locator so we can place the new fragment between.
                size_t second_idx = split_fragment_at(ins_frag, ins_off);

                // Give the second half a new locator strictly between the
                // original and the next distinct greater locator.
                Locator orig_loc = m_fragments[ins_frag].locator;
                Locator next_hi = Locator::max();
                for (size_t i = second_idx + 1; i < m_fragments.size(); ++i) {
                    if (m_fragments[i].locator > orig_loc) {
                        next_hi = m_fragments[i].locator;
                        break;
                    }
                }
                Locator second_loc = Locator::between(orig_loc, next_hi);

                // Extract the second half, change its locator, and re-insert
                // in sorted position to maintain the fragment list invariant.
                Lamport second_origin = m_fragments[second_idx].origin;
                Fragment moved = std::move(m_fragments[second_idx]);
                m_fragments.erase(m_fragments.begin() + static_cast<ptrdiff_t>(second_idx));
                moved.locator = second_loc;
                insert_fragment(std::move(moved));

                // Find the re-inserted fragment's new position
                size_t new_second_idx = 0;
                for (size_t i = 0; i < m_fragments.size(); ++i) {
                    if (m_fragments[i].origin == second_origin &&
                        m_fragments[i].locator == second_loc) {
                        new_second_idx = i;
                        break;
                    }
                }

                // Record the split relocation so remotes can apply it.
                // fragment_length is the length of the fragment BEFORE
                // the split (both halves combined), so that remotes know
                // exactly which characters belong to the second half.
                EditOperation::SplitRelocation reloc;
                reloc.fragment_origin = split_origin;
                reloc.split_offset = split_char_offset;
                reloc.fragment_length = split_frag_length;
                reloc.new_locator = second_loc;
                op.split_relocations.push_back(std::move(reloc));

                // Now insert before the second half
                ins_frag = new_second_idx;
            }

            // Compute locator for the new fragment
            Locator new_loc = locator_between(ins_frag);

            // Count UTF-8 characters
            uint32_t char_count = count_utf8_chars(replacement, static_cast<uint32_t>(replacement.size()));

            // Tick the clock for each character
            Lamport frag_origin = m_clock.tick();
            for (uint32_t c = 1; c < char_count; ++c) {
                m_clock.tick();
            }

            Fragment frag(frag_origin, new_loc, replacement, char_count);
            insert_fragment(frag);

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

    // Every edit operation gets its own unique timestamp. If no characters
    // were inserted (delete-only edit), we still tick once.
    if (op.inserted_fragments.empty()) {
        op.timestamp = m_clock.tick();
    } else {
        // The timestamp was already advanced by character insertions.
        // Use the last value consumed.
        op.timestamp = Lamport(m_replica_id, m_clock.value - 1);
    }

    // Update version vector
    m_version.observe(op.timestamp);

    // Push undo entry
    m_undo_stack.resize(m_undo_cursor);
    m_undo_stack.push_back(std::move(undo_entry));
    m_undo_cursor = m_undo_stack.size();

    normalize_fragments();
    return op;
}

// ---------------------------------------------------------------------------
// Remote edit application
// ---------------------------------------------------------------------------

bool Buffer::apply_remote_edit(const EditOperation &op) {
    // Deduplication
    if (m_version.observed(op.timestamp))
        return true;

    // Causal ordering
    if (!m_version.observed_all(op.version))
        return false;

    // Apply deletions by matching timestamps. Each character gets its
    // delete_count incremented; this correctly handles concurrent deletes
    // from multiple replicas (counter > 1 means multiple delete votes).
    for (auto &ts : op.deleted_timestamps) {
        for (size_t fi = 0; fi < m_fragments.size(); ++fi) {
            auto &f = m_fragments[fi];
            if (f.origin.replica_id != ts.replica_id) continue;
            if (ts.value < f.origin.value || ts.value >= f.origin.value + f.length)
                continue;

            if (f.length == 1) {
                f.delete_count++;
            } else {
                uint32_t offset = ts.value - f.origin.value;
                uint32_t byte_off = char_to_byte_offset(m_fragments[fi].content, offset);

                if (offset > 0) {
                    fi = split_fragment_at(fi, byte_off);
                }

                if (m_fragments[fi].length > 1) {
                    uint32_t cb = first_char_bytes(m_fragments[fi].content);
                    split_fragment_at(fi, cb);
                }
                m_fragments[fi].delete_count++;
            }
            break;
        }
    }

    // Apply split relocations: when the sender split a fragment to insert
    // in the middle, we apply the same split and locator change here.
    //
    // The target character is identified by its globally unique Lamport
    // timestamp, which never changes regardless of how the fragment has
    // been split by concurrent operations.  When the original fragment
    // was concurrently split into multiple sub-fragments, ALL sub-fragments
    // from the split point onward must be relocated.
    //
    // Concurrent split relocations at the same point or overlapping
    // ranges: two replicas may split the same fragment at the same or
    // different offsets, assigning different new locators.  To ensure
    // convergence regardless of application order, each sub-fragment's
    // effective locator is max(current_locator, proposed_new_locator).
    // This ensures the same locator always wins.
    for (auto &reloc : op.split_relocations) {
        Lamport target_ts(reloc.fragment_origin.replica_id,
                          reloc.fragment_origin.value + reloc.split_offset);

        // Find the fragment containing the target character.
        for (size_t fi = 0; fi < m_fragments.size(); ++fi) {
            auto &f = m_fragments[fi];
            if (f.origin.replica_id != target_ts.replica_id) continue;
            if (target_ts.value < f.origin.value ||
                target_ts.value >= f.origin.value + f.length) continue;

            // Split if the target char is not at the fragment start.
            uint32_t char_off = target_ts.value - f.origin.value;
            if (char_off > 0) {
                uint32_t byte_off = char_to_byte_offset(f.content, char_off);
                fi = split_fragment_at(fi, byte_off);
            }

            // Collect sub-fragments from the split point onward that
            // were part of the original fragment.  The original fragment
            // covered timestamps [fragment_origin.value,
            //   fragment_origin.value + fragment_length).
            // Sub-fragments are identified by: same replica_id,
            // contiguous timestamps, within the original range.
            // Each sub-fragment's effective locator is
            // max(current, new_locator) to resolve concurrent splits.
            uint32_t end_ts = reloc.fragment_origin.value
                              + reloc.fragment_length;
            uint32_t next_expected = target_ts.value;
            struct RelocEntry { size_t idx; Locator effective; };
            std::vector<RelocEntry> to_relocate;
            for (size_t si = 0; si < m_fragments.size(); ++si) {
                if (next_expected >= end_ts) break;
                auto &sf = m_fragments[si];
                if (sf.origin.replica_id != target_ts.replica_id) continue;
                if (sf.origin.value != next_expected) continue;
                // Clamp: only include characters up to end_ts
                // (the fragment might extend beyond if it was merged,
                // though that shouldn't happen in practice).
                Locator eff = (reloc.new_locator > sf.locator)
                              ? reloc.new_locator : sf.locator;
                if (eff != sf.locator) {
                    to_relocate.push_back({si, eff});
                }
                next_expected = sf.origin.value + sf.length;
            }

            // Relocate in reverse index order to keep indices stable.
            for (auto it = to_relocate.rbegin(); it != to_relocate.rend(); ++it) {
                Fragment moved = std::move(m_fragments[it->idx]);
                m_fragments.erase(m_fragments.begin() + static_cast<ptrdiff_t>(it->idx));
                moved.locator = it->effective;
                insert_fragment(std::move(moved));
            }
            break;
        }
    }

    // Apply insertions
    for (auto &ins : op.inserted_fragments) {
        Fragment frag(ins.origin, ins.locator, ins.content, ins.length);
        insert_fragment(frag);
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

    normalize_fragments();
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
    for (auto &key : op.undelete_keys) {
        for (auto &f : m_fragments) {
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

    normalize_fragments();
    return true;
}

// ---------------------------------------------------------------------------
// apply_ops
// ---------------------------------------------------------------------------

void Buffer::apply_ops(const std::vector<Operation> &ops) {
    for (auto &op : ops) {
        bool applied = std::visit([this](auto &o) -> bool {
            using T = std::decay_t<decltype(o)>;
            if constexpr (std::is_same_v<T, EditOperation>) {
                return apply_remote_edit(o);
            } else {
                return apply_remote_undo(o);
            }
        }, op);

        if (!applied) {
            m_deferred.push_back(op);
        }
    }

    retry_deferred();
}

void Buffer::retry_deferred() {
    bool progress = true;
    while (progress) {
        progress = false;
        for (auto it = m_deferred.begin(); it != m_deferred.end(); ) {
            bool applied = std::visit([this](auto &o) -> bool {
                using T = std::decay_t<decltype(o)>;
                if constexpr (std::is_same_v<T, EditOperation>) {
                    return apply_remote_edit(o);
                } else {
                    return apply_remote_undo(o);
                }
            }, *it);

            if (applied) {
                it = m_deferred.erase(it);
                progress = true;
            } else {
                ++it;
            }
        }
    }
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
    for (auto &key : entry.deleted_keys) {
        for (auto &f : m_fragments) {
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
    for (auto &key : entry.deleted_keys) {
        for (auto &f : m_fragments) {
            if (f.origin.replica_id == key.replica_id &&
                key.lamport_value >= f.origin.value &&
                key.lamport_value < f.origin.value + f.length) {
                f.delete_count++;
                break;
            }
        }
        op.undelete_keys.push_back(key);
    }

    op.timestamp = m_clock.tick();
    m_version.observe(op.timestamp);

    return op;
}

} // namespace CollabText::Crdt
