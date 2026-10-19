# Markoff Core candidate

Prepared 2026-10-11; finalized 2026-10-12. Branch `feat/fuguemax-placement` in
`/home/clinton/dev/collabtext/.worktrees/reliability-v2`.
New local candidate tag: `collabtext-markoff-core-2026-10-11`, package
`0.2.0-alpha.4`, capability manifest v16. Resolve the final pin with
`git rev-parse collabtext-markoff-core-2026-10-11^{commit}`.
The tag contains the final executable revision plus documentation/evidence
bookkeeping. The earlier adoption and Wave tags remain immutable. No consumer
checkout was edited, no consumer pin changed, and nothing was pushed, published
or merged into master.

## Delivered requests

1. `IdListEngine::visible_entries()` projects owned application values and full
   insertion identities in visible order. Duplicate values remain distinct;
   removal hides an identity and undo restores it. Exact-identity `element_of`
   and `try_anchor_of` overloads avoid first-match application-value ambiguity.
2. Scoped text/list `apply_counter_gap` accepts a host-authorized, already
   committed terminal interval. Canonical-byte retries are idempotent; changed
   bytes, overlap, wrong scope, covered state and conflicting retained input
   fail closed. Missing prior defers without retaining the gap. Successful
   application advances causal state without an edit/undo/publication callback
   and releases applicable document waits. The bounded terminal registry is
   preserved by section-4 checkpoints. Terminal unused identities cannot be
   operation allocations, insertion links, deletion or compensation targets;
   abandoned local undo metadata is refused.
3. CTLO/0002 insert/remove/undo and CTGP/0001 terminal records gain indefinite
   durable-reader obligations from this adoption cut. Literal CTLO archives
   preserve identity, visibility and canonical bytes; old host rows/hashes need
   no rewriting. No earlier experimental store migration is required.

The host still owns document keys, full-identity stream naming, bundle atomicity,
absence proofs, allocation durability and applied receipts. An uncertain append
or reservation must recover before a gap decision. Rebuild a failed tentative
view from the accepted prefix before applying its gap; restore only accepted
owner undo metadata. Collection/log pruning remain disabled for initial adoption.

## Implementation revisions

- `cb43520e5bbfc9378fc49c0053192863caaa4eed`: public query/addressing, terminal
  registry, checkpoint composition, readers, independent model and public trace.
- `e4c98f94fbcaaf2da63a13f5968db8483877aa81`: released-text notifications and
  list callback reentrancy guards.
- `aad2cd1dce5f5f23e1a7c5b4e4a67050b55ab36c`: compile-time assertions for both
  public element lookup signatures.
- `c0a0ef55ea9b6a3e448b9ef5c258f09933ebb127`: reproduced/fixed terminal identity
  reference and abandoned undo metadata regression. This is the final executable
  implementation revision; the tag's successor commit changes documentation only.

## Verification

Full Debug and Release suites passed **101/101** at `c0a0ef5`:

```sh
COLLABTEXT_VERIFY_JOBS=6 ./tools/verify-changed.sh --all --mode debug
COLLABTEXT_VERIFY_JOBS=6 ./tools/verify-changed.sh --all --mode release
```

Evidence paths relative to the isolated worktree:

- `build-roadmap-debug/verification/20261011T220608Z-changed-r7vzhlbq`
- `build-roadmap-release/verification/20261011T220608Z-changed-mmxvouz_`

The concurrent full sanitizer run passed 100/101. Its only failure was an
unchanged fuzz timing assertion: 200 pinpoint edits took 13,170 ms against a
10,000-ms limit while installed consumer builds competed for CPU. The assertions
for all other fuzz scenarios passed; no sanitizer finding was reported. This
failure was retained, not suppressed or converted into a pass. Full serial
ASan/UBSan with LeakSanitizer enabled was run separately with:

```sh
COLLABTEXT_VERIFY_JOBS=1 ./tools/verify-changed.sh --all --mode asan
```

The full serial run passed **101/101** at `c0a0ef5`, including the unchanged
fuzz timing assertion. Evidence:
`build-roadmap-asan/verification/20261012T054015Z-changed-dwojgjlu`.
Log: `/tmp/ct-markoff-asan-serial.log`. Leak checking remained enabled;
there were no ASan/UBSan/LeakSanitizer findings.

The initial automatic approval review for this unrestricted rerun timed out;
the allowed retry was approved. No sanitizer or timing assertion was disabled.
The unrestricted executor is needed for LeakSanitizer process inspection.

