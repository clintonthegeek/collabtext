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
  if (!(in >> magic >> actor_text) || magic != "CLH1")
    return Error(ErrorCode::corrupt_record, "unknown local history record format");
  auto actor = ActorId::parse(actor_text);
  if (!actor.ok() || actor.value() != owner)
    return Error(ErrorCode::actor_conflict,
                 "local history log belongs to another actor");
  auto cursor = number(in, "cursor");
  auto count = number(in, "entry count");
  if (!cursor.ok()) return cursor.error();
  if (!count.ok()) return count.error();
  if (count.value() > kMaxHistoryEntries || cursor.value() > count.value())
    return Error(ErrorCode::corrupt_record, "local history bounds are invalid");

  Crdt::LocalHistorySnapshot snapshot;
  snapshot.cursor = static_cast<size_t>(cursor.value());
  snapshot.entries.reserve(static_cast<size_t>(count.value()));
  for (uint64_t i = 0; i < count.value(); ++i) {
    Crdt::LocalUndoEntrySnapshot entry;
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
    snapshot.entries.push_back(std::move(entry));
  }
  std::string trailing;
  if (in >> trailing)
    return Error(ErrorCode::corrupt_record, "local history record has trailing fields");
  return snapshot;
}

std::string encode(const Crdt::LocalHistorySnapshot &snapshot,
                   const ActorId &owner) {
  std::ostringstream out;
  out << "CLH1 " << owner.to_string() << ' ' << snapshot.cursor << ' '
      << snapshot.entries.size();
  auto write_ids = [&](const std::vector<Crdt::Identity> &ids) {
    out << ' ' << ids.size();
    for (const auto &id : ids)
      out << ' ' << id.actor.to_string() << ' ' << id.counter;
  };
  for (const auto &entry : snapshot.entries) {
    write_ids(entry.inserted_items);
    write_ids(entry.deletion_operations);
  }
  out << '\n';
  return out.str();
}
} // namespace

Result<LocalHistoryLog> LocalHistoryLog::open(std::filesystem::path path,
                                               ActorId owner) {
  if (!owner.valid())
    return Error(ErrorCode::invalid_identity, "local history needs a valid owner");
  return LocalHistoryLog(std::move(path), std::move(owner));
}

Result<std::optional<Crdt::LocalHistorySnapshot>>
LocalHistoryLog::recover() const {
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

Result<void>
LocalHistoryLog::store_snapshot(const Crdt::LocalHistorySnapshot &snapshot) {
  if (snapshot.entries.size() > kMaxHistoryEntries ||
      snapshot.cursor > snapshot.entries.size())
    return Error(ErrorCode::limit_exceeded, "local history snapshot exceeds bounds");
  for (const auto &entry : snapshot.entries) {
    for (const auto &id : entry.inserted_items)
      if (id.actor != m_owner || id.counter == 0)
        return Error(ErrorCode::history_unavailable,
                     "cannot persist a non-owner undo item");
    for (const auto &id : entry.deletion_operations)
      if (id.actor != m_owner || id.counter == 0)
        return Error(ErrorCode::history_unavailable,
                     "cannot persist a non-owner deletion operation");
  }
  auto record = encode(snapshot, m_owner);
  if (record.size() > kMaxSnapshotBytes)
    return Error(ErrorCode::limit_exceeded, "local history snapshot exceeds byte limit");
  return replace_atomically(m_path, record);
}
} // namespace CollabText::Storage
