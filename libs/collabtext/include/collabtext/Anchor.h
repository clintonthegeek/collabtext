#pragma once
#include <collabtext/ActorId.h>
#include <collabtext/Engine.h>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

namespace CollabText {
enum class Primitive { text, list };
enum class Affinity { left, right };
enum class ReferentSide { preceding, following };
struct PositionScope {
    DocumentId document;
    StreamId stream;
    Primitive primitive;
    uint64_t epoch;
    bool operator==(const PositionScope&) const = default;
};
namespace detail { struct PositionPayload; struct PositionAccess; }
/// Immutable scoped causal position. Only engines and checked codecs mint it.
/// Copies share const ownership; a moved-from token is invalid input.
template<class Tag> class OpaquePosition {
public:
    OpaquePosition(const OpaquePosition&) = default;
    OpaquePosition(OpaquePosition&&) noexcept = default;
    OpaquePosition& operator=(const OpaquePosition&) = default;
    OpaquePosition& operator=(OpaquePosition&&) noexcept = default;
    ~OpaquePosition() = default;
private:
    friend struct detail::PositionAccess;
    std::shared_ptr<const detail::PositionPayload> payload;
    explicit OpaquePosition(std::shared_ptr<const detail::PositionPayload> p) : payload(std::move(p)) {}
};
using TextBoundary = OpaquePosition<struct TextBoundaryTag>;
using TextAttachment = OpaquePosition<struct TextAttachmentTag>;
using ListBoundary = OpaquePosition<struct ListBoundaryTag>;
using ListElement = OpaquePosition<struct ListElementTag>;
struct ResolvedTextPosition {
    uint32_t byte_offset;
    bool lost_left, lost_right;
    bool collapsed() const noexcept { return lost_left || lost_right; }
};
struct ResolvedListPosition {
    uint32_t element_index;
    bool lost_left, lost_right;
    bool collapsed() const noexcept { return lost_left || lost_right; }
};
using PositionToken = std::variant<TextBoundary,TextAttachment,ListBoundary,ListElement>;
/// Library-owned binary CTPT v1 bytes, including NUL. No layout is wire data.
/// Bounded to 64 KiB, 256-byte scope strings and 512 frontier actors. Decode
/// requires exact EOF, retains no input reference, creates no pin or identity.
Result<std::string> encode_position(const PositionToken& token);
Result<PositionToken> decode_position(std::string_view bytes);
/// Returns the checked scope embedded in a valid, live position token.
Result<PositionScope> position_scope(const PositionToken& token);
/// Validates stored CTPT bytes and rejects any scope mismatch explicitly.
Result<void> validate_position_scope(std::string_view bytes,
                                     const PositionScope& expected);
/// A typed read refuses a different kind instead of converting its meaning.
template<class Position> Result<Position> decode_position_as(std::string_view bytes) {
    auto decoded = decode_position(bytes);
    if (!decoded.ok()) return decoded.error();
    if (const auto* typed = std::get_if<Position>(&decoded.value())) return *typed;
    return Error(ErrorCode::wrong_primitive, "position token has another type");
}
} // namespace CollabText
