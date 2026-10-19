/// OpStream.h — transactional, transport-agnostic operation delivery contract.
///
/// The durable receipt contract is specified in docs/specs/vnext-transport.md.
/// A successful transport receipt is per stream, epoch and producer, and never
/// serves as semantic garbage-collection authorization.
#pragma once

#include <collabtext/ActorId.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace CollabText {

struct StreamScope {
    StreamId stream; ///< 1-128 ASCII bytes: letters, digits, underscore, hyphen, period, colon.
    uint64_t epoch = 0; ///< Nonzero membership epoch.

    [[nodiscard]] bool operator==(const StreamScope&) const = default;
};

/// Stable identity of one transport envelope. Sequence is independent of the
/// CRDT operation counter and contiguous within (stream, epoch, producer).
struct DeliveryToken {
    StreamId stream;
    uint64_t epoch = 0;
    ActorId producer;
    uint64_t sequence = 0;

    [[nodiscard]] bool operator==(const DeliveryToken&) const = default;
};

struct OutboundDelivery {
    DeliveryToken token;
    std::string payload; // Opaque bytes; transports preserve them exactly.
};

struct InboundDelivery {
    DeliveryToken token;
    std::string payload;
};

enum class PushDurability {
    queued,  ///< Accepted in volatile transport memory; process crash may lose it.
    durable  ///< Recoverable sender-side copy exists before push succeeds.
};

/// A durable application recovery point associated with a committed delivery.
class RecoveryPoint {
public:
    enum class Kind { replay_from_beginning, checkpoint };

    [[nodiscard]] static RecoveryPoint replay_from_beginning() {
        return RecoveryPoint(Kind::replay_from_beginning, {});
    }
    [[nodiscard]] static RecoveryPoint checkpoint(std::string id) {
        return RecoveryPoint(Kind::checkpoint, std::move(id));
    }

    [[nodiscard]] Kind kind() const noexcept { return m_kind; }
    [[nodiscard]] std::string_view checkpoint_id() const noexcept { return m_checkpoint_id; }
    [[nodiscard]] bool valid() const noexcept {
        return m_kind == Kind::replay_from_beginning || !m_checkpoint_id.empty();
    }

private:
    RecoveryPoint(Kind kind, std::string id)
        : m_kind(kind), m_checkpoint_id(std::move(id)) {}
    Kind m_kind;
    std::string m_checkpoint_id;
};

class DeliveryDecision {
public:
    enum class Kind { commit, retry, quarantine };

    [[nodiscard]] static DeliveryDecision commit(RecoveryPoint point) {
        return DeliveryDecision(Kind::commit, std::move(point), {});
    }
    [[nodiscard]] static DeliveryDecision retry() {
        return DeliveryDecision(Kind::retry, std::nullopt, {});
    }
    [[nodiscard]] static DeliveryDecision quarantine(Error reason) {
        return DeliveryDecision(Kind::quarantine, std::nullopt, std::move(reason));
    }

    [[nodiscard]] Kind kind() const noexcept { return m_kind; }
    [[nodiscard]] const std::optional<RecoveryPoint>& recovery_point() const noexcept {
        return m_recovery_point;
    }
    [[nodiscard]] const Error& error() const noexcept { return m_error; }

private:
    DeliveryDecision(Kind kind, std::optional<RecoveryPoint> recovery_point, Error error)
        : m_kind(kind), m_recovery_point(std::move(recovery_point)), m_error(std::move(error)) {}
    Kind m_kind;
    std::optional<RecoveryPoint> m_recovery_point;
    Error m_error;
};

using InboundHandler = std::function<DeliveryDecision(const InboundDelivery&)>;
using SubscriptionId = uint64_t;

/// A sender-side snapshot of a remote receiver's contiguous committed frontier.
/// The key is (peer, stream, epoch, producer); `through` names the last token.
struct StreamReceipt {
    ActorId peer;
    DeliveryToken through;

    [[nodiscard]] bool operator==(const StreamReceipt&) const = default;
};
using ReceiptHandler = std::function<void(const StreamReceipt&)>;

struct PollBudget {
    size_t max_records = 256;
    size_t max_payload_bytes = 1024 * 1024;
};

struct PollReport {
    size_t offered = 0;
    size_t committed = 0;
    size_t retried = 0;
    size_t quarantined = 0;
    size_t payload_bytes = 0;
    bool budget_limited = false;
};

/// Abstract transport contract. Implementations own callbacks and invoke them
/// synchronously from poll(), without holding internal locks.
class OpStream {
public:
    virtual ~OpStream() = default;

    /// Register the current membership epoch for a stream. Unknown streams are
    /// never created implicitly by any other method.
    [[nodiscard]] virtual Result<void> register_stream(const StreamScope& scope) = 0;

    /// Push using the caller's stable envelope token. `durable` success means
    /// recoverable sender-side bytes; it says nothing about peer receipt.
    [[nodiscard]] virtual Result<void> push(const OutboundDelivery& delivery,
                                            PushDurability durability) = 0;

    /// One inbound handler may be active for a scope at a time.
    [[nodiscard]] virtual Result<SubscriptionId> subscribe(
        const StreamScope& scope, InboundHandler handler) = 0;

    /// Observe receipt advances. This callback cannot authorize collection.
    [[nodiscard]] virtual Result<SubscriptionId> subscribe_receipts(
        ReceiptHandler handler) = 0;

    /// Remove either kind of subscription. Idempotent for an already removed id.
    [[nodiscard]] virtual Result<void> unsubscribe(SubscriptionId id) = 0;

    /// Offer bounded inbound work for one known scope. Without a handler this is
    /// idle and consumes nothing. `retry` and `quarantine` never advance receipts.
    [[nodiscard]] virtual Result<PollReport> poll(
        const StreamScope& scope, PollBudget budget = {}) = 0;

    /// Snapshot committed frontiers. The query performs no I/O.
    [[nodiscard]] virtual Result<std::vector<StreamReceipt>> receipts(
        const StreamScope& scope) const = 0;
};

} // namespace CollabText
