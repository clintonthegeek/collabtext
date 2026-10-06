#pragma once
// Independent boundary reference model. Supplied total order/visibility, never
// production ordering, anchor, identity, codec or tree helpers.
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace BoundaryReference {
enum class Primitive { text, list };
enum class Affinity { left, right };
enum class Status { resolved, detached, awaiting_state, unavailable, wrong_document, wrong_stream, wrong_primitive, wrong_epoch, invalid_input, not_found };
struct Context {
    std::string document, stream;
    Primitive primitive = Primitive::text;
    uint64_t epoch = 1;
    bool operator==(const Context&) const = default;
};
struct Item {
    std::string id;
    uint32_t width = 1; // scalar UTF-8 bytes or one list element
    bool visible = true;
    std::optional<uint64_t> application_value; // list lookup only, never token identity
};
struct State {
    Context context;
    std::vector<Item> order; // includes invisible retained logical items
    std::set<std::string> integrated;
    bool history_available = true; // a local clear/reset invalidates even empty-edge tokens
};
struct Token {
    Context context;
    std::optional<std::string> left, right; // absent = scoped START / END
    Affinity affinity = Affinity::left;
    std::set<std::string> mint_state;
};
struct ElementToken {
    Context context;
    std::string referent;
    std::set<std::string> mint_state;
};
struct Resolution {
    Status status;
    uint32_t offset = 0; // usable only for resolved
    bool lost_left = false, lost_right = false;
    bool collapsed() const { return lost_left || lost_right; }
    bool operator==(const Resolution&) const = default;
};
// Named spellings, so a fixture oracle and a model result are compared as text
// rather than by an enum integer that could drift between them.
inline std::string_view status_name(Status status) {
    switch (status) {
        case Status::resolved: return "resolved";
        case Status::detached: return "detached";
        case Status::awaiting_state: return "awaiting_state";
        case Status::unavailable: return "unavailable";
        case Status::wrong_document: return "wrong_document";
        case Status::wrong_stream: return "wrong_stream";
        case Status::wrong_primitive: return "wrong_primitive";
        case Status::wrong_epoch: return "wrong_epoch";
        case Status::invalid_input: return "invalid_input";
        case Status::not_found: return "not_found";
    }
    return "invalid_input";
}
inline std::optional<Status> status_from_name(std::string_view name) {
    constexpr Status all[] = {Status::resolved, Status::detached, Status::awaiting_state, Status::unavailable,
                              Status::wrong_document, Status::wrong_stream, Status::wrong_primitive,
                              Status::wrong_epoch, Status::invalid_input, Status::not_found};
    for (const auto status : all) if (status_name(status) == name) return status;
    return std::nullopt;
}
struct Mutations { bool ignore_affinity = false, erase_scope = false, missing_to_end = false; };
inline bool valid_context(const Context& c) {
    return (c.primitive == Primitive::text || c.primitive == Primitive::list) &&
        !c.document.empty() && c.document.size() <= 256 &&
        !c.stream.empty() && c.stream.size() <= 256 && c.epoch;
}
inline Status context_status(const Context& state, const Context& token) {
    if (state.document != token.document) return Status::wrong_document;
    if (state.stream != token.stream) return Status::wrong_stream;
    if (state.primitive != token.primitive) return Status::wrong_primitive;
    if (state.epoch != token.epoch) return Status::wrong_epoch;
    return Status::resolved;
}
inline bool valid(const State& state) {
    if (!valid_context(state.context)) return false;
    std::set<std::string> identities;
    uint64_t bytes = 0;
    for (const auto& item : state.order) {
        if (item.id.empty() || !identities.insert(item.id).second || !item.width ||
            (state.context.primitive == Primitive::text && item.width > 4) ||
            (state.context.primitive == Primitive::list && item.width != 1)) return false;
        if (item.visible) bytes += item.width;
    }
    return bytes <= std::numeric_limits<uint32_t>::max();
}
inline std::optional<Token> mint(const State& state, uint32_t offset, Affinity affinity) {
    if (!valid(state) || !state.history_available || (affinity != Affinity::left && affinity != Affinity::right)) return std::nullopt;
    uint64_t position = 0;
    std::optional<std::string> left;
    for (const auto& item : state.order) {
        if (!item.visible) continue;
        if (position == offset) return Token{state.context, left, item.id, affinity, state.integrated};
        position += item.width;
        if (offset < position) return std::nullopt;
        left = item.id;
    }
    if (position != offset) return std::nullopt;
    return Token{state.context, left, std::nullopt, affinity, state.integrated};
}
inline Resolution resolve(const State& state, const Token& token, Mutations mutation = {}) {
    if (!valid(state) || !valid_context(token.context) ||
        (token.left && token.left->empty()) || (token.right && token.right->empty()) || (token.affinity != Affinity::left && token.affinity != Affinity::right) ||
        (token.left && token.right && token.left == token.right)) return {Status::invalid_input};
    if (!mutation.erase_scope && state.context != token.context) return {context_status(state.context, token.context)};
    if (!std::includes(state.integrated.begin(), state.integrated.end(), token.mint_state.begin(), token.mint_state.end()))
        return {Status::awaiting_state};
    if (!state.history_available) return {Status::unavailable};
    uint32_t bytes = 0, lower = 0, upper = 0;
    bool found_left = !token.left, found_right = !token.right;
    bool lost_left = false, lost_right = false;
    std::optional<size_t> left_rank, right_rank;
    for (size_t rank = 0; rank < state.order.size(); ++rank) {
        const auto& item = state.order[rank];
        if (token.right && item.id == *token.right) {
            found_right = true; upper = bytes; lost_right = !item.visible; right_rank = rank;
        }
        if (item.visible) bytes += item.width;
        if (token.left && item.id == *token.left) {
            found_left = true; lower = bytes; lost_left = !item.visible; left_rank = rank;
        }
    }
    if (!token.right) upper = bytes;
    if (!found_left || !found_right) {
        if (mutation.missing_to_end) return {Status::resolved, bytes};
        return {Status::unavailable};
    }
    if ((left_rank && right_rank && *left_rank >= *right_rank) || lower > upper)
        return {Status::unavailable};
    const auto offset = token.affinity == Affinity::left && !mutation.ignore_affinity ? lower : upper;
    return {Status::resolved, offset, lost_left, lost_right};
}
// Strict attachment targets one explicitly selected neighbor. It never consumes
// a relocated caret offset after that item disappears. Undo can restore it while
// its retained identity exists; history loss remains unavailable.
inline Resolution resolve_strict(const State& state, const Token& token, bool following) {
    auto result = resolve(state, token);
    if (result.status != Status::resolved) return result;
    if (token.context.primitive != Primitive::text) return {Status::invalid_input};
    const auto& referent = following ? token.right : token.left;
    if (!referent) return {Status::invalid_input};
    if (following ? result.lost_right : result.lost_left) return {Status::detached};
    return result;
}
inline std::optional<int> compare(const State& state, const Token& a, const Token& b) {
    auto x = resolve(state, a), y = resolve(state, b);
    if (x.status != Status::resolved || y.status != Status::resolved) return std::nullopt;
    return x.offset < y.offset ? -1 : x.offset > y.offset ? 1 : 0;
}
// List element resolution is distinct from a text/caret gap. A hidden item is
// detached and a missing identity is unavailable; neither becomes list end.
inline Resolution resolve_element(const State& state, const Context& context, const std::string& id) {
    if (!valid(state)) return {Status::invalid_input};
    if (!valid_context(context) || id.empty()) return {Status::invalid_input};
    if (context != state.context) return {context_status(state.context, context)};
    if (state.context.primitive != Primitive::list) return {Status::invalid_input};
    if (!state.history_available) return {Status::unavailable};
    uint32_t index = 0;
    for (const auto& item : state.order) {
        if (item.id == id) return item.visible ? Resolution{Status::resolved, index} : Resolution{Status::detached};
        if (item.visible) ++index;
    }
    return {Status::unavailable};
}
inline Resolution resolve_element(const State& state, const ElementToken& token) {
    if (!valid(state) || !valid_context(token.context) || token.referent.empty()) return {Status::invalid_input};
    if (state.context != token.context) return {context_status(state.context,token.context)};
    if (!std::includes(state.integrated.begin(),state.integrated.end(),token.mint_state.begin(),token.mint_state.end())) return {Status::awaiting_state};
    return resolve_element(state,token.context,token.referent);
}
inline std::optional<ElementToken> element_of(const State& state, uint64_t application_value) {
    if (!valid(state) || !state.history_available || state.context.primitive != Primitive::list) return std::nullopt;
    for (const auto& i:state.order) if(i.visible && i.application_value == application_value)
        return ElementToken{state.context,i.id,state.integrated};
    return std::nullopt; // typed not_found in public mint API
}
} // namespace BoundaryReference
