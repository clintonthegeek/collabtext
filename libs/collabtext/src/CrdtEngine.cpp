#include "collabtext/CrdtEngine.h"
#include "crdt/Buffer.h"
#include "crdt/Utf16.h"

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
void CrdtEngine::local_clear() noexcept {
    m_impl->buffer.local_clear();
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
    uint32_t byte_start = static_cast<uint32_t>(
        Crdt::utf16_to_byte_offset(current, position));
    uint32_t byte_end = static_cast<uint32_t>(
        Crdt::utf16_to_byte_offset(current, position + length));
    auto op = m_impl->buffer.apply_local_edit({{byte_start, byte_end}}, {""});
    m_impl->emit_op(op);
    m_impl->notify();
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
    auto op = m_impl->buffer.undo();
    if (!op) return false;
    m_impl->emit_op(*op);
    m_impl->notify();
    return true;
}

bool CrdtEngine::redo()
{
    auto op = m_impl->buffer.redo();
    if (!op) return false;
    m_impl->emit_op(*op);
    m_impl->notify();
    return true;
}

void CrdtEngine::setOnChange(ChangeCallback cb)
{
    m_impl->on_change = std::move(cb);
}

void CrdtEngine::setOnLocalOp(LocalOpCallback cb)
{
    m_impl->on_local_op = std::move(cb);
}

bool CrdtEngine::applyRemoteOp(const Crdt::Operation &op)
{
    const auto outcomes = m_impl->buffer.apply_ops({op});
    if (!outcomes.empty() && outcomes.front().kind() != ApplyOutcomeKind::applied)
        return false;
    m_impl->notify();
    // Duplicate counts as handled: the operation is already in the document, so
    // telling a caller to retry it would make it retry forever.
    return true;
}

} // namespace CollabText
