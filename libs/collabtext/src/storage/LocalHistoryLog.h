#pragma once

#include "crdt/Buffer.h"
#include "storage/DurableFile.h"

#include <filesystem>
#include <optional>

namespace CollabText::Storage {

struct PendingLocalHistoryCommit {
  uint64_t sequence = 0;
  std::string operation;
  Crdt::LocalHistorySnapshot snapshot;
};

/// Actor-local undo snapshot checkpoint. The caller replays operation history
/// first, then restores the checkpoint into its Buffer.
class LocalHistoryLog {
public:
  [[nodiscard]] static Result<LocalHistoryLog>
  open(std::filesystem::path path, ActorId owner);

  LocalHistoryLog(LocalHistoryLog &&) noexcept = default;
  LocalHistoryLog &operator=(LocalHistoryLog &&) noexcept = default;
  LocalHistoryLog(const LocalHistoryLog &) = delete;
  LocalHistoryLog &operator=(const LocalHistoryLog &) = delete;

  [[nodiscard]] Result<std::optional<Crdt::LocalHistorySnapshot>> recover() const;
  [[nodiscard]] Result<void>
  store_snapshot(const Crdt::LocalHistorySnapshot &snapshot);
  [[nodiscard]] Result<std::optional<PendingLocalHistoryCommit>>
  recover_pending() const;
  [[nodiscard]] Result<void>
  store_pending(const PendingLocalHistoryCommit &pending);
  [[nodiscard]] Result<void> clear_pending();
  [[nodiscard]] Result<void> close() { return {}; }

private:
  LocalHistoryLog(std::filesystem::path path, ActorId owner)
      : m_path(std::move(path)), m_owner(std::move(owner)) {}

  std::filesystem::path m_path;
  ActorId m_owner;
};

} // namespace CollabText::Storage
