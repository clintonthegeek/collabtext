#include "crdt/IdList.h"
#include "crdt/SequenceOrder.h"
#include <cassert>

namespace CollabText::Crdt {

Result<std::vector<CollabText::detail::PositionItem>> IdList::position_items() const {
    std::vector<CollabText::detail::PositionItem> items;
    for (const auto& entry : get_entries()) {
        if (!entry.origin.actor.valid() || !entry.origin.counter || entry.origin.counter == UINT64_MAX)
            return Error(ErrorCode::history_unavailable, "retained list identity is invalid");
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
    m_undo_stack.resize(m_undo_cursor);  // truncate redo tail
    m_undo_stack.push_back(UndoEntry{ {UndoMapKey(origin)}, {} });
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
    m_undo_stack.resize(m_undo_cursor);
    m_undo_stack.push_back(UndoEntry{ {}, {del_id} });
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
    if (m_undo_cursor == 0) return std::nullopt;

    if (auto usable = check_allocator(); !usable.ok()) return std::nullopt;

    // Reserved before the cursor moves: a refused undo that had already
    // decremented m_undo_cursor would silently drop an entry from the history.
    auto reserved = m_allocator->reserve_run(1);
    if (!reserved.ok()) return std::nullopt;

    m_undo_cursor--;
    auto& entry = m_undo_stack[m_undo_cursor];

    IdListUndoOpVariant op;
    op.version = m_version;
    op.timestamp = Identity(m_allocator->actor(), reserved.value());

    for (auto& key : entry.inserted_keys) {
        const Identity edit_id = key.position;
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

    if (auto usable = check_allocator(); !usable.ok()) return std::nullopt;

    // Reserved before the cursor moves; see undo().
    auto reserved = m_allocator->reserve_run(1);
    if (!reserved.ok()) return std::nullopt;

    auto& entry = m_undo_stack[m_undo_cursor];
    m_undo_cursor++;

    IdListUndoOpVariant op;
    op.version = m_version;
    op.timestamp = Identity(m_allocator->actor(), reserved.value());

    for (auto& key : entry.inserted_keys) {
        const Identity edit_id = key.position;
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
    m_quarantined_bytes = 0;
    // Intentionally preserved: m_allocator, m_version,
    // m_max_undo_depth, m_on_change, m_on_local_op.
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

ApplyOutcome IdList::apply_one(const IdListOperation& op) {
    const Identity ts = get_idlist_op_identity(op);

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
