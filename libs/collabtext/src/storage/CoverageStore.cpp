#include "storage/CoverageStore.h"

#include "core/Sha256.h"

#include <algorithm>
#include <charconv>
#include <exception>
#include <fstream>
#include <locale>
#include <set>
#include <sstream>

namespace CollabText::Storage {
namespace {
constexpr size_t kMaxCoverageBytes = 8u * 1024u * 1024u;
constexpr size_t kMaxStreams = 4096;
constexpr size_t kMaxCells = 262144;

bool number(std::string_view text, uint64_t &value) {
  auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size() &&
         std::to_string(value) == text;
}

std::string hex(std::string_view bytes) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char byte : bytes) {
    out.push_back(digits[byte >> 4]);
    out.push_back(digits[byte & 15]);
  }
  return out;
}

bool unhex(std::string_view text, std::string &out) {
  if (text.size() % 2) return false;
  auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  out.clear();
  out.reserve(text.size() / 2);
  for (size_t i = 0; i < text.size(); i += 2) {
    const int high = digit(text[i]);
    const int low = digit(text[i + 1]);
    if (high < 0 || low < 0) return false;
    out.push_back(static_cast<char>((high << 4) | low));
  }
  return true;
}

bool is_sha256(std::string_view value) {
  if (value.size() != 64) return false;
  for (char c : value)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

Error malformed(std::string message) {
  return Error(ErrorCode::corrupt_record, std::move(message), "coverage.store");
}

template <typename Coverage>
Result<void> encode_frontiers(std::ostringstream &body, char tag,
                              const Coverage &coverage, size_t &cells) {
  for (const auto &[stream, frontier] : coverage) {
    if (stream.empty() || stream.size() > 4096 || frontier.empty() ||
        frontier.size() > kMaxCells - cells)
      return Error(ErrorCode::limit_exceeded, "coverage stream or cell limit exceeded");
    cells += frontier.size();
    body << "S " << tag << ' ' << hex(stream) << ' ' << frontier.size() << '\n';
    for (const auto &[producer, through] : frontier) {
      if (!producer.valid() || through == 0 || through == UINT64_MAX)
        return Error(ErrorCode::malformed,
                     "coverage contains an invalid producer or zero frontier");
      body << "A " << producer.to_string() << ' ' << through << '\n';
    }
  }
  return {};
}

Result<std::string> encode(const PeerCoverageEvidence &evidence) {
  if (!evidence.member.valid() || evidence.epoch == 0 ||
      !is_sha256(evidence.descriptor_hash) || !is_sha256(evidence.checkpoint_hash) ||
      evidence.operation.size() > kMaxStreams || evidence.delivery.size() > kMaxStreams)
    return Error(ErrorCode::malformed, "coverage evidence header is invalid");
  size_t cells = 0;
  std::ostringstream body;
  body.imbue(std::locale::classic());
  body << "CCOV2 " << evidence.member.to_string() << ' ' << evidence.epoch << ' '
       << evidence.descriptor_hash << ' ' << evidence.checkpoint_hash << ' '
       << evidence.operation.size() << ' ' << evidence.delivery.size() << '\n';
  if (auto encoded = encode_frontiers(body, 'O', evidence.operation, cells); !encoded)
    return encoded.error();
  if (auto encoded = encode_frontiers(body, 'D', evidence.delivery, cells); !encoded)
    return encoded.error();
  if (body.tellp() > static_cast<std::streamoff>(kMaxCoverageBytes))
    return Error(ErrorCode::limit_exceeded, "coverage evidence exceeds byte limit");
  std::string bytes = body.str();
  if (bytes.size() > kMaxCoverageBytes - 68)
    return Error(ErrorCode::limit_exceeded, "coverage evidence exceeds byte limit");
  bytes += "H " + sha256_hex(bytes) + "\n";
  return bytes;
}

Result<PeerCoverageEvidence> decode(std::string_view bytes) {
  if (bytes.empty() || bytes.size() > kMaxCoverageBytes || bytes.back() != '\n')
    return malformed("coverage evidence is empty, oversized, or truncated");
  const auto hash_start = bytes.rfind("H ");
  if (hash_start == std::string_view::npos || hash_start == 0 ||
      bytes.substr(hash_start).size() != 67 ||
      bytes.substr(hash_start + 2, 64) != sha256_hex(bytes.substr(0, hash_start)))
    return malformed("coverage evidence checksum is invalid");
  std::istringstream input(std::string(bytes.substr(0, hash_start)));
  std::string magic, owner_text, epoch_text, descriptor_hash, checkpoint_hash;
  std::string operation_count_text, delivery_count_text;
  if (!(input >> magic >> owner_text >> epoch_text >> descriptor_hash >>
        checkpoint_hash >> operation_count_text >> delivery_count_text) || magic != "CCOV2")
    return malformed("coverage evidence header is invalid");
  uint64_t operation_count_value = 0, delivery_count_value = 0;
  if (!number(operation_count_text, operation_count_value) ||
      !number(delivery_count_text, delivery_count_value) ||
      operation_count_value > kMaxStreams || delivery_count_value > kMaxStreams)
    return malformed("coverage stream count is invalid");
  const auto operation_count = static_cast<size_t>(operation_count_value);
  const auto delivery_count = static_cast<size_t>(delivery_count_value);
  std::string remainder;
  std::getline(input, remainder);
  if (!remainder.empty()) return malformed("coverage header has trailing data");
  auto owner = ActorId::parse(owner_text);
  uint64_t epoch = 0;
  if (!owner.ok() || !owner.value().valid() || owner.value().to_string() != owner_text ||
      !number(epoch_text, epoch) ||
      epoch == 0 || !is_sha256(descriptor_hash) || !is_sha256(checkpoint_hash))
    return malformed("coverage evidence identity or hashes are invalid");
  PeerCoverageEvidence result{owner.value(), epoch, descriptor_hash,
                              checkpoint_hash, {}, {}};
  size_t cells = 0;
  auto read_frontiers = [&](char expected_tag, size_t count,
                            auto &destination) -> Result<void> {
    std::string previous_stream;
    for (size_t s = 0; s < count; ++s) {
      std::string kind, tag_text, stream_hex, count_text;
      if (!(input >> kind >> tag_text >> stream_hex >> count_text) ||
          kind != "S" || tag_text.size() != 1 || tag_text[0] != expected_tag)
        return malformed("coverage stream record is truncated or mistagged");
      std::string stream;
      uint64_t cell_count = 0;
      if (!unhex(stream_hex, stream) || stream.empty() || stream.size() > 4096 ||
          (!previous_stream.empty() && !(previous_stream < stream)) ||
          !number(count_text, cell_count) || cell_count == 0 ||
          cell_count > kMaxCells - cells)
        return malformed("coverage stream is noncanonical or exceeds limits");
      cells += static_cast<size_t>(cell_count);
      previous_stream = stream;
      auto &frontier = destination[stream];
      ActorId previous_actor;
      bool have_previous = false;
      for (uint64_t i = 0; i < cell_count; ++i) {
        std::string actor_kind, actor_text, through_text;
        if (!(input >> actor_kind >> actor_text >> through_text) || actor_kind != "A")
          return malformed("coverage actor record is truncated");
        auto producer = ActorId::parse(actor_text);
        uint64_t through = 0;
        if (!producer.ok() || !producer.value().valid() ||
            producer.value().to_string() != actor_text ||
            !number(through_text, through) || through == 0 || through == UINT64_MAX ||
            (have_previous && !(previous_actor < producer.value())))
          return malformed("coverage actor frontier is invalid or noncanonical");
        frontier.emplace(producer.value(), through);
        previous_actor = producer.value();
        have_previous = true;
      }
    }
    return {};
  };
  if (auto read = read_frontiers('O', operation_count, result.operation); !read)
    return read.error();
  if (auto read = read_frontiers('D', delivery_count, result.delivery); !read)
    return read.error();
  std::string extra;
  if (input >> extra) return malformed("coverage evidence has trailing records");
  auto canonical = encode(result);
  if (!canonical.ok() || canonical.value() != bytes)
    return malformed("coverage evidence encoding is noncanonical");
  return result;
}

Result<void> validate_coverage(
    const MembershipDescriptor &descriptor, const PeerCoverageEvidence &evidence) {
  if (auto valid = validate_membership_descriptor(descriptor); !valid)
    return valid.error();
  if (evidence.epoch != descriptor.epoch ||
      evidence.descriptor_hash != descriptor.hash ||
      !std::binary_search(descriptor.members.begin(), descriptor.members.end(),
                          evidence.member))
    return Error(ErrorCode::wrong_epoch,
                 "coverage evidence does not belong to this membership descriptor");
  const auto validate = [&](const auto &coverage, bool operation) -> Result<void> {
    for (const auto &[stream, frontier] : coverage) {
      const auto declared = std::lower_bound(
          descriptor.base_frontiers.begin(), descriptor.base_frontiers.end(), stream,
          [](const auto &entry, const auto &name) { return entry.stream < name; });
      if (declared == descriptor.base_frontiers.end() || declared->stream != stream)
        return Error(ErrorCode::corrupt_record,
                     "coverage names a stream absent from membership descriptor", stream);
      const auto &producers = operation ? declared->operation : declared->delivery;
      for (const auto &[producer, through] : frontier) {
        if (!producer.valid() || through == 0 || through == UINT64_MAX ||
            !producers.contains(producer))
          return Error(ErrorCode::corrupt_record,
                       "coverage producer is absent from descriptor frontier", stream);
      }
    }
    for (const auto &base : descriptor.base_frontiers) {
      const auto &required = operation ? base.operation : base.delivery;
      const auto observed_stream = coverage.find(base.stream);
      for (const auto &[producer, through] : required) {
        if (through == 0) continue;
        if (observed_stream == coverage.end())
          return Error(ErrorCode::corrupt_record,
                       "coverage regresses below membership base frontier", base.stream);
        const auto observed = observed_stream->second.find(producer);
        if (observed == observed_stream->second.end() || observed->second < through)
          return Error(ErrorCode::corrupt_record,
                       "coverage regresses below membership base frontier", base.stream);
      }
    }
    return {};
  };
  if (auto valid = validate(evidence.operation, true); !valid) return valid.error();
  if (auto valid = validate(evidence.delivery, false); !valid) return valid.error();
  return {};
}

Result<void> verify_checkpoint(const CheckpointCoverageVerifier &verify,
                               const MembershipDescriptor &descriptor,
                               std::string_view checkpoint_hash,
                               const OperationCoverage &operation,
                               const DeliveryCoverage &delivery) {
  if (!verify)
    return Error(ErrorCode::awaiting_state,
                 "coverage requires a durable checkpoint verifier");
  try {
    return verify(descriptor, checkpoint_hash, operation, delivery);
  } catch (const std::exception &error) {
    return Error(ErrorCode::io_error, error.what(), "coverage checkpoint verifier threw");
  } catch (...) {
    return Error(ErrorCode::io_error,
                 "coverage checkpoint verifier threw an unknown exception");
  }
}
} // namespace

