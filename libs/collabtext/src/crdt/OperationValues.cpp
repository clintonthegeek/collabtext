#include "crdt/OperationPayload.h"

namespace CollabText::Crdt {
Identity op_identity(const Operation& op) noexcept {
    return visit_operation([](const auto& payload) { return payload.timestamp; }, op);
}
Identity idlist_op_identity(const IdListOperation& op) noexcept {
    return visit_operation([](const auto& payload) { return payload.timestamp; }, op);
}
} // namespace CollabText::Crdt
