#pragma once

#include <collabtext/Anchor.h>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace CollabText {

enum class AnchorPinAction { register_pin, release_pin };

/// Canonical consumer-owned event projected into a document's pin state.
/// Sequences start at 1 and must be contiguous.
struct AnchorPinEvent {
  uint64_t sequence = 0;
  std::string event_id;
  AnchorPinAction action = AnchorPinAction::register_pin;
  std::string reference_id;
  PositionScope scope;
  std::string token_bytes;
  bool operator==(const AnchorPinEvent&) const = default;
};

struct RetainedAnchorPin {
  std::string reference_id;
  PositionScope scope;
  std::string token_bytes;
};

/// Idempotent projection of the consumer's canonical pin events. The callback
/// must durably append the exact event to the consumer's canonical log before
/// this projection changes. Recovery calls replay() without that callback.
/// The callback is serialized under the registry lock and must not call back
/// into this registry. On an uncertain append result, reopen/replay the
/// canonical log before retrying.
class AnchorPinRegistry {
public:
  using Persist = std::function<Result<void>(const AnchorPinEvent&)>;

  static Result<std::unique_ptr<AnchorPinRegistry>> create(PositionScope, Persist);
  ~AnchorPinRegistry();
  AnchorPinRegistry(const AnchorPinRegistry&) = delete;
  AnchorPinRegistry& operator=(const AnchorPinRegistry&) = delete;

  Result<void> register_pin(uint64_t sequence, std::string event_id,
                            std::string reference_id, std::string token_bytes);
  Result<void> release_pin(uint64_t sequence, std::string event_id,
                           std::string reference_id);
  /// Apply one already durable canonical event. Identical replay is harmless;
  /// gaps and conflicting replays fail without changing projection state.
  Result<void> replay(const AnchorPinEvent&);

  [[nodiscard]] uint64_t projected_frontier() const noexcept;
  [[nodiscard]] Result<RetainedAnchorPin> lookup_pin(std::string_view reference_id) const;
  [[nodiscard]] std::vector<RetainedAnchorPin> live_pins() const;
  [[nodiscard]] bool recovery_complete(uint64_t canonical_frontier) const noexcept;

private:
  struct Impl;
  explicit AnchorPinRegistry(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> m_impl;
};

} // namespace CollabText
