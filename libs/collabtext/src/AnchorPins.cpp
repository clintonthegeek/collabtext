#include <collabtext/AnchorPins.h>
#include "core/Sha256.h"

#include <algorithm>
#include <mutex>
#include <optional>
#include <set>

namespace CollabText {
namespace {
constexpr size_t kMaxIdBytes = 256;
constexpr size_t kMaxTokenBytes = 64 * 1024;

Error conflict(std::string message) {
  return Error(ErrorCode::pin_conflict, std::move(message));
}

Error scope_error(const PositionScope& actual, const PositionScope& expected) {
  if (actual.document != expected.document)
    return Error(ErrorCode::wrong_document, "anchor pin document scope differs");
  if (actual.stream != expected.stream)
    return Error(ErrorCode::wrong_stream, "anchor pin stream scope differs");
  if (actual.primitive != expected.primitive)
    return Error(ErrorCode::wrong_primitive, "anchor pin primitive scope differs");
  if (actual.epoch != expected.epoch)
    return Error(ErrorCode::wrong_epoch, "anchor pin epoch scope differs");
  return Error(ErrorCode::malformed, "invalid anchor pin scope");
}

bool valid_id(std::string_view id) {
  return !id.empty() && id.size() <= kMaxIdBytes &&
         std::all_of(id.begin(), id.end(), [](unsigned char c) {
           return c >= 0x21 && c <= 0x7e;
         });
}

std::string fingerprint(const AnchorPinEvent& e) {
  std::string bytes;
  auto field = [&](std::string_view value) {
    const uint64_t n = value.size();
    for (unsigned i = 0; i != 8; ++i)
      bytes += static_cast<char>((n >> (i * 8)) & 0xff);
    bytes.append(value);
  };
  field(std::to_string(e.sequence));
  field(e.event_id);
  field(e.action == AnchorPinAction::register_pin ? "register" : "release");
  field(e.reference_id);
  field(e.scope.document);
  field(e.scope.stream);
  field(e.scope.primitive == Primitive::text ? "text" : "list");
  field(std::to_string(e.scope.epoch));
  field(e.token_bytes);
  return sha256_hex(bytes);
}
} // namespace

struct AnchorPinRegistry::Impl {
  PositionScope scope;
  Persist persist;
  mutable std::mutex mutex;
  uint64_t frontier = 0;
  std::map<uint64_t, std::string> by_sequence;
  std::map<std::string, uint64_t, std::less<>> by_event_id;
  // Presence with a null optional is a terminal release tombstone.
  std::map<std::string, std::optional<RetainedAnchorPin>, std::less<>> refs;

  Impl(PositionScope s, Persist p) : scope(std::move(s)), persist(std::move(p)) {}

  Result<void> validate_event(const AnchorPinEvent& e) const {
    if (!e.sequence || !valid_id(e.event_id) || !valid_id(e.reference_id))
      return Error(ErrorCode::malformed, "invalid anchor pin event identity");
    if (e.scope != scope) return scope_error(e.scope, scope);
    if (e.action == AnchorPinAction::register_pin) {
      if (e.token_bytes.empty() || e.token_bytes.size() > kMaxTokenBytes)
        return Error(ErrorCode::malformed, "invalid anchor pin token length");
      auto checked_scope = validate_position_scope(e.token_bytes, scope);
      if (!checked_scope) return checked_scope.error();
    } else if (!e.token_bytes.empty()) {
      return Error(ErrorCode::malformed, "release event contains token bytes");
    }
    return {};
  }

  Result<void> apply(const AnchorPinEvent& e) {
    auto valid = validate_event(e);
    if (!valid) return valid.error();
    const auto digest = fingerprint(e);
    if (e.sequence <= frontier) {
      const auto old = by_sequence.find(e.sequence);
      if (old != by_sequence.end() && old->second == digest) return {};
      return conflict("conflicting or unavailable anchor pin replay");
    }
    if (frontier == UINT64_MAX || e.sequence != frontier + 1)
      return Error(ErrorCode::awaiting_state, "anchor pin projection has a sequence gap");
    if (by_event_id.contains(e.event_id))
      return conflict("anchor pin event ID was reused");

    const auto existing = refs.find(e.reference_id);
    if (existing != refs.end()) {
      if (e.action == AnchorPinAction::register_pin) {
        const auto& live = existing->second;
        if (!live || live->scope != e.scope || live->token_bytes != e.token_bytes)
          return conflict("released or differently registered reference ID was reused");
        // Distinct canonical registration events for the same immutable pin are
        // accepted as idempotent projections, but still advance the frontier.
      } else {
        existing->second.reset();
      }
    } else if (e.action == AnchorPinAction::register_pin) {
      refs.emplace(e.reference_id,
                   RetainedAnchorPin{e.reference_id, e.scope, e.token_bytes});
    } else {
      refs.emplace(e.reference_id, std::nullopt); // preserve release-before-register
    }

    by_sequence.emplace(e.sequence, digest);
    by_event_id.emplace(e.event_id, e.sequence);
    frontier = e.sequence;
    return {};
  }
};

AnchorPinRegistry::AnchorPinRegistry(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl)) {}
AnchorPinRegistry::~AnchorPinRegistry() = default;

