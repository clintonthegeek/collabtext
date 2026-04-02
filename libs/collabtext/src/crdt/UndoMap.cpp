#include "crdt/UndoMap.h"

namespace CollabText::Crdt {

void UndoMap::undo(UndoMapKey key) {
    m_entries[key]++;
}

void UndoMap::redo(UndoMapKey key) {
    auto it = m_entries.find(key);
    if (it != m_entries.end() && it->second > 0) {
        it->second--;
        if (it->second == 0)
            m_entries.erase(it);
    }
}

bool UndoMap::is_undone(UndoMapKey key) const {
    auto it = m_entries.find(key);
    return it != m_entries.end() && it->second > 0;
}

uint32_t UndoMap::count(UndoMapKey key) const {
    auto it = m_entries.find(key);
    return it != m_entries.end() ? it->second : 0;
}

} // namespace CollabText::Crdt
