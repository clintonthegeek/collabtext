#include "storage/MembershipStore.h"

#include "core/Sha256.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>
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

void add_field(std::string &out, std::string_view value) {
  out += std::to_string(value.size());
  out += ':';
  out.append(value);
}

std::string preview_material(const RetirementPreview &preview) {
  std::string out = "retirement-preview-v1";
  add_field(out, preview.device.to_string());
  add_field(out, std::to_string(preview.parent_epoch));
  add_field(out, std::to_string(preview.next_epoch));
  add_field(out, preview.parent_descriptor_hash);
  for (const auto &member : preview.parent_members)
    add_field(out, member.to_string());
  out += "|acks|";
  for (const auto &member : preview.required_acknowledgers)
    add_field(out, member.to_string());
  out += "|impacts|";
  for (const auto &impact : preview.impacts) {
    add_field(out, impact.document);
    add_field(out, impact.stream);
    add_field(out, std::to_string(impact.epoch));
    add_field(out, std::to_string(impact.unsynced_operations));
    add_field(out, std::to_string(impact.retained_undo_groups));
    add_field(out, impact.recoverable_branch_hash);
  }
  return out;
}

bool same_preview(const RetirementPreview &a, const RetirementPreview &b) {
  return a.device == b.device && a.parent_epoch == b.parent_epoch &&
         a.next_epoch == b.next_epoch &&
         a.parent_descriptor_hash == b.parent_descriptor_hash &&
         a.parent_members == b.parent_members &&
         a.required_acknowledgers == b.required_acknowledgers &&
         a.impacts == b.impacts && a.digest == b.digest;
}

std::string enrollment_material(const EnrollmentPreview &preview) {
  std::string out = "enrollment-preview-v1";
  add_field(out, preview.device.to_string());
  add_field(out, std::to_string(preview.parent_epoch));
  add_field(out, std::to_string(preview.next_epoch));
  add_field(out, preview.parent_descriptor_hash);
  for (const auto &member : preview.parent_members)
    add_field(out, member.to_string());
  out += "|acks|";
  for (const auto &member : preview.required_acknowledgers)
    add_field(out, member.to_string());
  return out;
}

