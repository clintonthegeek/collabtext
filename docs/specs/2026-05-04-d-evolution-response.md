# Reply: D-Evolution Proposal — collabtext maintainer evaluation

**Date:** 2026-05-04
**Re:** Markoff `docs/specs/2026-05-02-d-evolution-proposal.md`
**Status:** Maintainer response; commitment to Option β with explicit scope line.

Thanks for the care that went into this. The proposal is well-scoped, the option analysis is honest, and the asks are framed in a way that makes "no" easy where it needs to be. We've read it in full.

Our answer is **yes to β, with a tightly drawn line.** Below is what we'll commit to and — equally importantly — what we won't, so both projects can plan against the same picture.

## What we'll do

A single new primitive `CollabText::Crdt::IdList`: a CRDT-shaped ordered list of opaque `uint64` elements, sharing collabtext's existing op-causality model, anchor model, undo machinery, and GC primitives. It will live alongside `Buffer`, not replace or subsume it. Concretely:

- **Element type: strictly opaque `uint64`.** No templated value, no per-element kind tag, no attribute bag. Application owns everything else.
- **Anchor type: the existing `Crdt::Anchor`,** not a new `ListAnchor`. The `(replica_id, char_value, bias)` triple already identifies a Lamport-stamped element; the type works for opaque list elements unchanged.
- **Op type: a separate `IdListOperation` variant,** not folded into `Operation`. Wire schema bumps additively (the same shape as the recent `schema_version 1 → 2` bump for segmented sync). Existing files and existing `Buffer`-only consumers are forward-compatible without changes.
- **API surface:** `insertAfter(anchor, id)`, `removeAt(anchor)`, `ids()`, `anchorOf(id, bias)`, `applyRemote(op)`, `setOnChange(cb)`, `undo()/redo()`, `collect_garbage()`, `compact(watermark)`. Mirroring `Buffer` where the analogue is exact.
- **Convergence/fuzz coverage from day one,** parallel to `tst_fuzz.cpp` / `tst_convergence.cpp` / `tst_realistic.cpp`. We treat this as roughly a third of the total effort.

Realistically: 4–8 weeks of focused work, not 8–16. The "easier than Buffer" observation is correct — no UTF-8 splitting, no multi-char fragments, no `split_relocations`, no `edits_since` reconciliation. Most of the supporting machinery (`SumTree`, `Locator`, `Anchor`, `Clock`, `UndoMap`, `OperationQueue`, `StreamSync`) is already element-type-agnostic and reusable verbatim.

## What we won't do, and want to be explicit about now

These aren't deferred decisions — these are the line we're drawing as a condition of saying yes to β.

1. **No `moveAfter` in v1.** Concurrent move semantics are genuinely subtle and we don't want to litigate them under deadline pressure. Express moves as remove + insert, accept the "loses intent" cost, and revisit only if a real Markoff feature concretely needs structural-move CRDT semantics. Our suspicion is it never will.

2. **No per-element values, ever.** Not in v1, not in v2. The moment `IdList` carries application data with its own merge rules, we've started building the framework the README disclaims. Sibling maps live in `Markoff::DocumentStructure`. If you later want a `Crdt::Map` primitive, that is a separate proposal with a separate scoping conversation, not a follow-on tweak to `IdList`.

3. **No cross-CRDT undo log primitive (your Q2 (b)/(c)).** Your tentative preference (a) is correct. The application owns the cross-CRDT edit log. Anything else turns collabtext into a transaction manager.

4. **No cross-CRDT GC coordination primitive (your Q3).** Each `IdList` and each `Buffer` exposes `compact(watermark)`. Coordinating watermarks across the document is `DocumentStructure`'s job. We will not invent a "document watermark" abstraction inside collabtext.

5. **No `CollabDocument` generalization.** `CollabDocument` stays Buffer-bound. Markoff composes `DocumentStructure` directly on top of public `IdList` + `Buffer` + the existing `StreamSync` (which is already multi-stream and won't need changes — register one stream for structure plus one per block, payloads are already opaque to the transport).

6. **No precedent for further primitives.** We want to flag this explicitly. `IdList` is defensible to us because it's the same algorithm as `Buffer` with a different element type — list CRDT over `uint64` instead of list CRDT over UTF-16 code units. It is *not* a precedent for `Map`, `Counter`, `Tree`, or nested compositions. If Markoff's roadmap eventually wants those, the answer will likely be "build it outside collabtext or fork." We'd rather say that now than discover it under feature pressure later.

## Answers to the open questions

- **Q1 (element value richness):** strictly opaque, as above. Settled.
- **Q2 (cross-CRDT undo):** (a). Settled.
- **Q3 (GC across block deletion):** (b), but the watermark coordination is application-side — we provide `compact(watermark)` per CRDT and that's the contract. Settled.
- **Q4 (anchor portability across moves):** anchors inside a moved block are stable because the per-block CRDT is untouched; anchors *into* the structural list (e.g. "after block X") follow the same delete+insert semantics as a remove + insert in `Buffer`. We'll document and test this explicitly.
- **Q5 (wire format compatibility post-D1):** transport-layer concern, as you suggest. The serialization is per-op with a discriminator; the application owns multiplexing. Existing `Buffer`-only ops keep their tags; `IdList` ops get new tags. Schema version bumps additively.

## Documentation and fixtures

We'll match `Buffer`'s documentation depth for `IdList`: concurrent semantics (insert-after-deleted-anchor, remove-vs-remove, insert-vs-remove races), per-element memory cost, wire-format/version expectations, and the undo model. The convergence fixtures will be public so Markoff's foundation tests can use them as ground truth.

## Joint review of `Markoff::DocumentStructure`

Yes, we'll do a review pass when you're ready (your §8.5). Most useful framing for that review: are you using `IdList` in ways that constrain its evolution (e.g. relying on undocumented ordering of concurrent inserts, or on garbage-collection timing)? We want to lock the contract narrowly so we keep room to optimize internals later.

## Timing

Restoration first on your side, no pressure on ours. If β starts before the end of restoration, we'd want a short API-shape exchange (a week of back-and-forth on the header file, no more) to avoid landing something that needs to change once D2 starts consuming it. Otherwise we'll start when restoration ships and you have appetite to consume.

## One thing we want from you in return

A short note in the Markoff D2 design — when it exists — explicitly recording the line we've drawn here, so future Markoff contributors looking at `IdList` understand they're looking at a deliberately small surface, not the first installment of a CRDT framework. That preserves the scoping decision for both projects against future drift.

Otherwise: this is a good proposal, the option you picked is the right one, and we're in.

---

*— collabtext maintainers*
