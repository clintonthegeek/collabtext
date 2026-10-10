#pragma once

#include "storage/DurableFile.h"

#include <collabtext/ActorId.h>

#include <filesystem>
#include <vector>

namespace CollabText::Storage {

/// A producer-authored, append-only membership fact. It records intent and
/// provenance; eligibility still requires validation against the G01 protocol
/// and acknowledgements from the complete epoch membership.
struct MembershipEvent {
  enum class Kind { Enrolled, Retired };
  uint64_t sequence = 0;
  uint64_t epoch = 0;
  ActorId subject;
  Kind kind = Kind::Enrolled;
  std::string evidence;
};

/// Durable per-producer journal for explicit membership transitions.
class MembershipStore {
public:
  [[nodiscard]] static Result<MembershipStore>
  open(std::filesystem::path path, ActorId producer,
       const StepHook *hook = nullptr);
  MembershipStore(MembershipStore &&) noexcept = default;
  MembershipStore &operator=(MembershipStore &&) noexcept = default;
  MembershipStore(const MembershipStore &) = delete;
  MembershipStore &operator=(const MembershipStore &) = delete;

  [[nodiscard]] Result<std::vector<MembershipEvent>> recover() const;
  [[nodiscard]] Result<void> append(const MembershipEvent &event);
  [[nodiscard]] uint64_t last_sequence() const { return m_last_sequence; }

private:
  MembershipStore(std::filesystem::path path, ActorId producer,
                  const StepHook *hook)
      : m_path(std::move(path)), m_producer(producer), m_hook(hook) {}
  std::filesystem::path m_path;
  ActorId m_producer;
  const StepHook *m_hook = nullptr;
  uint64_t m_last_sequence = 0;
};

} // namespace CollabText::Storage
