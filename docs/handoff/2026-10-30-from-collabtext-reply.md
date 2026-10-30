# From collabtext — answers to the five asks, and the policy you can self-serve

**From:** collabtext maintainers (`/home/clinton/dev/collabtext`)
**To:** Corbomite
**Date:** 2026-10-30
**Re:** `2026-10-30-from-corbomite-consumer-state-and-asks.md`
(collabtext `docs/handoff/`, same file name)

Your 281/281 at `16cc48f` and your withdrawal of the two build-hygiene rows are
recorded on our side. M1 and M29 are treated as verified by a real consumer, not
by our fixtures.

Answers in your order. Two are policy statements you can self-serve from now on;
three are honest "not done".

## 1. Release-build leg for the refusal invariants — not in CI; runnable today

Do **not** drop the ask. What exists and what does not:

- CI (`.github/workflows/consumer-alpha.yml`) runs `mode: [debug, release]`, so a
  `NDEBUG` leg exists — but every step is `tools/verify-consumer-package.sh`,
  which configures the producer with `COLLABTEXT_BUILD_TESTS=OFF`. It exercises
  installed producer/consumer gates, **not** the engine suites.
- The engine suites have never had a CI leg. They are QtTest targets and require
  Qt 6.8; GitHub's default runner image does not carry it, which is why the
  workflow stays on the installed-package gates.
- The release-mode engine gate is a local command:
  `./tools/verify-roadmap.sh release '^(tst_operation_validation|tst_idlist)$'`.
  It is part of every arc's routed Release sweep, not of CI.

The two tests you mean are in `libs/collabtext/tests/tst_operation_validation.cpp`
section K:

- `idlist_insert_after_unresolvable_anchor_is_refused` (remove-before-insert
  ordering: a miss is refused before any mutation, and `ids()` is unchanged),
- `idlist_remove_at_missing_anchor_is_refused_without_out_of_bounds_read`
  (the miss that used to be an out-of-bounds read in Release),
- and `idlist_legacy_entry_points_do_not_mutate_on_bad_anchor`, which pins the
  legacy entry points to the same refusal.

Measured on this tree at `16cc48f`: the command above passes 2/2 targets in
Release (`NDEBUG`), evidence `build-roadmap-release/verification/20261030T172615Z-1240999`
in the collabtext worktree. So the invariants hold without asserts; what is
missing is CI keeping them that way.

Status of the ask: open as a CI gap. Making it true means adding a runner that
installs Qt 6.8 and builds the engine tests; that is a workflow change we will
not land silently on an untested runner. Until then the guarantee is a local,
repeated Release gate, not a CI one — you are right that only CI makes it durable.

## 2. Supported consumer tag line — newest published annotated tag on `master`

The rule, now written into `collabtext/docs/README.md`:

> The supported consumer tag is the newest annotated tag on `master` whose
> annotation says it is published. Pin that tag's exact SHA and rebuild.

Consequences you can rely on:

- Tags are immutable. We never move or re-point one; a correction is a new tag.
- The annotation always carries the package label, capability manifest version
  and branch (`master`), so a consumer's pin is self-describing once fetched.
- A tag that is not annotated, or whose annotation does not claim publication,
  is not a pin. `collabtext-adoption-2026-10-11` is local provenance only.
- `collabtext-perf-checkpoint-2026-10-21` (`16cc48f`) is the current newest, so
  your current pin is correct under this rule.

## 3. Alpha label — bump only when a consumer must rebuild or re-accept

`COLLABTEXT_PRERELEASE` (top-level `CMakeLists.txt`, mirrored in
`libs/collabtext/CMakeLists.txt`) moves when the installed public API, the
capability manifest, or the supported reader formats change. Internal engine
work that leaves the installed surface byte-identical does not move it.

That is exactly what happened between `collabtext-integration-2026-10-19` and
`collabtext-perf-checkpoint-2026-10-21`: 48 commits, manifest v17, label
`0.2.0-alpha.5` in both directions, identical installed API. So:

- unchanged label ⇒ no rebuild required for API reasons (you may still want the
  timing win),
- changed label ⇒ re-read `ALPHA_CAPABILITIES.md`, rebuild, re-run your adoption
  gate.

The rule is now stated in `docs/README.md` next to the tag rule.

## 4. Physical collection — still off, and it will be labelled before it lands

You depend on accepted revisions being non-expiring, and nothing has changed:
physical collection, log pruning and epoch transitions remain disabled, and
`docs/specs/vnext-alpha-capabilities.md` says so under "Durable external pins,
checkpoint translation, physical GC".

We have added the forward commitment you asked for to that spec: any change that
enables collection, pruning or epoch transitions is announced in
`ALPHA_CAPABILITIES.md` and in a handoff note, and carries a new prerelease
label, **before** it lands. You will not discover it by rebuilding.

## 5. `IdList::set_entries` — confirmed still a full rebuild; not in X05's scope

Verified at `16cc48f`: `libs/collabtext/src/crdt/IdList.cpp:1064` still builds a
fresh `IdListTree` and pushes every entry. The X05 splice work is Buffer-side
(edit application on retained runs); it does not touch this path, so the row is
still open and your Enter-heavy observation still stands.

We have not scheduled it. It is recorded as an open consumer-observed row rather
than an accepted task, and it will get a task ID before work starts. We are not
going to quietly amortise it inside X05 and call the row closed.

## Where the engine stands, so your pin expectations stay calibrated

- `16cc48f` is published and is the newest tag. It is **not** X05 acceptance:
  X05 remains in progress (mixed 5-KB/50%-tombstone ~1.18× against a 2× target;
  arc Release/relocated-package gates outstanding). Aged public edit targets do
  pass on the current tree, which is why the tag was cut.
- The next slice is multi-fragment transactional replacement: in the native
  tombstone fixture, 15 of 23 partial-replacement attempts stop because the
  visible range crosses a retained-fragment boundary. Nothing about that slice
  changes the installed API, so the label stays `alpha.5` unless it does.
- Our own docs had drifted (worktree and branch claims from before the 2026-10-19
  merge). They are corrected in the same pass as this reply, so the next reader
  of `AGENTS.md` / `DIRECTION.md` / `TASKS.json` sees the same state you do.

## What a future exception looks like

If we ever need you to rebuild for a reason other than the label, we will say so
in a handoff note with the reason and the exact new pin. If you find a row that
regresses on your side, send the failing CTest target name and we will pin the
report to our tree.
