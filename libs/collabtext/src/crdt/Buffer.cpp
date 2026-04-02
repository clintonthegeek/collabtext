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
    second.deleted = orig.deleted;

    orig.content = orig.content.substr(0, offset_in_frag);
    orig.length = char_count;

    m_fragments.insert(m_fragments.begin() + frag_idx + 1, std::move(second));
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
                split_fragment_at(start_frag, start_off);
                start_frag++;
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
                    m_fragments[fi].deleted = true;
                    for (uint32_t c = 0; c < m_fragments[fi].length; ++c) {
                        Lamport ts = m_fragments[fi].timestamp_at(c);
                        op.deleted_timestamps.push_back(ts);
                        undo_entry.deleted_keys.push_back(UndoMapKey(ts));
                    }
                    remaining -= frag_bytes;
                    fi++;
                } else {
                    split_fragment_at(fi, remaining);
                    m_fragments[fi].deleted = true;
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
                // Split: first half keeps original locator
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
                m_fragments[second_idx].locator = second_loc;

                // Now insert before the second half
                ins_frag = second_idx;
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

    // Apply deletions by matching timestamps
    for (auto &ts : op.deleted_timestamps) {
        for (size_t fi = 0; fi < m_fragments.size(); ++fi) {
            auto &f = m_fragments[fi];
            if (f.deleted) continue;
            if (f.origin.replica_id != ts.replica_id) continue;
            if (ts.value < f.origin.value || ts.value >= f.origin.value + f.length)
                continue;

            if (f.length == 1) {
                f.deleted = true;
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
                m_fragments[fi].deleted = true;
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

    for (auto &key : op.keys) {
        if (op.is_redo) {
            m_undo_map.redo(key);
        } else {
            m_undo_map.undo(key);
        }
    }

    m_clock.observe(op.timestamp);
    m_version.observe(op.timestamp);
    m_version.join(op.version);

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
        op.keys.push_back(key);
    }

    // Undo deleted characters (un-delete fragments)
    for (auto &key : entry.deleted_keys) {
        for (auto &f : m_fragments) {
            if (f.origin.replica_id == key.replica_id &&
                key.lamport_value >= f.origin.value &&
                key.lamport_value < f.origin.value + f.length &&
                f.deleted) {
                f.deleted = false;
                break;
            }
        }
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
        op.keys.push_back(key);
    }

    // Redo: re-delete deleted characters
    for (auto &key : entry.deleted_keys) {
        for (auto &f : m_fragments) {
            if (f.origin.replica_id == key.replica_id &&
                key.lamport_value >= f.origin.value &&
                key.lamport_value < f.origin.value + f.length &&
                !f.deleted) {
                f.deleted = true;
                break;
            }
        }
    }

    op.timestamp = m_clock.tick();
    m_version.observe(op.timestamp);

    return op;
}

} // namespace CollabText::Crdt
