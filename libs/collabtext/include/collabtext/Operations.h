#pragma once

#include <collabtext/Engine.h>
#include <memory>

namespace CollabText::Crdt {
namespace detail { struct OperationPayload; struct OperationAccess; }

/// An immutable operation value. Copies share read-only storage; no public
/// constructor or accessor exposes its payload. Obtain values from the engine
/// or decode_operation. A default/moved-from value has no operation identity and
/// receivers reject it. Identity and the versioned codec are the public boundary.
class Operation {
public:
    Operation() noexcept = default;
    Operation(const Operation&) noexcept = default;
    Operation& operator=(const Operation&) noexcept = default;
    Operation(Operation&&) noexcept = default;
    Operation& operator=(Operation&&) noexcept = default;
    ~Operation() = default;

private:
    explicit Operation(std::shared_ptr<const detail::OperationPayload> payload) noexcept
        : m_payload(std::move(payload)) {}
    std::shared_ptr<const detail::OperationPayload> m_payload;
    friend struct detail::OperationAccess;
};

[[nodiscard]] Identity op_identity(const Operation&) noexcept;
} // namespace CollabText::Crdt
