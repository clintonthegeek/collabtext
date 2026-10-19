#include "crdt/IdList.h"
#include "crdt/Diagnostics.h"
#include "crdt/LocalHistoryBudget.h"
#include "crdt/SequenceOrder.h"
#include <algorithm>
#include <cassert>
#include <set>

namespace CollabText::Crdt {

namespace {
bool gap_conflicts(const CounterGapLedger& gaps, const IdListOperation& op) {
    if (gaps.records().empty() || !idlist_op_identity(op).actor.valid()) return false;
    if (gaps.conflicts(op)) return true;
    return visit_operation([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, IdListInsertOp>) {
            return (value.link.parent && gaps.contains(*value.link.parent)) ||
                   (value.link.right_origin && gaps.contains(*value.link.right_origin));
        } else if constexpr (std::is_same_v<T, IdListRemoveOp>) {
            return gaps.contains(value.target_origin);
        } else {
            for (const auto& [target, count] : value.counts) {
                (void)count;
                if (gaps.contains(target)) return true;
            }
            return false;
        }
    }, op);
}
}


Result<std::vector<CollabText::detail::PositionItem>> IdList::position_items() const {
    std::vector<CollabText::detail::PositionItem> items;
    for (const auto& entry : get_entries()) {
        if (!entry.origin.actor.valid() || !entry.origin.counter || entry.origin.counter == UINT64_MAX)
            return Error(ErrorCode::history_unavailable, "retained list identity is invalid");
        Diagnostics::record(Diagnostics::Counter::position_items_materialized);
        items.push_back({entry.origin, 1, entry.visible, entry.id});
    }
    return items;
}


IdList::IdList(SharedIdentityAllocator allocator)
    : m_allocator(std::move(allocator))
{
    // See Buffer's constructor: a null allocator is a programming error, an
    // invalid actor is a state, and only the first is asserted on.
    assert(m_allocator && "a list without an allocator cannot mint identity");
}

size_t IdList::find_origin(const std::vector<IdListEntry>& entries,
                           const Anchor& target) const {
    for (size_t i = 0; i < entries.size(); ++i) {
        Diagnostics::record(Diagnostics::Counter::origin_lookup_comparisons);
        if (entries[i].origin == target.position) {
            return i;
        }
    }
    return static_cast<size_t>(-1);
}

IdListOperation IdList::insert_after(const Anchor& after, uint64_t id) {
    // Legacy entry point: cannot report a refused request, so it returns an
    // empty operation and mutates nothing. See try_insert_after.
    auto result = try_insert_after(after, id);
    if (!result.ok()) return IdListOperation{};
    return std::move(result).value();
}

Result<IdListOperation> IdList::try_insert_after(const Anchor& after, uint64_t id) {
    // `Anchor::min()` is the one anchor naming no existing entry: it means
    // "insert at the head". Every other anchor must resolve, because the new
    // locator is computed between the predecessor's and successor's positions
    // and an unresolved anchor left those two at their defaults.
    if (!after.is_min() && find_origin(get_entries(), after) == static_cast<size_t>(-1)) {
        return Error(ErrorCode::unresolvable_anchor,
                     "insert_after: anchor names actor " +
                         after.position.actor.to_string() + " counter " +
                         std::to_string(after.position.counter) +
                         ", which is not in this list");
    }
    return insert_after_resolved(after, id);
}

Result<IdListOperation> IdList::insert_after_resolved(const Anchor& after, uint64_t id) {
    // a. Get entries vector
    auto entries = get_entries();

    // b. The new element goes immediately after the anchored entry (the head of
    // the list for the minimum anchor). The anchor may name a tombstone: the entry
    // is placed right after it in the full list, as the list has always done.
    std::optional<Identity> left;
    if (!after.is_min()) {
        assert(find_origin(entries, after) != static_cast<size_t>(-1) &&
               "insert_after_resolved: anchor must be resolved first");
        left = after.position;
    }

    // c. Mint its tree link from the current list. The same SequenceOrder code that
    // places a remote element then places this one, so a peer cannot put it elsewhere.
    const Link new_link = SequenceOrder::mint_link(entries, left);

    // d. Reserve this operation's identity. An insert creates one item, so the
    // interval is `n + 1 == 1` values and the item identity *is* the operation
    // identity (§3.2). They stay different domains by construction rather than
    // by accident: an IdList entry's origin is its item identity, and the
    // operation that created it is that same value.
    auto retirement_budget = check_retirement_budget_for_new_entry(
        sizeof(uint64_t) + sizeof(UndoMapKey), 1, 0);
    if (!retirement_budget.ok()) return retirement_budget.error();
    if (auto usable = check_allocator(); !usable.ok()) return usable.error();
    auto reserved = m_allocator->reserve_run(1);
    if (!reserved.ok()) return reserved.error().with_context("allocating identities for an IdList insert");
    const Identity origin(m_allocator->actor(), reserved.value());

    // e. Capture version before observing this op
    Global version_before = m_version;

    // f. Record this op in the version vector
    m_version.observe(origin);

    // h. Create and insert the entry
    IdListEntry e(origin, new_link, id);

    // i. Place it
    const bool placed = SequenceOrder::integrate(entries, std::move(e));
    assert(placed && "a link minted from this very list names a present element");
    (void)placed;

    // j. Rebuild the tree from entries
    set_entries(std::move(entries));

    // k. Push undo entry
    for (size_t i = m_undo_cursor; i < m_undo_stack.size(); ++i)
        retire_undo_entry(m_undo_stack[i]);
    m_undo_stack.resize(m_undo_cursor);  // truncate redo tail
    m_undo_stack.push_back(UndoEntry{
        origin, {origin}, {UndoMapKey(origin)}, {},
        sizeof(uint64_t) + sizeof(UndoMapKey)});
    m_undo_cursor = m_undo_stack.size();
    trim_undo_stack();

    // l. Notify change
    if (m_on_change) m_on_change();

    // m. Build and emit the operation
    IdListOperation op = IdListInsertOp{ origin, version_before, id, new_link };
    emit_local_op(op);

    return Result<IdListOperation>(std::move(op));
}

IdListOperation IdList::remove_at(const Anchor& target) {
    // Legacy entry point; see try_remove_at.
    auto result = try_remove_at(target);
    if (!result.ok()) return IdListOperation{};
    return std::move(result).value();
}

