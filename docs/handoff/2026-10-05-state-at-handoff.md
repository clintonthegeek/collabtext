# State at handoff — 2026-10-05

Branch `roadmap/reliability-v2`. This is the position a fresh executor inherits.
`AGENTS.md` has the same information in shorter form; this document has the
reasoning and the traps.

## What is done

| Area | Tasks | Note |
|---|---|---|
| Validation and design | V01–V08, D01 | with independent review corrections, 2026-10-04 |
| Primitive foundations | P01–P05, L01 | |
| Consumer requirements | W01, S01 | |
| Packaging arc | A01, A02a, **A06a** | A06a landed today; see below |

The registry is 72 tasks. `docs/roadmap/2026-10-reliability/TASKS.json` is
authoritative for status and dependencies; `validate-plan.py` and `check-scope.py`
both pass on it as of this commit (72 tasks, acyclic, F1–F10/R1–R14 covered,
W1–W6 consumer coverage, no prohibited direction required by any task).

### A06a, landed today

Commit series `666297b`, `b1cfa6c`, `f3360c0`. collabtext is now installable and
consumable:

- Component targets `CollabText::Core`, `::FileTransport`, `::QtAdapter`,
  `::Widgets`, each exported in its own export set so a Core-only consumer's
  configure output contains no reference to Qt at all.
- `collabtext-config.cmake` reports the producer's enabled components and fails a
  requested-but-unbuilt component with a message naming the option that would
  enable it.
- 16 public headers and the versioned `ALPHA_CAPABILITIES.md` install beside each
  other; nothing from `src/` installs.
- `tests/consumer-smoke/` is a separate CMake project: finds the package by
  prefix path, links an imported target, and compiles one translation unit per
  installed header to prove each is self-sufficient.
- `tools/verify-consumer-package.sh` installs to prefix A, **moves it to prefix
  B**, and only then configures a consumer against B with Qt/zstd/OpenSSL
  discovery disabled. The move is the check.

`docs/USING_VNEXT.md` is the consumer-facing how-to.

## What to do next: S04a

`TASKS.json` id `S04a`, "Specify scoped boundary identities and an independent
position model". Dependencies S01, P06a (steps 1–5), W01 are all met.

This is the gate for Wave. The manifest lists three things as **not implemented**
that Wave's R3 says are a data-corruption risk:

1. **Affinity.** An anchor's `Bias` is stored and never read. Left and right of
   the same character resolve identically. Any consumer positioning a cursor or a
   reply gets plausible-looking wrong answers rather than an error.
2. **Typed unresolvable anchors.** On the text facade, an anchor naming an item
   the engine does not have silently resolves to end-of-document. (The IdList
   facade's `try_anchor_of` *is* typed; the text path is not.)
3. **Position tokens.** `Anchor::end()` is a past-the-end flag, not a comparable
   token, and there is no stable versioned encoding to persist.

S04a's seven steps, in short: trace the current unsafe helpers; specify opaque
boundary tokens scoped by document/stream/primitive/epoch with *sufficient*
context (left/right neighbours alone are explicitly not accepted without model
evidence); specify typed outcomes that separate strict attachment loss from a
valid caret collapse; define the token codec and version rules; build an
independent reference model with fixtures covering both current and
*S02-repaired* concurrent orders; check the representation can express the
repaired ordering, not just today's interleaving; update the API guide with
compilable target signatures.

Acceptance requires an independent model (`tst_boundary_model` +
`libs/collabtext/tests/reference/BoundaryModel.h`) that passes WP1–WP4/WP7 and
**detects three mutations**: ignoring affinity, erasing scope, and mapping a
missing id to end. Those three mutation checks are the real gate — a model that
agrees with production code proves nothing, a model that disagrees when the
production behaviour is mutated does.

Then S04b implements it, W02 runs the installed baseline probes, W03 the strict
live-position probes.

## Verification, and the trap in it

Tiers are documented in `EXECUTION.md` under "Verification tiers".

```sh
./tools/verify-changed.sh                      # routes on the diff, ~30s
./tools/verify-changed.sh --thorough           # adds fuzz/property tests
./tools/verify-changed.sh --explain            # print the routing table
./tools/verify-roadmap.sh --fresh debug '.'    # acceptance: from scratch, once per arc
./tools/verify-consumer-package.sh --mode release --gate package --components core
```

Today the full Debug sweep took ~50 min to build and ~22 min to run 70 tests.
It was previously run at two workers, which is most of why it had stopped being
run at all. `jobs` now defaults to `nproc`.

**ccache 4.14 is installed and nothing uses it.** Wiring
`-DCMAKE_CXX_COMPILER_LAUNCHER=ccache` into the acceptance configuration is the
single biggest remaining speedup, and it was deliberately left out of this
commit because it forces one full rebuild to populate the cache.

### Traps

- **`tst_benchmark::memory_growth` trips the QtTest watchdog** at ~307s under a
  parallel Debug sweep, aborting with SIGABRT. It is a Debug-build timing limit
  on an uninstrumented measurement, not an assertion failure. The user has asked
  for it to be skipped rather than tuned; do not spend time on it and do not
  count it as a regression.
- **`/tmp` is a full 7.7G tmpfs.** Point `TMPDIR` inside the worktree for any
  long build.
- **Three defects here were checks that reported success after failing.** If you
  add a verification path, test that it fails: run the gate against a
  deliberately broken input and confirm a nonzero exit and a nonzero
  `results.json`. CTest exits 0 on a zero-test selection, so `--no-tests=error`
  is not optional.
- **`tst_realistic` takes ~450s** and is the long pole on test runtime.
- `verify-changed.sh` warns about changed files that matched no route. That
  warning is load-bearing — if a new source directory appears, add a route.

## Still open, deliberately

- **P06a step 6**, opaque operation values. Blocked on a recorded A02 boundary
  decision (true opaque handles versus internal accessors over a visible
  layout). Steps 1–5 are done and S04a does not need step 6. It was asked about
  and the answer was recorded; it was not answered with a decision.
- **F8**, concurrent inserts at one position interleave. Belongs to S01/S02/S03;
  the core smoke asserts the character *multiset* for exactly this reason.
- **Publication.** Not requested and not authorized. E07a/E07b prepare reviewable
  candidates; tagging, publishing, merging to master, editing Wave's checkout,
  and sending Wave a message are all separate actions needing the user's explicit
  instruction.

## Environment facts worth not rediscovering

- ctest names equal their build targets (`add_crdt_test`), so a regex can be used
  as a target list directly.
- The consumer's Core-only path must stay free of Qt: the three Qt headers are
  `CollabDocument.h`, `PresenceManager.h`, `SyncManager.h`, and the consumer
  skips them unless asked for the qt component.
- `StreamSync` is implemented but deliberately **not** exported — its header names
  zstd-reaching segment types, so it is not self-sufficient. Splitting those
  types out of the public header is what would let it ship.
- `ReplicaAllocator` is source-tree only and not public. A consumer supplies its
  own `IdentityAllocator` and owns durability.
