# Direct Channel Interface — Design Spec

**Date:** 2026-04-05
**Status:** Design
**Dependencies:** Serialization (message framing)

---

## 1. Motivation

The sync spec (§8) describes direct channels as optional low-latency
accelerators that can use TCP, WebSocket, XMPP, or any bidirectional
transport. The SyncManager needs a single interface to drive all of them
— connect, exchange messages, detect failure — without knowing which
transport is underneath.

This interface must be simple enough that adding a new transport
(Telegram bot API, Nostr relay, Yggdrasil, a custom UDP protocol) is a
weekend project for someone who knows that transport and nothing about
CRDTs.

---

## 2. Interface

```cpp
namespace CollabText::Transport {

/// A framed message. The channel moves these opaquely.
/// The SyncManager serializes/deserializes the content.
struct Message {
    std::vector<uint8_t> data;
};

/// Connection metadata provided by the SyncManager when requesting
/// a connection, and by the channel when accepting an incoming one.
struct PeerInfo {
    std::string replica_id;     // The remote replica we're connecting to
    std::string document_id;    // The document this channel is for
    std::string offer;          // Transport-specific address/token
                                // (e.g., "ws://192.168.1.5:9741",
                                //  "xmpp:alice@jabber.org/collabtext",
                                //  "tg:@collabtext_relay_bot")
};

/// Callback interface for channel events. The SyncManager implements this.
class ChannelListener {
public:
    virtual ~ChannelListener() = default;

    /// A message arrived from the peer.
    virtual void on_message(const Message& msg) = 0;

    /// The channel is now connected and ready for send().
    virtual void on_connected(const PeerInfo& peer) = 0;

    /// The channel has failed or been closed. Reason is human-readable.
    /// After this call, the channel object should be discarded.
    virtual void on_disconnected(const std::string& reason) = 0;
};

/// A single bidirectional channel to one peer for one document.
///
/// Lifecycle:
///   1. SyncManager creates the channel via a ChannelFactory.
///   2. Channel connects asynchronously. Calls on_connected() when ready.
///   3. SyncManager calls send(). Channel calls on_message() for incoming.
///   4. On failure, channel calls on_disconnected(). SyncManager discards it.
///
/// Threading: The channel owns its I/O. It may use a thread, an event
/// loop, or poll — the SyncManager doesn't care. Callbacks (on_message,
/// on_connected, on_disconnected) must be safe to call from any thread;
/// the SyncManager will handle synchronization internally.
class Channel {
public:
    virtual ~Channel() = default;

    /// Send a framed message to the peer. Returns false if the channel
    /// is not connected or the send buffer is full (backpressure).
    /// The SyncManager will retry or fall back to the file floor.
    virtual bool send(const Message& msg) = 0;

    /// Request a graceful close. The channel should finish sending
    /// queued messages, then call on_disconnected("closed").
    virtual void close() = 0;
};

/// Factory for creating channels of a specific transport type.
///
/// Each transport implementation provides one of these. The SyncManager
/// holds a list of factories (in priority order from the config) and
/// tries them in sequence when connecting to a peer.
///
/// A factory also handles incoming connections: when a remote peer
/// initiates a connection (e.g., an incoming WebSocket), the transport
/// runtime calls accept() to create the channel and hand it to the
/// SyncManager.
class ChannelFactory {
public:
    virtual ~ChannelFactory() = default;

    /// Human-readable transport name (e.g., "websocket", "xmpp", "tcp").
    /// Used in presence.json offers and log messages.
    virtual std::string transport_name() const = 0;

    /// Priority (lower = preferred). Used to sort factories when
    /// multiple transports are available. Typical values:
    ///   10  LAN TCP (fastest, most reliable)
    ///   20  LAN WebSocket
    ///   50  WAN WebSocket (through relay or direct)
    ///   80  XMPP (higher latency, but firewall-friendly)
    ///   90  Telegram/Nostr relay (highest latency, most available)
    virtual int priority() const = 0;

    /// Generate a connection offer for inclusion in presence.json.
    /// Returns a transport-specific address string that a remote peer
    /// can use to connect to us. Returns empty string if this transport
    /// cannot accept incoming connections (client-only).
    ///
    /// Examples:
    ///   "ws://192.168.1.5:9741/collabtext/doc-uuid"
    ///   "xmpp:clinton@jabber.org/collabtext"
    ///   "tcp://[fe80::1%eth0]:9741"
    virtual std::string make_offer() = 0;

    /// Attempt to connect to a peer using their offer string.
    /// Returns a Channel that will call on_connected() asynchronously
    /// if successful, or on_disconnected() if the connection fails.
    ///
    /// The timeout is a hint. The channel should give up and call
    /// on_disconnected() after approximately this duration.
    virtual std::unique_ptr<Channel> connect(
        const PeerInfo& peer,
        ChannelListener* listener,
        std::chrono::milliseconds timeout = std::chrono::seconds(3)
    ) = 0;

    /// Called by the transport runtime when an incoming connection
    /// arrives. The factory wraps it in a Channel and returns it.
    /// The SyncManager will verify the peer's document_id and
    /// replica_id via the handshake protocol.
    ///
    /// Not all transports support incoming connections. Return nullptr
    /// if this factory is connect-only.
    virtual std::unique_ptr<Channel> accept(
        ChannelListener* listener
    ) { return nullptr; }

    /// Start listening for incoming connections (if supported).
    /// Called once at SyncManager startup.
    virtual void start_listening() {}

    /// Stop listening and close all resources.
    virtual void stop() {}
};

}  // namespace CollabText::Transport
```

