#pragma once
// Compile-only S04a target declarations. These are not installed APIs or a backend.
#include <collabtext/ActorId.h>
#include <collabtext/Engine.h>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
namespace CollabText::PositionApiTarget {
enum class Primitive { text, list };
enum class Affinity { left, right };
enum class ReferentSide { preceding, following };
struct PositionScope {
    DocumentId document;
    StreamId stream;
    Primitive primitive;
    uint64_t epoch;
};
template<class Tag> class OpaquePosition {
public:
    OpaquePosition(const OpaquePosition&) = default;
    OpaquePosition(OpaquePosition&&) noexcept = default;
    OpaquePosition& operator=(const OpaquePosition&) = default;
    OpaquePosition& operator=(OpaquePosition&&) noexcept = default;
    ~OpaquePosition() = default;
private:
    friend struct PositionAccess; // private S04b factory/codec access, never a public builder
    struct Payload;
    std::shared_ptr<const Payload> payload;
    explicit OpaquePosition(std::shared_ptr<const Payload> p) : payload(std::move(p)) {}
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
[[nodiscard]] Result<std::string> encode_position(const PositionToken&);
[[nodiscard]] Result<PositionToken> decode_position(std::string_view);
// Add these members to the actual existing facades in S04b; no new engine.
class CrdtEngine {
public:
    [[nodiscard]] static Result<std::unique_ptr<CrdtEngine>> create(
        Crdt::SharedIdentityAllocator, PositionScope);
    [[nodiscard]] Result<TextBoundary> boundary_at(uint32_t utf8_byte_offset, Affinity) const;
    [[nodiscard]] Result<TextAttachment> attachment_at(uint32_t utf8_byte_offset, Affinity, ReferentSide) const;
    [[nodiscard]] Result<ResolvedTextPosition> resolve(const TextBoundary&) const;
    [[nodiscard]] Result<ResolvedTextPosition> resolve(const TextAttachment&) const;
    [[nodiscard]] Result<int> compare(const TextBoundary&, const TextBoundary&) const;
};
class IdListEngine {
public:
    [[nodiscard]] static Result<std::unique_ptr<IdListEngine>> create(
        Crdt::SharedIdentityAllocator, PositionScope);
    [[nodiscard]] Result<ListBoundary> boundary_at(uint32_t element_index, Affinity) const;
    [[nodiscard]] Result<ListElement> element_of(uint64_t application_value) const;
    [[nodiscard]] Result<ResolvedListPosition> resolve(const ListBoundary&) const;
    [[nodiscard]] Result<ResolvedListPosition> resolve(const ListElement&) const;
    [[nodiscard]] Result<int> compare(const ListBoundary&, const ListBoundary&) const;
};
} // namespace CollabText::PositionApiTarget
