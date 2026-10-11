#include "storage/CheckpointStore.h"

#include "core/Sha256.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>

namespace CollabText::Storage {
namespace {
constexpr std::string_view kManifestMagic = "CTCPTR01";
constexpr size_t kManifestBytes = 8 + 8 + 1 + 32 + 32 + 32;
constexpr size_t kManifestBodyBytes = kManifestBytes - 32;

struct Manifest {
  uint64_t generation = 0;
  std::string shareable_hash;
  std::optional<std::string> local_hash;
};

void put_u64(std::string &out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8)
    out.push_back(static_cast<char>((value >> shift) & 0xff));
}

uint64_t get_u64(std::string_view bytes, size_t offset) {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i)
    value = (value << 8) | static_cast<uint8_t>(bytes[offset + i]);
  return value;
}

std::string raw_digest(std::string_view hex) {
  std::string raw;
  raw.reserve(32);
  for (size_t i = 0; i < hex.size(); i += 2) {
    auto digit = [](char c) -> uint8_t {
      return c >= '0' && c <= '9' ? static_cast<uint8_t>(c - '0')
           : c >= 'a' && c <= 'f' ? static_cast<uint8_t>(c - 'a' + 10)
                                  : 0;
    };
    raw.push_back(static_cast<char>((digit(hex[i]) << 4) | digit(hex[i + 1])));
  }
  return raw;
}

std::optional<std::string> hex_digest(std::string_view raw) {
  if (raw.size() != 32) return {};
  constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (unsigned char byte : raw) {
    out.push_back(hex[byte >> 4]);
    out.push_back(hex[byte & 0x0f]);
  }
  return out;
}

std::string encode_manifest(const Manifest &manifest) {
  std::string bytes(kManifestMagic);
  put_u64(bytes, manifest.generation);
  bytes.push_back(manifest.local_hash ? '\1' : '\0');
  bytes += raw_digest(manifest.shareable_hash);
  bytes += manifest.local_hash ? raw_digest(*manifest.local_hash) : std::string(32, '\0');
  bytes += raw_digest(sha256_hex(bytes));
  return bytes;
}

std::optional<Manifest> decode_manifest(std::string_view bytes) {
  if (bytes.size() != kManifestBytes || bytes.substr(0, 8) != kManifestMagic)
    return {};
  const auto checksum = hex_digest(bytes.substr(kManifestBodyBytes, 32));
  if (!checksum || sha256_hex(bytes.substr(0, kManifestBodyBytes)) != *checksum)
    return {};
  const uint8_t has_local = static_cast<uint8_t>(bytes[16]);
  if (has_local > 1) return {};
  auto shareable = hex_digest(bytes.substr(17, 32));
  auto local = hex_digest(bytes.substr(49, 32));
  if (!shareable || !local) return {};
  Manifest result;
  result.generation = get_u64(bytes, 8);
  if (result.generation == 0) return {};
  result.shareable_hash = std::move(*shareable);
  if (has_local) result.local_hash = std::move(*local);
  else if (bytes.substr(49, 32) != std::string(32, '\0')) return {};
  return result;
}

Result<std::optional<std::string>> read_bounded(const std::filesystem::path &path,
                                                size_t limit) {
  std::error_code ec;
  const auto status = std::filesystem::symlink_status(path, ec);
  if (ec == std::errc::no_such_file_or_directory ||
      status.type() == std::filesystem::file_type::not_found)
    return std::optional<std::string>{};
  if (ec) return Error(ErrorCode::io_error, ec.message(), path.string());
  if (!std::filesystem::is_regular_file(status))
    return Error(ErrorCode::corrupt_record, "checkpoint path is not a regular file",
                 path.string());
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) return Error(ErrorCode::io_error, ec.message(), path.string());
  if (size > limit)
    return Error(ErrorCode::limit_exceeded, "checkpoint file exceeds its byte limit",
                 path.string());
  std::ifstream in(path, std::ios::binary);
  if (!in) return Error(ErrorCode::io_error, "cannot open checkpoint file", path.string());
  std::string bytes(static_cast<size_t>(size), '\0');
  if (size && !in.read(bytes.data(), static_cast<std::streamsize>(size)))
    return Error(ErrorCode::io_error, "cannot read complete checkpoint file", path.string());
  char extra;
  if (in.get(extra))
    return Error(ErrorCode::corrupt_record, "checkpoint file changed while being read",
                 path.string());
  return std::optional<std::string>(std::move(bytes));
}

std::filesystem::path manifest_path(const std::filesystem::path &directory,
                                    uint64_t generation) {
  return directory / ((generation & 1) ? "manifest-1" : "manifest-0");
}

std::filesystem::path artifact_path(const std::filesystem::path &directory,
                                    std::string_view kind,
                                    std::string_view hash) {
  return directory / (std::string(kind) + "-" + std::string(hash));
}