---

## 3. How the SyncManager Uses It

### Startup

```cpp
// SyncManager holds factories in priority order
std::vector<std::unique_ptr<ChannelFactory>> m_factories;

// Register transports (order = priority)
m_factories.push_back(std::make_unique<TcpChannelFactory>(port));
m_factories.push_back(std::make_unique<WebSocketChannelFactory>(port));
// m_factories.push_back(std::make_unique<XmppChannelFactory>(jid, password));

// Start listeners
for (auto& f : m_factories) f->start_listening();

// Write offers to presence.json
for (auto& f : m_factories) {
    auto offer = f->make_offer();
    if (!offer.empty())
        presence["offers"].push_back({f->transport_name(), offer});
}
```

### Connecting to a peer

When a remote replica's `presence.json` appears with offers:

```cpp
for (auto& factory : m_factories) {
    for (auto& offer : peer_offers) {
        if (offer.transport == factory->transport_name()) {
            auto channel = factory->connect(
                PeerInfo{peer_replica_id, document_id, offer.address},
                this,  // SyncManager is the ChannelListener
                3s);
            if (channel) {
                m_pending_channels[peer_replica_id] = std::move(channel);
                return;  // Try one at a time, best priority first
            }
        }
    }
}
```

### Message flow

Once connected, the SyncManager handles the handshake and streaming:

```cpp
void SyncManager::on_connected(const PeerInfo& peer) {
    // Send handshake (vector clock, capabilities)
    auto handshake = serialize_handshake(m_engine.version());
    m_channels[peer.replica_id]->send({handshake});
}

void SyncManager::on_message(const Message& msg) {
    auto parsed = deserialize_message(msg.data);
    switch (parsed.type) {
        case MsgType::Handshake:
            // Compare vector clocks, send catch-up
            send_catchup(parsed.vector_clock);
            break;
        case MsgType::Ops:
            // Feed to engine (deduplication is engine's job)
            m_engine.apply_ops(parsed.operations);
            // Also write to file floor (dual-write guarantee)
            flush_to_files(parsed.operations);
            break;
        case MsgType::Ephemeral:
            // Update remote cursor state
            update_ephemeral(parsed.replica_id, parsed.state);
            break;
        case MsgType::Ack:
            // Update GC watermark knowledge
            update_peer_version(parsed.replica_id, parsed.vector_clock);
            break;
        // ...
    }
}

void SyncManager::on_disconnected(const std::string& reason) {
    // Discard channel. File floor continues. Retry on next sync cycle.
    m_channels.erase(peer_replica_id);
}
```

---

## 4. What a Transport Implementer Provides

To add a new transport, implement `ChannelFactory` and `Channel`. That's
it. The implementer does NOT need to know about:

- CRDT operations or convergence
- Vector clocks or causal ordering
- Document structure or serialization format
- Presence, ephemeral state, or side streams
- The file floor or SQLite

They need to know:
- How to open a bidirectional byte-stream connection using their transport
- How to frame messages (length-prefixed or transport-native framing)
- How to detect connection failure

### Example: Minimal WebSocket transport