Result<CoverageStore> CoverageStore::open(std::filesystem::path directory,
                                           ActorId owner,
                                           const StepHook *hook) {
  if (!owner.valid())
    return Error(ErrorCode::invalid_identity, "coverage owner is invalid");
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) return Error(ErrorCode::io_error, ec.message(), directory.string());
  const auto lease_path = directory / ("." + owner.to_string() + ".coverage.lock");
  auto lease = ProcessLease::acquire(lease_path);
  if (!lease.ok()) return lease.error().with_context("opening coverage store");
  return CoverageStore(std::move(directory), owner, hook,
                       std::move(lease.value()));
}

std::filesystem::path CoverageStore::record_path(
    std::string_view descriptor_hash, std::string_view checkpoint_hash) const {
  return m_directory / std::string(descriptor_hash) /
         (std::string(checkpoint_hash) + "." + m_owner.to_string() + ".coverage");
}

Result<PeerCoverageEvidence> CoverageStore::publish(
    const MembershipDescriptor &descriptor, std::string_view checkpoint_hash,
    const OperationCoverage &operation, const DeliveryCoverage &delivery,
    const CheckpointCoverageVerifier &verify_checkpoint_callback) {
  if (!is_sha256(checkpoint_hash))
    return Error(ErrorCode::malformed, "coverage checkpoint hash is invalid");
  PeerCoverageEvidence evidence{m_owner, descriptor.epoch, descriptor.hash,
                               std::string(checkpoint_hash), operation, delivery};
  if (auto valid = validate_coverage(descriptor, evidence); !valid)
    return valid.error();
  auto verified = verify_checkpoint(verify_checkpoint_callback, descriptor,
                                    checkpoint_hash, operation, delivery);
  if (!verified.ok()) return verified.error();
  auto bytes = encode(evidence);
  if (!bytes.ok()) return bytes.error();
  const auto path = record_path(descriptor.hash, checkpoint_hash);
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) return Error(ErrorCode::io_error, ec.message(), path.parent_path().string());
  if (std::filesystem::exists(path, ec)) {
    if (ec) return Error(ErrorCode::io_error, ec.message(), path.string());
    std::ifstream existing(path, std::ios::binary);
    if (!existing)
      return Error(ErrorCode::io_error, "cannot read existing coverage record", path.string());
    existing.seekg(0, std::ios::end);
    const auto length = existing.tellg();
    if (length < 0 || static_cast<uint64_t>(length) > kMaxCoverageBytes)
      return Error(ErrorCode::corrupt_record,
                   "existing coverage record is invalid or oversized", path.string());
    existing.seekg(0);
    std::string prior(static_cast<size_t>(length), '\0');
    if (!prior.empty() && !existing.read(prior.data(),
                                         static_cast<std::streamsize>(prior.size())))
      return Error(ErrorCode::io_error, "cannot read existing coverage record", path.string());
    if (prior != bytes.value())
      return Error(ErrorCode::corrupt_record,
                   "immutable coverage path already contains different evidence",
                   path.string());
  } else if (ec) {
    return Error(ErrorCode::io_error, ec.message(), path.string());
  }
  auto written = replace_atomically(path, bytes.value(), m_hook);
  if (!written.ok()) return written.error();
  return evidence;
}

