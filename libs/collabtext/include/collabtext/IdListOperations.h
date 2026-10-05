#pragma once

#include <collabtext/Engine.h>
#include <memory>

namespace CollabText::Crdt {
namespace detail { struct IdListOperationPayload; struct OperationAccess; }

/// An immutable operation value. Copies share read-only storage; no public
/// constructor or accessor exposes its payload. Obtain values from the engine
/// or decode_idlist_operation. A default/moved-from value has no operation identity and
/// receivers reject it. Identity and the versioned codec are the public boundary.
class IdListOperation {
public:
    IdListOperation() noexcept = default;
    IdListOperation(const IdListOperation&) noexcept = default;
    IdListOperation& operator=(const IdListOperation&) noexcept = default;
    IdListOperation(IdListOperation&&) noexcept = default;
    IdListOperation& operator=(IdListOperation&&) noexcept = default;
    ~IdListOperation() = default;

private:
    explicit IdListOperation(std::shared_ptr<const detail::IdListOperationPayload> payload) noexcept
        : m_payload(std::move(payload)) {}
    std::shared_ptr<const detail::IdListOperationPayload> m_payload;
    friend struct detail::OperationAccess;
};

[[nodiscard]] Identity idlist_op_identity(const IdListOperation&) noexcept;
} // namespace CollabText::Crdt
