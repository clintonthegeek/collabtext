#include "collabtext/CrdtEngine.h"
#include "crdt/Buffer.h"
#include "crdt/Utf16.h"

#include <algorithm>
#include <limits>

namespace CollabText {

struct CrdtEngine::Impl {
    Crdt::Buffer buffer;
    ChangeCallback on_change;
    LocalOpCallback on_local_op;
    std::optional<PositionScope> scope;
    bool position_history_available = true;

    explicit Impl(Crdt::SharedIdentityAllocator allocator) : buffer(std::move(allocator)) {}

    void notify() {
        if (on_change) on_change();
    }

    void emit_op(const Crdt::Operation &op) {
        if (on_local_op) on_local_op(op);
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
Result<int> CrdtEngine::compare(const TextBoundary& a, const TextBoundary& b) const {
    auto x = resolve(a); if (!x) return x.error();
    auto y = resolve(b); if (!y) return y.error();
    return x.value().byte_offset < y.value().byte_offset ? -1 : x.value().byte_offset > y.value().byte_offset ? 1 : 0;
}
void CrdtEngine::discard_local_view() noexcept {
    m_impl->buffer.discard_local_view();
    m_impl->position_history_available = false;
}

void CrdtEngine::insert(int position, const std::string &text)
{
    uint32_t byte_offset = static_cast<uint32_t>(
        Crdt::utf16_to_byte_offset(m_impl->buffer.text(), position));
    auto op = m_impl->buffer.apply_local_edit({{byte_offset, byte_offset}}, {text});
    m_impl->emit_op(op);
    m_impl->notify();
}

void CrdtEngine::remove(int position, int length)
{
    const std::string current = m_impl->buffer.text();
    if (current.size() > std::numeric_limits<uint32_t>::max())
        return;
    uint32_t byte_start = static_cast<uint32_t>(
        Crdt::utf16_to_byte_offset(current, position));
    uint32_t byte_end = static_cast<uint32_t>(
        Crdt::utf16_to_byte_offset(current, position + length));
    auto op = m_impl->buffer.apply_local_edit({{byte_start, byte_end}}, {""});
    m_impl->emit_op(op);
    m_impl->notify();
}

Result<Crdt::Operation> CrdtEngine::try_replace(
    int position, int length, const std::string &text)
{
    const std::string current = m_impl->buffer.text();
    if (current.size() > std::numeric_limits<uint32_t>::max())
        return Error(ErrorCode::malformed,
                     "document byte length exceeds the operation offset range");
    const auto current_length = Crdt::utf16_length(current);
    if (position < 0 || length < 0 ||
        static_cast<uint64_t>(position) > current_length ||
        static_cast<uint64_t>(length) > current_length - static_cast<uint64_t>(position))
        return Error(ErrorCode::malformed, "text edit range is outside the document");

    const auto start16 = static_cast<size_t>(position);
    const auto end16 = start16 + static_cast<size_t>(length);
    const auto start_byte = Crdt::utf16_to_byte_offset(current, start16);
    const auto end_byte = Crdt::utf16_to_byte_offset(current, end16);
    if (Crdt::byte_to_utf16_offset(current, start_byte) != start16 ||
        Crdt::byte_to_utf16_offset(current, end_byte) != end16)
        return Error(ErrorCode::malformed,
                     "text edit range splits a UTF-16 surrogate pair");

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
    return static_cast<int>(Crdt::utf16_length(m_impl->buffer.text()));
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
    auto result = m_impl->buffer.try_undo();
    if (!result.ok()) return result.error();
    if (result.value()) {
        m_impl->emit_op(*result.value());
        m_impl->notify();
    }
    return std::move(result).value();
}

Result<std::optional<Crdt::Operation>> CrdtEngine::try_redo() {
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
    return m_impl->buffer.set_max_undo_depth(depth);
}
Result<void> CrdtEngine::set_max_undo_bytes(size_t bytes) {
    return m_impl->buffer.set_max_undo_bytes(bytes);
}

void CrdtEngine::setOnChange(ChangeCallback cb)
{
    m_impl->on_change = std::move(cb);
}

void CrdtEngine::setOnLocalOp(LocalOpCallback cb)
{
    m_impl->on_local_op = std::move(cb);
}

std::vector<ApplyOutcome> CrdtEngine::apply_ops(
    const std::vector<Crdt::Operation>& operations) {
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
