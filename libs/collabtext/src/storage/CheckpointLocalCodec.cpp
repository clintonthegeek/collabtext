#include "storage/CheckpointCodec.h"

#include <algorithm>
#include <array>
#include <limits>
#include <set>

namespace CollabText::Storage {
namespace {

constexpr uint32_t kMaxHistoryGroups = 100000;
constexpr uint32_t kMaxHistoryIdentities = 2000000;
constexpr uint64_t kMaxRetainedBytes = 64u * 1024u * 1024u;

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
void put_actor(std::string &out, const ActorId &actor) {
  for (const auto byte : actor.bytes())
    out.push_back(static_cast<char>(byte));
}
void put_identity(std::string &out, const Crdt::Identity &identity) {
  put_actor(out, identity.actor);
  put_u64(out, identity.counter);
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
Error malformed(std::string message) {
  return Error(ErrorCode::malformed, std::move(message), "checkpoint.local");
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
                       uint16_t(static_cast<uint8_t>(bytes[cursor + 1]));
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
  ActorId actor() {
    std::array<uint8_t, ActorId::kBytes> raw{};
    for (auto &byte : raw)
      byte = u8();
    return ActorId::from_bytes(raw);
  }
  Crdt::Identity identity() {
    const auto id = actor();
    return Crdt::Identity{id, u64()};
  }
};

Result<void> validate_history(const Crdt::LocalHistorySnapshot &history,
                              const ActorId &owner) {
  if (!owner.valid() || history.entries.size() > kMaxHistoryGroups ||
      history.retired_entries.size() > kMaxHistoryGroups ||
      history.entries.size() + history.retired_entries.size() >
          kMaxHistoryGroups ||
      history.cursor > history.entries.size())
    return malformed("local history owner, counts, or cursor are invalid");
  std::set<Crdt::Identity> groups;
  size_t identity_count = 0;
  auto valid_identity = [&](const Crdt::Identity &id) {
    return id.actor == owner && id.counter != 0 && id.counter != UINT64_MAX;
  };
  auto validate_entry = [&](const Crdt::LocalUndoEntrySnapshot &entry) -> bool {
    if (!valid_identity(entry.group_id) || entry.operation_ids.empty() ||
        entry.operation_ids.size() > kMaxHistoryIdentities ||
        entry.inserted_items.size() > kMaxHistoryIdentities ||
        entry.deletion_operations.size() > kMaxHistoryIdentities ||
        entry.retained_bytes > kMaxRetainedBytes ||
        !groups.insert(entry.group_id).second)
      return false;
    const size_t count = entry.operation_ids.size() +
                         entry.inserted_items.size() +
                         entry.deletion_operations.size() + 1;
    if (count > kMaxHistoryIdentities - identity_count)
      return false;
    identity_count += count;
    auto check_unique = [&](const std::vector<Crdt::Identity> &ids) {
      std::set<Crdt::Identity> unique;
      return std::all_of(ids.begin(), ids.end(), [&](const auto &id) {
        return valid_identity(id) && unique.insert(id).second;
      });
    };
    return check_unique(entry.operation_ids) &&
           check_unique(entry.inserted_items) &&
           check_unique(entry.deletion_operations);
  };
  for (const auto &entry : history.entries)
    if (!validate_entry(entry))
      return malformed("local history entry is invalid");
  for (const auto &entry : history.retired_entries)
    if (!validate_entry(entry))
      return malformed("retired local history entry is invalid");
  return Result<void>::success();
}

void put_ids(std::string &out, const std::vector<Crdt::Identity> &ids) {
  put_u32(out, static_cast<uint32_t>(ids.size()));
  for (const auto &id : ids)
    put_identity(out, id);
}
void put_entry(std::string &out, const Crdt::LocalUndoEntrySnapshot &entry) {
  put_identity(out, entry.group_id);
  put_ids(out, entry.operation_ids);
  put_ids(out, entry.inserted_items);
  put_ids(out, entry.deletion_operations);
  put_u64(out, static_cast<uint64_t>(entry.retained_bytes));
}

bool read_ids(Reader &reader, std::vector<Crdt::Identity> &ids,
              const ActorId &owner, size_t &identity_count) {
  if (!reader.need(4))
    return false;
  const auto count = reader.u32();
  if (count > kMaxHistoryIdentities - identity_count ||
      count > (reader.bytes.size() - reader.cursor) / 24)
    return false;
  identity_count += count;
  ids.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const auto id = reader.identity();
    if (id.actor != owner || !id.counter || id.counter == UINT64_MAX)
      return false;
    ids.push_back(id);
  }
  return true;
}

bool read_entry(Reader &reader, Crdt::LocalUndoEntrySnapshot &entry,
                const ActorId &owner, size_t &identity_count) {
  if (!reader.need(24 + 4 + 4 + 4 + 8))
    return false;
  entry.group_id = reader.identity();
  if (entry.group_id.actor != owner || !entry.group_id.counter ||
      entry.group_id.counter == UINT64_MAX ||
      !read_ids(reader, entry.operation_ids, owner, identity_count) ||
      entry.operation_ids.empty() ||
      !read_ids(reader, entry.inserted_items, owner, identity_count) ||
      !read_ids(reader, entry.deletion_operations, owner, identity_count) ||
      !reader.need(8))
    return false;
  const auto retained = reader.u64();
  if (retained > kMaxRetainedBytes || retained > SIZE_MAX)
    return false;
  entry.retained_bytes = static_cast<size_t>(retained);
  return true;
}

bool valid_context_strings(const CheckpointContext &context) {
  return static_cast<bool>(encode_checkpoint_context(context));
}

} // namespace

Result<CheckpointSection>
encode_checkpoint_local_owner(const CheckpointLocalOwner &owner) {
  if (!owner.actor.valid() || !valid_hash(owner.shareable_checkpoint_hash))
    return malformed("local owner or shareable checkpoint digest is invalid");
  std::string payload;
  put_actor(payload, owner.actor);
  payload.append(unhex(owner.shareable_checkpoint_hash));
  return CheckpointSection{1, 1, std::move(payload)};
}

Result<CheckpointLocalOwner>
decode_checkpoint_local_owner(const CheckpointSection &section) {
  if (section.id != 1)
    return malformed("local owner must be section 1");
  if (section.version != 1)
    return Error(ErrorCode::unsupported_version,
                 "local owner section version is unsupported",
                 "checkpoint.local");
  if (section.bytes.size() != 48)
    return malformed("local owner section size is invalid");
  Reader reader{section.bytes};
  CheckpointLocalOwner owner;
  owner.actor = reader.actor();
  owner.shareable_checkpoint_hash = hex(reader.bytes.substr(reader.cursor, 32));
  if (!owner.actor.valid())
    return malformed("local owner actor is invalid");
  return owner;
}

Result<CheckpointSection>
encode_checkpoint_local_history(const Crdt::LocalHistorySnapshot &history,
                                const ActorId &owner) {
  if (auto valid = validate_history(history, owner); !valid)
    return valid.error();
  std::string payload;
  put_actor(payload, owner);
  put_u64(payload, static_cast<uint64_t>(history.cursor));
  put_u32(payload, static_cast<uint32_t>(history.entries.size()));
  put_u32(payload, static_cast<uint32_t>(history.retired_entries.size()));
  for (const auto &entry : history.entries)
    put_entry(payload, entry);
  for (const auto &entry : history.retired_entries)
    put_entry(payload, entry);
  if (payload.size() > 64u * 1024u * 1024u)
    return Error(ErrorCode::limit_exceeded,
                 "local history section exceeds byte limit",
                 "checkpoint.local");
  return CheckpointSection{2, 1, std::move(payload)};
}

Result<Crdt::LocalHistorySnapshot>
decode_checkpoint_local_history(const CheckpointSection &section,
                                const ActorId &owner) {
  if (section.id != 2)
    return malformed("local history must be section 2");
  if (section.version != 1)
    return Error(ErrorCode::unsupported_version,
                 "local history section version is unsupported",
                 "checkpoint.local");
  if (section.bytes.size() > 64u * 1024u * 1024u || section.bytes.size() < 32)
    return malformed("local history section is truncated or oversized");
  Reader reader{section.bytes};
  if (reader.actor() != owner)
    return malformed("local history belongs to another actor");
  const auto cursor = reader.u64();
  const auto active_count = reader.u32();
  const auto retired_count = reader.u32();
  if (active_count > kMaxHistoryGroups || retired_count > kMaxHistoryGroups ||
      uint64_t(active_count) + retired_count > kMaxHistoryGroups ||
      cursor > active_count ||
      uint64_t(active_count + retired_count) >
          (reader.bytes.size() - reader.cursor) / 44)
    return malformed("local history header counts or cursor are invalid");
  Crdt::LocalHistorySnapshot history;
  history.cursor = static_cast<size_t>(cursor);
  history.entries.reserve(active_count);
  history.retired_entries.reserve(retired_count);
  size_t identity_count = 0;
  for (uint32_t i = 0; i < active_count; ++i) {
    Crdt::LocalUndoEntrySnapshot entry;
    if (!read_entry(reader, entry, owner, identity_count))
      return malformed("local history entry is invalid or truncated");
    history.entries.push_back(std::move(entry));
  }
  for (uint32_t i = 0; i < retired_count; ++i) {
    Crdt::LocalUndoEntrySnapshot entry;
    if (!read_entry(reader, entry, owner, identity_count))
      return malformed("retired local history entry is invalid or truncated");
    history.retired_entries.push_back(std::move(entry));
  }
  if (reader.cursor != reader.bytes.size())
    return malformed("local history section has trailing bytes");
  if (auto valid = validate_history(history, owner); !valid)
    return valid.error();
  return history;
}

Result<CheckpointSection>
encode_checkpoint_local_writer(const CheckpointLocalWriter &writer,
                               const CheckpointContext &context) {
  if (!writer.actor.valid() || !valid_context_strings(context) ||
      writer.document != context.document || writer.stream != context.stream ||
      writer.epoch != context.epoch || writer.next_identity_counter == 0 ||
      writer.document.size() > UINT16_MAX || writer.stream.size() > UINT16_MAX)
    return malformed("local writer state does not match checkpoint context");
  std::string payload;
  put_actor(payload, writer.actor);
  put_u16(payload, static_cast<uint16_t>(writer.document.size()));
  payload.append(writer.document);
  put_u16(payload, static_cast<uint16_t>(writer.stream.size()));
  payload.append(writer.stream);
  put_u64(payload, writer.epoch);
  put_u64(payload, writer.next_identity_counter);
  put_u64(payload, writer.durable_delivery_frontier);
  return CheckpointSection{3, 1, std::move(payload)};
}

Result<CheckpointLocalWriter>
decode_checkpoint_local_writer(const CheckpointSection &section,
                               const CheckpointContext &context) {
  if (section.id != 3)
    return malformed("local writer state must be section 3");
  if (section.version != 1)
    return Error(ErrorCode::unsupported_version,
                 "local writer section version is unsupported",
                 "checkpoint.local");
  if (!valid_context_strings(context) || section.bytes.size() < 42)
    return malformed("local writer section is truncated or context is invalid");
  Reader reader{section.bytes};
  CheckpointLocalWriter writer;
  writer.actor = reader.actor();
  const auto document_length = reader.u16();
  if (!document_length || !reader.need(size_t(document_length) + 2))
    return malformed("local writer document is truncated");
  writer.document.assign(reader.bytes.substr(reader.cursor, document_length));
  reader.cursor += document_length;
  const auto stream_length = reader.u16();
  if (!stream_length || !reader.need(size_t(stream_length) + 24))
    return malformed("local writer stream or counters are truncated");
  writer.stream.assign(reader.bytes.substr(reader.cursor, stream_length));
  reader.cursor += stream_length;
  writer.epoch = reader.u64();
  writer.next_identity_counter = reader.u64();
  writer.durable_delivery_frontier = reader.u64();
  if (reader.cursor != reader.bytes.size() || !writer.actor.valid() ||
      writer.document != context.document || writer.stream != context.stream ||
      writer.epoch != context.epoch || !writer.next_identity_counter)
    return malformed(
        "local writer state is noncanonical or mismatches context");
  return writer;
}

Result<CheckpointLocalCore>
decode_checkpoint_local_core(const CheckpointArtifact &artifact,
                             const ActorId &expected_owner,
                             std::string_view expected_shareable_hash,
                             const CheckpointContext &context) {
  if (artifact.kind != CheckpointKind::Local || artifact.sections.size() != 3 ||
      !expected_owner.valid() || !valid_hash(expected_shareable_hash))
    return malformed(
        "local core requires a complete artifact and valid expected binding");
  for (size_t i = 0; i < artifact.sections.size(); ++i)
    if (artifact.sections[i].id != i + 1)
      return malformed("local core sections are missing or unordered");
  auto owner = decode_checkpoint_local_owner(artifact.sections[0]);
  if (!owner)
    return owner.error();
  if (owner.value().actor != expected_owner ||
      owner.value().shareable_checkpoint_hash != expected_shareable_hash)
    return Error(ErrorCode::actor_conflict,
                 "local companion belongs to another owner or checkpoint",
                 "checkpoint.local");
  auto history =
      decode_checkpoint_local_history(artifact.sections[1], expected_owner);
  if (!history)
    return history.error();
  auto writer = decode_checkpoint_local_writer(artifact.sections[2], context);
  if (!writer)
    return writer.error();
  if (writer.value().actor != expected_owner)
    return Error(ErrorCode::actor_conflict,
                 "local writer identity differs from owner context",
                 "checkpoint.local");
  return CheckpointLocalCore{std::move(owner.value()),
                             std::move(history.value()),
                             std::move(writer.value())};
}

} // namespace CollabText::Storage
