#pragma once

#include <collabtext/LocalHistory.h>

#include <cstddef>
#include <limits>

namespace CollabText::Crdt::detail {

// These ceilings bound the pending publication queue independently of the
// active undo stack. Byte charges cover each snapshot object and its identity
// arrays; allocator overhead remains implementation-dependent.
inline constexpr size_t kMaxPendingUndoRetirementGroups = 100'000;
inline constexpr size_t kMaxPendingUndoRetirementBytes = 8 * 1024 * 1024;

inline size_t local_undo_retirement_charge(
    size_t operation_count, size_t inserted_count, size_t deletion_count) {
    size_t bytes = sizeof(LocalUndoEntrySnapshot);
    if (inserted_count > std::numeric_limits<size_t>::max() - operation_count)
        return std::numeric_limits<size_t>::max();
    const size_t operation_and_insert_count = operation_count + inserted_count;
    if (deletion_count > std::numeric_limits<size_t>::max() -
                             operation_and_insert_count)
        return std::numeric_limits<size_t>::max();
    const size_t identity_count = operation_and_insert_count + deletion_count;
    if (identity_count > (std::numeric_limits<size_t>::max() - bytes) /
                             sizeof(Identity))
        return std::numeric_limits<size_t>::max();
    return bytes + identity_count * sizeof(Identity);
}

inline size_t local_undo_retirement_charge(
    const LocalUndoEntrySnapshot &entry) {
    return local_undo_retirement_charge(entry.operation_ids.size(),
                                        entry.inserted_items.size(),
                                        entry.deletion_operations.size());
}

} // namespace CollabText::Crdt::detail
