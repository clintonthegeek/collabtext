#pragma once
#include <collabtext/Anchor.h>
#include <optional>
namespace CollabText::detail {
enum class PositionKind : uint8_t { text_boundary=1, list_boundary=2, list_element=3, text_attachment=4, text_marker=5 };
struct PositionPayload {
    PositionScope scope;
    PositionKind kind;
    Affinity affinity = Affinity::left;
    Crdt::Global frontier;
    std::optional<Crdt::Identity> left, right, referent;
    ReferentSide selection = ReferentSide::following;
};
struct PositionAccess {
    template<class Tag> static const PositionPayload* get(const OpaquePosition<Tag>& value) { return value.payload.get(); }
    template<class Position> static Position make(PositionPayload payload) {
        return Position(std::make_shared<const PositionPayload>(std::move(payload)));
    }
};
Result<void> validate_scope(const PositionScope&, Primitive expected);
Result<void> validate_position(const PositionPayload&);
Result<void> check_position_context(const PositionScope&, const PositionPayload*,
                                    const Crdt::Global&, bool history_available);
} // namespace CollabText::detail
