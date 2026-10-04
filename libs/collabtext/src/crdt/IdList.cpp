#include "crdt/IdList.h"
#include <cassert>

namespace CollabText::Crdt {

IdList::IdList(uint16_t replica_id)
    : m_replica_id(replica_id)
    , m_clock(replica_id, 1)
{}

size_t IdList::find_origin(const std::vector<IdListEntry>& entries,
                           const Anchor& target) const {
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].origin.replica_id == target.replica_id &&
            entries[i].origin.value == target.char_value) {
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
                         std::to_string(after.replica_id) + " counter " +
                         std::to_string(after.char_value) +
                         ", which is not in this list");
    }
    return insert_after_resolved(after, id);
}

IdListOperation IdList::insert_after_resolved(const Anchor& after, uint64_t id) {
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
        const size_t found = find_origin(entries, after);
        assert(found != static_cast<size_t>(-1) &&
               "insert_after_resolved: anchor must be resolved first");
        predecessor = entries[found].locator;
        successor = (found + 1 < entries.size()) ? entries[found + 1].locator
                                                 : Locator::max();
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

    // m. Build and emit the operation
    IdListOperation op = IdListInsertOp{ origin, version_before, id, new_loc };
    emit_local_op(op);

    return op;
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
                         std::to_string(target.replica_id) + " counter " +
                         std::to_string(target.char_value) +
                         ", which is not in this list");
    }
    return remove_at_resolved(target);
}

IdListOperation IdList::remove_at_resolved(const Anchor& target) {
    // a. Get entries
    auto entries = get_entries();

    // b. Find entry where origin matches target
    const size_t idx = find_origin(entries, target);
    assert(idx != static_cast<size_t>(-1) &&
           "remove_at_resolved: anchor must be resolved first");

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

    // l. Build and emit the operation
    IdListOperation op = IdListRemoveOp{ del_id, version_before, target_origin };
    emit_local_op(op);

    return op;
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
    emit_local_op(op);
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
    emit_local_op(op);
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

size_t IdList::collect_garbage() {
    // Local history cannot prove that remote redo or anchors have expired.
    // Retain all restoration state until certified reclamation is available.
    return 0;
}

size_t IdList::compact(const Global& /*watermark*/) {
    // Observing a deletion does not release a replica's undo/redo obligations.
    // A raw clock is not a reclamation certificate; retain identities and data.
    return 0;
}

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

void IdList::local_clear() noexcept {
    m_entry_tree = IdListTree{};
    m_undo_map.clear();
    m_undo_stack.clear();
    m_undo_cursor = 0;
    m_deferred_queue = IdListOperationQueue{};
    m_deferred_count = 0;
    m_deferred_bytes = 0;
    m_quarantined.clear();
    // Intentionally preserved: m_replica_id, m_clock, m_version,
    // m_max_undo_depth, m_on_change, m_on_local_op.
    // Intentionally not fired: m_on_change, m_on_local_op.
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

ApplyOutcome IdList::defer_or_quarantine(const IdListOperation& op,
                                         Lamport awaiting) {
    const size_t bytes = estimate_operation_bytes(op);

    if (deferred_budget_exhausted(m_deferred_budget, m_deferred_count,
                                   m_deferred_bytes, bytes)) {
        const Error reason(
            ErrorCode::deferred_queue_full,
            "deferral budget exhausted: " + std::to_string(m_deferred_count) +
                "/" + std::to_string(m_deferred_budget.max_operations) +
                " operations and " + std::to_string(m_deferred_bytes) + "/" +
                std::to_string(m_deferred_budget.max_bytes) + " bytes held",
            "waiting on actor " + std::to_string(awaiting.replica_id) +
                " counter " + std::to_string(awaiting.value));
        m_quarantined.push_back(QuarantinedIdListOp{op, reason});
        return ApplyOutcome::rejected(reason);
    }

    enqueue_deferred(IdListOpEntry{get_idlist_op_timestamp(op), op});
    m_deferred_count += 1;
    m_deferred_bytes += bytes;
    return ApplyOutcome::deferred(awaiting);
}

ApplyOutcome IdList::apply_one(const IdListOperation& op) {
    const Lamport ts = get_idlist_op_timestamp(op);

    // Identity first, and deliberately before the duplicate check: `observed()`
    // is vacuously true for counter 0, so an operation with no usable identity
    // would be reported as a duplicate no-op forever instead of being refused.
    if (const ValidationResult identity = validate_idlist_operation_identity(op);
        !identity.ok()) {
        return ApplyOutcome::rejected(identity.error);
    }

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

    if (!try_apply(op)) {
        return defer_or_quarantine(op, ts);
    }
    return ApplyOutcome::applied();
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
        size_t remaining_count = 0;
        size_t remaining_bytes = 0;
        m_deferred_queue.for_each([&](const IdListOpEntry& entry) {
            if (m_version.observed(get_idlist_op_timestamp(entry.op))) {
                // Already applied while it waited. Not a visible change, and
                // apply_concrete would not fire for it either.
                progress = true;
                return;
            }
            if (try_apply(entry.op)) {
                progress = true;
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
    if (!found) {
        // The causal gate said this removal's target should already be here. If
        // it is not, the operation is not applicable yet and must not be
        // recorded as applied: the old code asserted and then observed the
        // clock and version anyway, which advanced coverage over a removal that
        // never happened.
        return false;
    }
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
