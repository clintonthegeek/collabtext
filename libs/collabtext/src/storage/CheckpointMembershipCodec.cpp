#include "storage/CheckpointCodec.h"

#include "core/Sha256.h"

#include <algorithm>
#include <limits>

namespace CollabText::Storage {
namespace {

constexpr std::string_view kBundleMagic = "CTMB";
constexpr uint16_t kBundleVersion = 1;
constexpr size_t kBundleHeaderBytes = 8;
constexpr size_t kBundleDigestBytes = 64;
constexpr size_t kMaxBundleEntries = 4096;
constexpr size_t kMaxStreamIdBytes = 128;

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

struct Reader {
  std::string_view bytes;
  size_t cursor = 0;

  bool need(size_t count) const {
    return cursor <= bytes.size() && count <= bytes.size() - cursor;
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
  std::string_view take(size_t count) {
    const auto value = bytes.substr(cursor, count);
    cursor += count;
    return value;
  }
};

Error malformed_bundle(std::string message) {
  return Error(ErrorCode::malformed, std::move(message),
               "checkpoint.membership_bundle");
}

bool stream_id_less(std::string_view left, std::string_view right) {
  return std::lexicographical_compare(
      left.begin(), left.end(), right.begin(), right.end(),
      [](unsigned char a, unsigned char b) { return a < b; });
}

Result<CheckpointShareableCore>
decode_and_validate_stream(std::string_view artifact_bytes,
                           const std::string &stream,
                           const MembershipDescriptor &descriptor) {
  auto artifact = decode_checkpoint(artifact_bytes, CheckpointKind::Shareable);
  if (!artifact)
    return artifact.error().with_context("checkpoint.membership");
  auto decoded = decode_checkpoint_core(artifact.value());
  if (!decoded)
    return decoded.error().with_context("checkpoint.membership");
  const auto &core = decoded.value();
  if (core.context.stream != stream)
    return Error(ErrorCode::corrupt_record,
                 "checkpoint artifact stream differs from bundle entry",
                 "checkpoint.membership");
  if (core.context.epoch != descriptor.epoch ||
      core.context.parent_descriptor_hash != descriptor.parent_hash)
    return Error(ErrorCode::wrong_epoch,
                 "checkpoint context does not bind to membership descriptor",
                 "checkpoint.membership");
  if (auto valid =
          validate_checkpoint_coverage(core.context, core.coverage, descriptor);
      !valid)
    return valid.error().with_context("checkpoint.membership");
  if (auto valid =
          validate_checkpoint_translations(core.translations, descriptor);
      !valid)
    return valid.error().with_context("checkpoint.membership");
  return std::move(decoded.value());
}

} // namespace

Result<std::string> encode_membership_checkpoint_bundle(
    std::vector<MembershipCheckpointBundleEntry> entries) {
  if (entries.empty() || entries.size() > kMaxBundleEntries ||
      entries.size() > std::numeric_limits<uint16_t>::max())
    return Error(ErrorCode::limit_exceeded,
                 "membership checkpoint bundle entry count is invalid",
                 "checkpoint.membership_bundle");
  std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
    return stream_id_less(a.stream, b.stream);
  });
  std::string bytes(kBundleMagic);
  put_u16(bytes, kBundleVersion);
  put_u16(bytes, static_cast<uint16_t>(entries.size()));
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto &entry = entries[i];
    if (entry.stream.empty() || entry.stream.size() > kMaxStreamIdBytes ||
        entry.stream.find('\0') != std::string::npos ||
        entry.artifact_bytes.empty() ||
        (i != 0 && entries[i - 1].stream == entry.stream))
      return malformed_bundle(
          "membership checkpoint bundle entries are invalid or duplicated");
    auto artifact =
        decode_checkpoint(entry.artifact_bytes, CheckpointKind::Shareable);
    if (!artifact)
      return artifact.error().with_context("checkpoint.membership_bundle");
    auto core = decode_checkpoint_core(artifact.value());
    if (!core)
      return core.error().with_context("checkpoint.membership_bundle");
    if (core.value().context.stream != entry.stream)
      return Error(ErrorCode::wrong_epoch,
                   "checkpoint artifact stream differs from bundle entry",
                   "checkpoint.membership_bundle");
    if (entry.stream.size() > std::numeric_limits<uint32_t>::max() ||
        entry.artifact_bytes.size() > std::numeric_limits<uint64_t>::max() ||
        bytes.size() > kCheckpointMaxBytes - kBundleDigestBytes - 12 ||
        entry.stream.size() + 12 >
            kCheckpointMaxBytes - kBundleDigestBytes - bytes.size() ||
        entry.artifact_bytes.size() > kCheckpointMaxBytes - kBundleDigestBytes -
                                          bytes.size() - entry.stream.size() -
                                          12)
      return Error(ErrorCode::limit_exceeded,
                   "membership checkpoint bundle exceeds byte limit",
                   "checkpoint.membership_bundle");
    put_u32(bytes, static_cast<uint32_t>(entry.stream.size()));
    put_u64(bytes, static_cast<uint64_t>(entry.artifact_bytes.size()));
    bytes.append(entry.stream);
    bytes.append(entry.artifact_bytes);
  }
  if (bytes.size() > kCheckpointMaxBytes - kBundleDigestBytes)
    return Error(ErrorCode::limit_exceeded,
                 "membership checkpoint bundle exceeds byte limit",
                 "checkpoint.membership_bundle");
  bytes.append(sha256_hex(bytes));
  return bytes;
}

