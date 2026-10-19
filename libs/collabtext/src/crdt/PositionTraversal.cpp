#include "crdt/PositionTraversal.h"
#include "crdt/Buffer.h"
#include "crdt/Diagnostics.h"
namespace CollabText::detail {
namespace {
Error failure(ErrorCode code, const char* message) { return Error(code, message, "scoped position"); }
bool usable(Crdt::Identity id) { return id.actor.valid() && id.counter && id.counter != UINT64_MAX; }
bool affinity_valid(Affinity a) { return a == Affinity::left || a == Affinity::right; }

Result<void> validate_run(const Crdt::Fragment& run) {
    if (run.byte_length != run.text.size() ||
        (run.marker ? (run.length != 1 || !run.text.empty())
                    : ((run.length == 0) != run.text.empty())) ||
        (run.length && (!run.origin.actor.valid() || !run.origin.counter ||
                         run.length > UINT64_MAX - run.origin.counter)))
        return failure(ErrorCode::history_unavailable, "retained run metadata is invalid");
    return {};
}

uint32_t scalar_width(const std::string& text, size_t byte) {
    const auto lead = static_cast<unsigned char>(text[byte]);
    return lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
}

Result<uint32_t> scalar_index_at_byte(const Crdt::Fragment& run, uint32_t byte_offset) {
    if (byte_offset > run.byte_length)
        return failure(ErrorCode::not_scalar_boundary, "offset is outside retained run");
    if (run.byte_length == run.length) return byte_offset;
    size_t byte = 0;
    uint32_t scalar = 0;
    while (byte < byte_offset) {
        const uint32_t width = scalar_width(run.text, byte);
        if (width > run.text.size() - byte)
            return failure(ErrorCode::invalid_unicode, "invalid retained fragment text");
        for (uint32_t i = 1; i < width; ++i)
            if ((static_cast<unsigned char>(run.text[byte + i]) & 0xc0) != 0x80)
                return failure(ErrorCode::invalid_unicode, "invalid retained fragment text");
        byte += width;
        ++scalar;
    }
    if (byte != byte_offset)
        return failure(ErrorCode::not_scalar_boundary, "offset lies inside a UTF-8 scalar");
    return scalar;
}

Result<uint32_t> byte_offset_at_scalar(const Crdt::Fragment& run, uint32_t scalar_offset) {
    if (scalar_offset > run.length)
        return failure(ErrorCode::history_unavailable, "identity lies outside retained run");
    if (run.byte_length == run.length) return scalar_offset;
    size_t byte = 0;
    for (uint32_t scalar = 0; scalar < scalar_offset; ++scalar) {
        const uint32_t width = scalar_width(run.text, byte);
        if (width > run.text.size() - byte)
            return failure(ErrorCode::invalid_unicode, "invalid retained fragment text");
        for (uint32_t i = 1; i < width; ++i)
            if ((static_cast<unsigned char>(run.text[byte + i]) & 0xc0) != 0x80)
                return failure(ErrorCode::invalid_unicode, "invalid retained fragment text");
        byte += width;
    }
    return static_cast<uint32_t>(byte);
}
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
        Crdt::Diagnostics::record(Crdt::Diagnostics::Counter::position_items_examined);
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
        Crdt::Diagnostics::record(Crdt::Diagnostics::Counter::position_items_examined);
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
        Crdt::Diagnostics::record(Crdt::Diagnostics::Counter::position_items_examined);
        if (item.identity == *p.referent) {
            if (!item.visible) return failure(ErrorCode::detached, "list element is hidden");
            return ResolvedListPosition{static_cast<uint32_t>(index), false, false};
        }
        if (item.visible && ++index > UINT32_MAX) return failure(ErrorCode::limit_exceeded, "visible list exceeds uint32");
    }
    return failure(ErrorCode::history_unavailable, "list referent no longer retained");
}

