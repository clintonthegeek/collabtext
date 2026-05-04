#include "crdt/IdList.h"
#include <cassert>

namespace CollabText::Crdt {

IdList::IdList(uint16_t replica_id)
    : m_replica_id(replica_id)
    , m_clock(replica_id, 1)
{}

IdListOperation IdList::insert_after(const Anchor& after, uint64_t id) {
    // a. Get entries vector
    auto entries = get_entries();

    // b. Find predecessor and successor locators
    Locator predecessor;
    Locator successor;

    if (after.is_min()) {
        predecessor = Locator::min();
        if (entries.empty()) {
            successor = Locator::max();
        } else {
            successor = entries.front().locator;
        }
    } else {
        // Find the entry matching (after.replica_id, after.char_value)
        bool found = false;
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].origin.replica_id == after.replica_id &&
                entries[i].origin.value == after.char_value) {
                predecessor = entries[i].locator;
                successor = (i + 1 < entries.size())
                    ? entries[i + 1].locator
                    : Locator::max();
                found = true;
                break;
            }
        }
        assert(found && "insert_after: anchor not found in entries");
    }

    // c. Allocate new locator between predecessor and successor
    Locator new_loc = Locator::between(predecessor, successor);

    // d. Capture origin before bumping clock
    Lamport origin(m_replica_id, m_clock.value);

    // e. Capture version before observing this op
    Global version_before = m_version;

    // f. Bump clock
    m_clock = Lamport(m_replica_id, m_clock.value + 1);

    // g. Record this op in the version vector
    m_version.observe(origin);

    // h. Create and insert the entry
    IdListEntry e(origin, new_loc, id);

    // i. Insert into sorted entries vector
    insert_entry(entries, std::move(e));

    // j. Rebuild the tree from entries
    set_entries(std::move(entries));

    // k. Push undo entry
    m_undo_stack.resize(m_undo_cursor);  // truncate redo tail
    m_undo_stack.push_back(UndoEntry{ {UndoMapKey(origin)}, {} });
    m_undo_cursor = m_undo_stack.size();
    trim_undo_stack();

    // l. Notify change
    if (m_on_change) m_on_change();

    // m. Return the operation
    return IdListInsertOp{ origin, version_before, id, new_loc };
}

IdListOperation IdList::remove_at(const Anchor& target) {
    // a. Get entries
    auto entries = get_entries();

    // b. Find entry where origin matches target
    size_t idx = entries.size();
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].origin.replica_id == target.replica_id &&
            entries[i].origin.value == target.char_value) {
            idx = i;
            break;
        }
    }
    assert(idx < entries.size() && "remove_at: target anchor not found in entries");

    // c. Capture target_origin before any moves
    Lamport target_origin = entries[idx].origin;

    // d. Allocate deletion Lamport from current clock value
    Lamport del_id(m_replica_id, m_clock.value);

    // e. Bump clock
    m_clock = Lamport(m_replica_id, m_clock.value + 1);

    // f. Capture version before observing this op
    Global version_before = m_version;

    // g. Observe the deletion op in version vector
    m_version.observe(del_id);

    // h. Tombstone the entry
    entries[idx].deletions.push_back(del_id);

    // i. Rebuild tree (recomputes visible for all entries)
    set_entries(std::move(entries));

    // j. Push undo entry: empty inserted_keys, del_id in deletion_ids
    m_undo_stack.resize(m_undo_cursor);
    m_undo_stack.push_back(UndoEntry{ {}, {del_id} });
    m_undo_cursor = m_undo_stack.size();
    trim_undo_stack();

    // k. Notify change
    if (m_on_change) m_on_change();

    // l. Return operation
    return IdListRemoveOp{ del_id, version_before, target_origin };
}

void IdList::apply_ops(const std::vector<IdListOperation>& ops) {
    for (const auto& op : ops) {
        if (m_version.observed(get_idlist_op_timestamp(op))) continue; // already applied
        if (!try_apply(op)) {
            enqueue_deferred(IdListOpEntry{get_idlist_op_timestamp(op), op});
        }
    }
    retry_deferred();
}

