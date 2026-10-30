# From Corbomite — consumer state, and the asks that survive the re-triage

**From:** Corbomite (`/home/clinton/dev/Corbomite`)
**To:** collabtext maintainers
**Date:** 2026-10-30
**Re:** the collabtext rows in
`Corbomite/docs/handoff/2026-10-18-to-markoff-consolidated-open-asks.md`
(ASK-M1, M29, M2's engine half, plus the build rows in the smaller-items table)

**What we run: engine `16cc48f` (`collabtext-perf-checkpoint-2026-10-21`),
consumed through `Markoff/build-collabtext-alpha5/install` as a static
`libcollabtext_core.a`. Our suite is 281/281 on that engine
(`ctest -E "benchmark|perf"`).**

## What the engine got right, verified at our pin

- **M1 — typed anchor rejections.** `IdListEngine::try_remove_at` /
  `try_insert_after` return `Result<IdListOperation>` and the
  unresolvable-anchor cases are tested green at the pin ("we'd rather be shown
  wrong" was the ask; the fix shape is what we asked for: the miss is a
  first-class outcome checked **before** any mutation, and
  `apply_concrete(RemoveOp)` no longer observes a version for a delete that
  never happened).
- **M29 — the watermark API.** `Global::coverage_intersection`
  (`src/crdt/Clock.cpp:179`) replaced `Global::meet` as the GC watermark and
  `meet` stays only as the documented summary fold.
- **Build hygiene self-fixed.** At the pin the test support is a separate
  `STATIC` target and the internal `src/` include path is `PRIVATE` to it, and
  nothing installs into a consumer's export set — both rows from our 2026-10-18
  table (`collabtext/CMakeLists.txt:76-80` and `:85`) no longer describe the
  tree. Consider them withdrawn.

We say plainly that the single most valuable property these two fixes bought us
is that a *release* build can no longer silently corrupt or reorder the block
list on a stale anchor — that was the class of bug we had no way to see from
the host side.

## What we are asking for

1. **Keep the refusal invariants on a release-build leg in CI.** The typed
   rejections exist; what keeps the 2026-10-02 class from regressing is a test
   run with `NDEBUG` so a future "the assert was the only guard" change cannot
   ship quietly. If the perf-checkpoint line already runs such a leg, point at
   it and we will drop the ask; if not, two tests cover it (remove-before-
   insert ordering, and a miss under a release build) — both are in your
   quarantine logs from the round that fixed M1.

2. **Tell us the supported consumer tag line.** We currently follow the newest
   annotated tag your consumers mention (`perf-checkpoint-2026-10-21` now,
   `integration-2026-10-19` before it). If the rule is "newest annotated tag on
   `master`", say so once in your docs and every consumer self-serves; if it is
   "the tag the family pins per release", bless that. Either is fine — we
   would rather follow a stated default than ask per incident.

3. **Move the alpha label when a consumer must rebuild.** The 2026-10-21
   re-pin shipped a byte-identical API with the package label unchanged
   (`0.2.0-alpha.5` in both directions), which was correct. The flip side:
   whenever a change lands that *does* require a consumer rebuild, please bump
   the label (`0.2.0-alpha.6`). The label is the only downstream-visible signal
   that anything moved; without it a consumer rebuild is guesswork.

4. **One line of advance notice if physical collection ever comes back.**
   Physical collection is currently off by the family's deliberate decision,
   which we depend on — Corbomite treats accepted revisions as
   non-expiring for its cardinal retention behaviour. If custody, pruning or
   epoch transitions return, a note in `ALPHA_CAPABILITIES.md` before it lands
   is all we need to re-check our assumptions.

5. **One perf row, restated against the current pin.** `IdList::set_entries`
   (`src/crdt/IdList.cpp:1064`) still rebuilds the whole `IdListTree`
   (`IdListTree tree; for (auto& e : entries) tree.push_item(...)`), and it is
   on the bulk-entry paths. If the perf-checkpoint line already amortises this
   per edit, point at the commit and we drop the row; if not, an incremental
   update on that path is where our Enter-heavy documents would notice. We
   cannot measure your side better than you can — this row is an ask for
   either a fix or a pointer, not a design lecture.

## What we owe you

Same as last time: your tests run inside our suite, so anything you publish
that regresses one of these rows turns red on our side within a day. If that
ever happens, the failing target name is in our `ctest` output — tell us and
we will pin the regression report to your tree rather than ours.

## How to answer

Edit this file, or leave a reply under `docs/handoff/` — the standalone repo
and the `Markoff`-vendored tree both reach us through Markoff's install
prefix, so a reply anywhere either of you maintains is fine.
