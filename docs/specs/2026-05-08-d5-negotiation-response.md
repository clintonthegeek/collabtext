# Reply: D5 Negotiation Opener — collabtext maintainer evaluation

**Date:** 2026-05-08
**Re:** Markoff `docs/handoff/2026-05-07-collabtext-d5-negotiation-opener.md`
**Companion:** Markoff `docs/specs/2026-05-07-d5-collab-activation-design.md`
**Predecessor:** `docs/specs/2026-05-04-d-evolution-response.md` (the IdList scope-line)
**Status:** Maintainer response; commitment to Alternative A with explicit scope and timeline.

Thanks for the framing. The three-layer model is sharper than ours was, and reading the D5 spec end-to-end made the ask easier to size than the opener alone suggested. Your `ITransport` shape (D5 §4.1) gave us four concrete methods to target instead of an abstract surface to invent.

Our answer is **yes to Alternative A.** Below is what we'll commit to, what we won't, and when. The 2026-05-04 six-item scope-line stays binding; this is reshaping how ops leave the engine, not adding to what the engine does.

## What we'll do

A transport-agnostic CRDT op-stream boundary on the public collabtext surface, with `StreamSync` adopting that boundary as the file-backed reference implementation. The four-method shape from your D5 §4.1 is what we target on our side — same methods, same semantics — so your consumer's `ITransport` adapter is a thin wrap rather than a translation layer.

Concretely:

1. **Public op API on `CrdtEngine` and `IdList`.** `setOnLocalOp(callback<Operation>)` and `applyRemoteOp(Operation)` on each, surfaced through the public engine pImpl. The methods already exist inside `Buffer`; this is plumbing them through the public engine surface and adding the analogues to `IdList`.

2. **`Serialization.h` promoted to public include path** as `include/collabtext/Serialization.h`. `encode_operation` / `decode_operation` / `encode_idlist_operation` / `decode_idlist_operation` become public functions. Their argument types — `Operation`, `IdListOperation`, and the small support types they reference (`Lamport`, `Anchor`, `Global`, `Fragment`) — get publicly-visible declarations. We will mark fields as part of the contract or "subject to evolution" deliberately, not by accident.

3. **`OpStream` interface** in `include/collabtext/OpStream.h`. Four methods matching your `ITransport`:

   ```cpp
   class OpStream {
   public:
       virtual ~OpStream() = default;
       virtual void push(const std::string& stream, std::string bytes) = 0;
       using OnInbound = std::function<void(const std::string& stream,
                                            uint16_t producer_replica,
                                            std::string bytes)>;
       virtual void set_on_inbound(OnInbound cb) = 0;
       virtual uint64_t lowest_peer_acked_lamport() const = 0;
       virtual void set_on_ack_update(std::function<void(uint64_t)> cb) = 0;
   };
   ```

   Method names are negotiable in the joint-design pass below.

4. **`StreamSync` adopts `OpStream`.** Promoted from `src/crdt/StreamSync.h` to `include/collabtext/StreamSync.h`. Public name stays `StreamSync` — your wiring sketch already references it; the rename to `FileBackedOpStream` is cosmetic and adds churn for no consumer benefit. The class implements `OpStream`; existing methods stay; per-stream callback signature gains the per-entry / producer-aware overload required for `OpStream` compliance.

5. **Per-peer ack-frontier publication.** Each replica publishes its per-peer-source observations on every sync cycle (file format design call during implementation; either an extension to presence or a sibling file). `StreamSync::lowest_peer_acked_lamport()` aggregates over the enrolled peer set; `set_on_ack_update` fires on advance. We already track per-peer read cursors locally — this is publishing them outward.

6. **Convergence and ack-frontier test coverage.** Two-replica convergence over `StreamSync`-as-`OpStream` (no `SyncManager` on the path), ack-frontier correctness under partition / reconnect / silent-peer scenarios, idempotent re-delivery, catch-up replay. Roughly a third of the work, same proportion as IdList β.

## What we won't do, and want to be explicit about

These aren't deferrals — they're the line.

1. **No `MemoryOpStream` shipped from collabtext.** Your testapp's in-memory `ITransport` mock (D5 §4.4) is exactly the right shape and you're already writing it. We use `NetworkSim` for our own internal convergence tests at the algorithmic layer. We don't need two implementations of the same idea; you don't need ours.

2. **No `SyncManager` split.** Your consumer goes around `SyncManager` via `ITransport`-on-`OpStream`. `Identity::*` is opt-in by virtue of not being on your runtime path. `SyncManager` continues to exist as a convenience facade for consumers that want all-in-one wiring (presence + identity + ops + Syncthing-floor); it's no longer the only path. We'll document this explicitly. If we later split `SyncManager` for our own quality reasons, that's a separate decision and not on this work's critical path.