std::optional<IdListOperation> IdList::undo() {
    if (m_undo_cursor == 0) return std::nullopt;
    m_undo_cursor--;
    auto& entry = m_undo_stack[m_undo_cursor];

    IdListUndoOpVariant op;
    op.version = m_version;
    op.timestamp = m_clock.tick();

    for (auto& key : entry.inserted_keys) {
        Lamport edit_id(key.replica_id, key.lamport_value);
        uint32_t current = m_undo_map.undo_count(edit_id);
        m_undo_map.insert(UndoMapEntry{{edit_id, op.timestamp}, current + 1});
        op.counts.push_back({edit_id, current + 1});
    }
    for (auto& del_id : entry.deletion_ids) {
        uint32_t current = m_undo_map.undo_count(del_id);
        m_undo_map.insert(UndoMapEntry{{del_id, op.timestamp}, current + 1});
        op.counts.push_back({del_id, current + 1});
    }

    auto entries = get_entries();
    set_entries(std::move(entries));

    m_version.observe(op.timestamp);
    if (m_on_change) m_on_change();
    return op;
}

std::optional<IdListOperation> IdList::redo() {
    if (m_undo_cursor >= m_undo_stack.size()) return std::nullopt;
    auto& entry = m_undo_stack[m_undo_cursor];
    m_undo_cursor++;

    IdListUndoOpVariant op;
    op.version = m_version;
    op.timestamp = m_clock.tick();

    for (auto& key : entry.inserted_keys) {
        Lamport edit_id(key.replica_id, key.lamport_value);
        uint32_t current = m_undo_map.undo_count(edit_id);
        m_undo_map.insert(UndoMapEntry{{edit_id, op.timestamp}, current + 1});
        op.counts.push_back({edit_id, current + 1});
    }
    for (auto& del_id : entry.deletion_ids) {
        uint32_t current = m_undo_map.undo_count(del_id);
        m_undo_map.insert(UndoMapEntry{{del_id, op.timestamp}, current + 1});
        op.counts.push_back({del_id, current + 1});
    }

    auto entries = get_entries();
    set_entries(std::move(entries));

    m_version.observe(op.timestamp);
    if (m_on_change) m_on_change();
    return op;
}

