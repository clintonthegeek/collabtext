#include "storage/CheckpointCodec.h"

#include "core/Sha256.h"

#include <algorithm>
#include <array>

namespace CollabText::Storage {
namespace {

constexpr uint32_t kMaxPins = 65536;
constexpr size_t kMaxPinTokenBytes = 64u * 1024u;
constexpr size_t kMaxReferenceBytes = 256;

void put_u16(std::string &out, uint16_t value) {
  out.push_back(static_cast<char>(value >> 8));
  out.push_back(static_cast<char>(value));
}
void put_u32(std::string &out, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8)
    out.push_back(static_cast<char>(value >> shift));
}
void put_u64(std::string &out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8)
    out.push_back(static_cast<char>(value >> shift));
}
bool valid_hash(std::string_view hash) {
  return hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
uint8_t nibble(char c) {
  return static_cast<uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10);
}
std::string unhex(std::string_view hex) {
  std::string raw;
  raw.reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2)
    raw.push_back(static_cast<char>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
  return raw;
}
std::string hex(std::string_view raw) {
  constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(raw.size() * 2);
  for (unsigned char byte : raw) {
    out.push_back(digits[byte >> 4]);
    out.push_back(digits[byte & 15]);
  }
  return out;
}
bool valid_reference(std::string_view id) {
  return !id.empty() && id.size() <= kMaxReferenceBytes &&
         std::all_of(id.begin(), id.end(), [](unsigned char c) {
           return c >= 0x21 && c <= 0x7e;
         });
}
PositionScope scope_for(const CheckpointContext &context) {
  return PositionScope{context.document, context.stream,
                       context.engine == CheckpointEngineKind::Text
                           ? Primitive::text : Primitive::list,
                       context.epoch};
}
Error malformed(std::string message) {
  return Error(ErrorCode::malformed, std::move(message), "checkpoint.pins");
}

struct Reader {
  std::string_view bytes;
  size_t cursor = 0;
  bool need(size_t count) const {
    return cursor <= bytes.size() && count <= bytes.size() - cursor;
  }
  uint8_t u8() { return static_cast<uint8_t>(bytes[cursor++]); }
  uint16_t u16() {
    uint16_t value = (static_cast<uint16_t>(static_cast<uint8_t>(bytes[cursor])) << 8) |
                     static_cast<uint8_t>(bytes[cursor + 1]);
    cursor += 2;
    return value;
  }
  uint32_t u32() {
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i)
      value = (value << 8) | static_cast<uint8_t>(bytes[cursor++]);
    return value;
  }
  uint64_t u64() {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i)
      value = (value << 8) | static_cast<uint8_t>(bytes[cursor++]);
    return value;
  }
};

}  // namespace

Result<CheckpointSection> encode_checkpoint_pins(
    const CheckpointShareableCore::Pins &pins,
    const CheckpointContext &context) {
  if (auto valid = encode_checkpoint_context(context); !valid)
    return valid.error().with_context("checkpoint.pins");
  if (pins.live_pins.size() > kMaxPins)
    return Error(ErrorCode::limit_exceeded, "checkpoint pin count exceeds limit", "checkpoint.pins");
  if (!valid_hash(pins.canonical_projection_hash) ||
      pins.projected_frontier > pins.canonical_frontier ||
      pins.complete != (pins.projected_frontier == pins.canonical_frontier))
    return malformed("checkpoint pin projection identity or completeness is invalid");
  std::string payload;
  payload.reserve(53);
  put_u64(payload, pins.projected_frontier);
  put_u64(payload, pins.canonical_frontier);
  payload.push_back(pins.complete ? '\1' : '\0');
  payload.append(unhex(pins.canonical_projection_hash));
  put_u32(payload, static_cast<uint32_t>(pins.live_pins.size()));
  std::string_view previous;
  const auto expected_scope = scope_for(context);
  for (const auto &pin : pins.live_pins) {
    if (!valid_reference(pin.reference_id) ||
        (!previous.empty() && !(previous < pin.reference_id)) ||
        pin.scope.document != expected_scope.document ||
        pin.scope.stream != expected_scope.stream ||
        pin.scope.primitive != expected_scope.primitive ||
        pin.scope.epoch == 0 || pin.scope.epoch > expected_scope.epoch)
      return malformed("checkpoint pins are invalid, unordered, or outside context scope");
    if (pin.token_bytes.empty() || pin.token_bytes.size() > kMaxPinTokenBytes)
      return malformed("checkpoint pin token is empty or exceeds limit");
    if (auto valid = validate_position_scope(pin.token_bytes, pin.scope); !valid)
      return valid.error().with_context("checkpoint.pins");
    put_u16(payload, static_cast<uint16_t>(pin.reference_id.size()));
    payload.append(pin.reference_id);
    put_u64(payload, pin.scope.epoch);
    put_u32(payload, static_cast<uint32_t>(pin.token_bytes.size()));
    payload.append(pin.token_bytes);
    if (payload.size() > kCheckpointMaxBytes - 12)
      return Error(ErrorCode::limit_exceeded, "checkpoint pin section exceeds byte limit", "checkpoint.pins");
    previous = pin.reference_id;
  }
  return CheckpointSection{6, 1, std::move(payload)};
}

