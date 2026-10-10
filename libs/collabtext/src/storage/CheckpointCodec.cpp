#include "storage/CheckpointCodec.h"

#include "core/Sha256.h"

#include <algorithm>
#include <array>
#include <limits>

namespace CollabText::Storage {
namespace {

constexpr std::string_view kShareableMagic = "CTCK0001:";
constexpr std::string_view kLocalMagic = "CTCL0001:";
constexpr size_t kHeaderBytes = 9 + 8;
constexpr size_t kDigestBytes = 32;
constexpr size_t kSectionHeaderBytes = 12;

std::string_view magic(CheckpointKind kind) {
  return kind == CheckpointKind::Shareable ? kShareableMagic : kLocalMagic;
}

uint16_t required_count(CheckpointKind kind) {
  return kind == CheckpointKind::Shareable
             ? kCheckpointShareableSectionCount
             : kCheckpointLocalSectionCount;
}

void append_u16(std::string &out, uint16_t value) {
  out.push_back(static_cast<char>(value >> 8));
  out.push_back(static_cast<char>(value));
}

void append_u64(std::string &out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8)
    out.push_back(static_cast<char>(value >> shift));
}

uint16_t read_u16(std::string_view bytes, size_t at) {
  return static_cast<uint16_t>(
      (static_cast<uint16_t>(static_cast<uint8_t>(bytes[at])) << 8) |
      static_cast<uint8_t>(bytes[at + 1]));
}

uint64_t read_u64(std::string_view bytes, size_t at) {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i)
    value = (value << 8) | static_cast<uint8_t>(bytes[at + i]);
  return value;
}

std::string raw_digest(std::string_view input) {
  const auto hex = sha256_hex(input);
  std::string out;
  out.reserve(kDigestBytes);
  auto nibble = [](char c) -> uint8_t {
    return static_cast<uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10);
  };
  for (size_t i = 0; i < hex.size(); i += 2)
    out.push_back(static_cast<char>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
  return out;
}

Error malformed(std::string message) {
  return Error(ErrorCode::malformed, std::move(message), "checkpoint");
}

Error bad_sections(std::string message) {
  return Error(ErrorCode::unsupported_version, std::move(message), "checkpoint.sections");
}

Result<void> validate_sections(CheckpointKind kind,
                               const std::vector<CheckpointSection> &sections) {
  const auto count = required_count(kind);
  if (sections.size() != count)
    return bad_sections("checkpoint has missing or extra sections");
  for (size_t i = 0; i < sections.size(); ++i) {
    const auto expected_id = static_cast<uint16_t>(i + 1);
    const auto &section = sections[i];
    if (section.id != expected_id)
      return bad_sections("checkpoint section IDs must be complete and ordered");
    if (section.version != 1)
      return bad_sections("checkpoint section version is unsupported");
  }
  return Result<void>::success();
}

}  // namespace

Result<std::string> encode_checkpoint(
    CheckpointKind kind, std::vector<CheckpointSection> sections) {
  if (auto valid = validate_sections(kind, sections); !valid)
    return valid.error();

  size_t payload_size = sizeof(uint16_t);
  for (const auto &section : sections) {
    if (section.bytes.size() > kCheckpointMaxBytes ||
        payload_size > kCheckpointMaxBytes - kSectionHeaderBytes ||
        section.bytes.size() > kCheckpointMaxBytes - payload_size - kSectionHeaderBytes)
      return Error(ErrorCode::limit_exceeded, "checkpoint exceeds maximum size", "checkpoint");
    payload_size += kSectionHeaderBytes + section.bytes.size();
  }
  if (kHeaderBytes + payload_size + kDigestBytes > kCheckpointMaxBytes)
    return Error(ErrorCode::limit_exceeded, "checkpoint exceeds maximum size", "checkpoint");

  std::string out;
  out.reserve(kHeaderBytes + payload_size + kDigestBytes);
  out.append(magic(kind));
  append_u64(out, static_cast<uint64_t>(payload_size));
  append_u16(out, static_cast<uint16_t>(sections.size()));
  for (const auto &section : sections) {
    append_u16(out, section.id);
    append_u16(out, section.version);
    append_u64(out, static_cast<uint64_t>(section.bytes.size()));
    out.append(section.bytes);
  }
  out.append(raw_digest(out));
  return out;
}

Result<CheckpointArtifact> decode_checkpoint(std::string_view bytes,
                                             CheckpointKind expected_kind) {
  if (bytes.size() > kCheckpointMaxBytes)
    return Error(ErrorCode::limit_exceeded, "checkpoint exceeds maximum size", "checkpoint");
  const auto expected_magic = magic(expected_kind);
  if (bytes.size() < kHeaderBytes + sizeof(uint16_t) + kDigestBytes)
    return malformed("checkpoint frame is truncated");
  if (bytes.substr(0, expected_magic.size()) != expected_magic)
    return Error(ErrorCode::unknown_format, "checkpoint identifier or version mismatch", "checkpoint");

  const auto payload_length = read_u64(bytes, expected_magic.size());
  const size_t payload_start = kHeaderBytes;
  if (payload_length > kCheckpointMaxBytes ||
      payload_length > std::numeric_limits<size_t>::max() - payload_start - kDigestBytes)
    return Error(ErrorCode::limit_exceeded, "checkpoint payload length exceeds limit", "checkpoint");
  const size_t expected_size = payload_start + static_cast<size_t>(payload_length) + kDigestBytes;
  if (expected_size != bytes.size())
    return malformed("checkpoint declared length does not match complete input");

  const auto body_end = bytes.size() - kDigestBytes;
  const auto computed = raw_digest(bytes.substr(0, body_end));
  uint8_t difference = 0;
  for (size_t i = 0; i < kDigestBytes; ++i)
    difference |= static_cast<uint8_t>(computed[i]) ^ static_cast<uint8_t>(bytes[body_end + i]);
  if (difference != 0)
    return Error(ErrorCode::corrupt_record, "checkpoint integrity digest mismatch", "checkpoint.digest");

  const auto count = read_u16(bytes, payload_start);
  if (count == 0 || count > 32)
    return malformed("checkpoint section count is outside 1..32");
  if (count != required_count(expected_kind))
    return bad_sections("checkpoint has missing or extra sections");

  CheckpointArtifact artifact;
  artifact.kind = expected_kind;
  artifact.sections.reserve(count);
  size_t cursor = payload_start + sizeof(uint16_t);
  for (uint16_t i = 0; i < count; ++i) {
    if (cursor > body_end || body_end - cursor < kSectionHeaderBytes)
      return malformed("checkpoint section header is truncated");
    const auto id = read_u16(bytes, cursor);
    const auto version = read_u16(bytes, cursor + 2);
    const auto length = read_u64(bytes, cursor + 4);
    cursor += kSectionHeaderBytes;
    if (length > body_end - cursor)
      return malformed("checkpoint section payload is truncated or overflows frame");
    CheckpointSection section;
    section.id = id;
    section.version = version;
    section.bytes.assign(bytes.substr(cursor, static_cast<size_t>(length)));
    artifact.sections.push_back(std::move(section));
    cursor += static_cast<size_t>(length);
  }
  if (cursor != body_end)
    return malformed("checkpoint payload has trailing section bytes");
  if (auto valid = validate_sections(expected_kind, artifact.sections); !valid)
    return valid.error();
  return artifact;
}

}  // namespace CollabText::Storage