Result<void> store_immutable(const std::filesystem::path &path,
                             std::string_view expected,
                             const StepHook *hook) {
  auto existing = read_bounded(path, kCheckpointMaxBytes);
  if (!existing.ok()) return existing.error();
  if (existing.value() && *existing.value() == expected) return {};
  return replace_atomically(path, expected, hook);
}

Result<void> validate_local_bundle(const CheckpointShareableCore &shareable,
                                   const CheckpointLocalCore &local,
                                   const ActorId &owner) {
  const uint64_t observed = shareable.causal.version.get(owner);
  if (local.writer.next_identity_counter <= observed)
    return Error(ErrorCode::counter_regression,
                 "local writer counter does not cover its shareable causal frontier",
                 "checkpoint.local.writer");

  const auto delivery = shareable.coverage.delivery.find(owner);
  const uint64_t delivery_frontier = delivery == shareable.coverage.delivery.end()
      ? 0 : delivery->second;
  if (local.writer.durable_delivery_frontier != delivery_frontier)
    return Error(ErrorCode::corrupt_record,
                 "local writer delivery frontier differs from shareable coverage",
                 "checkpoint.local.writer");

  const auto history_is_covered = [&](const Crdt::LocalUndoEntrySnapshot &entry) {
    const auto covered = [&](const Crdt::Identity &identity) {
      return identity.actor == owner &&
             shareable.causal.version.observed(identity);
    };
    if (!covered(entry.group_id)) return false;
    for (const auto &identity : entry.operation_ids)
      if (!covered(identity)) return false;
    for (const auto &identity : entry.inserted_items)
      if (!covered(identity)) return false;
    for (const auto &identity : entry.deletion_operations)
      if (!covered(identity)) return false;
    return true;
  };
  for (const auto &entry : local.history.entries)
    if (!history_is_covered(entry))
      return Error(ErrorCode::corrupt_record,
                   "local undo history exceeds the shareable causal frontier",
                   "checkpoint.local.history");
  for (const auto &entry : local.history.retired_entries)
    if (!history_is_covered(entry))
      return Error(ErrorCode::corrupt_record,
                   "retired undo history exceeds the shareable causal frontier",
                   "checkpoint.local.history");
  return {};
}

} // namespace

Result<CheckpointStore> CheckpointStore::open(std::filesystem::path directory,
                                               const StepHook *hook) {
  try {
    std::filesystem::create_directories(directory);
    auto lease = ProcessLease::acquire(directory / "store.lock");
    if (!lease.ok()) return lease.error();
    return CheckpointStore(std::move(directory), hook, std::move(lease).value());
  } catch (const std::filesystem::filesystem_error &error) {
    return Error(ErrorCode::io_error, error.what(), "opening checkpoint store");
  }
}

Result<StoredCheckpoint> CheckpointStore::publish(
    std::string_view shareable_bytes,
    const std::optional<std::string> &local_bytes,
    const ActorId &local_owner) {
  auto artifact = decode_checkpoint(shareable_bytes, CheckpointKind::Shareable);
  if (!artifact.ok()) return artifact.error().with_context("publishing shareable checkpoint");
  auto core = decode_checkpoint_core(artifact.value());
  if (!core.ok()) return core.error().with_context("publishing shareable checkpoint");
  const auto shareable_hash = sha256_hex(shareable_bytes);
  if (local_bytes) {
    if (!local_owner.valid())
      return Error(ErrorCode::invalid_identity,
                   "a local checkpoint companion requires its owner actor");
    auto local_artifact = decode_checkpoint(*local_bytes, CheckpointKind::Local);
    if (!local_artifact.ok()) return local_artifact.error();
    auto local_core = decode_checkpoint_local_core(
        local_artifact.value(), local_owner, shareable_hash, core.value().context);
    if (!local_core.ok()) return local_core.error();
    auto local_binding = validate_local_bundle(core.value(), local_core.value(),
                                               local_owner);
    if (!local_binding.ok()) return local_binding.error();
  }

  auto share_stored = store_immutable(
      artifact_path(m_directory, "shareable", shareable_hash), shareable_bytes, m_hook);
  if (!share_stored.ok()) return share_stored.error();
  std::optional<std::string> local_hash;
  if (local_bytes) {
    local_hash = sha256_hex(*local_bytes);
    auto local_stored = store_immutable(
        artifact_path(m_directory, "local", *local_hash), *local_bytes, m_hook);
    if (!local_stored.ok()) return local_stored.error();
  }

  uint64_t latest = 0;
  for (uint64_t slot = 0; slot < 2; ++slot) {
    auto bytes = read_bounded(m_directory / (slot ? "manifest-1" : "manifest-0"),
                              kManifestBytes);
    if (!bytes.ok()) return bytes.error();
    if (bytes.value())
      if (auto decoded = decode_manifest(*bytes.value()))
        latest = std::max(latest, decoded->generation);
  }
  if (latest == std::numeric_limits<uint64_t>::max())
    return Error(ErrorCode::counter_exhausted, "checkpoint manifest generation exhausted");
  Manifest manifest{latest + 1, shareable_hash, local_hash};
  const auto id = shareable_hash;
  const auto bytes = encode_manifest(manifest);
  auto published = replace_atomically(manifest_path(m_directory, manifest.generation),
                                      bytes, m_hook);
  if (!published.ok()) return published.error().with_context("publishing checkpoint manifest");
  return StoredCheckpoint{manifest.generation, id, std::string(shareable_bytes), local_bytes};
}

