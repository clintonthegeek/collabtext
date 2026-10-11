#include "collabtext/CrdtEngine.h"
#include "crdt/Buffer.h"
#include "crdt/Utf16.h"
#include "crdt/OperationPayload.h"

#include <algorithm>
#include <limits>

namespace CollabText {

namespace detail {
struct CallbackSubscriptionState {
    virtual ~CallbackSubscriptionState() = default;
    virtual void disconnect() noexcept = 0;
    [[nodiscard]] virtual bool connected() const noexcept = 0;
};

template <class Callback>
struct CallbackSlotState final : CallbackSubscriptionState {
    explicit CallbackSlotState(Callback cb) : callback(std::move(cb)) {}
    void disconnect() noexcept override {
        active = false;
        callback = {};
    }
    [[nodiscard]] bool connected() const noexcept override { return active; }
    bool active = true;
    Callback callback;
};
} // namespace detail

CallbackSubscription::CallbackSubscription(
    std::weak_ptr<detail::CallbackSubscriptionState> state) noexcept
    : m_state(std::move(state)) {}

CallbackSubscription::~CallbackSubscription() { reset(); }
CallbackSubscription::CallbackSubscription(CallbackSubscription &&other) noexcept
    : m_state(std::move(other.m_state)) {}
CallbackSubscription &CallbackSubscription::operator=(CallbackSubscription &&other) noexcept {
    if (this != &other) {
        reset();
        m_state = std::move(other.m_state);
    }
    return *this;
}
void CallbackSubscription::reset() noexcept {
    if (auto state = m_state.lock()) state->disconnect();
    m_state.reset();
}
CallbackSubscription::operator bool() const noexcept {
    auto state = m_state.lock();
    return state && state->connected();
}

struct CrdtEngine::Impl {
    Crdt::Buffer buffer;
    std::shared_ptr<detail::CallbackSlotState<ChangeCallback>> on_change;
    std::shared_ptr<detail::CallbackSlotState<LocalOpCallback>> on_local_op;
    std::optional<PositionScope> scope;
    bool position_history_available = true;
    bool callback_active = false;

    struct CallbackScope {
        bool &active;
        explicit CallbackScope(bool &state) : active(state) { active = true; }
        ~CallbackScope() { active = false; }
    };

    explicit Impl(Crdt::SharedIdentityAllocator allocator) : buffer(std::move(allocator)) {}

    void notify() {
        auto slot = on_change;
        if (!slot || !slot->active || !slot->callback) return;
        auto callback = slot->callback;
        CallbackScope scope(callback_active);
        callback();
    }

