/// OpStream.h — Transport-agnostic op delivery boundary for collabtext
///
/// This header defines the public `OpStream` interface introduced under the
/// OpStream extraction (Task 2.1).  See also:
///   docs/handoff/2026-05-08-d5-joint-design-outcomes.md  §"Public type-surface form"
///   docs/specs/2026-05-08-d5-negotiation-response.md
///
/// ─── Ordering contract ───────────────────────────────────────────────────────
///
///   An implementation MUST deliver inbound entries within a single stream in
///   the order they were pushed by a given producer.  Entries from different
///   streams MAY be reordered.  Entries from different producers within the
///   same stream MAY be reordered.
///
/// ─── Stream naming ───────────────────────────────────────────────────────────
///
///   `stream_name` identifies which CRDT stream an op belongs to, e.g.:
///     "buffer:doc"       — Buffer ops for a named document
///     "idlist:structure" — IdList structural ops
///
///   The `producer_replica_id` in the inbound callback identifies the replica
///   that pushed the payload.  The `payload` is raw encoded op bytes (UTF-8
///   JSON from encode_operation / encode_idlist_operation).
///
/// ─── Method contract (mirrors Markoff's ITransport four-method form) ─────────
///
///   push()                   — send an op onto a named stream
///   set_on_inbound()         — register a callback for received ops
///   lowest_peer_acked_lamport() — cached GC-fence value; never does I/O
///   set_on_ack_update()      — register a callback for fence advances
///
/// ─── Usage ───────────────────────────────────────────────────────────────────
///
///   #include <collabtext/OpStream.h>
///
///   class MyTransport : public CollabText::OpStream {
///   public:
///       void push(const std::string& stream_name,
///                 const std::string& payload) override { /* … */ }
///       void set_on_inbound(
///           std::function<void(const std::string&, uint16_t, const std::string&)> cb)
///           override { m_inbound = std::move(cb); }
///       uint64_t lowest_peer_acked_lamport() const override { return m_fence; }
///       void set_on_ack_update(std::function<void(uint64_t)> cb) override
///           { m_ack_cb = std::move(cb); }
///   private:
///       std::function<void(const std::string&, uint16_t, const std::string&)> m_inbound;
///       std::function<void(uint64_t)> m_ack_cb;
///       uint64_t m_fence = 0;
///   };
///
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace CollabText {

/// Transport-agnostic op delivery boundary.
///
/// Implementations deliver serialised CRDT operations between replicas.
/// `StreamSync` (the file-backed reference implementation) implements this
/// interface.  Consumers that need direct-channel transports (TCP, WebRTC)
/// provide their own implementations.
///
/// See ordering contract in the file-level comment above.
class OpStream {
public:
    virtual ~OpStream() = default;

    /// Push serialised op bytes onto a named stream.
    ///
    /// \param stream_name   Identifies the CRDT stream (e.g. "buffer:doc").
    /// \param payload       Encoded op bytes (UTF-8 JSON).
    virtual void push(const std::string& stream_name,
                      const std::string& payload) = 0;

    /// Register a callback fired for each inbound op on a stream.
    ///
    /// Callback parameters:
    ///   stream_name        — which stream the op arrived on
    ///   producer_replica_id — the replica that pushed this payload
    ///   payload            — encoded op bytes (UTF-8 JSON)
    ///
    /// Only one callback is active at a time; calling this again replaces
    /// the previous registration.
    virtual void set_on_inbound(
        std::function<void(const std::string& /*stream_name*/,
                           uint16_t          /*producer_replica_id*/,
                           const std::string& /*payload*/)> cb) = 0;

    /// Return the lowest Lamport counter all enrolled peers have acked from us.
    ///
    /// This value is the GC fence: ops with Lamport counter ≤ this value are
    /// safe to collect.  The value is cached from the last poll cycle and
    /// never performs I/O.  Returns 0 if no peers are enrolled.
    virtual uint64_t lowest_peer_acked_lamport() const = 0;

    /// Register a callback fired when lowest_peer_acked_lamport() advances.
    ///
    /// Only one callback is active at a time; calling this again replaces
    /// the previous registration.
    virtual void set_on_ack_update(std::function<void(uint64_t /*new_fence*/)> cb) = 0;
};

} // namespace CollabText