Result<std::unique_ptr<AnchorPinRegistry>>
AnchorPinRegistry::create(PositionScope scope, Persist persist) {
  if (!valid_id(scope.document) || !valid_id(scope.stream) || !scope.epoch || !persist ||
      (scope.primitive != Primitive::text && scope.primitive != Primitive::list))
    return Error(ErrorCode::malformed, "invalid anchor pin registry configuration");
  return std::unique_ptr<AnchorPinRegistry>(
      new AnchorPinRegistry(std::make_unique<Impl>(std::move(scope), std::move(persist))));
}

Result<void> AnchorPinRegistry::register_pin(uint64_t sequence, std::string event_id,
                                              std::string reference_id,
                                              std::string token_bytes) {
  AnchorPinEvent event{sequence, std::move(event_id), AnchorPinAction::register_pin,
                       std::move(reference_id), m_impl->scope,
                       std::move(token_bytes)};
  auto valid = m_impl->validate_event(event);
  if (!valid) return valid.error();
  std::lock_guard lock(m_impl->mutex);
  // A retry after an uncertain caller outcome is idempotent and must not
  // append a second canonical record.
  if (event.sequence <= m_impl->frontier) {
    const auto old = m_impl->by_sequence.find(event.sequence);
    if (old != m_impl->by_sequence.end() && old->second == fingerprint(event)) return {};
    return conflict("conflicting or unavailable anchor pin replay");
  }
  if (m_impl->frontier == UINT64_MAX || event.sequence != m_impl->frontier + 1)
    return Error(event.sequence <= m_impl->frontier ? ErrorCode::counter_regression
                                                     : ErrorCode::awaiting_state,
                 "anchor pin sequence is not the next projection event");
  if (m_impl->by_event_id.contains(event.event_id))
    return conflict("anchor pin event ID was reused");
  const auto ref = m_impl->refs.find(event.reference_id);
  if (ref != m_impl->refs.end() &&
      (!ref->second || ref->second->scope != event.scope ||
       ref->second->token_bytes != event.token_bytes))
    return conflict("released or differently registered reference ID was reused");
  auto durable = m_impl->persist(event);
  if (!durable) return durable.error();
  return m_impl->apply(event);
}

Result<void> AnchorPinRegistry::release_pin(uint64_t sequence, std::string event_id,
                                             std::string reference_id) {
  AnchorPinEvent event{sequence, std::move(event_id), AnchorPinAction::release_pin,
                       std::move(reference_id), m_impl->scope, {}};
  std::lock_guard lock(m_impl->mutex);
  if (event.sequence <= m_impl->frontier) {
    const auto old = m_impl->by_sequence.find(event.sequence);
    if (old != m_impl->by_sequence.end() && old->second == fingerprint(event)) return {};
    return conflict("conflicting or unavailable anchor pin replay");
  }
  if (m_impl->frontier == UINT64_MAX || event.sequence != m_impl->frontier + 1)
    return Error(event.sequence <= m_impl->frontier ? ErrorCode::counter_regression
                                                     : ErrorCode::awaiting_state,
                 "anchor pin sequence is not the next projection event");
  if (m_impl->by_event_id.contains(event.event_id))
    return conflict("anchor pin event ID was reused");
  auto valid = m_impl->validate_event(event);
  if (!valid) return valid.error();
  auto durable = m_impl->persist(event);
  if (!durable) return durable.error();
  return m_impl->apply(event);
}

Result<void> AnchorPinRegistry::replay(const AnchorPinEvent& event) {
  std::lock_guard lock(m_impl->mutex);
  return m_impl->apply(event);
}

uint64_t AnchorPinRegistry::projected_frontier() const noexcept {
  std::lock_guard lock(m_impl->mutex);
  return m_impl->frontier;
}

Result<RetainedAnchorPin>
AnchorPinRegistry::lookup_pin(std::string_view reference_id) const {
  std::lock_guard lock(m_impl->mutex);
  const auto found = m_impl->refs.find(reference_id);
  if (found == m_impl->refs.end() || !found->second)
    return Error(ErrorCode::not_found, "anchor reference is not live");
  return *found->second;
}

std::vector<RetainedAnchorPin> AnchorPinRegistry::live_pins() const {
  std::lock_guard lock(m_impl->mutex);
  std::vector<RetainedAnchorPin> out;
  for (const auto& [id, pin] : m_impl->refs) {
    (void)id;
    if (pin) out.push_back(*pin);
  }
  return out;
}

bool AnchorPinRegistry::recovery_complete(uint64_t canonical_frontier) const noexcept {
  std::lock_guard lock(m_impl->mutex);
  return m_impl->frontier == canonical_frontier;
}
} // namespace CollabText
