#include "storage/CheckpointCodec.h"

#include "codec/Utf.h"
#include "crdt/SequenceOrder.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>

namespace CollabText::Storage {
namespace {

constexpr uint32_t kMaxEntries = 1'000'000;
constexpr size_t kSectionHeaderBytes = 12;

void put_u8(std::string &out, uint8_t value) {
  out.push_back(static_cast<char>(value));
}
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
void put_identity(std::string &out, const Crdt::Identity &id) {
  for (auto byte : id.actor.bytes()) out.push_back(static_cast<char>(byte));
  put_u64(out, id.counter);
}

struct Reader {
  std::string_view bytes;
  size_t cursor = 0;
  bool need(size_t count) const {
    return cursor <= bytes.size() && count <= bytes.size() - cursor;
  }
  uint8_t u8() {
    return static_cast<uint8_t>(bytes[cursor++]);
  }
  uint16_t u16() {
    const auto value = static_cast<uint16_t>(
        (static_cast<uint16_t>(static_cast<uint8_t>(bytes[cursor])) << 8) |
        static_cast<uint8_t>(bytes[cursor + 1]));
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
  Crdt::Identity identity() {
    std::array<uint8_t, ActorId::kBytes> raw{};
    for (auto &byte : raw) byte = static_cast<uint8_t>(bytes[cursor++]);
    const auto actor = ActorId::from_bytes(raw);
    return {actor, u64()};
  }
};

Error malformed(std::string message) {
  return Error(ErrorCode::malformed, std::move(message), "checkpoint.engine");
}

bool valid_identity(const Crdt::Identity &id) {
  return id.actor.valid() && id.counter != 0 && id.counter != UINT64_MAX;
}

Result<void> put_link(std::string &out, const Crdt::Link &link) {
  if (link.side != Crdt::Side::Left && link.side != Crdt::Side::Right)
    return malformed("checkpoint engine link side is invalid");
  put_u8(out, link.parent ? 1 : 0);
  if (link.parent) {
    if (!valid_identity(*link.parent)) return malformed("checkpoint link parent is invalid");
    put_identity(out, *link.parent);
  }
  put_u8(out, static_cast<uint8_t>(link.side));
  put_u8(out, link.right_origin ? 1 : 0);
  if (link.right_origin) {
    if (!valid_identity(*link.right_origin)) return malformed("checkpoint right origin is invalid");
    put_identity(out, *link.right_origin);
  }
  return Result<void>::success();
}

bool read_link(Reader &reader, Crdt::Link &link) {
  if (!reader.need(1)) return false;
  const auto has_parent = reader.u8();
  if (has_parent > 1 || (has_parent && !reader.need(24))) return false;
  if (has_parent) link.parent = reader.identity();
  if (!reader.need(2)) return false;
  const auto side = reader.u8();
  if (side > static_cast<uint8_t>(Crdt::Side::Right)) return false;
  link.side = static_cast<Crdt::Side>(side);
  const auto has_right = reader.u8();
  if (has_right > 1 || (has_right && !reader.need(24))) return false;
  if (has_right) link.right_origin = reader.identity();
  if ((link.parent && !valid_identity(*link.parent)) ||
      (link.right_origin && !valid_identity(*link.right_origin))) return false;
  if (link.side == Crdt::Side::Left && link.parent != link.right_origin) return false;
  return true;
}

Result<void> put_deletions(std::string &out,
                           const std::vector<Crdt::Identity> &deletions) {
  if (deletions.size() > kMaxEntries)
    return Error(ErrorCode::limit_exceeded, "checkpoint deletion list exceeds entry limit", "checkpoint.engine");
  if (out.size() > kCheckpointMaxBytes - kSectionHeaderBytes - 4 ||
      deletions.size() >
          (kCheckpointMaxBytes - kSectionHeaderBytes - out.size() - 4) / 24)
    return Error(ErrorCode::limit_exceeded, "checkpoint engine section exceeds byte limit", "checkpoint.engine");
  put_u32(out, static_cast<uint32_t>(deletions.size()));
  std::set<Crdt::Identity> unique;
  for (const auto &id : deletions) {
    if (!valid_identity(id) || !unique.insert(id).second)
      return malformed("checkpoint deletion identities are invalid or repeated");
    put_identity(out, id);
  }
  return Result<void>::success();
}

bool read_deletions(Reader &reader, std::vector<Crdt::Identity> &deletions) {
  if (!reader.need(4)) return false;
  const auto count = reader.u32();
  if (count > kMaxEntries || count > (reader.bytes.size() - reader.cursor) / 24)
    return false;
  deletions.reserve(count);
  std::set<Crdt::Identity> unique;
  for (uint32_t i = 0; i < count; ++i) {
    auto id = reader.identity();
    if (!valid_identity(id) || !unique.insert(id).second) return false;
    deletions.push_back(id);
  }
  return true;
}

}  // namespace

Result<CheckpointSection> encode_checkpoint_engine(
    const CheckpointEngineState &state) {
  if (state.kind == CheckpointEngineKind::Text && !state.entries.empty())
    return malformed("text checkpoint contains IdList entries");
  if (state.kind == CheckpointEngineKind::IdList && !state.fragments.empty())
    return malformed("IdList checkpoint contains text fragments");
  if (state.kind != CheckpointEngineKind::Text &&
      state.kind != CheckpointEngineKind::IdList)
    return malformed("checkpoint engine kind is invalid");
  const size_t count = state.kind == CheckpointEngineKind::Text
                           ? state.fragments.size() : state.entries.size();
  if (count > kMaxEntries)
    return Error(ErrorCode::limit_exceeded, "checkpoint engine exceeds entry limit", "checkpoint.engine");
  std::string payload;
  put_u16(payload, static_cast<uint16_t>(state.kind));
  put_u32(payload, static_cast<uint32_t>(count));
  if (state.kind == CheckpointEngineKind::Text) {
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    std::map<ActorId, std::vector<std::pair<uint64_t, uint64_t>>> by_actor;
    for (const auto &fragment : state.fragments) {
      size_t scalars = 0;
      if (!valid_identity(fragment.origin) || fragment.length == 0 ||
          fragment.length > UINT64_MAX - fragment.origin.counter ||
          fragment.byte_length != fragment.text.size() ||
          !Codec::utf8_length(fragment.text, scalars).ok() || scalars != fragment.length)
        return malformed("checkpoint fragment identity or UTF-8 lengths are invalid");
      put_identity(payload, fragment.origin);
      if (auto valid = put_link(payload, fragment.link); !valid) return valid.error();
      if (auto valid = put_deletions(payload, fragment.deletions); !valid) return valid.error();
      if (payload.size() > kCheckpointMaxBytes - kSectionHeaderBytes - 8 ||
          fragment.text.size() >
              kCheckpointMaxBytes - kSectionHeaderBytes - payload.size() - 8)
        return Error(ErrorCode::limit_exceeded, "checkpoint engine section exceeds byte limit", "checkpoint.engine");
      put_u32(payload, fragment.byte_length);
      put_u32(payload, fragment.length);
      payload.append(fragment.text);
      by_actor[fragment.origin.actor].emplace_back(
          fragment.origin.counter, fragment.origin.counter + fragment.length);
      if (payload.size() > kCheckpointMaxBytes - kSectionHeaderBytes)
        return Error(ErrorCode::limit_exceeded, "checkpoint engine section exceeds byte limit", "checkpoint.engine");
    }
    for (auto &[actor, intervals] : by_actor) {
      (void)actor;
      std::sort(intervals.begin(), intervals.end());
      for (size_t i = 1; i < intervals.size(); ++i)
        if (intervals[i - 1].second > intervals[i].first)
          return malformed("checkpoint fragment identity intervals overlap");
    }
    if (const auto issue = Crdt::SequenceOrder::check_consistency(state.fragments))
      return malformed(*issue);
  } else {
    std::set<Crdt::Identity> unique_origins;
    for (const auto &entry : state.entries) {
      if (!valid_identity(entry.origin) || !unique_origins.insert(entry.origin).second)
        return malformed("checkpoint IdList entry identity is invalid or repeated");
      put_identity(payload, entry.origin);
      if (auto valid = put_link(payload, entry.link); !valid) return valid.error();
      put_u64(payload, entry.id);
      if (auto valid = put_deletions(payload, entry.deletions); !valid) return valid.error();
      if (payload.size() > kCheckpointMaxBytes - kSectionHeaderBytes)
        return Error(ErrorCode::limit_exceeded, "checkpoint engine section exceeds byte limit", "checkpoint.engine");
    }
    if (const auto issue = Crdt::SequenceOrder::check_consistency(state.entries))
      return malformed(*issue);
  }
  return CheckpointSection{2, 1, std::move(payload)};
}

Result<CheckpointEngineState> decode_checkpoint_engine(
    const CheckpointSection &section) {
  if (section.id != 2) return malformed("checkpoint engine state must be section 2");
  if (section.version != 1)
    return Error(ErrorCode::unsupported_version, "checkpoint engine section version is unsupported", "checkpoint.engine");
  if (section.bytes.size() > kCheckpointMaxBytes - kSectionHeaderBytes)
    return Error(ErrorCode::limit_exceeded, "checkpoint engine section exceeds byte limit", "checkpoint.engine");
  Reader reader{section.bytes};
  if (!reader.need(6)) return malformed("checkpoint engine header is truncated");
  const auto kind = reader.u16();
  if (kind != static_cast<uint16_t>(CheckpointEngineKind::Text) &&
      kind != static_cast<uint16_t>(CheckpointEngineKind::IdList))
    return malformed("checkpoint engine kind is invalid");
  CheckpointEngineState state;
  state.kind = static_cast<CheckpointEngineKind>(kind);
  const auto count = reader.u32();
  if (count > kMaxEntries)
    return Error(ErrorCode::limit_exceeded, "checkpoint engine exceeds entry limit", "checkpoint.engine");
  if (state.kind == CheckpointEngineKind::Text) {
    constexpr size_t kMinFragmentBytes = 24 + 3 + 4 + 8 + 1;
    if (count > (reader.bytes.size() - reader.cursor) / kMinFragmentBytes)
      return malformed("checkpoint fragment array is truncated");
    state.fragments.reserve(count);
    std::map<ActorId, std::vector<std::pair<uint64_t, uint64_t>>> by_actor;
    for (uint32_t i = 0; i < count; ++i) {
      if (!reader.need(24)) return malformed("checkpoint fragment origin is truncated");
      Crdt::Fragment fragment;
      fragment.origin = reader.identity();
      if (!valid_identity(fragment.origin) || !read_link(reader, fragment.link) ||
          !read_deletions(reader, fragment.deletions) || !reader.need(8))
        return malformed("checkpoint fragment metadata is invalid or truncated");
      fragment.byte_length = reader.u32();
      fragment.length = reader.u32();
      if (fragment.byte_length > reader.bytes.size() - reader.cursor)
        return malformed("checkpoint fragment text is truncated");
      fragment.text.assign(reader.bytes.substr(reader.cursor, fragment.byte_length));
      reader.cursor += fragment.byte_length;
      size_t scalars = 0;
      if (fragment.length == 0 || fragment.length > UINT64_MAX - fragment.origin.counter ||
          !Codec::utf8_length(fragment.text, scalars).ok() || scalars != fragment.length)
        return malformed("checkpoint fragment text or identity range is invalid");
      by_actor[fragment.origin.actor].emplace_back(
          fragment.origin.counter, fragment.origin.counter + fragment.length);
      state.fragments.push_back(std::move(fragment));
    }
    for (auto &[actor, intervals] : by_actor) {
      (void)actor;
      std::sort(intervals.begin(), intervals.end());
      for (size_t i = 1; i < intervals.size(); ++i)
        if (intervals[i - 1].second > intervals[i].first)
          return malformed("checkpoint fragment identity intervals overlap");
    }
    if (const auto issue = Crdt::SequenceOrder::check_consistency(state.fragments))
      return malformed(*issue);
  } else {
    constexpr size_t kMinEntryBytes = 24 + 3 + 8 + 4;
    if (count > (reader.bytes.size() - reader.cursor) / kMinEntryBytes)
      return malformed("checkpoint IdList array is truncated");
    state.entries.reserve(count);
    std::set<Crdt::Identity> unique_origins;
    for (uint32_t i = 0; i < count; ++i) {
      if (!reader.need(24)) return malformed("checkpoint IdList origin is truncated");
      Crdt::IdListEntry entry;
      entry.origin = reader.identity();
      if (!valid_identity(entry.origin) || !unique_origins.insert(entry.origin).second ||
          !read_link(reader, entry.link) || !reader.need(12))
        return malformed("checkpoint IdList entry metadata is invalid or truncated");
      entry.id = reader.u64();
      if (!read_deletions(reader, entry.deletions))
        return malformed("checkpoint IdList deletion list is invalid or truncated");
      state.entries.push_back(std::move(entry));
    }
    if (const auto issue = Crdt::SequenceOrder::check_consistency(state.entries))
      return malformed(*issue);
  }
  if (reader.cursor != reader.bytes.size())
    return malformed("checkpoint engine section has trailing bytes");
  return state;
}

Result<CheckpointShareableCore> decode_checkpoint_core(
    const CheckpointArtifact &artifact) {
  if (artifact.kind != CheckpointKind::Shareable || artifact.sections.size() != 7)
    return malformed("checkpoint core requires a complete shareable artifact");
  for (size_t i = 0; i < artifact.sections.size(); ++i)
    if (artifact.sections[i].id != i + 1)
      return malformed("checkpoint core sections are missing or unordered");

  CheckpointShareableCore core;
  auto context = decode_checkpoint_context(artifact.sections[0]);
  if (!context) return context.error().with_context("checkpoint.core");
  core.context = std::move(context.value());
  auto engine = decode_checkpoint_engine(artifact.sections[1]);
  if (!engine) return engine.error().with_context("checkpoint.core");
  core.engine = std::move(engine.value());
  if (core.context.engine != core.engine.kind)
    return malformed("checkpoint context and engine section kinds disagree");
  auto causal = decode_checkpoint_causal(artifact.sections[2]);
  if (!causal) return causal.error().with_context("checkpoint.core");
  core.causal = std::move(causal.value());
  auto deferred = decode_checkpoint_deferred(
      artifact.sections[3], core.context, core.causal.version);
  if (!deferred) return deferred.error().with_context("checkpoint.core");
  core.deferred = std::move(deferred.value());
  auto coverage = decode_checkpoint_coverage(artifact.sections[4]);
  if (!coverage) return coverage.error().with_context("checkpoint.core");
  core.coverage = std::move(coverage.value());

  auto covered = [&](const Crdt::Identity &id) {
    return valid_identity(id) && core.causal.version.observed(id);
  };
  auto covered_link = [&](const Crdt::Link &link) {
    return (!link.parent || covered(*link.parent)) &&
           (!link.right_origin || covered(*link.right_origin));
  };
  if (core.engine.kind == CheckpointEngineKind::Text) {
    for (const auto &fragment : core.engine.fragments) {
      const auto last_item = Crdt::Identity(
          fragment.origin.actor,
          fragment.origin.counter + static_cast<uint64_t>(fragment.length) - 1);
      if (!covered(last_item) || !covered_link(fragment.link))
        return malformed("checkpoint text entry references identity outside causal state");
      for (const auto &deletion : fragment.deletions)
        if (!covered(deletion))
          return malformed("checkpoint text deletion is outside causal state");
    }
  } else {
    for (const auto &entry : core.engine.entries) {
      if (!covered(entry.origin) || !covered_link(entry.link))
        return malformed("checkpoint list entry references identity outside causal state");
      for (const auto &deletion : entry.deletions)
        if (!covered(deletion))
          return malformed("checkpoint list deletion is outside causal state");
    }
  }
  for (const auto &entry : core.causal.undo_entries)
    if (!covered(entry.key.edit_id) || !covered(entry.key.undo_id))
      return malformed("checkpoint UndoMap identity is outside causal state");
  for (const auto &[actor, frontier] : core.coverage.operation)
    if (core.causal.version.get(actor) < frontier)
      return malformed("checkpoint operation coverage exceeds causal state");
  for (const auto &record : core.deferred)
    if (core.coverage.delivery.contains(record.token.producer) &&
        core.coverage.delivery.at(record.token.producer) >= record.token.sequence)
      return malformed("deferred delivery is already included in committed coverage");
  auto pins = decode_checkpoint_pins(artifact.sections[5], core.context);
  if (!pins) return pins.error().with_context("checkpoint.core");
  core.pins = std::move(pins.value());
  return core;
}

}  // namespace CollabText::Storage
