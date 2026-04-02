#include "collabtext/CrdtEngine.h"
#include "crdt/Buffer.h"
#include "crdt/Utf16.h"

namespace CollabText {

struct CrdtEngine::Impl {
    Crdt::Buffer buffer;
    ChangeCallback on_change;

    explicit Impl(uint16_t rid) : buffer(rid) {}

    void notify() {
        if (on_change) on_change();
    }
};

CrdtEngine::CrdtEngine(uint16_t replica_id)
    : m_impl(std::make_unique<Impl>(replica_id))
{
}

CrdtEngine::~CrdtEngine() = default;

void CrdtEngine::insert(int position, const std::string &text)
{
    uint32_t byte_offset = static_cast<uint32_t>(
        Crdt::utf16_to_byte_offset(m_impl->buffer.text(), position));
    m_impl->buffer.apply_local_edit({{byte_offset, byte_offset}}, {text});
    m_impl->notify();
}

void CrdtEngine::remove(int position, int length)
{
    const std::string current = m_impl->buffer.text();
    uint32_t byte_start = static_cast<uint32_t>(
        Crdt::utf16_to_byte_offset(current, position));
    uint32_t byte_end = static_cast<uint32_t>(
        Crdt::utf16_to_byte_offset(current, position + length));
    m_impl->buffer.apply_local_edit({{byte_start, byte_end}}, {""});
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
    m_impl->notify();
    return true;
}

bool CrdtEngine::redo()
{
    auto op = m_impl->buffer.redo();
    if (!op) return false;
    m_impl->notify();
    return true;
}

void CrdtEngine::setOnChange(ChangeCallback cb)
{
    m_impl->on_change = std::move(cb);
}

} // namespace CollabText
