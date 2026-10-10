#include "storage/CheckpointCodec.h"

#include "codec/Utf.h"
#include <collabtext/Serialization.h>

#include "crdt/OperationPayload.h"
#include "crdt/OperationQueue.h"
#include "crdt/OperationValidation.h"

#include <algorithm>
#include <limits>
#include <optional>

namespace CollabText::Storage {
namespace {

constexpr uint32_t kMaxDeferredRecords = 4096;
constexpr uint32_t kMaxOperationBytes = 16u * 1024u * 1024u;
constexpr uint16_t kMaxReasonBytes = 2048;

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
  for (auto byte : actor.bytes()) out.push_back(static_cast<char>(byte));
}

struct Reader {
  std::string_view bytes;
  size_t cursor = 0;
  bool need(size_t count) const {
    return cursor <= bytes.size() && count <= bytes.size() - cursor;
  }
  uint8_t u8() { return static_cast<uint8_t>(bytes[cursor++]); }
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
  ActorId actor() {
    std::array<uint8_t, ActorId::kBytes> raw{};
    for (auto &byte : raw) byte = static_cast<uint8_t>(bytes[cursor++]);
    return ActorId::from_bytes(raw);
  }
};

Error malformed(std::string message) {
  return Error(ErrorCode::malformed, std::move(message), "checkpoint.deferred");
}

bool valid_reason(std::string_view reason) {
  return !reason.empty() && reason.size() <= kMaxReasonBytes &&
         reason.find('\0') == std::string_view::npos &&
         Codec::is_valid_utf8(reason);
}

bool valid_token(const DeliveryToken &token, const CheckpointContext &context) {
  return token.stream == context.stream && token.epoch == context.epoch &&
         token.producer.valid() && token.sequence != 0 &&
         token.sequence != UINT64_MAX;
}

bool valid_context(const CheckpointContext &context) {
  return !context.stream.empty() && context.stream.size() <= 256 &&
         context.stream.find('\0') == std::string::npos &&
         Codec::is_valid_utf8(context.stream) && context.epoch != 0 &&
         (context.engine == CheckpointEngineKind::Text ||
          context.engine == CheckpointEngineKind::IdList);
}

bool ordered_after(const DeliveryToken &left, const DeliveryToken &right) {
  if (left.producer != right.producer) return left.producer < right.producer;
  return left.sequence < right.sequence;
}

Result<void> validate_deferred_operation(
    const CheckpointShareableCore::DeferredRecord &record,
    CheckpointEngineKind kind, const Crdt::Global *applied_version) {
  if (record.operation_bytes.empty() ||
      record.operation_bytes.size() > kMaxOperationBytes)
    return malformed("checkpoint operation bytes are empty or exceed limit");
  Crdt::Identity identity;
  if (kind == CheckpointEngineKind::Text) {
    auto operation = Crdt::decode_operation(record.operation_bytes);
    if (!operation) {
      if (record.quarantined) return Result<void>::success();
      return operation.error().with_context("checkpoint.deferred");
    }
    identity = Crdt::get_op_timestamp(operation.value());
    if (!record.quarantined && applied_version) {
      const auto verdict = Crdt::validate_operation(operation.value(), *applied_version);
      if (verdict.verdict != Crdt::ValidationVerdict::deferred)
        return malformed("checkpoint deferred text operation is not causally deferred");
    }
  } else {
    auto operation = Crdt::decode_idlist_operation(record.operation_bytes);
    if (!operation) {
      if (record.quarantined) return Result<void>::success();
      return operation.error().with_context("checkpoint.deferred");
    }
    identity = Crdt::get_idlist_op_identity(operation.value());
    if (!record.quarantined && applied_version) {
      const auto verdict = Crdt::validate_idlist_operation(operation.value(), *applied_version);
      if (verdict.verdict != Crdt::ValidationVerdict::deferred)
        return malformed("checkpoint deferred list operation is not causally deferred");
    }
  }
  if (!record.quarantined &&
      (identity.actor != record.token.producer || identity.counter == 0 ||
       (applied_version && applied_version->observed(identity))))
    return malformed("deferred delivery identity disagrees with applied state or producer");
  return Result<void>::success();
}

}  // namespace