Result<IdListOperation> IdList::try_remove_at(const Anchor& target) {
    auto entries = get_entries();
    const size_t idx = find_origin(entries, target);
    if (idx == static_cast<size_t>(-1)) {
        // Previously `assert(idx < entries.size())` followed by `entries[idx]`,
        // so a Release build read one element past the end of the vector and
        // then tombstoned whatever happened to be there.
        return Error(ErrorCode::unresolvable_anchor,
                     "remove_at: anchor names actor " +
                         target.position.actor.to_string() + " counter " +
                         std::to_string(target.position.counter) +
                         ", which is not in this list");
    }
    return remove_at_resolved(target);
}

Result<IdListOperation> IdList::remove_at_resolved(const Anchor& target) {
    // a. Get entries
    auto entries = get_entries();

    // b. Find entry where origin matches target
    const size_t idx = find_origin(entries, target);
    assert(idx != static_cast<size_t>(-1) &&
           "remove_at_resolved: anchor must be resolved first");

    // c. Capture target_origin before any moves
    Identity target_origin = entries[idx].origin;

    // d. Reserve this operation's identity. A removal creates no items, so the
    // interval is one value and the deletion identity *is* the operation
    // identity (§3.3).
    auto retirement_budget = check_retirement_budget_for_new_entry(
        sizeof(uint64_t) + sizeof(Identity), 0, 1);
    if (!retirement_budget.ok()) return retirement_budget.error();
    if (auto usable = check_allocator(); !usable.ok()) return usable.error();
    auto reserved = m_allocator->reserve_run(1);
    if (!reserved.ok()) return reserved.error().with_context("allocating identities for an IdList insert");
    const Identity del_id(m_allocator->actor(), reserved.value());

    // f. Capture version before observing this op
    Global version_before = m_version;

    // g. Observe the deletion op in version vector
    m_version.observe(del_id);

    // h. Tombstone the entry
    entries[idx].deletions.push_back(del_id);

    // i. Rebuild tree (recomputes visible for all entries)
    set_entries(std::move(entries));

    // j. Push undo entry: empty inserted_keys, del_id in deletion_ids
    for (size_t i = m_undo_cursor; i < m_undo_stack.size(); ++i)
        retire_undo_entry(m_undo_stack[i]);
    m_undo_stack.resize(m_undo_cursor);
    m_undo_stack.push_back(UndoEntry{
        del_id, {del_id}, {}, {del_id},
        sizeof(uint64_t) + sizeof(Identity)});
    m_undo_cursor = m_undo_stack.size();
    trim_undo_stack();

    // k. Notify change
    if (m_on_change) m_on_change();

    // l. Build and emit the operation
    IdListOperation op = IdListRemoveOp{ del_id, version_before, target_origin };
    emit_local_op(op);

    return Result<IdListOperation>(std::move(op));
}

std::vector<ApplyOutcome> IdList::apply_ops(const std::vector<IdListOperation>& ops) {
    std::vector<ApplyOutcome> outcomes;
    outcomes.reserve(ops.size());
    for (const auto& op : ops) {
        outcomes.push_back(apply_one(op));
    }
    retry_deferred();
    return outcomes;
}

std::optional<IdListOperation> IdList::undo() {
    auto result = try_undo();
    if (!result.ok()) return std::nullopt;
    return std::move(result).value();
}

Result<std::optional<IdListOperation>> IdList::try_undo() {
    if (m_undo_cursor == 0) return std::optional<IdListOperation>{};

    return try_undo_group(m_undo_stack[m_undo_cursor - 1].group_id);
}

Result<std::optional<IdListOperation>> IdList::try_undo_group(const Identity &group_id) {
    auto found = std::find_if(m_undo_stack.begin(),
                              m_undo_stack.begin() + static_cast<std::ptrdiff_t>(m_undo_cursor),
                              [&](const UndoEntry &entry) { return entry.group_id == group_id; });
    if (found == m_undo_stack.begin() + static_cast<std::ptrdiff_t>(m_undo_cursor))
        return Error(ErrorCode::history_unavailable,
                     "requested IdList undo group is not active");

    if (auto usable = check_allocator(); !usable.ok()) return usable.error();

    // Reserved before the cursor moves: a refused undo that had already
    // decremented m_undo_cursor would silently drop an entry from the history.
    auto reserved = m_allocator->reserve_run(1);
    if (!reserved.ok()) return reserved.error();

    UndoEntry entry = std::move(*found);
    const auto index = static_cast<size_t>(std::distance(m_undo_stack.begin(), found));
    m_undo_stack.erase(m_undo_stack.begin() + static_cast<std::ptrdiff_t>(index));
    --m_undo_cursor;
    m_undo_stack.insert(m_undo_stack.begin() + static_cast<std::ptrdiff_t>(m_undo_cursor),
                        std::move(entry));
    auto& undo_entry = m_undo_stack[m_undo_cursor];

    IdListUndoOpVariant op;
    op.version = m_version;
    op.timestamp = Identity(m_allocator->actor(), reserved.value());

    for (auto& key : undo_entry.inserted_keys) {
        const Identity edit_id = key.position;
        uint32_t current = m_undo_map.undo_count(edit_id);
        m_undo_map.insert(UndoMapEntry{{edit_id, op.timestamp}, current + 1});
        op.counts.push_back({edit_id, current + 1});
    }
    for (auto& del_id : undo_entry.deletion_ids) {
        uint32_t current = m_undo_map.undo_count(del_id);
        m_undo_map.insert(UndoMapEntry{{del_id, op.timestamp}, current + 1});
        op.counts.push_back({del_id, current + 1});
    }

    auto entries = get_entries();
    set_entries(std::move(entries));

    m_version.observe(op.timestamp);
    if (m_on_change) m_on_change();
    emit_local_op(op);
    return std::optional<IdListOperation>(std::move(op));
}

std::optional<IdListOperation> IdList::redo() {
    auto result = try_redo();
    if (!result.ok()) return std::nullopt;
    return std::move(result).value();
}

Result<std::optional<IdListOperation>> IdList::try_redo() {
    if (m_undo_cursor >= m_undo_stack.size())
        return std::optional<IdListOperation>{};

    return try_redo_group(m_undo_stack[m_undo_cursor].group_id);
}

