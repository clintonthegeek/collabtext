#include "storage/CheckpointCodec.h"

#include <algorithm>
#include <array>

namespace CollabText::Storage {
namespace {

constexpr size_t kMaxFrontierCells = 262144;

void put_u32(std::string &out, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8)
    out.push_back(static_cast<char>(value >> shift));
}
void put_u64(std::string &out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8)
    out.push_back(static_cast<char>(value >> shift));
}
void put_actor(std::string &out, const ActorId &actor) {
  for (auto byte : actor.bytes()) out.push_back(static_cast<char>(byte));
}

struct Reader {
  std::string_view bytes;
  size_t cursor = 0;
  bool need(size_t count) const {
    return cursor <= bytes.size() && count <= bytes.size() - cursor;
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
  ActorId actor() {
    std::array<uint8_t, ActorId::kBytes> raw{};
    for (auto &byte : raw) byte = static_cast<uint8_t>(bytes[cursor++]);
    return ActorId::from_bytes(raw);
  }
};

Error malformed(std::string message) {
  return Error(ErrorCode::malformed, std::move(message), "checkpoint.coverage");
}

Result<void> put_frontier(std::string &out,
                          const std::map<ActorId, uint64_t> &frontier,
                          size_t &cells) {
  if (frontier.size() > UINT32_MAX || frontier.size() > kMaxFrontierCells - cells)
    return Error(ErrorCode::limit_exceeded,
                 "checkpoint coverage frontier exceeds cell limit",
                 "checkpoint.coverage");
  cells += frontier.size();
  put_u32(out, static_cast<uint32_t>(frontier.size()));
  for (const auto &[actor, counter] : frontier) {
    if (!actor.valid()) return malformed("checkpoint coverage contains invalid actor");
    put_actor(out, actor);
    put_u64(out, counter);
  }
  return Result<void>::success();
}

bool read_frontier(Reader &reader, std::map<ActorId, uint64_t> &frontier,
                   size_t &cells) {
  if (!reader.need(4)) return false;
  const auto count = reader.u32();
  if (count > kMaxFrontierCells - cells ||
      count > (reader.bytes.size() - reader.cursor) / 24)
    return false;
  cells += count;
  std::optional<ActorId> previous;
  for (uint32_t i = 0; i < count; ++i) {
    const auto actor = reader.actor();
    const auto counter = reader.u64();
    if (!actor.valid() || (previous && !(*previous < actor))) return false;
    frontier.emplace(actor, counter);
    previous = actor;
  }
  return true;
}

}  // namespace

Result<CheckpointSection> encode_checkpoint_coverage(
    const CheckpointShareableCore::Coverage &coverage) {
  std::string payload;
  size_t cells = 0;
  if (auto valid = put_frontier(payload, coverage.operation, cells); !valid)
    return valid.error();
  if (auto valid = put_frontier(payload, coverage.delivery, cells); !valid)
    return valid.error();
  if (payload.size() > kCheckpointMaxBytes - 12)
    return Error(ErrorCode::limit_exceeded,
                 "checkpoint coverage section exceeds byte limit",
                 "checkpoint.coverage");
  return CheckpointSection{5, 1, std::move(payload)};
}

Result<CheckpointShareableCore::Coverage> decode_checkpoint_coverage(
    const CheckpointSection &section) {
  if (section.id != 5) return malformed("checkpoint coverage must be section 5");
  if (section.version != 1)
    return Error(ErrorCode::unsupported_version,
                 "checkpoint coverage section version is unsupported",
                 "checkpoint.coverage");
  if (section.bytes.size() > kCheckpointMaxBytes - 12)
    return Error(ErrorCode::limit_exceeded,
                 "checkpoint coverage section exceeds byte limit",
                 "checkpoint.coverage");
  Reader reader{section.bytes};
  CheckpointShareableCore::Coverage coverage;
  size_t cells = 0;
  if (!read_frontier(reader, coverage.operation, cells) ||
      !read_frontier(reader, coverage.delivery, cells))
    return malformed("checkpoint coverage frontier is truncated or noncanonical");
  if (reader.cursor != reader.bytes.size())
    return malformed("checkpoint coverage section has trailing bytes");
  return coverage;
}

Result<void> validate_checkpoint_coverage(
    const CheckpointContext &context,
    const CheckpointShareableCore::Coverage &coverage,
    const MembershipDescriptor &descriptor) {
  if (auto valid = validate_membership_descriptor(descriptor); !valid)
    return valid.error().with_context("checkpoint.coverage");
  if (context.epoch != descriptor.epoch ||
      context.parent_descriptor_hash != descriptor.parent_hash)
    return Error(ErrorCode::wrong_epoch,
                 "checkpoint context does not bind to membership descriptor",
                 "checkpoint.coverage");
  const auto found = std::find_if(
      descriptor.base_frontiers.begin(), descriptor.base_frontiers.end(),
      [&](const auto &frontier) { return frontier.stream == context.stream; });
  if (found == descriptor.base_frontiers.end() ||
      found->operation != coverage.operation || found->delivery != coverage.delivery)
    return Error(ErrorCode::corrupt_record,
                 "checkpoint coverage differs from membership descriptor frontier",
                 "checkpoint.coverage");
  return Result<void>::success();
}

}  // namespace CollabText::Storage