    void emit_op(const Crdt::Operation &op) {
        auto slot = on_local_op;
        if (!slot || !slot->active || !slot->callback) return;
        auto callback = slot->callback;
        CallbackScope scope(callback_active);
        callback(op);
    }
};

CrdtEngine::CrdtEngine(Crdt::SharedIdentityAllocator allocator)
    : m_impl(std::make_unique<Impl>(std::move(allocator)))
{
}

CrdtEngine::~CrdtEngine() = default;

Result<std::unique_ptr<CrdtEngine>> CrdtEngine::create(Crdt::SharedIdentityAllocator allocator, PositionScope scope) {
    auto valid = detail::validate_scope(scope, Primitive::text);
    if (!valid) return valid.error();
    if (!allocator || !allocator->actor().valid()) return Error(ErrorCode::invalid_identity, "invalid allocator");
    auto engine = std::make_unique<CrdtEngine>(std::move(allocator));
    engine->m_impl->scope = std::move(scope);
    return engine;
}
Result<TextBoundary> CrdtEngine::boundary_at(uint32_t offset, Affinity affinity) const {
    if (!m_impl->scope) return Error(ErrorCode::unknown_stream, "engine has no bound position scope");
    if (!m_impl->position_history_available) return Error(ErrorCode::history_unavailable, "local history was cleared");
    auto items = m_impl->buffer.position_items(); if (!items) return items.error();
    auto p = detail::mint_boundary(*m_impl->scope, m_impl->buffer.version(), items.value(), offset, affinity);
    if (!p) return p.error();
    return detail::PositionAccess::make<TextBoundary>(std::move(p).value());
}
Result<TextAttachment> CrdtEngine::attachment_at(uint32_t offset, Affinity affinity, ReferentSide side) const {
    auto boundary = boundary_at(offset, affinity); if (!boundary) return boundary.error();
    auto p = *detail::PositionAccess::get(boundary.value());
    p.kind = detail::PositionKind::text_attachment; p.selection = side;
    auto valid = detail::validate_position(p); if (!valid) return valid.error();
    return detail::PositionAccess::make<TextAttachment>(std::move(p));
}
Result<ResolvedTextPosition> CrdtEngine::resolve(const TextBoundary& token) const {
    if (!m_impl->scope) return Error(ErrorCode::unknown_stream, "engine has no bound position scope");
    auto p = detail::PositionAccess::get(token);
    auto valid = detail::check_position_context(*m_impl->scope, p, m_impl->buffer.version(), m_impl->position_history_available);
    if (!valid) return valid.error();
    auto items = m_impl->buffer.position_items(); if (!items) return items.error();
    return detail::resolve_boundary(*p, items.value());
}
Result<ResolvedTextPosition> CrdtEngine::resolve(const TextAttachment& token) const {
    if (!m_impl->scope) return Error(ErrorCode::unknown_stream, "engine has no bound position scope");
    auto p = detail::PositionAccess::get(token);
    auto valid = detail::check_position_context(*m_impl->scope, p, m_impl->buffer.version(), m_impl->position_history_available);
    if (!valid) return valid.error();
    auto items = m_impl->buffer.position_items(); if (!items) return items.error();
    return detail::resolve_boundary(*p, items.value());
}
Result<ResolvedTextPosition> CrdtEngine::resolve(const TextMarker& token) const {
    if (!m_impl->scope) return Error(ErrorCode::unknown_stream, "engine has no bound position scope");
    const auto* p = detail::PositionAccess::get(token);
    auto valid = detail::check_position_context(*m_impl->scope, p, m_impl->buffer.version(), m_impl->position_history_available);
    if (!valid) return valid.error();
    auto offset = m_impl->buffer.marker_offset(*p->referent);
    if (!offset) return offset.error();
    return ResolvedTextPosition{offset.value(), false, false};
}

Result<MarkerInsertion> CrdtEngine::try_insert_marker(uint32_t offset) {
    if (m_impl->callback_active) return Error(ErrorCode::reentrant_mutation, "mutation during callback");
    if (!m_impl->scope) return Error(ErrorCode::unknown_stream, "marker requires a bound position scope");
    if (!m_impl->position_history_available) return Error(ErrorCode::history_unavailable, "local view was discarded");
    auto operation = m_impl->buffer.try_insert_marker(offset); if (!operation) return operation.error();
    const auto& edit = Crdt::operation_get<Crdt::EditOperation>(operation.value());
    detail::PositionPayload payload{*m_impl->scope, detail::PositionKind::text_marker};
    payload.frontier = m_impl->buffer.version(); payload.referent = edit.inserted_fragments[0].origin;
    auto marker = detail::PositionAccess::make<TextMarker>(std::move(payload));
    m_impl->emit_op(operation.value()); m_impl->notify();
    return MarkerInsertion{std::move(marker), std::move(operation).value()};
}

Result<MarkerInsertion> CrdtEngine::try_insert_marker(const TextMarker& target, MarkerSide side) {
    if (m_impl->callback_active) return Error(ErrorCode::reentrant_mutation, "mutation during callback");
    auto valid = resolve(target); if (!valid) return valid.error();
    auto operation = m_impl->buffer.try_insert_relative(*detail::PositionAccess::get(target)->referent, side, "", true);
    if (!operation) return operation.error();
    const auto& edit = Crdt::operation_get<Crdt::EditOperation>(operation.value());
    detail::PositionPayload payload{*m_impl->scope, detail::PositionKind::text_marker};
    payload.frontier = m_impl->buffer.version(); payload.referent = edit.inserted_fragments[0].origin;
    auto marker = detail::PositionAccess::make<TextMarker>(std::move(payload));
    m_impl->emit_op(operation.value()); m_impl->notify();
    return MarkerInsertion{std::move(marker), std::move(operation).value()};
}

Result<Crdt::Operation> CrdtEngine::try_insert_at_marker(const TextMarker& target, MarkerSide side, const std::string& text) {
    if (m_impl->callback_active) return Error(ErrorCode::reentrant_mutation, "mutation during callback");
    auto valid = resolve(target); if (!valid) return valid.error();
    auto operation = m_impl->buffer.try_insert_relative(*detail::PositionAccess::get(target)->referent, side, text, false);
    if (!operation) return operation.error();
    m_impl->emit_op(operation.value()); m_impl->notify(); return std::move(operation).value();
}

Result<Crdt::Operation> CrdtEngine::try_remove_marker(const TextMarker& target) {
    if (m_impl->callback_active) return Error(ErrorCode::reentrant_mutation, "mutation during callback");
    auto valid = resolve(target); if (!valid) return valid.error();
    auto operation = m_impl->buffer.try_remove_marker(*detail::PositionAccess::get(target)->referent);
    if (!operation) return operation.error();
    m_impl->emit_op(operation.value()); m_impl->notify(); return std::move(operation).value();
}

Result<int> CrdtEngine::compare(const TextBoundary& a, const TextBoundary& b) const {
    auto x = resolve(a); if (!x) return x.error();
    auto y = resolve(b); if (!y) return y.error();
    return x.value().byte_offset < y.value().byte_offset ? -1 : x.value().byte_offset > y.value().byte_offset ? 1 : 0;
}
void CrdtEngine::discard_local_view() noexcept {
    if (m_impl->callback_active) return;
    m_impl->buffer.discard_local_view();
    m_impl->position_history_available = false;
}

void CrdtEngine::insert(int position, const std::string &text)
{
    (void)try_insert(position, text);
}

void CrdtEngine::remove(int position, int length)
{
    (void)try_remove(position, length);
}

Result<Crdt::Operation> CrdtEngine::try_insert(int position,
                                               const std::string &text) {
    return try_replace(position, 0, text);
}

Result<Crdt::Operation> CrdtEngine::try_remove(int position, int length) {
    return try_replace(position, length, {});
}

Result<Crdt::Operation> CrdtEngine::try_replace(
    int position, int length, const std::string &text)
{
    if (m_impl->callback_active)
        return Error(ErrorCode::reentrant_mutation,
                     "cannot mutate an engine from its notification callback");
    auto range = m_impl->buffer.utf16_range(position, length);
    if (!range) return range.error();
    const auto [start_byte, end_byte] = range.value();

    if (text.size() > std::numeric_limits<uint32_t>::max())
        return Error(ErrorCode::malformed,
                     "inserted text exceeds the operation offset range");

    auto operation = m_impl->buffer.try_apply_local_edit(
        {{static_cast<uint32_t>(start_byte), static_cast<uint32_t>(end_byte)}},
        {text});
    if (!operation.ok()) return operation.error();
    m_impl->emit_op(operation.value());
    m_impl->notify();
    return std::move(operation).value();
}

std::string CrdtEngine::text() const
{
    return m_impl->buffer.text();
}

int CrdtEngine::length() const
{
    return static_cast<int>(m_impl->buffer.utf16_length());
}

bool CrdtEngine::undo()
{
    auto result = try_undo();
    return result.ok() && result.value().has_value();
}

bool CrdtEngine::redo()
{
    auto result = try_redo();
    return result.ok() && result.value().has_value();
}

Result<std::optional<Crdt::Operation>> CrdtEngine::try_undo() {
    if (m_impl->callback_active)
        return Error(ErrorCode::reentrant_mutation,
                     "cannot mutate an engine from its notification callback");
    auto result = m_impl->buffer.try_undo();
    if (!result.ok()) return result.error();
    if (result.value()) {
        m_impl->emit_op(*result.value());
        m_impl->notify();
    }
    return std::move(result).value();
}

Result<std::optional<Crdt::Operation>> CrdtEngine::try_redo() {
    if (m_impl->callback_active)
        return Error(ErrorCode::reentrant_mutation,
                     "cannot mutate an engine from its notification callback");
    auto result = m_impl->buffer.try_redo();
    if (!result.ok()) return result.error();
    if (result.value()) {
        m_impl->emit_op(*result.value());
        m_impl->notify();
    }
    return std::move(result).value();
}

size_t CrdtEngine::undo_depth() const { return m_impl->buffer.undo_depth(); }
size_t CrdtEngine::undo_history_bytes() const { return m_impl->buffer.undo_history_bytes(); }
size_t CrdtEngine::max_undo_depth() const { return m_impl->buffer.max_undo_depth(); }
size_t CrdtEngine::max_undo_bytes() const { return m_impl->buffer.max_undo_bytes(); }
Result<void> CrdtEngine::set_max_undo_depth(size_t depth) {
    if (m_impl->callback_active)
        return Error(ErrorCode::reentrant_mutation,
                     "cannot mutate an engine from its notification callback");
    return m_impl->buffer.set_max_undo_depth(depth);
}
Result<void> CrdtEngine::set_max_undo_bytes(size_t bytes) {
    if (m_impl->callback_active)
        return Error(ErrorCode::reentrant_mutation,
                     "cannot mutate an engine from its notification callback");
    return m_impl->buffer.set_max_undo_bytes(bytes);
}

Result<void> CrdtEngine::set_deferred_operation_budget(
    DeferredOperationBudget budget) {
    if (m_impl->callback_active)
        return Error(ErrorCode::reentrant_mutation,
                     "cannot mutate an engine from its notification callback");
    if (m_impl->buffer.deferred_count() > budget.max_operations ||
        m_impl->buffer.deferred_bytes() > budget.max_bytes)
        return Error(ErrorCode::limit_exceeded,
                     "new deferral budget is below currently retained operations");
    m_impl->buffer.set_deferred_budget(
        Crdt::DeferredBudget{budget.max_operations, budget.max_bytes});
    return {};
}

DeferredOperationBudget CrdtEngine::deferred_operation_budget() const {
    const auto &budget = m_impl->buffer.deferred_budget();
    return {budget.max_operations, budget.max_bytes};
}

size_t CrdtEngine::deferred_operation_count() const {
    return m_impl->buffer.deferred_count();
}

size_t CrdtEngine::deferred_operation_bytes() const {
    return m_impl->buffer.deferred_bytes();
}

void CrdtEngine::setOnChange(ChangeCallback cb)
{
    if (m_impl->on_change) m_impl->on_change->disconnect();
    m_impl->on_change.reset();
    if (cb) m_impl->on_change = std::make_shared<detail::CallbackSlotState<ChangeCallback>>(std::move(cb));
}

Result<CallbackSubscription> CrdtEngine::subscribe_on_change(ChangeCallback cb) {
    if (!cb) return Error(ErrorCode::malformed, "change callback is empty");
    setOnChange(std::move(cb));
    return CallbackSubscription(m_impl->on_change);
}

void CrdtEngine::setOnLocalOp(LocalOpCallback cb)
{
    if (m_impl->on_local_op) m_impl->on_local_op->disconnect();
    m_impl->on_local_op.reset();
    if (cb) m_impl->on_local_op = std::make_shared<detail::CallbackSlotState<LocalOpCallback>>(std::move(cb));
}

Result<CallbackSubscription> CrdtEngine::subscribe_on_local_op(LocalOpCallback cb) {
    if (!cb) return Error(ErrorCode::malformed, "local operation callback is empty");
    setOnLocalOp(std::move(cb));
    return CallbackSubscription(m_impl->on_local_op);
}

std::vector<ApplyOutcome> CrdtEngine::apply_ops(
    const std::vector<Crdt::Operation>& operations) {
    if (m_impl->callback_active) {
        std::vector<ApplyOutcome> outcomes;
        outcomes.reserve(operations.size());
        for (size_t i = 0; i < operations.size(); ++i)
            outcomes.push_back(ApplyOutcome::rejected(Error(
                ErrorCode::reentrant_mutation,
                "cannot mutate an engine from its notification callback")));
        return outcomes;
    }
    auto outcomes = m_impl->buffer.apply_ops(operations);
    if (std::any_of(outcomes.begin(), outcomes.end(), [](const ApplyOutcome& outcome) {
            return outcome.kind() == ApplyOutcomeKind::applied;
        }))
        m_impl->notify();
    return outcomes;
}

ApplyOutcome CrdtEngine::apply_remote(const Crdt::Operation& operation) {
    auto outcomes = apply_ops({operation});
    return outcomes.empty() ? ApplyOutcome::rejected(
        Error(ErrorCode::malformed, "engine returned no apply outcome")) : std::move(outcomes.front());
}

} // namespace CollabText

namespace CollabText {
Crdt::LocalHistorySnapshot CrdtEngine::local_history_snapshot() const { return m_impl->buffer.local_history_snapshot(); }
std::optional<Crdt::LocalUndoEntrySnapshot> CrdtEngine::next_undo_group() const { return m_impl->buffer.next_undo_group(); }
std::optional<Crdt::LocalUndoEntrySnapshot> CrdtEngine::next_redo_group() const { return m_impl->buffer.next_redo_group(); }
bool CrdtEngine::coalesce_last_undo() {
    if (m_impl->callback_active) return false;
    return m_impl->buffer.coalesce_last_undo();
}
Result<void> CrdtEngine::restore_local_history(const Crdt::LocalHistorySnapshot& snapshot) {
    if (m_impl->callback_active) return Error(ErrorCode::reentrant_mutation, "mutation during callback");
    return m_impl->buffer.restore_local_history(snapshot);
}
Result<void> CrdtEngine::acknowledge_undo_retirements(const std::vector<Crdt::Identity>& groups) {
    if (m_impl->callback_active) return Error(ErrorCode::reentrant_mutation, "mutation during callback");
    return m_impl->buffer.acknowledge_undo_retirements(groups);
}
}