Result<std::optional<IdListOperation>> IdList::try_redo_group(const Identity &group_id) {
    auto found = std::find_if(m_undo_stack.begin() + static_cast<std::ptrdiff_t>(m_undo_cursor),
                              m_undo_stack.end(),
                              [&](const UndoEntry &entry) { return entry.group_id == group_id; });
    if (found == m_undo_stack.end())
        return Error(ErrorCode::history_unavailable,
                     "requested IdList redo group is not available");

    if (auto usable = check_allocator(); !usable.ok()) return usable.error();

    // Reserved before the cursor moves; see undo().
    auto reserved = m_allocator->reserve_run(1);
    if (!reserved.ok()) return reserved.error();

    UndoEntry entry = std::move(*found);
    const auto index = static_cast<size_t>(std::distance(m_undo_stack.begin(), found));
    m_undo_stack.erase(m_undo_stack.begin() + static_cast<std::ptrdiff_t>(index));
    m_undo_stack.insert(m_undo_stack.begin() + static_cast<std::ptrdiff_t>(m_undo_cursor),
                        std::move(entry));
    auto& redo_entry = m_undo_stack[m_undo_cursor];
    ++m_undo_cursor;

    IdListUndoOpVariant op;
    op.version = m_version;
    op.timestamp = Identity(m_allocator->actor(), reserved.value());

    for (auto& key : redo_entry.inserted_keys) {
        const Identity edit_id = key.position;
        uint32_t current = m_undo_map.undo_count(edit_id);
        m_undo_map.insert(UndoMapEntry{{edit_id, op.timestamp}, current + 1});
        op.counts.push_back({edit_id, current + 1});
    }
    for (auto& del_id : redo_entry.deletion_ids) {
        uint32_t current = m_undo_map.undo_count(del_id);
        m_undo_map.insert(UndoMapEntry{{del_id, op.timestamp}, current + 1});
        op.counts.push_back({del_id, current + 1});
    }

    auto entries = get_entries();
    set_entries(std::move(entries));

    m_version.observe(op.timestamp);
    if (m_on_change) m_on_change();
    emit_local_op(op);
    return std::optional<IdListOperation>(std::move(op));
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
    previous.operation_ids.insert(previous.operation_ids.end(),
                                  latest.operation_ids.begin(),
                                  latest.operation_ids.end());
    previous.retained_bytes = latest.retained_bytes > SIZE_MAX - previous.retained_bytes
                                  ? SIZE_MAX
                                  : previous.retained_bytes + latest.retained_bytes;
    m_undo_stack.erase(m_undo_stack.begin()
                       + static_cast<ptrdiff_t>(m_undo_cursor - 1));
    m_undo_cursor--;
    recalculate_undo_bytes();
    return true;
}

LocalUndoEntrySnapshot IdList::snapshot_undo_entry(const UndoEntry &entry) const {
    LocalUndoEntrySnapshot saved;
    saved.group_id = entry.group_id;
    saved.operation_ids = entry.operation_ids;
    saved.inserted_items.reserve(entry.inserted_keys.size());
    for (const auto &key : entry.inserted_keys)
        saved.inserted_items.push_back(key.position);
    saved.deletion_operations = entry.deletion_ids;
    saved.retained_bytes = entry.retained_bytes;
    return saved;
}

Result<void> IdList::set_max_undo_depth(size_t depth) {
    if (depth > detail::kMaxPendingUndoRetirementGroups)
        return Error(ErrorCode::limit_exceeded,
                     "undo history group limit exceeds 100,000");
    auto budget = check_retirement_budget_for_limits(depth, m_max_undo_bytes);
    if (!budget.ok()) return budget.error();
    m_max_undo_depth = depth;
    trim_undo_stack();
    return Result<void>::success();
}

Result<void> IdList::set_max_undo_bytes(size_t bytes) {
    auto budget = check_retirement_budget_for_limits(m_max_undo_depth, bytes);
    if (!budget.ok()) return budget.error();
    m_max_undo_bytes = bytes;
    trim_undo_stack();
    return Result<void>::success();
}

void IdList::recalculate_undo_bytes() {
    m_undo_bytes = 0;
    for (const auto &entry : m_undo_stack) {
        if (entry.retained_bytes > SIZE_MAX - m_undo_bytes) {
            m_undo_bytes = SIZE_MAX;
            return;
        }
        m_undo_bytes += entry.retained_bytes;
    }
}

void IdList::trim_undo_stack() {
    recalculate_undo_bytes();
    while (!m_undo_stack.empty() &&
           (m_undo_stack.size() > m_max_undo_depth ||
            m_undo_bytes > m_max_undo_bytes)) {
        retire_undo_entry(m_undo_stack.front());
        m_undo_stack.erase(m_undo_stack.begin());
        if (m_undo_cursor != 0) --m_undo_cursor;
        recalculate_undo_bytes();
    }
}

void IdList::retire_undo_entry(const UndoEntry &entry) {
    Diagnostics::record(Diagnostics::Counter::undo_retirement_groups);
    Diagnostics::record(Diagnostics::Counter::undo_retirement_operation_ids,
                        entry.operation_ids.size());
    Diagnostics::record(Diagnostics::Counter::undo_retirement_inserted_items,
                        entry.inserted_keys.size());
    Diagnostics::record(Diagnostics::Counter::undo_retirement_deletions,
                        entry.deletion_ids.size());
    Diagnostics::record(Diagnostics::Counter::undo_retirement_budget_bytes,
        detail::local_undo_retirement_charge(entry.operation_ids.size(),
                                             entry.inserted_keys.size(),
                                             entry.deletion_ids.size()));
    auto snapshot = snapshot_undo_entry(entry);
    m_retired_undo_bytes += detail::local_undo_retirement_charge(snapshot);
    m_retired_undo_entries.push_back(std::move(snapshot));
}

Result<void> IdList::check_retirement_budget_for_limits(size_t depth,
                                                         size_t bytes) const {
    size_t remaining_bytes = m_undo_bytes;
    size_t added_bytes = 0;
    size_t added_groups = 0;
    size_t first = 0;
    size_t count = m_undo_stack.size();
    while (count > depth || remaining_bytes > bytes) {
        if (first >= m_undo_stack.size()) break;
        const auto &entry = m_undo_stack[first++];
        remaining_bytes -= std::min(remaining_bytes, entry.retained_bytes);
        const size_t charge = detail::local_undo_retirement_charge(
            entry.operation_ids.size(), entry.inserted_keys.size(),
            entry.deletion_ids.size());
        if (charge > detail::kMaxPendingUndoRetirementBytes -
                         std::min(added_bytes, detail::kMaxPendingUndoRetirementBytes))
            return Error(ErrorCode::limit_exceeded,
                         "pending undo retirement memory budget would be exceeded");
        added_bytes += charge;
        ++added_groups;
        --count;
    }
    const size_t existing_groups = m_retired_undo_entries.size();
    if (existing_groups > detail::kMaxPendingUndoRetirementGroups ||
        added_groups > detail::kMaxPendingUndoRetirementGroups - existing_groups ||
        count > detail::kMaxPendingUndoRetirementGroups - existing_groups - added_groups ||
        added_bytes > detail::kMaxPendingUndoRetirementBytes -
                          std::min(m_retired_undo_bytes,
                                   detail::kMaxPendingUndoRetirementBytes))
        return Error(ErrorCode::limit_exceeded,
                     "pending undo retirement memory budget would be exceeded");
    return Result<void>::success();
}