Result<std::optional<StoredCheckpoint>> CheckpointStore::recover(
    const ActorId &local_owner,
    const CheckpointCandidateValidator &validate_candidate,
    const std::optional<CheckpointContext> &expected_context) const {
  if (!validate_candidate)
    return Error(ErrorCode::malformed,
                 "checkpoint recovery requires a membership/provider validator");
  struct Candidate { Manifest manifest; };
  std::vector<Candidate> candidates;
  std::optional<Error> manifest_error;
  for (uint64_t slot = 0; slot < 2; ++slot) {
    auto bytes = read_bounded(m_directory / (slot ? "manifest-1" : "manifest-0"),
                              kManifestBytes);
    if (!bytes.ok()) {
      manifest_error = bytes.error();
      continue;
    }
    if (bytes.value())
      if (auto decoded = decode_manifest(*bytes.value())) candidates.push_back({*decoded});
  }
  if (candidates.empty()) {
    std::error_code ec;
    const bool one_exists = std::filesystem::exists(m_directory / "manifest-0", ec) ||
                            std::filesystem::exists(m_directory / "manifest-1", ec);
    if (ec) return Error(ErrorCode::io_error, ec.message(), m_directory.string());
    if (one_exists && manifest_error) return *manifest_error;
    if (one_exists)
      return Error(ErrorCode::corrupt_record, "checkpoint manifests are missing or corrupt",
                   m_directory.string());
    return std::optional<StoredCheckpoint>{};
  }
  std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) {
    return a.manifest.generation > b.manifest.generation;
  });
  std::optional<Error> last_error;
  for (const auto &candidate : candidates) {
    const auto &manifest = candidate.manifest;
    auto share = read_bounded(artifact_path(m_directory, "shareable",
                                            manifest.shareable_hash),
                              kCheckpointMaxBytes);
    if (!share.ok()) { last_error = share.error(); continue; }
    if (!share.value() || sha256_hex(*share.value()) != manifest.shareable_hash) {
      last_error = Error(ErrorCode::corrupt_record,
                         "checkpoint shareable artifact is missing or has the wrong digest");
      continue;
    }
    auto frame = decode_checkpoint(*share.value(), CheckpointKind::Shareable);
    if (!frame.ok()) { last_error = frame.error(); continue; }
    auto core = decode_checkpoint_core(frame.value());
    if (!core.ok()) { last_error = core.error(); continue; }
    if (expected_context && core.value().context != *expected_context) {
      last_error = Error(ErrorCode::actor_conflict,
                         "checkpoint context does not match the requested document epoch");
      continue;
    }
    std::optional<std::string> local_bytes;
    if (manifest.local_hash) {
      auto local = read_bounded(artifact_path(m_directory, "local", *manifest.local_hash),
                                kCheckpointMaxBytes);
      if (!local.ok()) { last_error = local.error(); continue; }
      if (!local.value() || sha256_hex(*local.value()) != *manifest.local_hash) {
        last_error = Error(ErrorCode::corrupt_record,
                           "checkpoint local artifact is missing or has the wrong digest");
        continue;
      }
      auto local_frame = decode_checkpoint(*local.value(), CheckpointKind::Local);
      if (!local_frame.ok()) { last_error = local_frame.error(); continue; }
      auto local_core = decode_checkpoint_local_core(
          local_frame.value(), local_owner, manifest.shareable_hash,
          core.value().context);
      if (!local_core.ok()) { last_error = local_core.error(); continue; }
      auto local_binding = validate_local_bundle(core.value(), local_core.value(),
                                                 local_owner);
      if (!local_binding.ok()) { last_error = local_binding.error(); continue; }
      local_bytes = std::move(*local.value());
    }
    try {
      auto accepted = validate_candidate(*share.value(), local_bytes);
      if (!accepted.ok()) { last_error = accepted.error(); continue; }
    } catch (const std::exception &error) {
      last_error = Error(ErrorCode::io_error, error.what(),
                         "validating checkpoint candidate");
      continue;
    } catch (...) {
      last_error = Error(ErrorCode::io_error,
                         "checkpoint candidate validator threw an unknown exception");
      continue;
    }
    return std::optional<StoredCheckpoint>(StoredCheckpoint{
        manifest.generation, manifest.shareable_hash, std::move(*share.value()),
        std::move(local_bytes)});
  }
  return last_error.value_or(Error(ErrorCode::corrupt_record,
                                   "no valid checkpoint manifest candidate"));
}

} // namespace CollabText::Storage
