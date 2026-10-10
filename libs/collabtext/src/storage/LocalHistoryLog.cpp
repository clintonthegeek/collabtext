#include "storage/LocalHistoryLog.h"

#include <charconv>
#include <fstream>
#include <sstream>

namespace CollabText::Storage {
namespace {
constexpr size_t kMaxSnapshotBytes = 64 * 1024 * 1024;
constexpr size_t kMaxHistoryEntries = 100000;

Result<uint64_t> number(std::istream &in, std::string_view field) {
  std::string token;
  if (!(in >> token))
    return Error(ErrorCode::corrupt_record,
                 "local history record is missing " + std::string(field));
  uint64_t value = 0;
  const auto [end, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
  if (ec != std::errc{} || end != token.data() + token.size())
    return Error(ErrorCode::corrupt_record,
                 "local history record has invalid " + std::string(field));
  return value;
}

Result<Crdt::Identity> identity(std::istream &in, const ActorId &owner) {
  std::string actor_text;
  if (!(in >> actor_text))
    return Error(ErrorCode::corrupt_record, "local history identity is incomplete");
  auto actor = ActorId::parse(actor_text);
  if (!actor.ok() || actor.value() != owner)
    return Error(ErrorCode::corrupt_record,
                 "local history contains an identity for another actor");
  auto counter = number(in, "identity counter");
  if (!counter.ok() || counter.value() == 0)
    return Error(ErrorCode::corrupt_record, "local history identity counter is invalid");
  return Crdt::Identity{owner, counter.value()};
}

Result<Crdt::LocalHistorySnapshot> decode(std::string_view line,
                                          const ActorId &owner) {
  std::istringstream in{std::string(line)};
  std::string magic, actor_text;
  if (!(in >> magic >> actor_text) || magic != "CLH4")
    return Error(ErrorCode::corrupt_record, "unknown local history record format");
  auto actor = ActorId::parse(actor_text);
  if (!actor.ok() || actor.value() != owner)
    return Error(ErrorCode::actor_conflict,
                 "local history log belongs to another actor");
  auto cursor = number(in, "cursor");
  auto count = number(in, "entry count");
  auto retired_count = number(in, "retired group count");
  if (!cursor.ok()) return cursor.error();
  if (!count.ok()) return count.error();
  if (!retired_count.ok()) return retired_count.error();
  if (count.value() > kMaxHistoryEntries ||
      retired_count.value() > kMaxHistoryEntries ||
      count.value() + retired_count.value() > kMaxHistoryEntries ||
      cursor.value() > count.value())
    return Error(ErrorCode::corrupt_record, "local history bounds are invalid");

  Crdt::LocalHistorySnapshot snapshot;
  snapshot.cursor = static_cast<size_t>(cursor.value());
  auto read_entry = [&]() -> Result<Crdt::LocalUndoEntrySnapshot> {
    Crdt::LocalUndoEntrySnapshot entry;
    auto group = identity(in, owner);
    if (!group.ok()) return group.error();
    entry.group_id = group.value();
    auto operations = number(in, "group operation count");
    if (!operations.ok()) return operations.error();
    if (operations.value() == 0 || operations.value() > kMaxHistoryEntries)
      return Error(ErrorCode::corrupt_record, "local history group size is invalid");
    entry.operation_ids.reserve(static_cast<size_t>(operations.value()));
    for (uint64_t j = 0; j < operations.value(); ++j) {
      auto id = identity(in, owner);
      if (!id.ok()) return id.error();
      entry.operation_ids.push_back(id.value());
    }
    auto inserted = number(in, "inserted item count");
    if (!inserted.ok()) return inserted.error();
    if (inserted.value() > kMaxHistoryEntries)
      return Error(ErrorCode::corrupt_record, "local history entry is too large");
    entry.inserted_items.reserve(static_cast<size_t>(inserted.value()));
    for (uint64_t j = 0; j < inserted.value(); ++j) {
      auto id = identity(in, owner);
      if (!id.ok()) return id.error();
      entry.inserted_items.push_back(id.value());
    }
    auto deletions = number(in, "deletion count");
    if (!deletions.ok()) return deletions.error();
    if (deletions.value() > kMaxHistoryEntries)
      return Error(ErrorCode::corrupt_record, "local history entry is too large");
    entry.deletion_operations.reserve(static_cast<size_t>(deletions.value()));
    for (uint64_t j = 0; j < deletions.value(); ++j) {
      auto id = identity(in, owner);
      if (!id.ok()) return id.error();
      entry.deletion_operations.push_back(id.value());
    }
    auto retained = number(in, "retained history bytes");
    if (!retained.ok()) return retained.error();
    if (retained.value() > kMaxSnapshotBytes)
      return Error(ErrorCode::corrupt_record,
                   "local history retained-byte count exceeds the limit");
    entry.retained_bytes = static_cast<size_t>(retained.value());
    return entry;
  };
  snapshot.entries.reserve(static_cast<size_t>(count.value()));
  for (uint64_t i = 0; i < count.value(); ++i) {
    auto entry = read_entry();
    if (!entry.ok()) return entry.error();
    snapshot.entries.push_back(std::move(entry).value());
  }
  snapshot.retired_entries.reserve(static_cast<size_t>(retired_count.value()));
  for (uint64_t i = 0; i < retired_count.value(); ++i) {
    auto entry = read_entry();
    if (!entry.ok()) return entry.error();
    snapshot.retired_entries.push_back(std::move(entry).value());
  }
  std::string trailing;
  if (in >> trailing)
    return Error(ErrorCode::corrupt_record, "local history record has trailing fields");
  return snapshot;
}

std::string encode(const Crdt::LocalHistorySnapshot &snapshot,
                   const ActorId &owner) {
  std::ostringstream out;
  out << "CLH4 " << owner.to_string() << ' ' << snapshot.cursor << ' '
      << snapshot.entries.size() << ' ' << snapshot.retired_entries.size();
  auto write_ids = [&](const std::vector<Crdt::Identity> &ids) {
    out << ' ' << ids.size();
    for (const auto &id : ids)
      out << ' ' << id.actor.to_string() << ' ' << id.counter;
  };
  auto write_entry = [&](const auto &entry) {
    out << ' ' << entry.group_id.actor.to_string() << ' ' << entry.group_id.counter;
    write_ids(entry.operation_ids);
    write_ids(entry.inserted_items);
    write_ids(entry.deletion_operations);
    out << ' ' << entry.retained_bytes;
  };
  for (const auto &entry : snapshot.entries) write_entry(entry);
  for (const auto &entry : snapshot.retired_entries) write_entry(entry);
  out << '\n';
  return out.str();
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

Result<std::string> unhex(std::string_view text) {
  if (text.size() % 2)
    return Error(ErrorCode::corrupt_record, "pending operation hex is truncated");
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  std::string bytes;
  bytes.reserve(text.size() / 2);
  for (size_t i = 0; i < text.size(); i += 2) {
    int hi = nibble(text[i]), lo = nibble(text[i + 1]);
    if (hi < 0 || lo < 0)
      return Error(ErrorCode::corrupt_record, "pending operation hex is invalid");
    bytes.push_back(static_cast<char>((hi << 4) | lo));
  }
  return bytes;
}

bool belongs_to(const Crdt::LocalHistorySnapshot &snapshot,
                const ActorId &owner) {
  auto valid_entry = [&](const auto &entry) {
    if (entry.group_id.actor != owner || entry.group_id.counter == 0 ||
        entry.operation_ids.empty()) return false;
    for (const auto &id : entry.operation_ids)
      if (id.actor != owner || id.counter == 0) return false;
    for (const auto &id : entry.inserted_items)
      if (id.actor != owner || id.counter == 0) return false;
    for (const auto &id : entry.deletion_operations)
      if (id.actor != owner || id.counter == 0) return false;
    return true;
  };
  for (const auto &entry : snapshot.entries)
    if (!valid_entry(entry)) return false;
  for (const auto &entry : snapshot.retired_entries)
    if (!valid_entry(entry)) return false;
  return true;
}
} // namespace

Result<LocalHistoryLog> LocalHistoryLog::open(std::filesystem::path path,
                                               ActorId owner,
                                               const StepHook *hook) {
  if (!owner.valid())
    return Error(ErrorCode::invalid_identity, "local history needs a valid owner");
  return LocalHistoryLog(std::move(path), std::move(owner), hook);
}

Result<std::optional<Crdt::LocalHistorySnapshot>>
LocalHistoryLog::recover() const {
  if (!std::filesystem::exists(m_path))
    return std::optional<Crdt::LocalHistorySnapshot>{};
  std::ifstream file(m_path, std::ios::binary);
  if (!file)
    return Error(ErrorCode::io_error, "cannot read local history log",
                 m_path.string());
  std::string contents((std::istreambuf_iterator<char>(file)), {});
  if (contents.size() > kMaxSnapshotBytes)
    return Error(ErrorCode::limit_exceeded, "local history log exceeds byte limit");
  if (contents.empty()) return std::optional<Crdt::LocalHistorySnapshot>{};
  if (contents.back() != '\n' || contents.find('\n') != contents.size() - 1)
    return Error(ErrorCode::corrupt_record,
                 "local history checkpoint is not one complete record",
                 m_path.string());
  auto parsed = decode(std::string_view(contents).substr(0, contents.size() - 1),
                       m_owner);
  if (!parsed.ok()) return parsed.error().with_context(m_path.string());
  return std::optional<Crdt::LocalHistorySnapshot>(std::move(parsed).value());
}

Result<std::optional<PendingLocalHistoryCommit>>
LocalHistoryLog::recover_pending() const {
  auto path = m_path;
  path += ".pending";
  if (!std::filesystem::exists(path))
    return std::optional<PendingLocalHistoryCommit>{};
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return Error(ErrorCode::io_error, "cannot read pending history commit",
                 path.string());
  std::string bytes((std::istreambuf_iterator<char>(file)), {});
  if (bytes == "CLP0\n") return std::optional<PendingLocalHistoryCommit>{};
  const size_t first_end = bytes.find('\n');
  const size_t second_end = first_end == std::string::npos
                                ? std::string::npos
                                : bytes.find('\n', first_end + 1);
  if (first_end == std::string::npos || second_end != bytes.size() - 1 ||
      bytes.size() > kMaxSnapshotBytes)
    return Error(ErrorCode::corrupt_record,
                 "pending history commit is incomplete", path.string());
  std::istringstream header(bytes.substr(0, first_end));
  std::string magic, payload_hex;
  if (!(header >> magic) || magic != "CLP1")
    return Error(ErrorCode::corrupt_record,
                 "pending history commit format is invalid", path.string());
  auto sequence = number(header, "delivery sequence");
  if (!sequence.ok() || sequence.value() == 0)
    return Error(ErrorCode::corrupt_record,
                 "pending history delivery sequence is invalid");
  if (!(header >> payload_hex))
    return Error(ErrorCode::corrupt_record,
                 "pending history operation is missing");
  std::string extra;
  if (header >> extra)
    return Error(ErrorCode::corrupt_record,
                 "pending history commit has trailing header fields");
  auto payload = unhex(payload_hex);
  if (!payload.ok()) return payload.error();
  auto snapshot = decode(
      std::string_view(bytes).substr(first_end + 1, second_end - first_end - 1),
      m_owner);
  if (!snapshot.ok()) return snapshot.error();
  return std::optional<PendingLocalHistoryCommit>(PendingLocalHistoryCommit{
      sequence.value(), std::move(payload).value(),
      std::move(snapshot).value()});
}

Result<void>
LocalHistoryLog::store_snapshot(const Crdt::LocalHistorySnapshot &snapshot) {
  if (snapshot.entries.size() > kMaxHistoryEntries ||
      snapshot.retired_entries.size() > kMaxHistoryEntries ||
      snapshot.entries.size() + snapshot.retired_entries.size() > kMaxHistoryEntries ||
      snapshot.cursor > snapshot.entries.size())
    return Error(ErrorCode::limit_exceeded, "local history snapshot exceeds bounds");
  if (!belongs_to(snapshot, m_owner))
    return Error(ErrorCode::history_unavailable,
                 "cannot persist history identities for another actor");
  auto record = encode(snapshot, m_owner);
  if (record.size() > kMaxSnapshotBytes)
    return Error(ErrorCode::limit_exceeded, "local history snapshot exceeds byte limit");
  return replace_atomically(m_path, record, m_hook);
}

Result<void> LocalHistoryLog::store_pending(
    const PendingLocalHistoryCommit &pending) {
  if (pending.sequence == 0 || pending.operation.empty() ||
      pending.snapshot.entries.size() > kMaxHistoryEntries ||
      pending.snapshot.retired_entries.size() > kMaxHistoryEntries ||
      pending.snapshot.entries.size() + pending.snapshot.retired_entries.size() >
          kMaxHistoryEntries ||
      pending.snapshot.cursor > pending.snapshot.entries.size() ||
      !belongs_to(pending.snapshot, m_owner))
    return Error(ErrorCode::history_unavailable,
                 "pending local commit is invalid or outside configured bounds");
  auto snapshot = encode(pending.snapshot, m_owner);
  std::string bytes = "CLP1 " + std::to_string(pending.sequence) + " " +
                      hex(pending.operation) + "\n" + snapshot;
  if (bytes.size() > kMaxSnapshotBytes)
    return Error(ErrorCode::limit_exceeded,
                 "pending local history commit exceeds byte limit");
  auto path = m_path;
  path += ".pending";
  return replace_atomically(path, bytes, m_hook);
}

Result<void> LocalHistoryLog::clear_pending() {
  auto path = m_path;
  path += ".pending";
  return replace_atomically(path, "CLP0\n", m_hook);
}
} // namespace CollabText::Storage