bool same_preview(const EnrollmentPreview &a, const EnrollmentPreview &b) {
  return a.device == b.device && a.parent_epoch == b.parent_epoch &&
         a.next_epoch == b.next_epoch &&
         a.parent_descriptor_hash == b.parent_descriptor_hash &&
         a.parent_members == b.parent_members &&
         a.required_acknowledgers == b.required_acknowledgers &&
         a.digest == b.digest;
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

Result<RetirementPreview> preview_retirement(
    const MembershipDescriptor &current, const ActorId &device,
    std::vector<RetirementImpact> impacts) {
  if (current.epoch == 0 || current.epoch == UINT64_MAX ||
      current.hash.empty() || current.hash.size() > 4096 ||
      !device.valid() || current.members.size() < 2 ||
      current.members.size() > 4096 || impacts.size() > 4096)
    return Error(ErrorCode::malformed, "retirement descriptor is invalid or cannot retire a member");
  auto members = current.members;
  std::sort(members.begin(), members.end());
  if (std::any_of(members.begin(), members.end(), [](const ActorId &id) { return !id.valid(); }) ||
      std::adjacent_find(members.begin(), members.end()) != members.end() ||
      !std::binary_search(members.begin(), members.end(), device))
    return Error(ErrorCode::malformed, "retirement descriptor has unknown or duplicate members");
  for (const auto &impact : impacts)
    if (impact.document.empty() || impact.document.size() > 4096 ||
        impact.stream.empty() || impact.stream.size() > 4096 ||
        impact.epoch != current.epoch || impact.recoverable_branch_hash.empty() ||
        impact.recoverable_branch_hash.size() > 4096)
      return Error(ErrorCode::malformed, "retirement consequence data is incomplete");
  std::sort(impacts.begin(), impacts.end());
  if (std::adjacent_find(impacts.begin(), impacts.end(), [](const auto &a, const auto &b) {
        return a.document == b.document && a.stream == b.stream;
      }) != impacts.end())
    return Error(ErrorCode::malformed, "retirement preview repeats a document stream");

  RetirementPreview preview;
  preview.device = device;
  preview.parent_epoch = current.epoch;
  preview.next_epoch = current.epoch + 1;
  preview.parent_descriptor_hash = current.hash;
  preview.parent_members = std::move(members);
  preview.impacts = std::move(impacts);
  for (const auto &member : preview.parent_members)
    if (member != device) preview.required_acknowledgers.push_back(member);
  preview.digest = sha256_hex(preview_material(preview));
  return preview;
}

Result<EnrollmentPreview> preview_enrollment(
    const MembershipDescriptor &current, const ActorId &device) {
  if (current.epoch == 0 || current.epoch == UINT64_MAX ||
      current.hash.empty() || current.hash.size() > 4096 ||
      !device.valid() || current.members.empty() ||
      current.members.size() >= 4096)
    return Error(ErrorCode::malformed,
                 "enrollment descriptor is invalid or cannot add a member");
  auto members = current.members;
  std::sort(members.begin(), members.end());
  if (std::any_of(members.begin(), members.end(),
                  [](const ActorId &id) { return !id.valid(); }) ||
      std::adjacent_find(members.begin(), members.end()) != members.end() ||
      std::binary_search(members.begin(), members.end(), device))
    return Error(ErrorCode::malformed,
                 "enrollment descriptor has invalid members or device is already enrolled");

  EnrollmentPreview preview;
  preview.device = device;
  preview.parent_epoch = current.epoch;
  preview.next_epoch = current.epoch + 1;
  preview.parent_descriptor_hash = current.hash;
  preview.parent_members = std::move(members);
  preview.required_acknowledgers = preview.parent_members;
  preview.required_acknowledgers.push_back(device);
  std::sort(preview.required_acknowledgers.begin(),
            preview.required_acknowledgers.end());
  preview.digest = sha256_hex(enrollment_material(preview));
  return preview;
}

Result<MembershipStore> MembershipStore::open(std::filesystem::path path,
                                             ActorId producer,
                                             const StepHook *hook) {
  if (!producer.valid())
    return Error(ErrorCode::invalid_identity, "membership producer is invalid");
  auto lease_path = path;
  lease_path += ".lock";
  auto lease = ProcessLease::acquire(lease_path);
  if (!lease.ok()) return lease.error().with_context("opening membership journal");
  MembershipStore store(std::move(path), producer, hook,
                        std::move(lease.value()));
  auto events = store.recover();
  if (!events.ok()) return events.error();
  store.m_last_sequence = events.value().empty() ? 0 : events.value().back().sequence;
  store.m_last_epoch = events.value().empty() ? 0 : events.value().back().epoch;
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
  uint64_t previous_epoch = 0;
  size_t start = 0;
  while (start < bytes.size()) {
    auto end = bytes.find('\n', start);
    if (end == std::string::npos)
      return Error(ErrorCode::corrupt_record, "membership journal ends with an incomplete record");
    auto event = parse_line(std::string_view(bytes).substr(start, end - start),
                            m_producer, expected);
    if (!event.ok()) return event.error();
    if (previous_epoch != 0 &&
        (previous_epoch == UINT64_MAX || event.value().epoch != previous_epoch + 1))
      return Error(ErrorCode::corrupt_record,
                   "membership epochs are not contiguous");
    previous_epoch = event.value().epoch;
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
      (event.kind != MembershipEvent::Kind::Enrolled &&
       event.kind != MembershipEvent::Kind::Retired) ||
      event.evidence.empty() || event.evidence.size() > kMaxEvidenceBytes ||
      m_last_sequence == UINT64_MAX ||
      (m_last_epoch != 0 &&
       (m_last_epoch == UINT64_MAX || event.epoch != m_last_epoch + 1)))
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
    m_last_epoch = recovered.value().empty() ? 0 : recovered.value().back().epoch;
    return result.error();
  }
  m_last_sequence = event.sequence;
  m_last_epoch = event.epoch;
  return {};
}