Result<CheckpointSection> encode_checkpoint_deferred(
    const std::vector<CheckpointShareableCore::DeferredRecord> &records,
    const CheckpointContext &context,
    const Crdt::Global &applied_version) {
  if (records.size() > kMaxDeferredRecords)
    return Error(ErrorCode::limit_exceeded, "checkpoint deferred record count exceeds limit", "checkpoint.deferred");
  if (!valid_context(context))
    return malformed("checkpoint deferred context is invalid");
  std::string payload;
  put_u32(payload, static_cast<uint32_t>(records.size()));
  const DeliveryToken *previous = nullptr;
  for (const auto &record : records) {
    if (!valid_token(record.token, context) ||
        (previous && !ordered_after(*previous, record.token)))
      return malformed("checkpoint delivery tokens are invalid, duplicated, or unordered");
    if (record.quarantined ? !valid_reason(record.reason) : !record.reason.empty())
      return malformed("checkpoint quarantine reason is missing or attached to deferred input");
    if (auto valid = validate_deferred_operation(record, context.engine, &applied_version); !valid)
      return valid.error();
    if (record.operation_bytes.size() > kMaxOperationBytes)
      return Error(ErrorCode::limit_exceeded, "checkpoint operation bytes exceed limit", "checkpoint.deferred");
    put_actor(payload, record.token.producer);
    put_u64(payload, record.token.sequence);
    payload.push_back(record.quarantined ? '\1' : '\0');
    put_u32(payload, static_cast<uint32_t>(record.operation_bytes.size()));
    payload.append(record.operation_bytes);
    put_u16(payload, static_cast<uint16_t>(record.reason.size()));
    payload.append(record.reason);
    if (payload.size() > kCheckpointMaxBytes - 12)
      return Error(ErrorCode::limit_exceeded, "checkpoint deferred section exceeds byte limit", "checkpoint.deferred");
    previous = &record.token;
  }
  return CheckpointSection{4, 1, std::move(payload)};
}

Result<std::vector<CheckpointShareableCore::DeferredRecord>>
decode_checkpoint_deferred(const CheckpointSection &section,
                           const CheckpointContext &context,
                           const Crdt::Global &applied_version) {
  if (section.id != 4) return malformed("checkpoint deferred data must be section 4");
  if (section.version != 1)
    return Error(ErrorCode::unsupported_version, "checkpoint deferred section version is unsupported", "checkpoint.deferred");
  if (section.bytes.size() > kCheckpointMaxBytes - 12)
    return Error(ErrorCode::limit_exceeded, "checkpoint deferred section exceeds byte limit", "checkpoint.deferred");
  if (!valid_context(context))
    return malformed("checkpoint deferred context is invalid");
  Reader reader{section.bytes};
  if (!reader.need(4)) return malformed("checkpoint deferred count is truncated");
  const auto count = reader.u32();
  if (count > kMaxDeferredRecords)
    return Error(ErrorCode::limit_exceeded, "checkpoint deferred record count exceeds limit", "checkpoint.deferred");
  constexpr size_t kMinimumRecordBytes = ActorId::kBytes + 8 + 1 + 4 + 2;
  if (count > (reader.bytes.size() - reader.cursor) / kMinimumRecordBytes)
    return malformed("checkpoint deferred records are truncated");
  std::vector<CheckpointShareableCore::DeferredRecord> records;
  records.reserve(count);
  std::optional<DeliveryToken> previous;
  for (uint32_t i = 0; i < count; ++i) {
    if (!reader.need(ActorId::kBytes + 8 + 1 + 4))
      return malformed("checkpoint deferred record header is truncated");
    CheckpointShareableCore::DeferredRecord record;
    record.token.stream = context.stream;
    record.token.epoch = context.epoch;
    record.token.producer = reader.actor();
    record.token.sequence = reader.u64();
    const auto quarantined = reader.u8();
    if (quarantined > 1) return malformed("checkpoint deferred disposition is invalid");
    record.quarantined = quarantined != 0;
    const auto operation_length = reader.u32();
    if (operation_length == 0 || operation_length > kMaxOperationBytes ||
        !reader.need(static_cast<size_t>(operation_length) + 2))
      return malformed("checkpoint operation bytes are truncated or too large");
    record.operation_bytes.assign(reader.bytes.substr(reader.cursor, operation_length));
    reader.cursor += operation_length;
    const auto reason_length = reader.u16();
    if (!reader.need(reason_length)) return malformed("checkpoint quarantine reason is truncated");
    record.reason.assign(reader.bytes.substr(reader.cursor, reason_length));
    reader.cursor += reason_length;
    if (!valid_token(record.token, context) ||
        (previous && !ordered_after(*previous, record.token)) ||
        (record.quarantined ? !valid_reason(record.reason) : !record.reason.empty()))
      return malformed("checkpoint delivery token order or quarantine reason is invalid");
    if (auto valid = validate_deferred_operation(
            record, context.engine, record.quarantined ? nullptr : &applied_version); !valid)
      return valid.error();
    previous = record.token;
    records.push_back(std::move(record));
  }
  if (reader.cursor != reader.bytes.size())
    return malformed("checkpoint deferred section has trailing bytes");
  return records;
}

}  // namespace CollabText::Storage
