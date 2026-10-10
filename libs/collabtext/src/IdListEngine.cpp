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
};

IdListEngine::IdListEngine(Crdt::SharedIdentityAllocator allocator)
    : m_impl(std::make_unique<Impl>(std::move(allocator))) {}

IdListEngine::~IdListEngine() = default;
IdListEngine::IdListEngine(IdListEngine &&) noexcept = default;
IdListEngine &IdListEngine::operator=(IdListEngine &&) noexcept = default;

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
    return m_impl->list.try_insert_after(after, id);
}

Result<Crdt::IdListOperation> IdListEngine::try_remove_at(const Crdt::Anchor &target) {
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
    return m_impl->list.apply_ops(ops);
}

void IdListEngine::set_on_local_op(LocalOpCallback cb) {
    m_impl->list.set_on_local_op(std::move(cb));
}
void IdListEngine::set_on_change(ChangeCallback cb) {
    m_impl->list.set_on_change(std::move(cb));
}

Result<std::optional<Crdt::IdListOperation>> IdListEngine::try_undo() {
    return m_impl->list.try_undo();
}
Result<std::optional<Crdt::IdListOperation>> IdListEngine::try_redo() {
    return m_impl->list.try_redo();
}
std::optional<Crdt::IdListOperation> IdListEngine::undo() { return m_impl->list.undo(); }
std::optional<Crdt::IdListOperation> IdListEngine::redo() { return m_impl->list.redo(); }
size_t IdListEngine::undo_depth() const { return m_impl->list.undo_depth(); }
Crdt::LocalHistorySnapshot IdListEngine::local_history_snapshot() const {
    return m_impl->list.local_history_snapshot();
}
Result<void> IdListEngine::restore_local_history(
    const Crdt::LocalHistorySnapshot &snapshot) {
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
    return m_impl->list.acknowledge_undo_retirements(group_ids);
}
bool IdListEngine::coalesce_last_undo() { return m_impl->list.coalesce_last_undo(); }
Result<void> IdListEngine::set_max_undo_depth(size_t depth) {
    return m_impl->list.set_max_undo_depth(depth);
}
size_t IdListEngine::max_undo_depth() const { return m_impl->list.max_undo_depth(); }
Result<void> IdListEngine::set_max_undo_bytes(size_t bytes) {
    return m_impl->list.set_max_undo_bytes(bytes);
}
size_t IdListEngine::max_undo_bytes() const {
    return m_impl->list.max_undo_bytes();
}
size_t IdListEngine::undo_history_bytes() const {
    return m_impl->list.undo_history_bytes();
}

void IdListEngine::local_clear() noexcept {
    m_impl->list.local_clear();
    m_impl->position_history_available = false;
}

}  // namespace CollabText
