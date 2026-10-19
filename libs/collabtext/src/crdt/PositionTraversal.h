#pragma once
#include "crdt/PositionPayload.h"
#include <vector>
namespace CollabText::Crdt { class Buffer; }
namespace CollabText::detail {
/// Narrow read-only bridge to Buffer's retained fragment tree. Keeps position
/// queries inside the native engine without exposing tree state publicly.
class PositionTraversalAccess {
public:
    static Result<PositionPayload> mint_boundary(const PositionScope&, const Crdt::Global&,
        const Crdt::Buffer&, uint32_t offset, Affinity);
    static Result<ResolvedTextPosition> resolve_boundary(const PositionPayload&, const Crdt::Buffer&);
};
/// Projection of existing native retained order/visibility. No sequence state.
struct PositionItem {
    Crdt::Identity identity;
    uint32_t width;
    bool visible;
    std::optional<uint64_t> application_value;
    bool marker = false;
};
Result<PositionPayload> mint_boundary(const PositionScope&, const Crdt::Global&,
    const std::vector<PositionItem>&, uint32_t offset, Affinity);
Result<ResolvedTextPosition> resolve_boundary(const PositionPayload&, const std::vector<PositionItem>&);
Result<ResolvedListPosition> resolve_element(const PositionPayload&, const std::vector<PositionItem>&);
} // namespace CollabText::detail
