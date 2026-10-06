#include <collabtext/Anchor.h>
#include "crdt/PositionPayload.h"
#include <algorithm>

namespace CollabText {
namespace {
constexpr size_t token_limit = 64u << 10;
Error malformed(const char* message) { return Error(ErrorCode::malformed, message, "CTPT v1"); }
class Writer {
public:
    std::string bytes = "CTPT";
    void number(uint64_t value, size_t width) {
        for (size_t n = width; n; --n) bytes.push_back(static_cast<char>((value >> ((n - 1) * 8)) & 255));
    }
    void string(std::string_view text) { number(text.size(),2); bytes.append(text); }
    void identity(Crdt::Identity id) {
        for (uint8_t byte : id.actor.bytes()) bytes.push_back(static_cast<char>(byte));
        number(id.counter,8);
    }
    void fence(const std::optional<Crdt::Identity>& item) {
        number(item ? 1 : 0,1);
        if (item) identity(*item);
    }
};
class Reader {
public:
    explicit Reader(std::string_view input) : input(input) {}
    std::string_view input;
    size_t position = 0;
    Error error;
    void fail(const char* message) { if (!error) error = malformed(message); }
    bool available(size_t n) {
        if (error) return false;
        if (n > input.size() - position) { fail("truncated token field"); return false; }
        return true;
    }
    uint64_t number(size_t width) {
        if (!available(width)) return 0;
        uint64_t value = 0;
        for (size_t n=0; n<width; ++n) value=(value<<8)|static_cast<unsigned char>(input[position++]);
        return value;
    }
    std::string string() {
        const auto size = number(2);
        if (size == 0 || size > 256) { fail("invalid scope string length"); return {}; }
        if (!available(size)) return {};
        auto value = std::string(input.substr(position,size)); position += size; return value;
    }
    Crdt::Identity identity() {
        if (!available(24)) return {};
        std::array<uint8_t,16> bytes{};
        for (auto& byte:bytes) byte=static_cast<unsigned char>(input[position++]);
        const auto counter=number(8);
        Crdt::Identity id(ActorId::from_bytes(bytes),counter);
        if (!id.actor.valid() || !counter || counter == UINT64_MAX) fail("invalid token item/frontier identity");
        return id;
    }
    std::optional<Crdt::Identity> fence() {
        const auto tag=number(1);
        if (tag > 1) { fail("invalid fence tag"); return {}; }
        return tag ? std::optional<Crdt::Identity>(identity()) : std::nullopt;
    }
};
}
Result<std::string> encode_position(const PositionToken& token) {
    return std::visit([](const auto& typed)->Result<std::string> {
        const auto* p=detail::PositionAccess::get(typed);
        if (!p) return malformed("moved-from position");
        auto valid=detail::validate_position(*p); if (!valid) return valid.error();
        Writer w; w.number(1,2); w.number(static_cast<uint8_t>(p->kind),1);
        w.number(p->affinity == Affinity::right ? 1 : 0,1);
        w.string(p->scope.document); w.string(p->scope.stream); w.number(p->scope.epoch,8);
        if (p->kind == detail::PositionKind::text_attachment)
            w.number(p->selection == ReferentSide::preceding ? 1 : 2,1);
        if (p->kind == detail::PositionKind::list_element) w.identity(*p->referent);
        else { w.fence(p->left); w.fence(p->right); }
        w.number(p->frontier.pair_count(),2);
        for (size_t n=0; n<p->frontier.pair_count(); ++n) {
            auto [actor,head]=p->frontier.pair(n); w.identity({actor,head});
        }
        if (w.bytes.size()>token_limit) return Error(ErrorCode::limit_exceeded,"position frame exceeds 64 KiB");
        return std::move(w.bytes);
    },token);
}
Result<PositionToken> decode_position(std::string_view bytes) {
    if (bytes.size()>token_limit) return malformed("position input exceeds 64 KiB");
    if (bytes.size()<4) {
        if (!bytes.empty() && std::string_view("CTPT").substr(0,bytes.size())==bytes)
            return malformed("truncated position magic");
        return Error(ErrorCode::unknown_format,"unknown position format");
    }
    if (bytes.substr(0,4)!="CTPT") return Error(ErrorCode::unknown_format,"unknown position format");
    Reader r(bytes); r.position=4;
    const auto version=r.number(2);
    if (r.error) return r.error;
    if (version!=1) return Error(ErrorCode::unsupported_version,"unsupported position version");
    const auto kind=r.number(1), affinity=r.number(1);
    if (r.error) return r.error;
    if (kind<1 || kind>4 || affinity>1 || (kind==3 && affinity)) return malformed("invalid position kind/affinity");
    detail::PositionPayload p;
    p.kind=static_cast<detail::PositionKind>(kind);
    p.affinity=affinity ? Affinity::right : Affinity::left;
    p.scope.document=r.string(); p.scope.stream=r.string(); p.scope.epoch=r.number(8);
    p.scope.primitive=(kind==1 || kind==4) ? Primitive::text : Primitive::list;
    if (kind==4) {
        const auto selected=r.number(1);
        if (selected!=1 && selected!=2) r.fail("invalid attachment selection");
        p.selection=selected==1 ? ReferentSide::preceding : ReferentSide::following;
    }
    if (kind==3) p.referent=r.identity();
    else { p.left=r.fence(); p.right=r.fence(); }
    const auto count=r.number(2);
    if (count>512) r.fail("frontier exceeds 512 actors");
    if (r.error) return r.error;
    std::optional<ActorId> previous;
    for (size_t n=0; n<count; ++n) {
        const auto id=r.identity(); if (r.error) return r.error;
        if (previous && !(*previous<id.actor)) return malformed("frontier is duplicated or unsorted");
        p.frontier.observe(id); previous=id.actor;
    }
    if (r.error) return r.error;
    if (r.position!=bytes.size()) return malformed("trailing position bytes");
    auto valid=detail::validate_position(p);
    if (!valid) return malformed("invalid position structure/context/frontier");
    switch (p.kind) {
    case detail::PositionKind::text_boundary: return PositionToken{detail::PositionAccess::make<TextBoundary>(std::move(p))};
    case detail::PositionKind::list_boundary: return PositionToken{detail::PositionAccess::make<ListBoundary>(std::move(p))};
    case detail::PositionKind::list_element: return PositionToken{detail::PositionAccess::make<ListElement>(std::move(p))};
    case detail::PositionKind::text_attachment: return PositionToken{detail::PositionAccess::make<TextAttachment>(std::move(p))};
    }
    return malformed("invalid token kind");
}
} // namespace CollabText
