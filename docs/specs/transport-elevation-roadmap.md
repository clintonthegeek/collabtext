# Transport Elevation Roadmap

**Date:** 2026-05-01
**Status:** Design / planning
**Dependencies:** `direct-channel-interface-design.md`, `CRDT_SYNC_SPEC.md` §7–§8

---

## 1. Purpose

The sync spec establishes that Syncthing is the floor and that direct
channels are opportunistic accelerators negotiated through `presence.json`.
The channel-factory interface exists. What is *not* on the record is which
transports to actually build, in what order, and why — given the network
topologies, OS mix, and operational realities a Syncthing-using audience
will throw at us.

This document is the honest accounting. It is opinionated. It exists so
the next person to ask "why aren't we using WebRTC / mDNS / XMPP yet?"
gets an answer instead of a debate.

---

## 2. The Deployments We Will Actually See

Ranked by frequency among the realistic user base (people who already
run Syncthing or would consider it):

1. **Same user, multiple devices, same LAN.** The README's headline
   case: laptop + desktop at home. Both behind one NAT, same broadcast
   domain. Direct connection almost always works.
2. **Same user, multiple devices, different networks.** Laptop on
   coffee-shop wifi, desktop at home. Two separate NATs. No direct path
   without hole-punching or an overlay.
3. **Small team on a shared overlay** (Tailscale, ZeroTier, Yggdrasil,
   WireGuard mesh, or Syncthing's own relay+discovery). The peer
   *looks* LAN-ish from the app's perspective: a routable IP, no NAT in
   between. Increasingly common in this audience.
4. **Small team, no overlay, mixed home networks.** Two consumer NATs,
   sometimes one CGNAT (mobile tether, T-Mobile home internet, many
   EU ISPs). Direct connection is a coin flip and getting worse every
   year as CGNAT spreads.
5. **Captive portals, hotel networks, corporate egress filtering.** UDP
   often blocked; only TCP/443 outbound is reliable.
6. **Air-gapped / USB sneakernet / NAS-only.** The file floor is the
   only path. No elevation possible. By design, this still works.

OS mix matters less than network mix because the channel interface is
already byte-stream-shaped. Two OS-flavored gotchas to plan around:

- **Firewalls.** Windows prompts on first `listen()`. macOS prompts and
  remembers per-binary-signature, so unsigned dev builds re-prompt.
  Linux mostly fine. mDNS/Bonjour behavior varies wildly across all
  three.
- **Mobile background lifecycle.** iOS aggressively kills long-lived
  sockets; Android less so but still. Any future mobile build cannot
  assume always-on TCP and will need push-driven wake-up.

---

## 3. What "Elevate" Can Actually Mean

| Tier | Transport | Covers cases | Honest cost |
|------|-----------|--------------|-------------|
| 0 | Plain TCP / WS on LAN, IP from `presence.json` | 1, 3 | 1–2 weeks. No new deps beyond Qt's network stack. |
| 1 | + mDNS/Bonjour discovery | 1 with DHCP churn | small, but Avahi/Bonjour/Windows-mDNS friction; not strictly needed if `presence.json` carries IP |
| 2 | WebRTC DataChannel (ICE + STUN, optional TURN) | 2, 4, 5 | large. Pulls libdatachannel or libwebrtc. ICE signalling rides over `presence.json` — that part is elegant. Inherits a giant attack surface and a TURN operational story. |
| 3 | WSS to a relay we operate | everything except 6 | medium effort, *forever* infra cost — we become the operator |
| 4 | XMPP / Matrix / Nostr piggyback | 2–5, no infra of our own | medium effort, latency floor in the hundreds of ms, depends on third-party uptime |
| 5 | Hijack the existing overlay (Tailscale, Yggdrasil, etc.) | 3 | zero. Tier 0 already gets it for free. |

The thing worth saying out loud: in 2026 the realistic non-overlay WAN
P2P answer is **WebRTC or nothing**. Hand-rolled UDP hole punching in
C++ across Linux/macOS/Windows/Android is a multi-month project with a
long bug tail. WebRTC bundles ICE + DTLS + SCTP and has been beaten on
by browsers for a decade. iroh / libp2p / pion exist but bring Rust/Go
runtimes; for a Qt6 C++ app, **libdatachannel** is the least-bad fit.

---

## 4. Build Order

Each step ships value on its own and validates the next.

### Step 1 — TCP factory, LAN-only, IP via `presence.json`

Implements `ChannelFactory` verbatim. Each replica advertises its
local IPs (filtered, see §5) and a port; peers try direct TCP. Covers
case 1 (the README's main story) and case 3 (overlay users — Tailscale
users get this for free). Validates the elevation/collapse machinery
against a real socket. No NAT logic, no infra, no third party. This
is the 80/20.

**Definition of done:** two replicas in the same LAN converge over TCP
within ~100ms of an edit; killing the TCP path silently falls back to
file floor without losing data.

### Step 2 — Robustness for step 1

Where step 1 turns from demo to product. Easy to underestimate:

- Multi-interface enumeration. Don't advertise `192.168.122.1/libvirt`,
  `172.17.0.1/docker`, link-local, or VPN tunnels the peer can't reach.
- IPv6, including link-local with zone IDs.
- Firewall UX on Windows/macOS (signed binaries, deterministic port).
- Per-peer reachability cache (see §6).
- Retry/backoff on `ECONNREFUSED` and `ETIMEDOUT` so we don't hammer
  unreachable offers on every sync cycle.

### Step 3 — Decide the WAN story explicitly

Two real options. Pick one before building:

- **3a. WebRTC DataChannel via libdatachannel.** Use `presence.json`
  as the signalling channel: ICE candidates and SDP exchanged via the
  file floor. This is a *good* fit for our architecture — the sync
  floor bootstraps the fast path, no signalling server required. Covers
  cases 2, 4, 5. Brings a large dependency and a TURN-server question
  for the cases where ICE fails (≈10–20% of consumer NAT pairs without
  CGNAT, much higher with).
- **3b. Skip P2P, ship a relay.** Smaller code surface; we own infra
  forever and lose the "no server" story. Probably not what we want,
  but it's the honest alternative and we should reject it on purpose,
  not by drift.

Recommendation: **3a**. Defer the TURN decision until we measure ICE
success rate in the wild — many users on overlays won't need it.

### Step 4 — XMPP or similar, low priority

A no-infra, no-WebRTC fallback for users who want it. Easy to add later
because the factory abstraction is already there. Don't block on this.

### Explicitly NOT first

- **mDNS/Bonjour.** `presence.json` already carries the IP. mDNS is a
  nicety that costs cross-platform pain. Reconsider after step 2.
- **Telegram, Nostr, or other novelty transports.** Cute, low ROI.
- **QUIC.** Not portable enough yet without pulling msquic or quiche;
  WebRTC's SCTP-over-DTLS already gives us message-oriented over UDP.
- **Custom UDP hole-punching.** See §3.

---

## 5. Multi-Interface Hygiene

Step 1 will fail in confusing ways if we naively advertise every IP
`getifaddrs()` returns. Concrete rules for `make_offer()`:

- Skip loopback (`127.0.0.0/8`, `::1`).
- Skip well-known virtual ranges by default: `172.17.0.0/16` (docker
  default), `192.168.122.0/24` (libvirt default), `10.0.x.x` ranges
  belonging to common VPN clients when detectable.
- Skip IPv6 link-local (`fe80::/10`) unless there's no other option;
  if advertised, include the zone ID and accept that cross-host use is
  fragile.
- Always advertise overlay-assigned addresses (Tailscale `100.64/10`
  CGNAT range, ZeroTier, Yggdrasil `200::/7`) — these are the whole
  point of case 3.
- Allow user override via config: explicit allow-list and deny-list of
  interface names or CIDRs.

---

## 6. Per-Peer Reachability — the Spec Gap

The factory list is global and priority-ordered. Reachability is
**per-peer and dynamic**: a TCP offer on `192.168.1.5` is tier-0 for
the peer on the same LAN and unreachable for the peer on hotel wifi.
Today the spec hand-waves this as "network heuristics outside the
transport interface." That gap will bite during step 1 and become
painful by step 3.

Design this into the SyncManager *before* step 1 lands so step 3 doesn't
require restructuring. Minimal shape:

```cpp
struct PeerReachability {
    std::string transport_name;
    std::string offer;
    enum Status { Unknown, Reachable, Unreachable } status;
    std::chrono::steady_clock::time_point last_attempt;
    std::chrono::milliseconds last_rtt;   // when Reachable
    std::string last_failure;             // when Unreachable
};
```

Rules:

- On successful connect, mark `Reachable` with measured RTT. Prefer
  this offer for this peer on next elevation, regardless of global
  factory priority.
- On failure, mark `Unreachable` with reason and timestamp. Don't retry
  this specific offer for `min(60s * 2^consecutive_failures, 1h)`.
- Reset on a fresh `presence.json` from the peer (their network may
  have changed) or on local network change events.
- Cache lives in `~/.config/collabtext/` — never synced, per-machine.

This turns the global factory priority into a *default ordering*
overridden by observed reality, which is what we actually want.

---

## 7. Out of Scope for This Roadmap

- **Encryption** at the transport layer (TLS, DTLS, OMEMO) — handled
  by each transport implementation per the channel interface spec §7.
- **Authentication** — the CRDT handshake covers document/replica
  verification. Pairing UX (how two strangers establish trust) is a
  separate document.
- **Mobile platforms** — out of scope until desktop story is solid.
  When it lands, the lifecycle constraints from §2 will force a
  separate transport (likely a relay with push wake-up), not an
  adaptation of these.

---

## 8. Success Criteria

By end of step 2, `collabtext` should deliver sub-second convergence
on:

- Two devices, same home LAN.
- Two devices, both on Tailscale (or any WireGuard-shaped overlay).
- Two devices in a corporate office on the same VLAN.

…and degrade silently to file-floor latency in every other case
without data loss. That is the bar before we touch WebRTC.