Result<void> IdList::check_retirement_budget_for_new_entry(
    size_t retained_bytes, size_t inserted_count, size_t deletion_count) const {
    size_t added_bytes = 0;
    size_t added_groups = 0;
    auto add = [&](size_t charge) -> bool {
        if (charge > detail::kMaxPendingUndoRetirementBytes -
                         std::min(added_bytes, detail::kMaxPendingUndoRetirementBytes))
            return false;
        added_bytes += charge;
        ++added_groups;
        return true;
    };
    for (size_t i = m_undo_cursor; i < m_undo_stack.size(); ++i) {
        const auto &entry = m_undo_stack[i];
        if (!add(detail::local_undo_retirement_charge(
                entry.operation_ids.size(), entry.inserted_keys.size(),
                entry.deletion_ids.size())))
            return Error(ErrorCode::limit_exceeded,
                         "pending undo retirement memory budget would be exceeded");
    }
    size_t remaining_bytes = 0;
    for (size_t i = 0; i < m_undo_cursor; ++i) {
        if (m_undo_stack[i].retained_bytes > SIZE_MAX - remaining_bytes) {
            remaining_bytes = SIZE_MAX;
            break;
        }
        remaining_bytes += m_undo_stack[i].retained_bytes;
    }
    remaining_bytes = retained_bytes > SIZE_MAX - remaining_bytes
                          ? SIZE_MAX : remaining_bytes + retained_bytes;
    size_t count = m_undo_cursor + 1;
    size_t first = 0;
    const size_t candidate_charge = detail::local_undo_retirement_charge(
        1, inserted_count, deletion_count);
    while (count > m_max_undo_depth || remaining_bytes > m_max_undo_bytes) {
        if (first < m_undo_cursor) {
            const auto &entry = m_undo_stack[first++];
            remaining_bytes -= std::min(remaining_bytes, entry.retained_bytes);
            if (!add(detail::local_undo_retirement_charge(
                    entry.operation_ids.size(), entry.inserted_keys.size(),
                    entry.deletion_ids.size())))
                return Error(ErrorCode::limit_exceeded,
                             "pending undo retirement memory budget would be exceeded");
        } else {
            remaining_bytes -= std::min(remaining_bytes, retained_bytes);
            if (!add(candidate_charge))
                return Error(ErrorCode::limit_exceeded,
                             "pending undo retirement memory budget would be exceeded");
        }
        --count;
    }
    const size_t existing_groups = m_retired_undo_entries.size();
    if (existing_groups > detail::kMaxPendingUndoRetirementGroups ||
        added_groups > detail::kMaxPendingUndoRetirementGroups - existing_groups ||
        count > detail::kMaxPendingUndoRetirementGroups - existing_groups - added_groups ||
        added_bytes > detail::kMaxPendingUndoRetirementBytes -
                          std::min(m_retired_undo_bytes,
                                   detail::kMaxPendingUndoRetirementBytes))
        return Error(ErrorCode::limit_exceeded,
                     "pending undo retirement memory budget would be exceeded");
    return Result<void>::success();
}

LocalHistorySnapshot IdList::local_history_snapshot() const {
    LocalHistorySnapshot snapshot;
    snapshot.cursor = m_undo_cursor;
    snapshot.retired_entries = m_retired_undo_entries;
    snapshot.entries.reserve(m_undo_stack.size());
    for (const auto &entry : m_undo_stack)
        snapshot.entries.push_back(snapshot_undo_entry(entry));
    return snapshot;
}

std::optional<LocalUndoEntrySnapshot> IdList::next_undo_group() const {
    if (m_undo_cursor == 0) return std::nullopt;
    return local_history_snapshot().entries[m_undo_cursor - 1];
}

std::optional<LocalUndoEntrySnapshot> IdList::next_redo_group() const {
    if (m_undo_cursor >= m_undo_stack.size()) return std::nullopt;
    return local_history_snapshot().entries[m_undo_cursor];
}

Result<void> IdList::acknowledge_undo_retirements(
    const std::vector<Identity> &group_ids) {
    std::set<Identity> unique;
    for (const auto &id : group_ids) {
        if (!unique.insert(id).second)
            return Error(ErrorCode::malformed,
                         "undo retirement acknowledgement repeats a group");
        if (std::none_of(m_retired_undo_entries.begin(),
                         m_retired_undo_entries.end(),
                         [&](const auto &entry) { return entry.group_id == id; }))
            return Error(ErrorCode::not_found,
                         "undo retirement group is not pending");
    }
    for (const auto &entry : m_retired_undo_entries)
        if (unique.contains(entry.group_id))
            m_retired_undo_bytes -= detail::local_undo_retirement_charge(entry);
    std::erase_if(m_retired_undo_entries, [&](const auto &entry) {
        return unique.contains(entry.group_id);
    });
    return Result<void>::success();
}

