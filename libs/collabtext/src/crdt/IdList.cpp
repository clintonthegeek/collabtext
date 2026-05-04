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
    m_undo_stack.push_back(UndoEntry{ {UndoMapKey(origin)}, {} });
    m_undo_cursor = m_undo_stack.size();
    trim_undo_stack();

    // l. Notify change
    if (m_on_change) m_on_change();

    // m. Return the operation
    return IdListInsertOp{ origin, version_before, id, new_loc };
}

IdListOperation IdList::remove_at(const Anchor&) {
    assert(false && "IdList::remove_at not yet implemented");
    return IdListRemoveOp{};
}

void IdList::apply_ops(const std::vector<IdListOperation>&) {
    assert(false && "IdList::apply_ops not yet implemented");
}

std::optional<IdListOperation> IdList::undo() { return std::nullopt; }
std::optional<IdListOperation> IdList::redo() { return std::nullopt; }
bool IdList::coalesce_last_undo() { return false; }

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

Anchor IdList::anchor_of(uint64_t, Bias) const { return Anchor::min(); }
Anchor IdList::anchor_at_index(uint32_t, Bias) const { return Anchor::min(); }
uint32_t IdList::resolve_anchor(const Anchor&) const { return 0; }
int IdList::compare_anchors(const Anchor&, const Anchor&) const { return 0; }

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
    assert(false && "IdList::try_apply not yet implemented");
    return false;
}

void IdList::retry_deferred() {
    // stub — implemented in β5
}

void IdList::enqueue_deferred(IdListOpEntry entry) {
    m_deferred_queue.push_item(std::move(entry));
}

bool IdList::apply_concrete(const IdListInsertOp&) {
    assert(false && "IdList::apply_concrete(insert) not yet implemented");
    return false;
}

bool IdList::apply_concrete(const IdListRemoveOp&) {
    assert(false && "IdList::apply_concrete(remove) not yet implemented");
    return false;
}

bool IdList::apply_concrete(const IdListUndoOpVariant&) {
    assert(false && "IdList::apply_concrete(undo) not yet implemented");
    return false;
}

} // namespace CollabText::Crdt
