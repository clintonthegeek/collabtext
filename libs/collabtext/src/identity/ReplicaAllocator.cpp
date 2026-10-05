#include "ReplicaAllocator.h"

namespace CollabText::Identity {

ReplicaAllocator::ReplicaAllocator(ReplicaState* state, std::string document,
                                   std::string stream)
    : m_state(state), m_document(std::move(document)), m_stream(std::move(stream)) {
    if (m_state) {
        // Peek once so `peek()` is meaningful before the first reservation; a
        // stream with no counter yet starts at the same place a fresh counter
        // does.
        if (auto value = m_state->peek_stream(m_stream); value.ok())
            m_peek = value.value();
    }
}

Result<uint64_t> ReplicaAllocator::reserve_run(uint64_t count) {
    if (m_failed) return m_error;
    if (!m_state) {
        m_failed = true;
        m_error = Error(ErrorCode::writer_conflict,
                        "identity allocator has no replica state");
        return m_error;
    }
    auto reserved = m_state->reserve_run(m_stream, m_document, m_epoch, count);
    if (!reserved.ok()) {
        // counter_exhausted is a normal end state, not a broken owner: report it
        // without poisoning the handle. Anything else (an I/O failure) means the
        // in-memory counter is now ahead of the file and this process must not
        // issue from it again.
        if (reserved.error().code() != ErrorCode::counter_exhausted) {
            m_failed = true;
            m_error = reserved.error().with_context("reserving an identity run");
            return m_error;
        }
        return reserved.error();
    }
    m_peek = reserved.value() + count;
    return reserved;
}

void ReplicaAllocator::observe(uint64_t counter) {
    if (m_failed || !m_state) return;
    if (counter >= m_peek) m_peek = counter + 1;
    (void)m_state->observe_remote(m_stream, counter);
}

uint64_t ReplicaAllocator::peek() const noexcept { return m_peek; }

const ActorId& ReplicaAllocator::actor() const noexcept {
    static const ActorId kInvalid;
    return m_state ? m_state->actor() : kInvalid;
}

} // namespace CollabText::Identity