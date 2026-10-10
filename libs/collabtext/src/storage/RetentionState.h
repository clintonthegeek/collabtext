#pragma once

#include "storage/DurableFile.h"
#include <collabtext/Engine.h>

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

namespace CollabText::Storage {

/// Per-stream, producer-local monotonic set of undo groups whose history has
/// been retired. It never asserts that a peer has acknowledged the retirement.
/// Open holds an OS writer lease for the state lifetime so concurrent handles
/// cannot publish colliding journal sequences.
class RetentionState {
public:
  [[nodiscard]] static Result<RetentionState>
  open(std::filesystem::path path, ActorId producer,
       const StepHook *hook = nullptr);
  RetentionState(RetentionState &&) noexcept = default;
  RetentionState &operator=(RetentionState &&) noexcept = default;
  RetentionState(const RetentionState &) = delete;
  RetentionState &operator=(const RetentionState &) = delete;

  /// The callback must durably commit the trimmed private history first.
  /// If it fails, no retirement record is published. A later journal failure
  /// leaves a conservative prefix; retrying the same groups is idempotent.
  [[nodiscard]] Result<void> publish_after_durable_trim(
      std::string_view stream, const std::vector<Crdt::Identity> &groups,
      const std::function<Result<void>()> &commit_trim);
  [[nodiscard]] bool is_retired(std::string_view stream,
                                const Crdt::Identity &group) const;
  [[nodiscard]] const std::set<Crdt::Identity> *retired_groups(
      std::string_view stream) const;

private:
  RetentionState(std::filesystem::path path, ActorId producer,
                 const StepHook *hook, ProcessLease lease)
      : m_path(std::move(path)), m_producer(producer), m_hook(hook),
        m_lease(std::move(lease)) {}
  [[nodiscard]] Result<void> recover();
  std::filesystem::path m_path;
  ActorId m_producer;
  const StepHook *m_hook = nullptr;
  std::optional<ProcessLease> m_lease;
  uint64_t m_sequence = 0;
  std::map<std::string, std::set<Crdt::Identity>> m_retired;
};

} // namespace CollabText::Storage