Result<std::optional<PeerCoverageEvidence>> CoverageStore::recover(
    const MembershipDescriptor &descriptor, std::string_view checkpoint_hash,
    const CheckpointCoverageVerifier &verify_checkpoint_callback) const {
  if (!is_sha256(checkpoint_hash))
    return Error(ErrorCode::malformed, "coverage checkpoint hash is invalid");
  if (auto valid = validate_membership_descriptor(descriptor); !valid)
    return valid.error();
  const auto path = record_path(descriptor.hash, checkpoint_hash);
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) return Error(ErrorCode::io_error, ec.message(), path.string());
    if (!exists)
      return std::optional<PeerCoverageEvidence>{};
    return Error(ErrorCode::io_error, "cannot read coverage evidence", path.string());
  }
  input.seekg(0, std::ios::end);
  const auto length = input.tellg();
  if (length < 0 || static_cast<uint64_t>(length) > kMaxCoverageBytes)
    return Error(ErrorCode::limit_exceeded, "coverage evidence exceeds byte limit");
  input.seekg(0);
  std::string bytes(static_cast<size_t>(length), '\0');
  if (!bytes.empty() && !input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
    return Error(ErrorCode::io_error, "cannot read coverage evidence contents", path.string());
  auto evidence = decode(bytes);
  if (!evidence.ok()) return evidence.error().with_context(path.string());
  if (evidence.value().member != m_owner ||
      evidence.value().descriptor_hash != descriptor.hash ||
      evidence.value().checkpoint_hash != checkpoint_hash)
    return Error(ErrorCode::corrupt_record,
                 "coverage record does not match its content-addressed path");
  if (auto valid = validate_coverage(descriptor, evidence.value()); !valid)
    return valid.error();
  auto verified = verify_checkpoint(verify_checkpoint_callback, descriptor,
                                    checkpoint_hash, evidence.value().operation,
                                    evidence.value().delivery);
  if (!verified.ok()) return verified.error();
  return std::optional<PeerCoverageEvidence>(std::move(evidence.value()));
}

} // namespace CollabText::Storage