Result<std::vector<MembershipCheckpointBundleEntry>>
decode_membership_checkpoint_bundle(std::string_view bytes) {
  if (bytes.size() > kCheckpointMaxBytes)
    return Error(ErrorCode::limit_exceeded,
                 "membership checkpoint bundle exceeds byte limit",
                 "checkpoint.membership_bundle");
  if (bytes.size() < kBundleHeaderBytes + kBundleDigestBytes)
    return malformed_bundle("membership checkpoint bundle is truncated");
  const auto payload = bytes.substr(0, bytes.size() - kBundleDigestBytes);
  const auto digest = bytes.substr(bytes.size() - kBundleDigestBytes);
  if (sha256_hex(payload) != digest)
    return Error(ErrorCode::corrupt_record,
                 "membership checkpoint bundle digest mismatch",
                 "checkpoint.membership_bundle");
  Reader reader{payload};
  if (reader.take(kBundleMagic.size()) != kBundleMagic)
    return malformed_bundle("membership checkpoint bundle magic is invalid");
  if (!reader.need(4))
    return malformed_bundle("membership checkpoint bundle header is truncated");
  if (reader.u16() != kBundleVersion)
    return Error(ErrorCode::unsupported_version,
                 "membership checkpoint bundle version is unsupported",
                 "checkpoint.membership_bundle");
  const auto count = reader.u16();
  if (count == 0 || count > kMaxBundleEntries)
    return malformed_bundle(
        "membership checkpoint bundle entry count is invalid");
  std::vector<MembershipCheckpointBundleEntry> entries;
  entries.reserve(count);
  std::string previous;
  for (uint16_t i = 0; i < count; ++i) {
    if (!reader.need(12))
      return malformed_bundle(
          "membership checkpoint bundle entry is truncated");
    const auto stream_size = reader.u32();
    const auto artifact_size = reader.u64();
    if (stream_size == 0 || stream_size > kMaxStreamIdBytes ||
        artifact_size == 0 ||
        artifact_size > std::numeric_limits<size_t>::max() ||
        !reader.need(stream_size) ||
        artifact_size > reader.bytes.size() - reader.cursor - stream_size)
      return malformed_bundle(
          "membership checkpoint bundle entry length is invalid");
    auto stream = reader.take(stream_size);
    auto artifact = reader.take(static_cast<size_t>(artifact_size));
    const std::string stream_name(stream);
    if (stream.find('\0') != std::string_view::npos ||
        (i != 0 && !stream_id_less(previous, stream_name)))
      return malformed_bundle(
          "membership checkpoint bundle streams are not canonical");
    previous = stream_name;
    entries.push_back({stream_name, std::string(artifact)});
  }
  if (reader.cursor != payload.size())
    return malformed_bundle("membership checkpoint bundle has trailing bytes");
  return entries;
}