Result<CheckpointShareableCore::Pins> decode_checkpoint_pins(
    const CheckpointSection &section, const CheckpointContext &context) {
  if (section.id != 6) return malformed("checkpoint pins must be section 6");
  if (section.version != 1)
    return Error(ErrorCode::unsupported_version, "checkpoint pins section version is unsupported", "checkpoint.pins");
  if (section.bytes.size() > kCheckpointMaxBytes - 12)
    return Error(ErrorCode::limit_exceeded, "checkpoint pin section exceeds byte limit", "checkpoint.pins");
  if (auto valid = encode_checkpoint_context(context); !valid)
    return valid.error().with_context("checkpoint.pins");
  Reader reader{section.bytes};
  if (!reader.need(53)) return malformed("checkpoint pin section header is truncated");
  CheckpointShareableCore::Pins pins;
  pins.projected_frontier = reader.u64();
  pins.canonical_frontier = reader.u64();
  const auto complete = reader.u8();
  if (complete > 1) return malformed("checkpoint pin completeness flag is invalid");
  pins.complete = complete != 0;
  pins.canonical_projection_hash = hex(reader.bytes.substr(reader.cursor, 32));
  reader.cursor += 32;
  const auto count = reader.u32();
  if (count > kMaxPins)
    return Error(ErrorCode::limit_exceeded, "checkpoint pin count exceeds limit", "checkpoint.pins");
  if (count > (reader.bytes.size() - reader.cursor) / 16)
    return malformed("checkpoint pin array is truncated");
  if (pins.projected_frontier > pins.canonical_frontier ||
      pins.complete != (pins.projected_frontier == pins.canonical_frontier))
    return malformed("checkpoint pin projection completeness is inconsistent");
  pins.live_pins.reserve(count);
  std::string previous;
  const auto expected_scope = scope_for(context);
  for (uint32_t i = 0; i < count; ++i) {
    if (!reader.need(2)) return malformed("checkpoint pin reference length is truncated");
    const auto reference_length = reader.u16();
    if (reference_length == 0 || reference_length > kMaxReferenceBytes ||
        !reader.need(static_cast<size_t>(reference_length) + 12))
      return malformed("checkpoint pin reference is truncated or too long");
    std::string reference(reader.bytes.substr(reader.cursor, reference_length));
    reader.cursor += reference_length;
    const auto source_epoch = reader.u64();
    const auto token_length = reader.u32();
    if (token_length == 0 || token_length > kMaxPinTokenBytes ||
        !reader.need(token_length))
      return malformed("checkpoint pin token is truncated or too long");
    std::string token(reader.bytes.substr(reader.cursor, token_length));
    reader.cursor += token_length;
    if (!valid_reference(reference) || (!previous.empty() && !(previous < reference)))
      return malformed("checkpoint pin references are invalid, duplicated, or unordered");
    PositionScope source_scope = expected_scope;
    source_scope.epoch = source_epoch;
    if (source_epoch == 0 || source_epoch > expected_scope.epoch)
      return malformed("checkpoint pin source epoch is invalid");
    if (auto valid = validate_position_scope(token, source_scope); !valid)
      return valid.error().with_context("checkpoint.pins");
    pins.live_pins.push_back({std::move(reference), source_scope, std::move(token)});
    previous = pins.live_pins.back().reference_id;
  }
  if (reader.cursor != reader.bytes.size())
    return malformed("checkpoint pin section has trailing bytes");
  return pins;
}

}  // namespace CollabText::Storage