bool IdList::coalesce_last_undo() {
    if (m_undo_cursor < 2) return false;
    auto& latest   = m_undo_stack[m_undo_cursor - 1];
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

void IdList::set_max_undo_depth(size_t depth) {
    m_max_undo_depth = depth;
    trim_undo_stack();
}

void IdList::trim_undo_stack() {
    if (m_undo_stack.size() <= m_max_undo_depth) return;
    size_t excess = m_undo_stack.size() - m_max_undo_depth;
    m_undo_stack.erase(m_undo_stack.begin(),
                       m_undo_stack.begin() + static_cast<ptrdiff_t>(excess));
    if (excess > m_undo_cursor) m_undo_cursor = 0;
    else m_undo_cursor -= excess;
}

std::vector<uint64_t> IdList::ids() const {
    std::vector<uint64_t> result;
    result.reserve(m_entry_tree.summary().visible_count);
    m_entry_tree.for_each([&](const IdListEntry& e) {
        if (e.visible) result.push_back(e.id);
    });
    return result;
}

uint32_t IdList::size() const {
    return m_entry_tree.summary().visible_count;
}

Anchor IdList::anchor_at_index(uint32_t index, Bias bias) const {
    uint32_t visible_seen = 0;
    Anchor result = Anchor::max();
    bool found = false;
    m_entry_tree.for_each([&](const IdListEntry& e) {
        if (found) return;
        if (!e.visible) return;
        if (visible_seen == index) {
            result = Anchor(e.origin.replica_id, e.origin.value, bias);
            found = true;
            return;
        }
        ++visible_seen;
    });
    return result;
}

Anchor IdList::anchor_of(uint64_t id, Bias bias) const {
    Anchor result = Anchor::max();
    bool found = false;
    m_entry_tree.for_each([&](const IdListEntry& e) {
        if (found) return;
        if (!e.visible) return;
        if (e.id == id) {
            result = Anchor(e.origin.replica_id, e.origin.value, bias);
            found = true;
        }
    });
    return result;
}

uint32_t IdList::resolve_anchor(const Anchor& a) const {
    if (a.is_min()) return 0;
    if (a.is_max()) return size();
    uint32_t accumulated = 0;
    uint32_t result = size();
    bool found = false;
    m_entry_tree.for_each([&](const IdListEntry& e) {
        if (found) return;
        if (e.origin.replica_id == a.replica_id &&
            e.origin.value == a.char_value) {
            result = accumulated;
            found = true;
            return;
        }
        if (e.visible) ++accumulated;
    });
    return result;
}

int IdList::compare_anchors(const Anchor& a, const Anchor& b) const {
    uint32_t pa = resolve_anchor(a);
    uint32_t pb = resolve_anchor(b);
    if (pa < pb) return -1;
    if (pa > pb) return 1;
    if (a.bias != b.bias)
        return (a.bias == Bias::Left) ? -1 : 1;
    return 0;
}

size_t IdList::collect_garbage() { return 0; }
size_t IdList::compact(const Global&) { return 0; }

std::vector<IdListEntry> IdList::entries() const {
    return m_entry_tree.items();
}

size_t IdList::tombstone_count() const {
    size_t n = 0;
    m_entry_tree.for_each([&](const IdListEntry& e) { if (!e.visible) ++n; });
    return n;
}

size_t IdList::entry_count() const {
    size_t n = 0;
    m_entry_tree.for_each([&](const IdListEntry&) { ++n; });
    return n;
}

std::vector<IdListEntry> IdList::get_entries() const {
    return m_entry_tree.items();
}

void IdList::set_entries(std::vector<IdListEntry>&& entries) {
    for (auto& e : entries)
        e.visible = e.compute_visible(m_undo_map);
    IdListTree tree;
    for (auto& e : entries) tree.push_item(std::move(e));
    m_entry_tree = std::move(tree);
}

void IdList::insert_entry(std::vector<IdListEntry>& entries, IdListEntry entry) const {
    auto it = std::lower_bound(entries.begin(), entries.end(), entry,
        [](const IdListEntry& a, const IdListEntry& b) {
            auto cmp = a.locator <=> b.locator;
            if (cmp != 0) return cmp < 0;
            return a.origin < b.origin;
        });
    entries.insert(it, std::move(entry));
}

bool IdList::try_apply(const IdListOperation& op) {
    // Check causal dependencies satisfied
    if (!m_version.observed_all(get_idlist_op_version(op))) return false;
    return std::visit([&](const auto& concrete) -> bool {
        return apply_concrete(concrete);
    }, op);
}

void IdList::retry_deferred() {
    bool progress = true;
    while (progress) {
        progress = false;
        IdListOperationQueue remaining;
        m_deferred_queue.for_each([&](const IdListOpEntry& entry) {
            if (m_version.observed(get_idlist_op_timestamp(entry.op))) {
                progress = true;
                return;
            }
            if (try_apply(entry.op)) {
                progress = true;
            } else {
                remaining.push_item(entry);
            }
        });
        m_deferred_queue = std::move(remaining);
    }
}

void IdList::enqueue_deferred(IdListOpEntry entry) {
    m_deferred_queue.push_item(std::move(entry));
}

bool IdList::apply_concrete(const IdListInsertOp& op) {
    auto entries = get_entries();
    IdListEntry e(op.timestamp, op.locator, op.id);
    insert_entry(entries, std::move(e));
    m_clock.observe(op.timestamp);
    m_version.observe(op.timestamp);
    m_version.join(op.version);
    set_entries(std::move(entries));
    if (m_on_change) m_on_change();
    return true;
}

bool IdList::apply_concrete(const IdListRemoveOp& op) {
    auto entries = get_entries();
    bool found = false;
    for (auto& e : entries) {
        if (e.origin.replica_id == op.target_origin.replica_id &&
            e.origin.value == op.target_origin.value) {
            e.deletions.push_back(op.timestamp);
            found = true;
            break;
        }
    }
    assert(found && "apply_concrete(remove): target_origin not in tree — causal gate should have prevented this");
    m_clock.observe(op.timestamp);
    m_version.observe(op.timestamp);
    m_version.join(op.version);
    set_entries(std::move(entries));
    if (m_on_change) m_on_change();
    return true;
}

bool IdList::apply_concrete(const IdListUndoOpVariant& op) {
    for (auto& [edit_id, count] : op.counts) {
        m_undo_map.insert(UndoMapEntry{{edit_id, op.timestamp}, count});
    }
    m_clock.observe(op.timestamp);
    m_version.observe(op.timestamp);
    m_version.join(op.version);
    auto entries = get_entries();
    set_entries(std::move(entries));
    if (m_on_change) m_on_change();
    return true;
}

} // namespace CollabText::Crdt
