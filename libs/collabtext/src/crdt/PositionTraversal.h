#pragma once
#include "crdt/PositionPayload.h"
#include <vector>
namespace CollabText::detail {
/// Projection of existing native retained order/visibility. No sequence state.
struct PositionItem {
    Crdt::Identity identity;
    uint32_t width;
    bool visible;
    std::optional<uint64_t> application_value;
};
Result<PositionPayload> mint_boundary(const PositionScope&, const Crdt::Global&,
    const std::vector<PositionItem>&, uint32_t offset, Affinity);
Result<ResolvedTextPosition> resolve_boundary(const PositionPayload&, const std::vector<PositionItem>&);
Result<ResolvedListPosition> resolve_element(const PositionPayload&, const std::vector<PositionItem>&);
} // namespace CollabText::detail