```cpp
class WebSocketChannel : public Channel {
    websocket_connection m_ws;
    ChannelListener* m_listener;
public:
    WebSocketChannel(websocket_connection ws, ChannelListener* l)
        : m_ws(std::move(ws)), m_listener(l)
    {
        m_ws.on_message([this](auto data) {
            m_listener->on_message({data.begin(), data.end()});
        });
        m_ws.on_close([this](auto reason) {
            m_listener->on_disconnected(reason);
        });
    }

    bool send(const Message& msg) override {
        return m_ws.send(msg.data);
    }

    void close() override { m_ws.close(); }
};

class WebSocketChannelFactory : public ChannelFactory {
    uint16_t m_port;
    ws_server m_server;
public:
    std::string transport_name() const override { return "websocket"; }
    int priority() const override { return 20; }

    std::string make_offer() override {
        return "ws://" + local_ip() + ":" + std::to_string(m_port);
    }

    std::unique_ptr<Channel> connect(
        const PeerInfo& peer, ChannelListener* l,
        std::chrono::milliseconds timeout) override
    {
        auto ws = ws_connect(peer.offer, timeout);
        if (!ws) return nullptr;
        l->on_connected(peer);
        return std::make_unique<WebSocketChannel>(std::move(ws), l);
    }

    void start_listening() override {
        m_server.on_accept([this](auto ws, auto listener) {
            // SyncManager registers as listener for incoming channels
            return std::make_unique<WebSocketChannel>(std::move(ws), listener);
        });
        m_server.listen(m_port);
    }

    void stop() override { m_server.close(); }
};
```

### Example: Telegram relay transport

```cpp
class TelegramChannelFactory : public ChannelFactory {
    std::string m_bot_token;
    std::string m_chat_id;
public:
    std::string transport_name() const override { return "telegram"; }
    int priority() const override { return 90; }  // Last resort

    std::string make_offer() override {
        return "tg:" + m_chat_id;  // Peer needs to be in the same group
    }

    std::unique_ptr<Channel> connect(
        const PeerInfo& peer, ChannelListener* l,
        std::chrono::milliseconds timeout) override
    {
        // "Connecting" to a Telegram relay just means polling the chat
        // for messages tagged with our document_id
        auto ch = std::make_unique<TelegramChannel>(
            m_bot_token, peer.offer, peer.document_id, l);
        ch->start_polling();
        return ch;
    }
    // No incoming — polling handles both directions
};
```

---

## 5. Design Decisions

**Why byte buffers, not typed messages?**
Because the transport layer shouldn't depend on the serialization format.
If we change the message format (add compression, switch to protobuf,
etc.), no transport implementation needs to change. The SyncManager owns
serialization; transports own delivery.

**Why callbacks instead of async/await or futures?**
Because C++ has no standard async runtime. Callbacks work with any I/O
model: threads, event loops, io_uring, Qt's event loop, libuv, Boost.Asio.
A transport implementer uses whatever async model their library provides
and calls the callbacks when events occur.

**Why per-document channels, not multiplexed?**
Simplicity. Each document gets independent channels. A failure in one
document's channel doesn't affect another. Multiplexing is an
optimization that can be layered on top (a multiplexing ChannelFactory
that creates virtual channels over a shared connection) without changing
the interface.

**Why is priority on the factory, not the offer?**
Because priority reflects the transport's characteristics (LAN TCP is
always faster than Telegram relay), not the specific peer. Per-peer
priority adjustment (e.g., "this peer is on my LAN, prefer TCP") can be
handled by the SyncManager when selecting which factory to try, using
network heuristics outside the transport interface.

---

## 6. Relationship to presence.json

The `presence.json` file (CRDT_SYNC_SPEC §7) carries connection offers:

```json
{
  "replica_id": "laptop-3a",
  "heartbeat": "2026-04-05T14:30:00Z",
  "offers": [
    {"transport": "tcp", "address": "tcp://192.168.1.5:9741"},
    {"transport": "websocket", "address": "ws://192.168.1.5:9742"},
    {"transport": "xmpp", "address": "xmpp:clinton@jabber.org/collabtext"}
  ]
}
```

Each offer's `transport` field matches a `ChannelFactory::transport_name()`.
The `address` field is the `offer` string passed to `ChannelFactory::connect()`.
The SyncManager generates offers by calling `make_offer()` on each
registered factory, and parses remote offers by matching transport names
to local factories.

---

## 7. What This Interface Does NOT Cover

- **Encryption.** Transport-level encryption (TLS for WebSocket, OMEMO
  for XMPP) is the transport implementation's responsibility. The
  interface moves plaintext byte buffers. If the transport is encrypted,
  the channel handles that transparently.

- **Authentication.** The CRDT handshake (§8.3 in sync spec) verifies
  document_id and exchanges vector clocks. Transport-level auth (API
  keys, certificates) is the transport's concern.

- **NAT traversal.** Transports that need hole-punching, STUN/TURN, or
  relay discovery handle that internally. The SyncManager just calls
  `connect()` with the offer string.

- **Message ordering.** The CRDT engine handles out-of-order delivery.
  Transports do NOT need to guarantee ordering. If a transport delivers
  messages out of order, the engine's causal queue handles it.

- **Reliability.** Transports do NOT need to guarantee delivery. The
  file floor ensures every operation eventually arrives. A transport
  that drops messages is merely slower, not incorrect.
