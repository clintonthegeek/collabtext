#include "crdt/UndoMap.h"

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

uint32_t UndoMap::undo_count(Lamport edit_id) const {
    uint32_t max_count = 0;
    UndoTreeKey start_key{edit_id, Lamport::min()};
    auto cursor = m_tree.cursor<UndoTreeKeyDim>();
    cursor.seek(UndoTreeKeyDim{start_key}, Bias::Left);
    while (auto *entry = cursor.item()) {
        if (!(entry->key.edit_id == edit_id))
            break;
        if (entry->undo_count > max_count)
            max_count = entry->undo_count;
        cursor.next();
    }
    return max_count;
}

bool UndoMap::is_undone(Lamport edit_id) const {
    return undo_count(edit_id) % 2 == 1;
}

bool UndoMap::was_undone(Lamport edit_id, const Global &version) const {
    uint32_t max_count = 0;
    UndoTreeKey start_key{edit_id, Lamport::min()};
    auto cursor = m_tree.cursor<UndoTreeKeyDim>();
    cursor.seek(UndoTreeKeyDim{start_key}, Bias::Left);
    while (auto *entry = cursor.item()) {
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

void UndoMap::clear() {
    m_tree = {};
}

} // namespace CollabText::Crdt