Result<void> IdList::restore_local_history(const LocalHistorySnapshot &snapshot) {
    if (snapshot.cursor > snapshot.entries.size() ||
        snapshot.entries.size() > m_max_undo_depth ||
        snapshot.retired_entries.size() >
            detail::kMaxPendingUndoRetirementGroups ||
        snapshot.entries.size() > detail::kMaxPendingUndoRetirementGroups -
                                      snapshot.retired_entries.size())
        return Error(ErrorCode::malformed,
                     "IdList history cursor or depth is outside configured bounds");
    const auto &owner = m_allocator->actor();
    size_t restored_bytes = 0;
    std::vector<UndoEntry> restored;
    restored.reserve(snapshot.entries.size());
    std::vector<LocalUndoEntrySnapshot> retired;
    retired.reserve(snapshot.retired_entries.size());
    size_t retired_bytes = 0;
    auto valid_local_id = [&](const Identity &id) {
        return id.actor == owner && id.counter != 0 &&
               id.counter <= m_version.get(owner) && !m_counter_gaps.contains(id);
    };
    for (const auto &saved : snapshot.entries) {
        if (saved.retained_bytes > m_max_undo_bytes -
                                       std::min(restored_bytes, m_max_undo_bytes))
            return Error(ErrorCode::limit_exceeded,
                         "IdList history exceeds configured byte bound");
        restored_bytes += saved.retained_bytes;
        UndoEntry entry;
        if (!valid_local_id(saved.group_id) || saved.operation_ids.empty())
            return Error(ErrorCode::history_unavailable,
                         "IdList undo group metadata is absent from owner history");
        entry.group_id = saved.group_id;
        for (const auto &id : saved.operation_ids) {
            if (!valid_local_id(id))
                return Error(ErrorCode::history_unavailable,
                             "IdList undo group member is absent from owner history");
            entry.operation_ids.push_back(id);
        }
        entry.inserted_keys.reserve(saved.inserted_items.size());
        for (const auto &id : saved.inserted_items) {
            if (!valid_local_id(id))
                return Error(ErrorCode::history_unavailable,
                             "IdList undo item is absent from replayed owner history");
            entry.inserted_keys.emplace_back(id);
        }
        for (const auto &id : saved.deletion_operations) {
            if (!valid_local_id(id))
                return Error(ErrorCode::history_unavailable,
                             "IdList deletion is absent from replayed owner history");
            entry.deletion_ids.push_back(id);
        }
        entry.retained_bytes = saved.retained_bytes;
        restored.push_back(std::move(entry));
    }
    for (const auto &saved : snapshot.retired_entries) {
        const size_t charge = detail::local_undo_retirement_charge(saved);
        if (charge > detail::kMaxPendingUndoRetirementBytes -
                         std::min(retired_bytes,
                                  detail::kMaxPendingUndoRetirementBytes))
            return Error(ErrorCode::limit_exceeded,
                         "pending undo retirement memory budget exceeded");
        retired_bytes += charge;
        if (!valid_local_id(saved.group_id) || saved.operation_ids.empty())
            return Error(ErrorCode::history_unavailable,
                         "retired IdList group is absent from owner history");
        for (const auto &id : saved.operation_ids)
            if (!valid_local_id(id))
                return Error(ErrorCode::history_unavailable,
                             "retired IdList member is absent from owner history");
        for (const auto &id : saved.inserted_items)
            if (!valid_local_id(id))
                return Error(ErrorCode::history_unavailable,
                             "retired IdList item is absent from owner history");
        for (const auto &id : saved.deletion_operations)
            if (!valid_local_id(id))
                return Error(ErrorCode::history_unavailable,
                             "retired IdList deletion is absent from owner history");
        retired.push_back(saved);
    }
    m_undo_stack = std::move(restored);
    m_retired_undo_entries = std::move(retired);
    m_retired_undo_bytes = retired_bytes;
    m_undo_cursor = snapshot.cursor;
    m_undo_bytes = restored_bytes;
    return Result<void>::success();
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
    Anchor result = Anchor::end();
    bool found = false;
    m_entry_tree.for_each([&](const IdListEntry& e) {
        if (found) return;
        if (!e.visible) return;
        if (visible_seen == index) {
            result = Anchor(e.origin, bias);
            found = true;
            return;
        }
        ++visible_seen;
    });
    return result;
}

Anchor IdList::anchor_of(uint64_t id, Bias bias) const {
    return try_anchor_of(id, bias).value_or(Anchor::end());
}

Result<Anchor> IdList::try_anchor_of(uint64_t id, Bias bias) const {
    Anchor result = Anchor::end();
    bool found = false;
    m_entry_tree.for_each([&](const IdListEntry& e) {
        if (found) return;
        if (!e.visible) return;
        if (e.id == id) {
            result = Anchor(e.origin, bias);
            found = true;
        }
    });
    if (!found)
        return Error(ErrorCode::unresolvable_anchor, "visible list id was not found",
                     std::to_string(id));
    return result;
}