3. **No deep `Operation` ABI stability.** The types are public — names, encoders, decoders, sufficient struct definitions to round-trip — but field-level evolution is reserved with conventional deprecation and additive `schema_version` bumps. Consumers should round-trip via `encode`/`decode`, not depend on internal struct layout.

4. **No `app/` migration in this window.** `app/collabedit/` and `app/testapp/` currently use `StreamSync` directly for chat/comments. They're paused per our project posture and will stay paused. They'll continue compiling against the public interface; deeper integration with the new `OpStream` shape happens when widget lab resumes. This does not affect what your consumer sees.

5. **No transport implementations beyond the file-backed reference.** Direct-channel work (TCP, WebRTC) per `transport-elevation-roadmap.md` lands as additional `OpStream` implementations on a separate track. Not part of this commitment.

6. **No re-litigation of the six-item D-evolution scope-line.** All six items still hold. This refactor reshapes the boundary of what already exists; it does not add CRDT primitives, generalise `CollabDocument`, or add cross-CRDT coordination. Your §4 of the opener acknowledged this; we're saying it back to be sure.

## On your specific asks

- **Per-peer ack-frontier reporting.** Yes, the four-method shape from D5 §4.1. Single scalar (`lowest_peer_acked_lamport`) plus an advance callback. Aggregation across enrolled peer set is consumer-side, as your spec already places it.

- **Catch-up replay.** Already supported by `StreamSync`'s segment readers. The `OpStream` contract documents that an implementation MAY deliver historical entries on first-attach; the file-backed reference does. Your testapp's mock decides its own semantics.

- **`StreamSync` rename.** Declined. Not because we disagree on the spirit, but because the churn isn't worth it — your wiring sketch references `StreamSync` by name, existing tests reference it, documentation references it. The boundary shape is what matters; the name is incidental.

- **Markoff testapp as demo consumer.** Yes please. We'd be happy to point at your testapp from our README/docs when it lands. Our `CollabPlainTextEdit` stays as the smallest-possible single-`Buffer` example; your testapp becomes the realistic multi-CRDT dogfood. Coordination on timing follows when you're closer to landing.

- **Joint-design pass.** Yes. Your eyes on the public `OpStream` header before it sets, and on the per-peer ack-frontier file format before it ships. Same shape as the IdList header pass we offered in 2026-05-04 — a week of back-and-forth on the headers, no more.

## Timing

Realistically: **~4 weeks of focused work**, sequenced **after IdList β**. β is mid-implementation; sequencing this in front would fragment both. We expect IdList β to land in 4–6 weeks at current pace; this work follows immediately. Target landing of the `OpStream` boundary roughly **8–10 weeks from now**, give or take.

If you want to start exercising it sooner against a partial deliverable, we can land items 1–2 above (public op API + public serialization) early — those are useful in isolation, take ~2 weeks, and don't gate on the full `OpStream` interface. Your D5 implementation could begin against the partial deliverable. Tell us if that helps; otherwise we'll land the whole thing as one piece.

You said you don't gate on us. Taking that seriously: if D5 ships against current shape and we land the refactor afterward, the consumer wiring simplifies but Markoff itself doesn't change. We'll preserve enough compatibility that the swap is local to the consumer code.

## One thing we want from you

A short note in the Markoff D5 spec — when the refactor lands — explicitly recording that the `OpStream` boundary is what you wire against, and that earlier shape-independent fallback paths are no longer the working assumption. Same purpose as the line we asked for on IdList: preserves the scoping decision against future drift on both sides.

## Closing

Your framing is sharper than ours was. The three-layer model is the right model, your `ITransport` shape is the right shape, and the boundary you're asking for is one we'd want for our own roadmap (direct-channel + WebRTC work lands cleaner with `OpStream` as a sibling abstraction). We're in for the full Alternative A.

---

*— collabtext maintainers*

---

**Status update — 2026-05-08:** Commitment fulfilled. Tagged `opstream-v1` on commit `1a1a894`. All deliverables landed: public `OpStream` interface, `StreamSync` as the file-backed reference impl, per-peer ack-frontier, full end-to-end convergence test coverage. Markoff acceptance recorded in `~/dev/Markoff/.worktrees/foundation-exploration/docs/handoff/2026-05-08-collabtext-d5-acceptance.md`. One post-acceptance cleanup (Lamport::replica_id contract clarification) landed in the same tag.