Result<MembershipEvent> MembershipStore::commit_retirement(
    const RetirementPreview &preview, const MembershipDescriptor &current,
    const std::vector<RetirementImpact> &current_impacts,
    const std::vector<MembershipAcknowledgement> &acknowledgements,
    std::string_view explicit_confirmation,
    const DurableAcknowledgementVerifier &verify_acknowledgement) {
  auto fresh = preview_retirement(current, preview.device, current_impacts);
  if (!fresh.ok()) return fresh.error();
  if (!same_preview(preview, fresh.value()))
    return Error(ErrorCode::wrong_epoch,
                 "retirement preview is stale; review current consequences again");
  auto persisted_events = recover();
  if (!persisted_events.ok()) return persisted_events.error();
  if (!persisted_events.value().empty() &&
      persisted_events.value().back().epoch != current.epoch)
    return Error(ErrorCode::wrong_epoch,
                 "retirement parent epoch does not match the latest journal transition");
  if (explicit_confirmation.empty() || explicit_confirmation.size() > 4096 ||
      !verify_acknowledgement)
    return Error(ErrorCode::malformed,
                 "retirement requires explicit confirmation and an acknowledgement verifier");
  if (acknowledgements.size() != preview.required_acknowledgers.size())
    return Error(ErrorCode::awaiting_state,
                 "retirement is awaiting acknowledgements from every remaining member");
  std::map<ActorId, std::string> signatures;
  for (const auto &ack : acknowledgements) {
    if (!ack.actor.valid() || ack.signature.empty() || ack.signature.size() > 4096 ||
        !signatures.emplace(ack.actor, ack.signature).second)
      return Error(ErrorCode::malformed, "retirement acknowledgement is invalid or duplicated");
  }
  for (const auto &member : preview.required_acknowledgers) {
    auto ack = signatures.find(member);
    if (ack == signatures.end())
      return Error(ErrorCode::awaiting_state,
                   "retirement acknowledgement set does not match remaining members");
    auto verified = verify_acknowledgement(member, preview.digest, ack->second);
    if (!verified.ok()) return verified.error();
  }
  if (m_last_sequence == UINT64_MAX)
    return Error(ErrorCode::limit_exceeded, "membership event sequence is exhausted");
  std::string evidence = "retirement-v1 " + hex(preview.digest) + " ";
  const auto material = preview_material(preview);
  evidence += hex(material) + " " + hex(explicit_confirmation) + " " +
              std::to_string(signatures.size());
  for (const auto &[actor, signature] : signatures)
    evidence += " " + actor.to_string() + " " + hex(signature);
  if (evidence.size() > kMaxEvidenceBytes)
    return Error(ErrorCode::limit_exceeded, "retirement evidence exceeds journal limit");
  MembershipEvent event{m_last_sequence + 1, preview.next_epoch, preview.device,
                        MembershipEvent::Kind::Retired, std::move(evidence)};
  auto persisted = append(event);
  if (!persisted.ok()) return persisted.error();
  return event;
}

Result<MembershipEvent> MembershipStore::commit_enrollment(
    const EnrollmentPreview &preview, const MembershipDescriptor &current,
    const std::vector<MembershipAcknowledgement> &acknowledgements,
    const DurableAcknowledgementVerifier &verify_acknowledgement) {
  auto fresh = preview_enrollment(current, preview.device);
  if (!fresh.ok()) return fresh.error();
  if (!same_preview(preview, fresh.value()))
    return Error(ErrorCode::wrong_epoch,
                 "enrollment preview is stale; review current membership again");
  auto persisted_events = recover();
  if (!persisted_events.ok()) return persisted_events.error();
  if (!persisted_events.value().empty() &&
      persisted_events.value().back().epoch != current.epoch)
    return Error(ErrorCode::wrong_epoch,
                 "enrollment parent epoch does not match the latest journal transition");
  if (!verify_acknowledgement)
    return Error(ErrorCode::malformed,
                 "enrollment requires an acknowledgement verifier");
  if (acknowledgements.size() != preview.required_acknowledgers.size())
    return Error(ErrorCode::awaiting_state,
                 "enrollment is awaiting acknowledgements from every current member and the joining device");
  std::map<ActorId, std::string> signatures;
  for (const auto &ack : acknowledgements) {
    if (!ack.actor.valid() || ack.signature.empty() || ack.signature.size() > 4096 ||
        !signatures.emplace(ack.actor, ack.signature).second)
      return Error(ErrorCode::malformed,
                   "enrollment acknowledgement is invalid or duplicated");
  }
  for (const auto &member : preview.required_acknowledgers) {
    auto ack = signatures.find(member);
    if (ack == signatures.end())
      return Error(ErrorCode::awaiting_state,
                   "enrollment acknowledgement set does not match current membership and joining device");
    auto verified = verify_acknowledgement(member, preview.digest, ack->second);
    if (!verified.ok()) return verified.error();
  }
  if (m_last_sequence == UINT64_MAX)
    return Error(ErrorCode::limit_exceeded, "membership event sequence is exhausted");
  std::string evidence = "enrollment-v1 " + hex(preview.digest) + " " +
                         hex(enrollment_material(preview)) + " " +
                         std::to_string(signatures.size());
  for (const auto &[actor, signature] : signatures)
    evidence += " " + actor.to_string() + " " + hex(signature);
  if (evidence.size() > kMaxEvidenceBytes)
    return Error(ErrorCode::limit_exceeded,
                 "enrollment evidence exceeds journal limit");
  MembershipEvent event{m_last_sequence + 1, preview.next_epoch, preview.device,
                        MembershipEvent::Kind::Enrolled, std::move(evidence)};
  auto persisted = append(event);
  if (!persisted.ok()) return persisted.error();
  return event;
}

} // namespace CollabText::Storage