The independent interval/coverage model ran with `-DNDEBUG` before dependent
gap integration. `consumer_markoff_core` exercises both stream kinds, repeated
list values/concurrent actors, delete/undo/original identity, literal CTLO
archives, gap deferral/retry/scope conflicts, released-change callbacks, a synced
host-log/private-reservation reopen boundary and two fresh replay peers.
Checkpoint tests cover section 4 with markers, prefix truncation, trailing bytes,
terminal registry bounds, conflicting pending allocations and restored conflict
checks. The unused-reference regression failed before hardening and passes after
it, including remote undo/deletion and abandoned owner history.

Roadmap validation, strict scope checks, `git diff --check` and all 14 verification
routing tests passed.

## Relocated installed package gates

At `c0a0ef5`, all three modes passed **5/5** installed CTests, **16/16** strict
Wave-shaped cases and **4/4** Markdown cases:

```sh
COLLABTEXT_VERIFY_JOBS=3 ./tools/verify-consumer-package.sh --mode MODE \
  --gate live --components core --producer-build build-roadmap-MODE \
  --output-dir /tmp/ct-markoff-MODE-live-c0a0ef5
```

Each mode's `results.json` records producer revision, manifest v16, hashes and
commands. Installed headers/links use relocated `prefix-b`, with optional
Qt/zstd/OpenSSL discovery disabled in Core-only consumers. The unknown-component
negative check passed. Both producer and consumer were instrumented for the
leak-enabled sanitizer package gate. The registry contains 18 rows and all 18 observed results passed. Its 16-case
live strict set excludes the WP6 concurrent-paste and WP8 durable-pin rows; both
also passed. This does not complete physical GC certification.

Usable local Release prefix:
`/tmp/ct-markoff-release-live-c0a0ef5/prefix-b`.
This installation's executable source is identical to the tagged documentation
successor; it has not been published.

## Direct reader-upgrade check

The same independent literal CTLO insert/remove/undo source passed **1/1**
against each relocated installation:

- alpha.3 at `e1ff945bd338c526501e244515bcbbdbe3ffc96c`, prefix
  `/tmp/ct-wave-release-live-e1ff945/prefix-b`;
- alpha.4 at `c0a0ef55ea9b6a3e448b9ef5c258f09933ebb127`, prefix above.

Reproducer source/CMake: `/tmp/ct-markoff-ctlo-upgrade`;
builds `/tmp/ct-markoff-ctlo-upgrade/alpha3` and `alpha4`.
Logs `/tmp/ct-markoff-ctlo-alpha3.log`, `/tmp/ct-markoff-ctlo-alpha4.log`.
Both decoders applied literal archives with exact canonical bytes and expected
insert/remove/undo visibility; the installed Markoff trace additionally checks
restored insertion identities and order. No current writer generated these
fixtures at test time.

## Actual Markoff-owned probe and actionable failure

Read-only checkout HEAD `28e9a734bf5d383ddac9a132653c3691e5e72bd1`, with
uncommitted adoption work. This is not claimed to be a clean revision snapshot.
The probe/adapter/Markoff-core helper/header source digest was
`53e4620a4ed2611444ca52325e5e0d35e90752384746d009b7e39942c83ddae1`;
manifest `/tmp/ct-markoff-owned-source.json`. Those files did not change during
verification. Built out of tree against each package:

```sh
cmake -S /home/clinton/dev/Markoff/tests/collabtext-vnext \
  -B /tmp/markoff-ct-alpha4-probe -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/tmp/ct-markoff-release-live-c0a0ef5/prefix-b
cmake --build /tmp/markoff-ct-alpha4-probe -j3
ctest --test-dir /tmp/markoff-ct-alpha4-probe --output-on-failure --no-tests=error
```

Compilation passed; runtime **0/1**: `unknown_stream: scoped text stream name is
not a canonical block identity`. The identical source also compiled and failed
**0/1** against alpha.3 (`/tmp/markoff-ct-alpha3-control`), with the same error.
Logs `/tmp/markoff-ct-alpha4-probe.log`, `/tmp/markoff-ct-alpha3-control.log`.

The failure is before Core construction: `tests/collabtext-vnext/main.cpp:64`
sets `stream = "block-1"`, whereas `ScopedTextStream::create` requires Markoff's
`parseBlockTextStreamName`. Derive the shared fixture scope through the existing
`blockTextStreamName(BlockIdentity)` helper, using full actor/counter identity;
reuse it consistently across peers, restart and routes. Do not weaken canonical
name validation. Upstream does not change consumer fixtures or infer production
acceptance from its own successful trace.

M01–M05 are upstream complete after these checks. **M06 and Q06 remain open**:
Markoff must finish real BlockId/routing/persistence adoption, add owned terminal
gap/archive coverage, fix/rerun its fixture, and explicitly accept the exact
candidate. Wave also needs same-candidate production acceptance. Final engine
and both consumer gates plus explicit user merge authorization govern master.
See [Markoff response](../handoff/markoff-core-response.md) and
[common adoption handoff](../handoff/consumer-adoption.md).