Result<void> validate_checkpoint_artifact_membership(
    std::string_view checkpoint_artifact_bytes,
    const MembershipDescriptor &descriptor) {
  if (auto valid = validate_membership_descriptor(descriptor); !valid)
    return valid.error().with_context("checkpoint.membership");
  if (sha256_hex(checkpoint_artifact_bytes) != descriptor.base_checkpoint_hash)
    return Error(ErrorCode::corrupt_record,
                 "checkpoint digest differs from membership base checkpoint",
                 "checkpoint.membership");
  if (descriptor.base_frontiers.size() != 1)
    return Error(
        ErrorCode::corrupt_record,
        "single checkpoint artifact cannot represent every membership stream",
        "checkpoint.membership");
  auto core = decode_and_validate_stream(
      checkpoint_artifact_bytes, descriptor.base_frontiers.front().stream,
      descriptor);
  if (!core)
    return core.error();
  return Result<void>::success();
}

Result<void>
validate_membership_checkpoint_bundle(std::string_view bundle_bytes,
                                      const MembershipDescriptor &descriptor) {
  if (auto valid = validate_membership_descriptor(descriptor); !valid)
    return valid.error().with_context("checkpoint.membership_bundle");
  if (sha256_hex(bundle_bytes) != descriptor.base_checkpoint_hash)
    return Error(
        ErrorCode::corrupt_record,
        "checkpoint bundle digest differs from membership base checkpoint",
        "checkpoint.membership_bundle");
  auto entries = decode_membership_checkpoint_bundle(bundle_bytes);
  if (!entries)
    return entries.error();
  if (entries.value().size() != descriptor.base_frontiers.size())
    return Error(ErrorCode::corrupt_record,
                 "checkpoint bundle does not cover every membership stream",
                 "checkpoint.membership_bundle");
  for (size_t i = 0; i < entries.value().size(); ++i) {
    const auto &entry = entries.value()[i];
    if (entry.stream != descriptor.base_frontiers[i].stream)
      return Error(
          ErrorCode::corrupt_record,
          "checkpoint bundle stream set differs from membership descriptor",
          "checkpoint.membership_bundle");
    auto core = decode_and_validate_stream(entry.artifact_bytes, entry.stream,
                                           descriptor);
    if (!core)
      return core.error().with_context("checkpoint.membership_bundle");
  }
  return Result<void>::success();
}

MembershipCheckpointVerifier make_membership_checkpoint_verifier(
    MembershipCheckpointBundleResolver resolve_bundle) {
  if (!resolve_bundle)
    return {};
  return [resolve_bundle = std::move(resolve_bundle)](
             const MembershipDescriptor &parent,
             const MembershipDescriptor &next) -> Result<void> {
    if (auto valid = validate_membership_descriptor(parent); !valid)
      return valid.error().with_context("membership checkpoint parent");
    if (parent.epoch == std::numeric_limits<uint64_t>::max() ||
        next.parent_hash != parent.hash || next.epoch != parent.epoch + 1)
      return Error(ErrorCode::wrong_epoch,
                   "checkpoint descriptor does not extend the supplied parent",
                   "checkpoint.membership_bundle");
    auto bytes = resolve_bundle(next.base_checkpoint_hash);
    if (!bytes)
      return bytes.error().with_context(
          "resolving membership checkpoint bundle");
    return validate_membership_checkpoint_bundle(bytes.value(), next);
  };
}

} // namespace CollabText::Storage
