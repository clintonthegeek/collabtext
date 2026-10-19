/// src/IdListEngine.cpp — the IdListEngine facade over Crdt::IdList.
///
/// Roadmap task A02a. Deliberately a thin forwarder: every method is one call
/// into the engine, and nothing here adds policy. A facade that started
/// deciding things would be a second implementation of the list, and the list
/// already has one.

#include "collabtext/IdListEngine.h"

#include "crdt/IdList.h"

#include <utility>

namespace CollabText {

class IdListEngine::Impl {
public:
    explicit Impl(Crdt::SharedIdentityAllocator allocator)
        : list(std::move(allocator)) {}

    Crdt::IdList list;
    std::optional<PositionScope> scope;
    bool position_history_available = true;
    bool callback_active = false;
    struct CallbackScope {
        bool& active; bool prior;
        explicit CallbackScope(bool& flag) : active(flag), prior(flag) { active = true; }
        ~CallbackScope() { active = prior; }
    };
};

IdListEngine::IdListEngine(Crdt::SharedIdentityAllocator allocator)
    : m_impl(std::make_unique<Impl>(std::move(allocator))) {}

IdListEngine::~IdListEngine() = default;
IdListEngine::IdListEngine(IdListEngine &&) noexcept = default;
IdListEngine &IdListEngine::operator=(IdListEngine &&) noexcept = default;

ApplyOutcome IdListEngine::apply_counter_gap(const PositionScope& scope, std::string_view bytes) {
    auto gap = Crdt::decode_counter_gap(bytes); if (!gap) return ApplyOutcome::rejected(gap.error());
    auto canonical = Crdt::encode_counter_gap(gap.value()); if (!canonical) return ApplyOutcome::rejected(canonical.error());
    if (canonical.value() != bytes) return ApplyOutcome::rejected(Error(ErrorCode::malformed, "counter gap bytes are not canonical"));
    return apply_counter_gap(scope, gap.value());
}

ApplyOutcome IdListEngine::apply_counter_gap(const PositionScope& scope, const Crdt::CounterGap& gap) {
    if (m_impl->callback_active) return ApplyOutcome::rejected(Error(ErrorCode::reentrant_mutation, "mutation during callback"));
    if (!m_impl->scope) return ApplyOutcome::rejected(Error(ErrorCode::unknown_stream, "engine has no bound scope"));
    const auto& expected = *m_impl->scope;
    if (scope.document != expected.document) return ApplyOutcome::rejected(Error(ErrorCode::wrong_document, "counter gap document mismatch"));
    if (scope.stream != expected.stream) return ApplyOutcome::rejected(Error(ErrorCode::wrong_stream, "counter gap stream mismatch"));
    if (scope.primitive != expected.primitive) return ApplyOutcome::rejected(Error(ErrorCode::wrong_primitive, "counter gap primitive mismatch"));
    if (scope.epoch != expected.epoch) return ApplyOutcome::rejected(Error(ErrorCode::wrong_epoch, "counter gap epoch mismatch"));
    if (!m_impl->position_history_available) return ApplyOutcome::rejected(Error(ErrorCode::history_unavailable, "local view was discarded"));
    return m_impl->list.apply_counter_gap(gap);
}

Result<std::unique_ptr<IdListEngine>> IdListEngine::create(Crdt::SharedIdentityAllocator allocator, PositionScope scope) {
    auto valid = detail::validate_scope(scope, Primitive::list);
    if (!valid) return valid.error();
    if (!allocator || !allocator->actor().valid()) return Error(ErrorCode::invalid_identity, "invalid allocator");
    auto engine = std::make_unique<IdListEngine>(std::move(allocator));
    engine->m_impl->scope = std::move(scope);
    return engine;
}
Result<ListBoundary> IdListEngine::boundary_at(uint32_t offset, Affinity affinity) const {
    if (!m_impl->scope) return Error(ErrorCode::unknown_stream, "engine has no bound position scope");
    if (!m_impl->position_history_available) return Error(ErrorCode::history_unavailable, "local history was cleared");
    auto items = m_impl->list.position_items(); if (!items) return items.error();
    auto p = detail::mint_boundary(*m_impl->scope, m_impl->list.version(), items.value(), offset, affinity);
    if (!p) return p.error();
    return detail::PositionAccess::make<ListBoundary>(std::move(p).value());
}
Result<ListElement> IdListEngine::element_of(uint64_t value) const {
    if (!m_impl->scope) return Error(ErrorCode::unknown_stream, "engine has no bound position scope");
    if (!m_impl->position_history_available) return Error(ErrorCode::history_unavailable, "local history was cleared");
    auto items = m_impl->list.position_items(); if (!items) return items.error();
    for (const auto& item : items.value()) if (item.visible && item.application_value == value) {
        detail::PositionPayload p{*m_impl->scope, detail::PositionKind::list_element};
        p.referent = item.identity; p.frontier = m_impl->list.version();
        auto valid = detail::validate_position(p); if (!valid) return valid.error();
        return detail::PositionAccess::make<ListElement>(std::move(p));
    }
    return Error(ErrorCode::not_found, "application value has no visible list element");
}
Result<std::vector<VisibleIdListEntry>> IdListEngine::visible_entries() const {
    if (!m_impl->position_history_available) return Error(ErrorCode::history_unavailable, "local view was discarded");
    auto items = m_impl->list.position_items(); if (!items) return items.error();
    std::vector<VisibleIdListEntry> result;
    for (const auto& item : items.value()) if (item.visible) {
        if (!item.application_value) return Error(ErrorCode::history_unavailable, "retained list application value is unavailable");
        result.push_back({*item.application_value, item.identity});
    }
    return result;
}
Result<Crdt::Anchor> IdListEngine::try_anchor_of(const Crdt::Identity& identity, Crdt::Bias bias) const {
    if (!identity.actor.valid() || !identity.counter || identity.counter == UINT64_MAX) return Error(ErrorCode::invalid_identity, "invalid entry identity");
    auto entries = visible_entries(); if (!entries) return entries.error();
    for (const auto& entry : entries.value()) if (entry.identity == identity) return Crdt::Anchor{identity,bias};
    return Error(ErrorCode::unresolvable_anchor, "identity has no visible list entry");
}
Result<ListElement> IdListEngine::element_of(const Crdt::Identity& identity) const {
    if (!m_impl->scope) return Error(ErrorCode::unknown_stream, "engine has no bound position scope");
    auto anchor = try_anchor_of(identity); if (!anchor) return anchor.error();
    detail::PositionPayload p{*m_impl->scope, detail::PositionKind::list_element};
    p.referent = identity; p.frontier = m_impl->list.version();
    auto valid = detail::validate_position(p); if (!valid) return valid.error();
    return detail::PositionAccess::make<ListElement>(std::move(p));
}

Result<ResolvedListPosition> IdListEngine::resolve(const ListBoundary& token) const {
    if (!m_impl->scope) return Error(ErrorCode::unknown_stream, "engine has no bound position scope");
    auto p = detail::PositionAccess::get(token);
    auto valid = detail::check_position_context(*m_impl->scope, p, m_impl->list.version(), m_impl->position_history_available);
    if (!valid) return valid.error();
    auto items = m_impl->list.position_items(); if (!items) return items.error();
    auto result = detail::resolve_boundary(*p, items.value()); if (!result) return result.error();
    const auto& value = result.value();
    return ResolvedListPosition{value.byte_offset, value.lost_left, value.lost_right};
}
Result<ResolvedListPosition> IdListEngine::resolve(const ListElement& token) const {
    if (!m_impl->scope) return Error(ErrorCode::unknown_stream, "engine has no bound position scope");
    auto p = detail::PositionAccess::get(token);
    auto valid = detail::check_position_context(*m_impl->scope, p, m_impl->list.version(), m_impl->position_history_available);
    if (!valid) return valid.error();
    auto items = m_impl->list.position_items(); if (!items) return items.error();
    return detail::resolve_element(*p, items.value());
}
Result<int> IdListEngine::compare(const ListBoundary& a, const ListBoundary& b) const {
    auto x = resolve(a); if (!x) return x.error();
    auto y = resolve(b); if (!y) return y.error();
    return x.value().element_index < y.value().element_index ? -1 : x.value().element_index > y.value().element_index ? 1 : 0;
}

Result<Crdt::IdListOperation> IdListEngine::try_insert_after(
    const Crdt::Anchor &after, uint64_t id) {
    if (m_impl->callback_active) return Error(ErrorCode::reentrant_mutation, "mutation during callback");
    return m_impl->list.try_insert_after(after, id);
}

Result<Crdt::IdListOperation> IdListEngine::try_remove_at(const Crdt::Anchor &target) {
    if (m_impl->callback_active) return Error(ErrorCode::reentrant_mutation, "mutation during callback");
    return m_impl->list.try_remove_at(target);
}

std::vector<uint64_t> IdListEngine::ids() const { return m_impl->list.ids(); }
uint32_t IdListEngine::size() const { return m_impl->list.size(); }
size_t IdListEngine::tombstone_count() const { return m_impl->list.tombstone_count(); }
size_t IdListEngine::entry_count() const { return m_impl->list.entry_count(); }

Result<Crdt::Anchor> IdListEngine::try_anchor_of(uint64_t id, Crdt::Bias bias) const {
    return m_impl->list.try_anchor_of(id, bias);
}

const Crdt::Global &IdListEngine::version() const { return m_impl->list.version(); }
const ActorId &IdListEngine::actor_id() const { return m_impl->list.actor_id(); }

std::vector<ApplyOutcome> IdListEngine::apply_ops(
    const std::vector<Crdt::IdListOperation> &ops) {
    if (m_impl->callback_active) return std::vector<ApplyOutcome>(ops.size(), ApplyOutcome::rejected(Error(ErrorCode::reentrant_mutation, "mutation during callback")));
    return m_impl->list.apply_ops(ops);
}

void IdListEngine::set_on_local_op(LocalOpCallback cb) {
    auto* impl = m_impl.get();
    m_impl->list.set_on_local_op(cb ? LocalOpCallback([impl, cb = std::move(cb)](const auto& op) { auto callback = cb; Impl::CallbackScope guard(impl->callback_active); callback(op); }) : LocalOpCallback{});
}
void IdListEngine::set_on_change(ChangeCallback cb) {
    auto* impl = m_impl.get();
    m_impl->list.set_on_change(cb ? ChangeCallback([impl, cb = std::move(cb)] { auto callback = cb; Impl::CallbackScope guard(impl->callback_active); callback(); }) : ChangeCallback{});
}

Result<std::optional<Crdt::IdListOperation>> IdListEngine::try_undo() {
    if (m_impl->callback_active) return Error(ErrorCode::reentrant_mutation, "mutation during callback");
    return m_impl->list.try_undo();
}
Result<std::optional<Crdt::IdListOperation>> IdListEngine::try_redo() {
    if (m_impl->callback_active) return Error(ErrorCode::reentrant_mutation, "mutation during callback");
    return m_impl->list.try_redo();
}
Result<std::optional<Crdt::IdListOperation>> IdListEngine::try_undo_group(
    const Crdt::Identity &group_id) {
    if (m_impl->callback_active) return Error(ErrorCode::reentrant_mutation, "mutation during callback");
    return m_impl->list.try_undo_group(group_id);
}
Result<std::optional<Crdt::IdListOperation>> IdListEngine::try_redo_group(
    const Crdt::Identity &group_id) {
    if (m_impl->callback_active) return Error(ErrorCode::reentrant_mutation, "mutation during callback");
    return m_impl->list.try_redo_group(group_id);
}
std::optional<Crdt::IdListOperation> IdListEngine::undo() { auto result = try_undo(); return result ? result.value() : std::nullopt; }
std::optional<Crdt::IdListOperation> IdListEngine::redo() { auto result = try_redo(); return result ? result.value() : std::nullopt; }
size_t IdListEngine::undo_depth() const { return m_impl->list.undo_depth(); }
Crdt::LocalHistorySnapshot IdListEngine::local_history_snapshot() const {
    return m_impl->list.local_history_snapshot();
}
Result<void> IdListEngine::restore_local_history(
    const Crdt::LocalHistorySnapshot &snapshot) {
    if (m_impl->callback_active) return Error(ErrorCode::reentrant_mutation, "mutation during callback");
    return m_impl->list.restore_local_history(snapshot);
}
std::optional<Crdt::LocalUndoEntrySnapshot>
IdListEngine::next_undo_group() const {
    return m_impl->list.next_undo_group();
}
std::optional<Crdt::LocalUndoEntrySnapshot>
IdListEngine::next_redo_group() const {
    return m_impl->list.next_redo_group();
}
Result<void> IdListEngine::acknowledge_undo_retirements(
    const std::vector<Crdt::Identity> &group_ids) {
    if (m_impl->callback_active) return Error(ErrorCode::reentrant_mutation, "mutation during callback");
    return m_impl->list.acknowledge_undo_retirements(group_ids);
}
bool IdListEngine::coalesce_last_undo() { return !m_impl->callback_active && m_impl->list.coalesce_last_undo(); }
Result<void> IdListEngine::set_max_undo_depth(size_t depth) {
    if (m_impl->callback_active) return Error(ErrorCode::reentrant_mutation, "mutation during callback");
    return m_impl->list.set_max_undo_depth(depth);
}
size_t IdListEngine::max_undo_depth() const { return m_impl->list.max_undo_depth(); }
Result<void> IdListEngine::set_max_undo_bytes(size_t bytes) {
    if (m_impl->callback_active) return Error(ErrorCode::reentrant_mutation, "mutation during callback");
    return m_impl->list.set_max_undo_bytes(bytes);
}
size_t IdListEngine::max_undo_bytes() const {
    return m_impl->list.max_undo_bytes();
}
size_t IdListEngine::undo_history_bytes() const {
    return m_impl->list.undo_history_bytes();
}

void IdListEngine::discard_local_view() noexcept {
    if (m_impl->callback_active) return;
    m_impl->list.discard_local_view();
    m_impl->position_history_available = false;
}

}  // namespace CollabText
