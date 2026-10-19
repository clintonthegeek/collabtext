#include "crdt/UndoMap.h"
#include "crdt/Diagnostics.h"

namespace CollabText::Crdt {

void UndoMap::insert(UndoMapEntry entry) {
    if (m_tree.empty()) {
        m_tree.push_item(std::move(entry));
        return;
    }
    UndoTreeKeyDim target{entry.key};
    auto cursor = m_tree.cursor<UndoTreeKeyDim>();
    cursor.seek(UndoTreeKeyDim::zero(), Bias::Left);
    SumTree<UndoMapEntry, UNDO_MAP_B> new_tree;
    new_tree.push_tree(cursor.slice(target));
    new_tree.push_item(std::move(entry));
    new_tree.push_tree(cursor.suffix());
    m_tree = std::move(new_tree);
}

uint32_t UndoMap::undo_count(Identity edit_id) const {
    Diagnostics::record(Diagnostics::Counter::undo_map_queries);
    uint32_t max_count = 0;
    UndoTreeKey start_key{edit_id, Identity::min()};
    auto cursor = m_tree.cursor<UndoTreeKeyDim>();
    cursor.seek(UndoTreeKeyDim{start_key}, Bias::Left);
    while (auto *entry = cursor.item()) {
        Diagnostics::record(Diagnostics::Counter::undo_map_entries_examined);
        if (!(entry->key.edit_id == edit_id))
            break;
        if (entry->undo_count > max_count)
            max_count = entry->undo_count;
        cursor.next();
    }
    return max_count;
}

bool UndoMap::is_undone(Identity edit_id) const {
    return undo_count(edit_id) % 2 == 1;
}

bool UndoMap::was_undone(Identity edit_id, const Global &version) const {
    Diagnostics::record(Diagnostics::Counter::undo_map_queries);
    uint32_t max_count = 0;
    UndoTreeKey start_key{edit_id, Identity::min()};
    auto cursor = m_tree.cursor<UndoTreeKeyDim>();
    cursor.seek(UndoTreeKeyDim{start_key}, Bias::Left);
    while (auto *entry = cursor.item()) {
        Diagnostics::record(Diagnostics::Counter::undo_map_entries_examined);
        if (!(entry->key.edit_id == edit_id))
            break;
        if (version.observed(entry->key.undo_id)) {
            if (entry->undo_count > max_count)
                max_count = entry->undo_count;
        }
        cursor.next();
    }
    return max_count % 2 == 1;
}

size_t UndoMap::size() const {
    size_t count = 0;
    m_tree.for_each([&](const UndoMapEntry &) { ++count; });
    return count;
}

std::vector<UndoMapEntry> UndoMap::entries() const {
    return m_tree.items();
}

Result<void> UndoMap::restore_entries(
    const std::vector<UndoMapEntry> &entries) {
    UndoMap rebuilt;
    std::optional<UndoTreeKey> previous;
    for (const auto &entry : entries) {
        const auto &edit = entry.key.edit_id;
        const auto &undo = entry.key.undo_id;
        if (!edit.actor.valid() || edit.counter == 0 ||
            !undo.actor.valid() || undo.counter == 0 || entry.undo_count == 0)
            return Error(ErrorCode::malformed,
                         "checkpoint UndoMap entry has an invalid identity or count",
                         "checkpoint.causal.undo_map");
        if (previous && !(*previous < entry.key))
            return Error(ErrorCode::malformed,
                         "checkpoint UndoMap entries are duplicated or unordered",
                         "checkpoint.causal.undo_map");
        rebuilt.insert(entry);
        previous = entry.key;
    }
    m_tree = std::move(rebuilt.m_tree);
    return Result<void>::success();
}

void UndoMap::clear() {
    m_tree = {};
}

} // namespace CollabText::Crdt