Result<PositionPayload> PositionTraversalAccess::mint_boundary(
    const PositionScope& scope, const Crdt::Global& version, const Crdt::Buffer& buffer,
    uint32_t offset, Affinity affinity) {
    if (!affinity_valid(affinity)) return failure(ErrorCode::malformed, "invalid affinity");
    PositionPayload p{scope, scope.primitive == Primitive::text ? PositionKind::text_boundary : PositionKind::list_boundary};
    p.affinity = affinity;
    p.frontier = version;
    uint64_t prefix = 0;
    bool finished = false;
    std::optional<Error> error;
    buffer.m_fragment_tree.for_each([&](const Crdt::Fragment& run) {
        if (finished || error) return;
        auto valid = validate_run(run);
        if (!valid) { error = valid.error(); return; }
        if (!run.visible || run.marker) return;
        const uint64_t end = prefix + run.byte_length;
        if (end > UINT32_MAX) {
            error = failure(ErrorCode::limit_exceeded, "visible position exceeds uint32");
            return;
        }
        if (offset >= prefix && offset < end) {
            auto scalar = scalar_index_at_byte(run, offset - static_cast<uint32_t>(prefix));
            if (!scalar) { error = scalar.error(); return; }
            if (scalar.value()) p.left = run.item_at(scalar.value() - 1);
            p.right = run.item_at(scalar.value());
            finished = true;
            return;
        }
        if (offset == prefix) {
            p.right = run.origin;
            finished = true;
            return;
        }
        p.left = run.item_at(run.length - 1);
        prefix = end;
    });
    if (error) return *error;
    if (!finished && offset > prefix) return failure(ErrorCode::off_end, "offset exceeds visible length");
    if (!finished && offset != prefix) return failure(ErrorCode::not_scalar_boundary, "offset is not a scalar boundary");
    auto valid = validate_position(p);
    if (!valid) return valid.error();
    return p;
}

Result<ResolvedTextPosition> PositionTraversalAccess::resolve_boundary(
    const PositionPayload& p, const Crdt::Buffer& buffer) {
    uint64_t prefix = 0;
    uint64_t rank = 0;
    bool left_found = !p.left, right_found = !p.right;
    bool lost_left = false, lost_right = false;
    uint32_t left_position = 0, right_position = 0;
    std::optional<uint64_t> left_rank, right_rank;
    std::optional<Error> error;
    buffer.m_fragment_tree.for_each([&](const Crdt::Fragment& run) {
        if (error) return;
        auto valid = validate_run(run);
        if (!valid) { error = valid.error(); return; }
        if (run.length > UINT64_MAX - rank) {
            error = failure(ErrorCode::limit_exceeded, "retained position rank overflow");
            return;
        }
        const uint64_t run_end = run.origin.counter + run.length;
        const auto locate = [&](const std::optional<Crdt::Identity>& identity,
                                bool& found, bool& lost, std::optional<uint64_t>& found_rank,
                                bool is_left) {
            if (!identity || identity->actor != run.origin.actor ||
                identity->counter < run.origin.counter || identity->counter >= run_end) return;
            if (found_rank) {
                error = failure(ErrorCode::history_unavailable, "duplicate retained identity");
                return;
            }
            const uint32_t scalar = static_cast<uint32_t>(identity->counter - run.origin.counter);
            found = true;
            lost = !run.visible;
            found_rank = rank + scalar;
            uint64_t position = prefix;
            if (!run.visible) {
                if (is_left) left_position = static_cast<uint32_t>(position);
                else right_position = static_cast<uint32_t>(position);
                return;
            }
            auto byte = byte_offset_at_scalar(run, scalar + (is_left ? 1u : 0u));
            if (!byte) { error = byte.error(); return; }
            position += byte.value();
            if (position > UINT32_MAX) {
                error = failure(ErrorCode::limit_exceeded, "visible position exceeds uint32");
                return;
            }
            if (is_left) left_position = static_cast<uint32_t>(position);
            else right_position = static_cast<uint32_t>(position);
        };
        locate(p.left, left_found, lost_left, left_rank, true);
        locate(p.right, right_found, lost_right, right_rank, false);
        if (error) return;
        if (run.visible) {
            prefix += run.byte_length;
            if (prefix > UINT32_MAX)
                error = failure(ErrorCode::limit_exceeded, "visible position exceeds uint32");
        }
        rank += run.length;
    });
    if (error) return *error;
    if (!p.right) right_position = static_cast<uint32_t>(prefix);
    if (!left_found || !right_found ||
        (left_rank && right_rank && *left_rank >= *right_rank) || left_position > right_position)
        return failure(ErrorCode::history_unavailable, "retained fences missing or inverted");
    if (p.kind == PositionKind::text_attachment &&
        (p.selection == ReferentSide::following ? lost_right : lost_left))
        return failure(ErrorCode::detached, "selected content item is hidden");
    return ResolvedTextPosition{p.affinity == Affinity::left ? left_position : right_position,
                                lost_left, lost_right};
}
} // namespace CollabText::detail
