#include "storage/CheckpointCodec.h"

#include "core/Sha256.h"

#include <algorithm>

namespace CollabText::Storage {
namespace {

constexpr uint32_t kMaxTranslations = 65536;
constexpr size_t kMaxTokenBytes = 64u * 1024u;

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
bool valid_hash(std::string_view value) {
  return value.size() == 64 &&
         std::all_of(value.begin(), value.end(), [](char c) {
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
    raw.push_back(
        static_cast<char>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
  return raw;
}
bool valid_reference(std::string_view id) {
  return !id.empty() && id.size() <= 256 &&
         std::all_of(id.begin(), id.end(),
                     [](unsigned char c) { return c >= 0x21 && c <= 0x7e; });
}
PositionScope scope_for(const CheckpointContext &context, uint64_t epoch) {
  return PositionScope{context.document, context.stream,
                       context.engine == CheckpointEngineKind::Text
                           ? Primitive::text
                           : Primitive::list,
                       epoch};
}
Error malformed(std::string message) {
  return Error(ErrorCode::malformed, std::move(message),
               "checkpoint.translations");
}

void put_entry(std::string &out,
               const CheckpointShareableCore::Translation &entry) {
  put_u16(out, static_cast<uint16_t>(entry.reference_id.size()));
  out.append(entry.reference_id);
  put_u64(out, entry.source_epoch);
  put_u32(out, static_cast<uint32_t>(entry.source_token_bytes.size()));
  out.append(entry.source_token_bytes);
  put_u32(out, static_cast<uint32_t>(entry.current_token_bytes.size()));
  out.append(entry.current_token_bytes);
}

Result<void>
validate_entries(const CheckpointShareableCore::TokenTranslations &translations,
                 const CheckpointContext &context,
                 const CheckpointShareableCore::Pins &pins) {
  if (auto valid = encode_checkpoint_context(context); !valid)
    return valid.error().with_context("checkpoint.translations");
  if (auto valid = encode_checkpoint_pins(pins, context); !valid)
    return valid.error().with_context("checkpoint.translations");
  if (translations.supported_token_versions.empty() ||
      translations.supported_token_versions.size() > 64 ||
      !std::is_sorted(translations.supported_token_versions.begin(),
                      translations.supported_token_versions.end()) ||
      std::adjacent_find(translations.supported_token_versions.begin(),
                         translations.supported_token_versions.end()) !=
          translations.supported_token_versions.end() ||
      std::any_of(translations.supported_token_versions.begin(),
                  translations.supported_token_versions.end(),
                  [](uint32_t v) { return v != 1; }))
    return malformed("supported token versions are invalid or noncanonical");
  if (translations.entries.size() > kMaxTranslations)
    return Error(ErrorCode::limit_exceeded,
                 "checkpoint translation count exceeds limit",
                 "checkpoint.translations");

  size_t old_pin_count = 0;
  for (const auto &pin : pins.live_pins)
    if (pin.scope.epoch < context.epoch)
      ++old_pin_count;
  if (old_pin_count != translations.entries.size())
    return malformed(
        "translation entries do not exactly cover old-epoch live pins");

  std::string_view previous;
  size_t encoded_size = 8;
  for (const auto &entry : translations.entries) {
    if (!valid_reference(entry.reference_id) ||
        (!previous.empty() && !(previous < entry.reference_id)) ||
        entry.source_epoch == 0 || entry.source_epoch >= context.epoch ||
        entry.source_token_bytes.empty() || entry.current_token_bytes.empty() ||
        entry.source_token_bytes.size() > kMaxTokenBytes ||
        entry.current_token_bytes.size() > kMaxTokenBytes)
      return malformed(
          "translation entry is invalid, unordered, or outside bounds");
    const auto pin = std::lower_bound(
        pins.live_pins.begin(), pins.live_pins.end(), entry.reference_id,
        [](const RetainedAnchorPin &candidate, std::string_view id) {
          return candidate.reference_id < id;
        });
    if (pin == pins.live_pins.end() ||
        pin->reference_id != entry.reference_id ||
        pin->scope.epoch != entry.source_epoch ||
        pin->token_bytes != entry.source_token_bytes)
      return malformed("translation source does not match its retained pin");
    if (auto valid = validate_position_scope(
            entry.source_token_bytes, scope_for(context, entry.source_epoch));
        !valid)
      return valid.error().with_context("checkpoint.translations");
    if (auto valid = validate_position_scope(entry.current_token_bytes,
                                             scope_for(context, context.epoch));
        !valid)
      return valid.error().with_context("checkpoint.translations");
    previous = entry.reference_id;
    const auto entry_size = size_t{18} + entry.reference_id.size() +
                            entry.source_token_bytes.size() +
                            entry.current_token_bytes.size();
    if (entry_size > kCheckpointMaxBytes - encoded_size)
      return Error(ErrorCode::limit_exceeded,
                   "checkpoint translation section exceeds byte limit",
                   "checkpoint.translations");
    encoded_size += entry_size;
  }
  return Result<void>::success();
}

struct Reader {
  std::string_view bytes;
  size_t cursor = 0;
  bool need(size_t count) const {
    return cursor <= bytes.size() && count <= bytes.size() - cursor;
  }
  uint8_t u8() { return static_cast<uint8_t>(bytes[cursor++]); }
  uint16_t u16() {
    const auto value = (uint16_t(static_cast<uint8_t>(bytes[cursor])) << 8) |
                       uint8_t(static_cast<uint8_t>(bytes[cursor + 1]));
    cursor += 2;
    return value;
  }
  uint32_t u32() {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i)
      value = (value << 8) | u8();
    return value;
  }
  uint64_t u64() {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
      value = (value << 8) | u8();
    return value;
  }
};

} // namespace

Result<CheckpointSection> encode_checkpoint_translations(
    const CheckpointShareableCore::TokenTranslations &translations,
    const CheckpointContext &context,
    const CheckpointShareableCore::Pins &pins) {
  if (auto valid = validate_entries(translations, context, pins); !valid)
    return valid.error();
  std::string payload;
  put_u32(payload,
          static_cast<uint32_t>(translations.supported_token_versions.size()));
  for (const auto version : translations.supported_token_versions)
    put_u32(payload, version);
  put_u32(payload, static_cast<uint32_t>(translations.entries.size()));
  for (const auto &entry : translations.entries)
    put_entry(payload, entry);
  const auto hash = sha256_hex(payload);
  if ((!translations.manifest_hash.empty() &&
       (!valid_hash(translations.manifest_hash) ||
        translations.manifest_hash != hash)))
    return malformed(
        "translation manifest hash does not match canonical entries");
  payload.append(unhex(hash));
  if (payload.size() > kCheckpointMaxBytes - 12)
    return Error(ErrorCode::limit_exceeded,
                 "checkpoint translation section exceeds byte limit",
                 "checkpoint.translations");
  return CheckpointSection{7, 1, std::move(payload)};
}

Result<CheckpointShareableCore::TokenTranslations>
decode_checkpoint_translations(const CheckpointSection &section,
                               const CheckpointContext &context,
                               const CheckpointShareableCore::Pins &pins) {
  if (section.id != 7)
    return malformed("checkpoint translations must be section 7");
  if (section.version != 1)
    return Error(ErrorCode::unsupported_version,
                 "checkpoint translations section version is unsupported",
                 "checkpoint.translations");
  if (section.bytes.size() > kCheckpointMaxBytes - 12 ||
      section.bytes.size() < 40)
    return malformed(
        "checkpoint translation section is truncated or oversized");
  Reader reader{section.bytes};
  CheckpointShareableCore::TokenTranslations translations;
  const auto version_count = reader.u32();
  if (version_count == 0 || version_count > 64 ||
      version_count > (reader.bytes.size() - reader.cursor - 36) / 4)
    return malformed("supported token version list is invalid");
  translations.supported_token_versions.reserve(version_count);
  for (uint32_t i = 0; i < version_count; ++i)
    translations.supported_token_versions.push_back(reader.u32());
  if (!reader.need(4 + 32))
    return malformed("translation count is truncated");
  const auto count = reader.u32();
  if (count > kMaxTranslations ||
      count > (reader.bytes.size() - reader.cursor - 32) / 18)
    return malformed("translation entry count exceeds section bounds");
  translations.entries.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (!reader.need(2))
      return malformed("translation reference length is truncated");
    const auto ref_len = reader.u16();
    if (!ref_len || ref_len > 256 || !reader.need(size_t(ref_len) + 12))
      return malformed("translation reference is invalid or truncated");
    CheckpointShareableCore::Translation entry;
    entry.reference_id.assign(reader.bytes.substr(reader.cursor, ref_len));
    reader.cursor += ref_len;
    entry.source_epoch = reader.u64();
    const auto source_len = reader.u32();
    if (!source_len || source_len > kMaxTokenBytes ||
        !reader.need(size_t(source_len) + 4))
      return malformed("translation source token is invalid or truncated");
    entry.source_token_bytes.assign(
        reader.bytes.substr(reader.cursor, source_len));
    reader.cursor += source_len;
    const auto current_len = reader.u32();
    if (!current_len || current_len > kMaxTokenBytes ||
        !reader.need(size_t(current_len) + 32))
      return malformed("translated token is invalid or truncated");
    entry.current_token_bytes.assign(
        reader.bytes.substr(reader.cursor, current_len));
    reader.cursor += current_len;
    translations.entries.push_back(std::move(entry));
  }
  if (reader.cursor + 32 != reader.bytes.size())
    return malformed(
        "checkpoint translation section has trailing or missing bytes");
  const auto raw_hash = reader.bytes.substr(reader.cursor, 32);
  const auto computed = sha256_hex(reader.bytes.substr(0, reader.cursor));
  if (raw_hash != unhex(computed))
    return malformed(
        "translation manifest hash does not match canonical entries");
  translations.manifest_hash = computed;
  if (auto valid = validate_entries(translations, context, pins); !valid)
    return valid.error();
  return translations;
}

Result<void> validate_checkpoint_translations(
    const CheckpointShareableCore::TokenTranslations &translations,
    const MembershipDescriptor &descriptor) {
  if (translations.supported_token_versions !=
          descriptor.supported_token_versions ||
      translations.manifest_hash != descriptor.translation_manifest_hash)
    return malformed("checkpoint token translation metadata differs from "
                     "membership descriptor");
  return Result<void>::success();
}

} // namespace CollabText::Storage
