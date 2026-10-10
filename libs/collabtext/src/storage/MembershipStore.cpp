#include "storage/MembershipStore.h"

#include "core/Sha256.h"

#include <charconv>
#include <fstream>
#include <sstream>

namespace CollabText::Storage {
namespace {
constexpr size_t kMaxLogBytes = 64 * 1024 * 1024;
constexpr size_t kMaxEvidenceBytes = 1024 * 1024;

std::string hex(std::string_view input) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(input.size() * 2);
  for (unsigned char c : input) {
    out.push_back(digits[c >> 4]);
    out.push_back(digits[c & 15]);
  }
  return out;
}

bool unhex(std::string_view input, std::string &out) {
  if (input.size() % 2) return false;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  out.clear();
  out.reserve(input.size() / 2);
  for (size_t i = 0; i < input.size(); i += 2) {
    int hi = nibble(input[i]), lo = nibble(input[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return true;
}

bool parse_u64(std::string_view s, uint64_t &v) {
  auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  return ec == std::errc{} && end == s.data() + s.size();
}

std::string body(const MembershipEvent &e, const ActorId &producer) {
  return "CME1 " + producer.to_string() + " " + std::to_string(e.sequence) + " " +
         std::to_string(e.epoch) + " " + e.subject.to_string() + " " +
         (e.kind == MembershipEvent::Kind::Enrolled ? "E" : "R") + " " +
         hex(e.evidence);
}

Result<MembershipEvent> parse_line(std::string_view line,
                                   const ActorId &producer,
                                   uint64_t expected) {
  auto checksum_pos = line.rfind(' ');
  if (checksum_pos == std::string_view::npos ||
      sha256_hex(line.substr(0, checksum_pos)) != line.substr(checksum_pos + 1))
    return Error(ErrorCode::corrupt_record, "membership record checksum mismatch");
  std::istringstream in(std::string(line.substr(0, checksum_pos)));
  std::string magic, producer_s, seq_s, epoch_s, actor_s, kind_s, evidence_s, extra;
  if (!(in >> magic >> producer_s >> seq_s >> epoch_s >> actor_s >> kind_s >> evidence_s) ||
      (in >> extra) || magic != "CME1")
    return Error(ErrorCode::corrupt_record, "membership record has invalid fields");
  auto record_producer = ActorId::parse(producer_s);
  if (!record_producer.ok() || record_producer.value() != producer)
    return Error(ErrorCode::actor_conflict, "membership journal belongs to another producer");
  MembershipEvent event;
  if (!parse_u64(seq_s, event.sequence) || event.sequence != expected ||
      !parse_u64(epoch_s, event.epoch) || event.epoch == 0)
    return Error(ErrorCode::corrupt_record, "membership sequence or epoch is invalid");
  auto actor = ActorId::parse(actor_s);
  if (!actor.ok() || !actor.value().valid())
    return Error(ErrorCode::corrupt_record, "membership subject is invalid");
  event.subject = actor.value();
  if (kind_s == "E") event.kind = MembershipEvent::Kind::Enrolled;
  else if (kind_s == "R") event.kind = MembershipEvent::Kind::Retired;
  else return Error(ErrorCode::corrupt_record, "membership event kind is invalid");
  if (!unhex(evidence_s, event.evidence) || event.evidence.size() > kMaxEvidenceBytes)
    return Error(ErrorCode::corrupt_record, "membership evidence is invalid");
  return event;
}
} // namespace

Result<MembershipStore> MembershipStore::open(std::filesystem::path path,
                                             ActorId producer,
                                             const StepHook *hook) {
  if (!producer.valid())
    return Error(ErrorCode::invalid_identity, "membership producer is invalid");
  MembershipStore store(std::move(path), producer, hook);
  auto events = store.recover();
  if (!events.ok()) return events.error();
  store.m_last_sequence = events.value().empty() ? 0 : events.value().back().sequence;
  return store;
}

Result<std::vector<MembershipEvent>> MembershipStore::recover() const {
  std::ifstream in(m_path, std::ios::binary);
  if (!in) {
    if (!std::filesystem::exists(m_path)) return std::vector<MembershipEvent>{};
    return Error(ErrorCode::io_error, "cannot read membership journal");
  }
  in.seekg(0, std::ios::end);
  auto length = in.tellg();
  if (length < 0 || static_cast<uint64_t>(length) > kMaxLogBytes)
    return Error(ErrorCode::corrupt_record, "membership journal exceeds size limit");
  in.seekg(0);
  std::string bytes(static_cast<size_t>(length), '\0');
  if (!bytes.empty() && !in.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
    return Error(ErrorCode::io_error, "cannot read membership journal contents");
  std::vector<MembershipEvent> events;
  uint64_t expected = 1;
  size_t start = 0;
  while (start < bytes.size()) {
    auto end = bytes.find('\n', start);
    if (end == std::string::npos)
      return Error(ErrorCode::corrupt_record, "membership journal ends with an incomplete record");
    auto event = parse_line(std::string_view(bytes).substr(start, end - start),
                            m_producer, expected);
    if (!event.ok()) return event.error();
    events.push_back(std::move(event.value()));
    if (expected == UINT64_MAX)
      return Error(ErrorCode::corrupt_record, "membership sequence overflow");
    ++expected;
    start = end + 1;
  }
  return events;
}

Result<void> MembershipStore::append(const MembershipEvent &event) {
  if (event.sequence != m_last_sequence + 1 || event.sequence == 0 ||
      event.epoch == 0 || !event.subject.valid() ||
      event.evidence.empty() || event.evidence.size() > kMaxEvidenceBytes ||
      m_last_sequence == UINT64_MAX)
    return Error(ErrorCode::malformed, "membership event is invalid or out of sequence");
  auto log = DurableAppendFile::open(m_path, m_hook);
  if (!log.ok()) return log.error();
  auto record = body(event, m_producer);
  record += " " + sha256_hex(record) + "\n";
  auto result = log.value().append(record);
  if (!result.ok()) {
    // append may have reached the file before a later sync step failed.
    auto recovered = recover();
    if (!recovered.ok()) return recovered.error();
    m_last_sequence = recovered.value().empty() ? 0 : recovered.value().back().sequence;
    return result.error();
  }
  m_last_sequence = event.sequence;
  return {};
}

} // namespace CollabText::Storage
