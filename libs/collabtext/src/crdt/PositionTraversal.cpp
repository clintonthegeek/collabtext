#include "crdt/PositionTraversal.h"
#include <limits>
namespace CollabText::detail {
namespace {
Error failure(ErrorCode code, const char* message) { return Error(code, message, "scoped position"); }
bool usable(Crdt::Identity id) { return id.actor.valid() && id.counter && id.counter != UINT64_MAX; }
bool affinity_valid(Affinity a) { return a == Affinity::left || a == Affinity::right; }
}
Result<void> validate_scope(const PositionScope& scope, Primitive expected) {
    if (scope.document.empty() || scope.stream.empty() || !scope.epoch)
        return failure(ErrorCode::malformed, "scope requires nonempty document/stream and nonzero epoch");
    if (scope.document.size() > 256 || scope.stream.size() > 256)
        return failure(ErrorCode::limit_exceeded, "scope strings exceed 256 bytes");
    if (scope.primitive != Primitive::text && scope.primitive != Primitive::list)
        return failure(ErrorCode::malformed, "invalid scope primitive");
    if (scope.primitive != expected) return failure(ErrorCode::wrong_primitive, "scope primitive differs from engine");
    return {};
}
Result<void> validate_position(const PositionPayload& p) {
    const bool text = p.kind == PositionKind::text_boundary || p.kind == PositionKind::text_attachment || p.kind == PositionKind::text_marker;
    if (!text && p.kind != PositionKind::list_boundary && p.kind != PositionKind::list_element)
        return failure(ErrorCode::malformed, "invalid position kind");
    auto scope = validate_scope(p.scope, text ? Primitive::text : Primitive::list);
    if (!scope) return scope.error();
    if (!affinity_valid(p.affinity) || ((p.kind == PositionKind::list_element || p.kind == PositionKind::text_marker) && p.affinity != Affinity::left))
        return failure(ErrorCode::malformed, "invalid position affinity");
    if (p.frontier.pair_count() > 512) return failure(ErrorCode::limit_exceeded, "mint frontier exceeds 512 actors");
    for (size_t i = 0; i < p.frontier.pair_count(); ++i) {
        auto [actor, head] = p.frontier.pair(i);
        if (!usable({actor,head})) return failure(ErrorCode::malformed, "invalid mint frontier component");
    }
    auto check_item = [&](const std::optional<Crdt::Identity>& item) {
        return !item || (usable(*item) && p.frontier.observed(*item));
    };
    if (!check_item(p.left) || !check_item(p.right) || !check_item(p.referent))
        return failure(ErrorCode::malformed, "item identity is invalid or outside mint frontier");
    if (p.kind == PositionKind::list_element || p.kind == PositionKind::text_marker) {
        if (!p.referent || p.left || p.right) return failure(ErrorCode::malformed, "invalid list referent");
    } else {
        if (p.referent || (p.left && p.right && p.left == p.right))
            return failure(ErrorCode::malformed, "invalid boundary fences");
    }
    if (p.kind == PositionKind::text_attachment) {
        if (p.selection != ReferentSide::preceding && p.selection != ReferentSide::following)
            return failure(ErrorCode::malformed, "invalid attachment selection");
        if (!(p.selection == ReferentSide::preceding ? p.left : p.right))
            return failure(ErrorCode::malformed, "strict attachment selects an edge");
    }
    return {};
}
Result<void> check_position_context(const PositionScope& scope, const PositionPayload* p,
                                    const Crdt::Global& current, bool history) {
    if (!p) return failure(ErrorCode::malformed, "moved-from position");
    auto valid = validate_position(*p); if (!valid) return valid.error();
    if (scope.document != p->scope.document) return failure(ErrorCode::wrong_document, "position belongs to another document");
    if (scope.stream != p->scope.stream) return failure(ErrorCode::wrong_stream, "position belongs to another stream");
    if (scope.primitive != p->scope.primitive) return failure(ErrorCode::wrong_primitive, "position belongs to another primitive");
    if (scope.epoch != p->scope.epoch) return failure(ErrorCode::wrong_epoch, "position belongs to another epoch");
    if (!current.observed_all(p->frontier)) return failure(ErrorCode::awaiting_state, "mint causal state has not arrived");
    if (!history) return failure(ErrorCode::history_unavailable, "retained history was cleared");
    return {};
}
Result<PositionPayload> mint_boundary(const PositionScope& scope, const Crdt::Global& version,
    const std::vector<PositionItem>& items, uint32_t offset, Affinity affinity) {
    if (!affinity_valid(affinity)) return failure(ErrorCode::malformed, "invalid affinity");
    PositionPayload p{scope, scope.primitive == Primitive::text ? PositionKind::text_boundary : PositionKind::list_boundary};
    p.affinity = affinity; p.frontier = version;
    uint64_t prefix = 0;
    bool found = false;
    for (const auto& item : items) {
        if (!item.visible || item.marker) continue;
        if (prefix == offset && !found) { p.right = item.identity; found = true; }
        if (!found) p.left = item.identity;
        prefix += item.width;
        if (prefix > UINT32_MAX) return failure(ErrorCode::limit_exceeded, "visible position exceeds uint32");
        if (!found && offset < prefix) return failure(ErrorCode::not_scalar_boundary, "offset lies inside a UTF-8 scalar");
    }
    if (offset > prefix) return failure(ErrorCode::off_end, "offset exceeds visible length");
    if (!found && offset != prefix) return failure(ErrorCode::not_scalar_boundary, "offset is not a scalar boundary");
    auto valid = validate_position(p); if (!valid) return valid.error();
    return p;
}
Result<ResolvedTextPosition> resolve_boundary(const PositionPayload& p, const std::vector<PositionItem>& items) {
    uint64_t prefix = 0, lower = 0, upper = 0;
    bool left_found = !p.left, right_found = !p.right;
    bool lost_left = false, lost_right = false;
    std::optional<size_t> left_rank, right_rank;
    for (size_t rank = 0; rank < items.size(); ++rank) {
        const auto& item = items[rank];
        if (p.right && item.identity == *p.right) {
            if (right_rank) return failure(ErrorCode::history_unavailable, "duplicate retained identity");
            right_found = true; upper = prefix; lost_right = !item.visible; right_rank = rank;
        }
        if (item.visible) prefix += item.width;
        if (prefix > UINT32_MAX) return failure(ErrorCode::limit_exceeded, "visible position exceeds uint32");
        if (p.left && item.identity == *p.left) {
            if (left_rank) return failure(ErrorCode::history_unavailable, "duplicate retained identity");
            left_found = true; lower = prefix; lost_left = !item.visible; left_rank = rank;
        }
    }
    if (!p.right) upper = prefix;
    if (!left_found || !right_found || (left_rank && right_rank && *left_rank >= *right_rank) || lower > upper)
        return failure(ErrorCode::history_unavailable, "retained fences missing or inverted");
    if (p.kind == PositionKind::text_attachment && (p.selection == ReferentSide::following ? lost_right : lost_left))
        return failure(ErrorCode::detached, "selected content item is hidden");
    return ResolvedTextPosition{static_cast<uint32_t>(p.affinity == Affinity::left ? lower : upper), lost_left, lost_right};
}
Result<ResolvedListPosition> resolve_element(const PositionPayload& p, const std::vector<PositionItem>& items) {
    uint64_t index = 0;
    for (const auto& item : items) {
        if (item.identity == *p.referent) {
            if (!item.visible) return failure(ErrorCode::detached, "list element is hidden");
            return ResolvedListPosition{static_cast<uint32_t>(index), false, false};
        }
        if (item.visible && ++index > UINT32_MAX) return failure(ErrorCode::limit_exceeded, "visible list exceeds uint32");
    }
    return failure(ErrorCode::history_unavailable, "list referent no longer retained");
}
} // namespace CollabText::detail
