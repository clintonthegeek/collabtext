#include "storage/CheckpointCodec.h"

#include "core/Sha256.h"
#include "codec/Utf.h"

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

void append_u32(std::string &out, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8)
    out.push_back(static_cast<char>(value >> shift));
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

uint32_t read_u32(std::string_view bytes, size_t at) {
  uint32_t value = 0;
  for (size_t i = 0; i < 4; ++i)
    value = (value << 8) | static_cast<uint8_t>(bytes[at + i]);
  return value;
}

uint64_t read_u64(std::string_view bytes, size_t at);

void append_actor(std::string &out, const ActorId &actor) {
  for (const auto byte : actor.bytes()) out.push_back(static_cast<char>(byte));
}

ActorId read_actor(std::string_view bytes, size_t at) {
  std::array<uint8_t, ActorId::kBytes> raw{};
  for (size_t i = 0; i < raw.size(); ++i)
    raw[i] = static_cast<uint8_t>(bytes[at + i]);
  return ActorId::from_bytes(raw);
}

void append_identity(std::string &out, const Crdt::Identity &identity) {
  append_actor(out, identity.actor);
  append_u64(out, identity.counter);
}

Crdt::Identity read_identity(std::string_view bytes, size_t at) {
  return Crdt::Identity(read_actor(bytes, at), read_u64(bytes, at + ActorId::kBytes));
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

bool valid_hash(std::string_view hash) {
  if (hash.size() != 64) return false;
  return std::all_of(hash.begin(), hash.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}

uint8_t hex_nibble(char c) {
  return static_cast<uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10);
}

std::string hex_digest(std::string_view raw) {
  constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(raw.size() * 2);
  for (const unsigned char byte : raw) {
    out.push_back(digits[byte >> 4]);
    out.push_back(digits[byte & 0x0f]);
  }
  return out;
}

bool valid_context_string(std::string_view value) {
  return !value.empty() && value.size() <= 256 &&
         value.find('\0') == std::string_view::npos &&
         Codec::is_valid_utf8(value);
}

Result<void> validate_context(const CheckpointContext &context) {
  if (!valid_context_string(context.document) ||
      !valid_context_string(context.stream))
    return Error(ErrorCode::malformed,
                 "checkpoint document and stream must be valid nonempty UTF-8 identifiers of at most 256 bytes",
                 "checkpoint.context");
  if (context.epoch == 0)
    return Error(ErrorCode::malformed,
                 "checkpoint context epoch must be nonzero",
                 "checkpoint.context");
  if (context.engine != CheckpointEngineKind::Text &&
      context.engine != CheckpointEngineKind::IdList)
    return Error(ErrorCode::malformed,
                 "checkpoint context engine kind is invalid",
                 "checkpoint.context");
  if (!context.parent_descriptor_hash.empty() &&
      !valid_hash(context.parent_descriptor_hash))
    return Error(ErrorCode::malformed,
                 "checkpoint parent descriptor hash must be 64 lowercase hexadecimal digits",
                 "checkpoint.context");
  return Result<void>::success();
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
    const bool supported = kind == CheckpointKind::Shareable && section.id == 2
        ? (section.version == 2 || section.version == 3 || section.version == 4)
        : section.version == 1;
    if (!supported)
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

Result<CheckpointSection> encode_checkpoint_context(
    const CheckpointContext &context) {
  if (auto valid = validate_context(context); !valid) return valid.error();
  std::string payload;
  payload.reserve(2 + 2 + context.document.size() + 2 + context.stream.size() +
                  8 + 1 + (context.parent_descriptor_hash.empty() ? 0 : 32));
  append_u16(payload, static_cast<uint16_t>(context.engine));
  append_u16(payload, static_cast<uint16_t>(context.document.size()));
  payload.append(context.document);
  append_u16(payload, static_cast<uint16_t>(context.stream.size()));
  payload.append(context.stream);
  append_u64(payload, context.epoch);
  payload.push_back(context.parent_descriptor_hash.empty() ? '\0' : '\1');
  if (!context.parent_descriptor_hash.empty()) {
    for (size_t i = 0; i < context.parent_descriptor_hash.size(); i += 2)
      payload.push_back(static_cast<char>(
          (hex_nibble(context.parent_descriptor_hash[i]) << 4) |
          hex_nibble(context.parent_descriptor_hash[i + 1])));
  }
  return CheckpointSection{1, 1, std::move(payload)};
}

Result<CheckpointContext> decode_checkpoint_context(
    const CheckpointSection &section) {
  if (section.id != 1)
    return Error(ErrorCode::malformed,
                 "checkpoint context must be section 1",
                 "checkpoint.context");
  if (section.version != 1)
    return Error(ErrorCode::unsupported_version,
                 "checkpoint context section version is unsupported",
                 "checkpoint.context");
  const std::string_view bytes(section.bytes);
  size_t cursor = 0;
  auto need = [&](size_t count) {
    return cursor <= bytes.size() && count <= bytes.size() - cursor;
  };
  if (!need(2)) return malformed("checkpoint context engine kind is truncated");
  CheckpointContext context;
  const auto engine = read_u16(bytes, cursor);
  cursor += 2;
  if (engine != static_cast<uint16_t>(CheckpointEngineKind::Text) &&
      engine != static_cast<uint16_t>(CheckpointEngineKind::IdList))
    return malformed("checkpoint context engine kind is invalid");
  context.engine = static_cast<CheckpointEngineKind>(engine);

  if (!need(2)) return malformed("checkpoint context document length is truncated");
  const auto document_length = read_u16(bytes, cursor);
  cursor += 2;
  if (document_length > 256 || !need(document_length))
    return malformed("checkpoint context document is truncated or too long");
  context.document.assign(bytes.substr(cursor, document_length));
  cursor += document_length;

  if (!need(2)) return malformed("checkpoint context stream length is truncated");
  const auto stream_length = read_u16(bytes, cursor);
  cursor += 2;
  if (stream_length > 256 || !need(stream_length))
    return malformed("checkpoint context stream is truncated or too long");
  context.stream.assign(bytes.substr(cursor, stream_length));
  cursor += stream_length;

  if (!need(9)) return malformed("checkpoint context epoch or parent flag is truncated");
  context.epoch = read_u64(bytes, cursor);
  cursor += 8;
  const auto has_parent = static_cast<uint8_t>(bytes[cursor++]);
  if (has_parent > 1)
    return malformed("checkpoint context parent hash flag is invalid");
  if (has_parent) {
    if (!need(32)) return malformed("checkpoint context parent hash is truncated");
    context.parent_descriptor_hash = hex_digest(bytes.substr(cursor, 32));
    cursor += 32;
  }
  if (cursor != bytes.size())
    return malformed("checkpoint context has trailing bytes");
  if (auto valid = validate_context(context); !valid) return valid.error();
  return context;
}

Result<CheckpointSection> encode_checkpoint_causal(
    const CheckpointCausalState &state) {
  constexpr size_t kMaxEntries = 1'000'000;
  if (state.version.pair_count() > kMaxEntries ||
      state.undo_entries.size() > kMaxEntries)
    return Error(ErrorCode::limit_exceeded,
                 "checkpoint causal state exceeds entry limit",
                 "checkpoint.causal");
  std::string payload;
  append_u32(payload, static_cast<uint32_t>(state.version.pair_count()));
  for (size_t i = 0; i < state.version.pair_count(); ++i) {
    const auto [actor, counter] = state.version.pair(i);
    if (!actor.valid() || counter == 0 ||
        (i && !(state.version.pair(i - 1).first < actor)))
      return malformed("checkpoint causal version vector is not canonical");
    append_actor(payload, actor);
    append_u64(payload, counter);
  }
  append_u32(payload, static_cast<uint32_t>(state.undo_entries.size()));
  const Crdt::UndoMapEntry *previous = nullptr;
  for (const auto &entry : state.undo_entries) {
    if (!entry.key.edit_id.actor.valid() || entry.key.edit_id.counter == 0 ||
        !entry.key.undo_id.actor.valid() || entry.key.undo_id.counter == 0 ||
        entry.undo_count == 0 ||
        (previous && !(previous->key.edit_id < entry.key.edit_id ||
                       (previous->key.edit_id == entry.key.edit_id &&
                        previous->key.undo_id < entry.key.undo_id))))
      return malformed("checkpoint UndoMap entries are not canonical");
    append_identity(payload, entry.key.edit_id);
    append_identity(payload, entry.key.undo_id);
    append_u32(payload, entry.undo_count);
    previous = &entry;
  }
  if (payload.size() > kCheckpointMaxBytes - kSectionHeaderBytes)
    return Error(ErrorCode::limit_exceeded,
                 "checkpoint causal section exceeds byte limit",
                 "checkpoint.causal");
  return CheckpointSection{3, 1, std::move(payload)};
}

Result<CheckpointCausalState> decode_checkpoint_causal(
    const CheckpointSection &section) {
  constexpr uint32_t kMaxEntries = 1'000'000;
  if (section.id != 3)
    return malformed("checkpoint causal state must be section 3");
  if (section.version != 1)
    return Error(ErrorCode::unsupported_version,
                 "checkpoint causal section version is unsupported",
                 "checkpoint.causal");
  const std::string_view bytes(section.bytes);
  size_t cursor = 0;
  auto need = [&](size_t count) {
    return cursor <= bytes.size() && count <= bytes.size() - cursor;
  };
  if (!need(4)) return malformed("checkpoint version vector count is truncated");
  const auto version_count = read_u32(bytes, cursor);
  cursor += 4;
  if (version_count > kMaxEntries)
    return Error(ErrorCode::limit_exceeded, "checkpoint version vector exceeds entry limit", "checkpoint.causal");
  if (version_count > (bytes.size() - cursor) / 24)
    return malformed("checkpoint version vector is truncated");
  CheckpointCausalState state;
  std::optional<ActorId> previous_actor;
  for (uint32_t i = 0; i < version_count; ++i) {
    const auto actor = read_actor(bytes, cursor);
    const auto counter = read_u64(bytes, cursor + ActorId::kBytes);
    cursor += 24;
    if (!actor.valid() || counter == 0 ||
        (previous_actor && !(*previous_actor < actor)))
      return malformed("checkpoint version vector is not canonical");
    state.version.observe(Crdt::Identity(actor, counter));
    previous_actor = actor;
  }
  if (!need(4)) return malformed("checkpoint UndoMap count is truncated");
  const auto undo_count = read_u32(bytes, cursor);
  cursor += 4;
  if (undo_count > kMaxEntries)
    return Error(ErrorCode::limit_exceeded, "checkpoint UndoMap exceeds entry limit", "checkpoint.causal");
  constexpr size_t kUndoBytes = 52;
  if (undo_count > (bytes.size() - cursor) / kUndoBytes)
    return malformed("checkpoint UndoMap entries are truncated");
  state.undo_entries.reserve(undo_count);
  for (uint32_t i = 0; i < undo_count; ++i) {
    Crdt::UndoMapEntry entry;
    entry.key.edit_id = read_identity(bytes, cursor);
    entry.key.undo_id = read_identity(bytes, cursor + 24);
    entry.undo_count = read_u32(bytes, cursor + 48);
    cursor += kUndoBytes;
    state.undo_entries.push_back(entry);
  }
  if (cursor != bytes.size()) return malformed("checkpoint causal section has trailing bytes");
  Crdt::UndoMap validator;
  if (auto valid = validator.restore_entries(state.undo_entries); !valid)
    return valid.error().with_context("checkpoint.causal");
  return state;
}

}  // namespace CollabText::Storage