uint32_t IdList::resolve_anchor(const Anchor& a) const {
    if (a.is_min()) return 0;
    if (a.is_end()) return size();
    uint32_t accumulated = 0;
    uint32_t result = size();
    bool found = false;
    m_entry_tree.for_each([&](const IdListEntry& e) {
        if (found) return;
        if (e.origin == a.position) {
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

size_t IdList::collect_garbage() {
    // Local history cannot prove that remote redo or anchors have expired.
    // Retain all restoration state until certified reclamation is available.
    return 0;
}

Result<size_t> IdList::collect_garbage(
    const std::set<Identity> &pinned_identities) {
    if (!reclamation_binding_is_well_formed(m_reclamation) ||
        m_reclamation.empty())
        return Error(ErrorCode::awaiting_state,
                     "IdList collection requires an installed certified frontier");
    for (const auto &[actor, through] : m_reclamation.operation_frontier)
        if (m_version.get(actor) < through)
            return Error(ErrorCode::counter_regression,
                         "installed IdList reclamation frontier exceeds causal state");

    auto entries = get_entries();
    std::vector<bool> remove(entries.size(), false);
    size_t reclaimed = 0;
    const auto undo_snapshot = m_undo_map.entries();
    const auto retained_by_local_history = [&](const IdListEntry &entry) {
        for (const auto &undo : m_undo_stack) {
            for (const auto &key : undo.inserted_keys)
                if (key.position == entry.origin) return true;
            for (const auto &deletion : undo.deletion_ids)
                if (std::find(entry.deletions.begin(), entry.deletions.end(),
                              deletion) != entry.deletions.end())
                    return true;
        }
        for (const auto &undo : m_retired_undo_entries) {
            if (std::find(undo.inserted_items.begin(), undo.inserted_items.end(),
                          entry.origin) != undo.inserted_items.end())
                return true;
            for (const auto &deletion : undo.deletion_operations)
                if (std::find(entry.deletions.begin(), entry.deletions.end(),
                              deletion) != entry.deletions.end())
                    return true;
        }
        return false;
    };

    for (size_t i = 0; i < entries.size(); ++i) {
        const auto &entry = entries[i];
        if (entry.visible ||
            !tombstone_is_covered(m_reclamation.operation_frontier,
                                  entry.origin, 1, entry.deletions) ||
            retained_by_local_history(entry) ||
            pinned_identities.contains(entry.origin))
            continue;

        bool referenced = false;
        for (size_t j = 0; j < entries.size() && !referenced; ++j) {
            if (i == j) continue;
            const auto &link = entries[j].link;
            referenced = (link.parent && *link.parent == entry.origin) ||
                         (link.right_origin &&
                          *link.right_origin == entry.origin);
        }
        if (referenced) continue;

        bool has_uncovered_undo = false;
        for (const auto &event : undo_snapshot) {
            const bool targets_entry =
                event.key.edit_id == entry.origin ||
                std::find(entry.deletions.begin(), entry.deletions.end(),
                          event.key.edit_id) != entry.deletions.end();
            if (targets_entry && !operation_is_covered(
                                     m_reclamation.operation_frontier,
                                     event.key.undo_id)) {
                has_uncovered_undo = true;
                break;
            }
        }
        if (!has_uncovered_undo) {
            remove[i] = true;
            ++reclaimed;
        }
    }

    if (reclaimed == 0) return size_t{0};
    const auto is_reclaimed_target = [&](const Identity &identity) {
        for (size_t i = 0; i < entries.size(); ++i)
            if (remove[i] &&
                (entries[i].origin == identity ||
                 std::find(entries[i].deletions.begin(),
                           entries[i].deletions.end(), identity) !=
                     entries[i].deletions.end()))
                return true;
        return false;
    };

    auto undo_entries = m_undo_map.entries();
    std::erase_if(undo_entries, [&](const UndoMapEntry &event) {
        return is_reclaimed_target(event.key.edit_id);
    });
    UndoMap rebuilt_undo;
    if (auto restored = rebuilt_undo.restore_entries(undo_entries);
        !restored.ok())
        return restored.error();

    IdListTree rebuilt_tree;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (remove[i]) continue;
        auto &entry = entries[i];
        entry.visible = entry.compute_visible(rebuilt_undo);
        rebuilt_tree.push_item(std::move(entry));
    }
    m_undo_map = std::move(rebuilt_undo);
    m_entry_tree = std::move(rebuilt_tree);
    return reclaimed;
}

std::vector<IdListEntry> IdList::entries() const {
    return m_entry_tree.items();
}

IdListCheckpointState IdList::checkpoint_state() const {
    IdListCheckpointState snapshot;
    snapshot.version = m_version;
    snapshot.counter_gaps = m_counter_gaps.records();
    snapshot.reclamation = m_reclamation;
    snapshot.entries = get_entries();
    snapshot.undo_entries = m_undo_map.entries();
    snapshot.deferred = m_deferred_queue.items();
    snapshot.quarantined = m_quarantined;
    return snapshot;
}

Result<void> IdList::restore_checkpoint_state(
    const IdListCheckpointState &snapshot) {
    if (!reclamation_binding_is_well_formed(snapshot.reclamation))
        return Error(ErrorCode::malformed,
                     "checkpoint reclamation binding is incomplete or invalid",
                     "checkpoint.engine.reclamation");
    for (const auto &[actor, through] : snapshot.reclamation.operation_frontier)
        if (snapshot.version.get(actor) < through)
            return Error(ErrorCode::counter_regression,
                         "checkpoint reclamation frontier exceeds causal state",
                         "checkpoint.engine.reclamation");
    if (snapshot.version.get(actor_id()) >= m_allocator->peek())
        return Error(ErrorCode::counter_regression,
                     "checkpoint exceeds the owner's durable identity reservation",
                     "checkpoint.owner_writer");
    if (snapshot.deferred.size() > m_deferred_budget.max_operations ||
        snapshot.quarantined.size() > m_quarantine_budget.max_operations)
        return Error(ErrorCode::limit_exceeded,
                     "checkpoint deferred or quarantine record count exceeds configured budget",
                     "checkpoint.deferred");
    IdList rebuilt(m_allocator);
    auto gaps = rebuilt.m_counter_gaps.restore(snapshot.counter_gaps, snapshot.version);
    if (!gaps) return gaps.error();
    rebuilt.m_deferred_budget = m_deferred_budget;
    rebuilt.m_quarantine_budget = m_quarantine_budget;
    rebuilt.m_max_undo_depth = m_max_undo_depth;
    rebuilt.m_max_undo_bytes = m_max_undo_bytes;
    rebuilt.m_reclamation = snapshot.reclamation;

    auto valid_identity = [](const Identity &id) {
        return id.actor.valid() && id.counter != 0 && id.counter != UINT64_MAX;
    };
    auto observed_identity = [&](const Identity &id) {
        return valid_identity(id) && snapshot.version.observed(id) && !rebuilt.m_counter_gaps.contains(id);
    };
    auto map_restore = rebuilt.m_undo_map.restore_entries(snapshot.undo_entries);
    if (!map_restore) return map_restore.error();
    for (const auto &entry : snapshot.undo_entries) {
        if (!observed_identity(entry.key.edit_id) ||
            !observed_identity(entry.key.undo_id))
            return Error(ErrorCode::malformed,
                         "checkpoint UndoMap references an identity outside its causal state",
                         "checkpoint.causal.undo_map");
    }

    std::set<Identity> item_identities;
    for (const auto &entry : snapshot.entries) {
        if (!observed_identity(entry.origin) || !item_identities.insert(entry.origin).second)
            return Error(ErrorCode::malformed,
                         "checkpoint list entry identity is invalid or duplicated",
                         "checkpoint.engine.id_list");
        if (entry.link.side != Side::Left && entry.link.side != Side::Right)
            return Error(ErrorCode::malformed,
                         "checkpoint list entry has an invalid placement side",
                         "checkpoint.engine.id_list");
        if ((entry.link.parent && !observed_identity(*entry.link.parent)) ||
            (entry.link.right_origin && !observed_identity(*entry.link.right_origin)))
            return Error(ErrorCode::malformed,
                         "checkpoint list link is outside its causal state",
                         "checkpoint.engine.id_list");
        std::set<Identity> deletions;
        for (const auto &deletion : entry.deletions)
            if (!observed_identity(deletion) || !deletions.insert(deletion).second)
                return Error(ErrorCode::malformed,
                             "checkpoint list deletion identity is invalid or repeated",
                             "checkpoint.engine.id_list");
    }
    if (const auto issue = SequenceOrder::check_consistency(snapshot.entries))
        return Error(ErrorCode::malformed, *issue, "checkpoint.engine.id_list.links");
    rebuilt.m_version = snapshot.version;
    auto entries = snapshot.entries;
    rebuilt.set_entries(std::move(entries));

    size_t deferred_bytes = 0;
    std::set<Identity> deferred_identities;
    for (const auto &entry : snapshot.deferred) {
        if (gap_conflicts(rebuilt.m_counter_gaps, entry.op)) return CounterGapLedger::conflict();
        const auto timestamp = get_idlist_op_identity(entry.op);
        if (timestamp != entry.timestamp || !valid_identity(timestamp) ||
            snapshot.version.observed(timestamp) ||
            !deferred_identities.insert(timestamp).second)
            return Error(ErrorCode::malformed,
                         "checkpoint deferred list operations have invalid or duplicate identities",
                         "checkpoint.deferred");
        const auto verdict = validate_idlist_operation(entry.op, snapshot.version);
        if (verdict.verdict == ValidationVerdict::rejected)
            return verdict.error.with_context("checkpoint.deferred");
        const auto bytes = estimate_operation_bytes(entry.op);
        if (bytes > SIZE_MAX - deferred_bytes ||
            bytes > m_deferred_budget.max_bytes -
                        std::min(deferred_bytes, m_deferred_budget.max_bytes))
            return Error(ErrorCode::limit_exceeded,
                         "checkpoint deferred input exceeds configured byte budget",
                         "checkpoint.deferred");
        deferred_bytes += bytes;
        rebuilt.m_deferred_queue.push_item(entry);
    }
    if (deferred_bytes > m_deferred_budget.max_bytes)
        return Error(ErrorCode::limit_exceeded,
                     "checkpoint deferred input exceeds configured budget",
                     "checkpoint.deferred");
    rebuilt.m_deferred_count = snapshot.deferred.size();
    rebuilt.m_deferred_bytes = deferred_bytes;

    size_t quarantine_bytes = 0;
    for (const auto &entry : snapshot.quarantined) {
        if (gap_conflicts(rebuilt.m_counter_gaps, entry.op)) return CounterGapLedger::conflict();
        const auto timestamp = get_idlist_op_identity(entry.op);
        if (!valid_identity(timestamp) || snapshot.version.observed(timestamp) ||
            entry.reason.empty() || !deferred_identities.insert(timestamp).second)
            return Error(ErrorCode::malformed,
                         "checkpoint quarantine record is invalid",
                         "checkpoint.quarantine");
        const auto bytes = estimate_operation_bytes(entry.op);
        if (bytes > SIZE_MAX - quarantine_bytes ||
            bytes > m_quarantine_budget.max_bytes -
                        std::min(quarantine_bytes, m_quarantine_budget.max_bytes))
            return Error(ErrorCode::limit_exceeded,
                         "checkpoint quarantine exceeds configured byte budget",
                         "checkpoint.quarantine");
        quarantine_bytes += bytes;
    }
    if (quarantine_bytes > m_quarantine_budget.max_bytes)
        return Error(ErrorCode::limit_exceeded,
                     "checkpoint quarantine exceeds configured budget",
                     "checkpoint.quarantine");
    rebuilt.m_quarantined = snapshot.quarantined;
    rebuilt.m_quarantined_bytes = quarantine_bytes;

    auto history = rebuilt.restore_local_history(local_history_snapshot());
    if (!history) return history.error().with_context("checkpoint.owner_history");
    rebuilt.m_on_change = m_on_change;
    rebuilt.m_on_local_op = m_on_local_op;
    *this = std::move(rebuilt);
    return Result<void>::success();
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
    auto entries = m_entry_tree.items();
    Diagnostics::record(Diagnostics::Counter::idlist_snapshots);
    Diagnostics::record(Diagnostics::Counter::idlist_snapshot_entries,
                        entries.size());
    return entries;
}

void IdList::set_entries(std::vector<IdListEntry>&& entries) {
    Diagnostics::record(Diagnostics::Counter::idlist_tree_rebuild_entries,
                        entries.size());
    for (auto& e : entries)
        e.visible = e.compute_visible(m_undo_map);
    IdListTree tree;
    for (auto& e : entries) tree.push_item(std::move(e));
    m_entry_tree = std::move(tree);
}

void IdList::discard_local_view() noexcept {
    m_entry_tree = IdListTree{};
    m_undo_map.clear();
    m_undo_stack.clear();
    m_undo_cursor = 0;
    m_undo_bytes = 0;
    m_deferred_queue = IdListOperationQueue{};
    m_deferred_count = 0;
    m_deferred_bytes = 0;
    m_quarantined.clear();
    m_quarantined_bytes = 0;
    // Intentionally preserved: m_allocator, m_version,
    // m_max_undo_depth, m_max_undo_bytes, m_on_change, m_on_local_op.
    // Intentionally not fired: m_on_change, m_on_local_op.
}

ApplyOutcome IdList::defer_or_quarantine(const IdListOperation& op,
                                         Identity awaiting) {
    const size_t bytes = estimate_operation_bytes(op);

    if (deferred_budget_exhausted(m_deferred_budget, m_deferred_count,
                                   m_deferred_bytes, bytes)) {
        const Error reason(
            ErrorCode::deferred_queue_full,
            "deferral budget exhausted: " + std::to_string(m_deferred_count) +
                "/" + std::to_string(m_deferred_budget.max_operations) +
                " operations and " + std::to_string(m_deferred_bytes) + "/" +
                std::to_string(m_deferred_budget.max_bytes) + " bytes held",
            "waiting on actor " + awaiting.actor.to_string() +
                " counter " + std::to_string(awaiting.counter));
        if (!deferred_budget_exhausted(m_quarantine_budget, m_quarantined.size(),
                                       m_quarantined_bytes, bytes)) {
            m_quarantined.push_back(QuarantinedIdListOp{op, reason});
            m_quarantined_bytes += bytes;
        }
        // The offered operation is a const reference: ownership never transfers
        // on refusal. The store/transport must retain it until accepted.

        return ApplyOutcome::rejected(reason);
    }

    enqueue_deferred(IdListOpEntry{get_idlist_op_identity(op), op});
    m_deferred_count += 1;
    m_deferred_bytes += bytes;
    return ApplyOutcome::deferred(awaiting);
}

ApplyOutcome IdList::apply_counter_gap(const CounterGap& gap) {
    auto outcome = m_counter_gaps.check(gap, m_version);
    if (outcome.kind() != ApplyOutcomeKind::applied) return outcome;
    CounterGapLedger offered; Global prior = gap.prior; offered.commit(gap, prior);
    bool conflict = false;
    m_deferred_queue.for_each([&](const auto& entry) { conflict |= gap_conflicts(offered, entry.op); });
    for (const auto& entry : m_quarantined) conflict |= gap_conflicts(offered, entry.op);
    if (conflict) return ApplyOutcome::rejected(CounterGapLedger::conflict());
    m_counter_gaps.commit(gap, m_version);
    observe_remote({gap.actor, gap.first + gap.count - 1});
    retry_deferred();
    return ApplyOutcome::applied();
}

ApplyOutcome IdList::apply_one(const IdListOperation& op) {
    const Identity ts = get_idlist_op_identity(op);

    // Identity first, and deliberately before the duplicate check: `observed()`
    // is vacuously true for counter 0, so an operation with no usable identity
    // would be reported as a duplicate no-op forever instead of being refused.
    if (const ValidationResult identity = validate_idlist_operation_identity(op);
        !identity.ok()) {
        return ApplyOutcome::rejected(identity.error);
    }

    if (gap_conflicts(m_counter_gaps, op)) return ApplyOutcome::rejected(CounterGapLedger::conflict());
    if (m_version.observed(ts)) return ApplyOutcome::duplicate();

    // Validity before causality: an operation that can never be applied is
    // refused now rather than queued behind a dependency that cannot help.
    const ValidationResult verdict = validate_idlist_operation(op, m_version);
    switch (verdict.verdict) {
    case ValidationVerdict::rejected:
        return ApplyOutcome::rejected(verdict.error);
    case ValidationVerdict::deferred:
        return defer_or_quarantine(op, *verdict.awaiting);
    case ValidationVerdict::valid:
        break;
    }

    m_apply_error.reset();
    if (!try_apply(op)) {
        // An element attached to something this list does not hold is not a causal
        // wait: say so now instead of parking it behind a prerequisite that cannot help.
        if (m_apply_error) return ApplyOutcome::rejected(*m_apply_error);
        return defer_or_quarantine(op, ts);
    }
    return ApplyOutcome::applied();
}

bool IdList::try_apply(const IdListOperation& op) {
    // Check causal dependencies satisfied
    if (!m_version.observed_all(get_idlist_op_version(op))) return false;
    return visit_operation([&](const auto& concrete) -> bool {
        return apply_concrete(concrete);
    }, op);
}

void IdList::retry_deferred() {
    bool progress = true;
    while (progress) {
        progress = false;
        IdListOperationQueue remaining;
        size_t remaining_count = 0;
        size_t remaining_bytes = 0;
        m_deferred_queue.for_each([&](const IdListOpEntry& entry) {
            if (m_version.observed(get_idlist_op_identity(entry.op))) {
                // Already applied while it waited. Not a visible change, and
                // apply_concrete would not fire for it either.
                progress = true;
                return;
            }
            m_apply_error.reset();
            if (try_apply(entry.op)) {
                progress = true;
            } else if (m_apply_error) {
                // It waited for its prerequisites and then proved unappliable.
                progress = true;
                const size_t bytes = estimate_operation_bytes(entry.op);
                if (!deferred_budget_exhausted(m_quarantine_budget, m_quarantined.size(),
                                               m_quarantined_bytes, bytes)) {
                    m_quarantined.push_back(QuarantinedIdListOp{entry.op, *m_apply_error});
                    m_quarantined_bytes += bytes;
                }
            } else {
                remaining.push_item(entry);
                ++remaining_count;
                remaining_bytes += estimate_operation_bytes(entry.op);
            }
        });
        m_deferred_queue = std::move(remaining);
        m_deferred_count = remaining_count;
        m_deferred_bytes = remaining_bytes;
    }
}

void IdList::enqueue_deferred(IdListOpEntry entry) {
    m_deferred_queue.push_item(std::move(entry));
}

bool IdList::apply_concrete(const IdListInsertOp& op) {
    auto entries = get_entries();
    for (const auto *dependency : {&op.link.parent, &op.link.right_origin}) {
        if (*dependency &&
            std::none_of(entries.begin(), entries.end(), [&](const auto &entry) {
                return entry.origin == **dependency;
            }) &&
            operation_is_covered(m_reclamation.operation_frontier,
                                 **dependency)) {
            m_apply_error = Error(
                ErrorCode::history_unavailable,
                "inserted element depends on a reclaimed placement identity",
                "placement dependency " + (*dependency)->actor.to_string() +
                    "/" + std::to_string((*dependency)->counter));
            return false;
        }
    }
    IdListEntry e(op.timestamp, op.link, op.id);
    if (!SequenceOrder::integrate(entries, std::move(e))) {
        m_apply_error = Error(ErrorCode::malformed,
                              "inserted element attaches to an element that is not in the list",
                              "element " + op.timestamp.actor.to_string() + "/" +
                                  std::to_string(op.timestamp.counter));
        return false;
    }
    observe_remote(op.timestamp);
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
        if (e.origin == op.target_origin) {
            e.deletions.push_back(op.timestamp);
            found = true;
            break;
        }
    }
    if (!found) {
        // The causal gate said this removal's target should already be here. If
        // it is not, the operation is not applicable yet and must not be
        // recorded as applied: the old code asserted and then observed the
        // clock and version anyway, which advanced coverage over a removal that
        // never happened.
        if (operation_is_covered(m_reclamation.operation_frontier,
                                 op.target_origin))
            m_apply_error = Error(
                ErrorCode::history_unavailable,
                "remove targets an element reclaimed at the certified frontier");
        return false;
    }
    observe_remote(op.timestamp);
    m_version.observe(op.timestamp);
    m_version.join(op.version);
    set_entries(std::move(entries));
    if (m_on_change) m_on_change();
    return true;
}

bool IdList::apply_concrete(const IdListUndoOpVariant& op) {
    for (const auto &[edit_id, count] : op.counts) {
        (void)count;
        if (operation_is_covered(m_reclamation.operation_frontier, edit_id)) {
            m_apply_error = Error(
                ErrorCode::history_unavailable,
                "undo targets an operation at or below the certified reclamation frontier",
                "undo target " + edit_id.actor.to_string() + "/" +
                    std::to_string(edit_id.counter));
            return false;
        }
    }
    for (auto& [edit_id, count] : op.counts) {
        m_undo_map.insert(UndoMapEntry{{edit_id, op.timestamp}, count});
    }
    observe_remote(op.timestamp);
    m_version.observe(op.timestamp);
    m_version.join(op.version);
    auto entries = get_entries();
    set_entries(std::move(entries));
    if (m_on_change) m_on_change();
    return true;
}

bool IdList::apply_remote_op(const IdListOperation& op) {
    // Legacy signature: it cannot express four outcomes, so it reports only
    // "was this refused?". It deliberately still returns true for a duplicate,
    // because re-delivery is a success from the caller's point of view and
    // changing that would break callers that treat the result as "accepted".
    // Use apply_remote_op_checked() to see the distinction.
    return apply_remote_op_checked(op).kind() != ApplyOutcomeKind::rejected;
}

ApplyOutcome IdList::apply_remote_op_checked(const IdListOperation& op) {
    const auto outcomes = apply_ops({op});
    return outcomes.empty() ? ApplyOutcome::rejected(
                                  Error(ErrorCode::malformed,
                                        "apply_remote_op produced no outcome"))
                            : outcomes.front();
}

} // namespace CollabText::Crdt
