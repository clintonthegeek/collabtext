# Wave Core candidate — 2026-10-11

Candidate: annotated local tag `collabtext-wave-core-2026-10-11` on
`feat/fuguemax-placement`, package `0.2.0-alpha.3`, capability manifest v15.
Resolve the exact adoption pin with `git rev-parse collabtext-wave-core-2026-10-11^{commit}`.
The tag includes implementation and final evidence/documentation bookkeeping.
The earlier `collabtext-adoption-2026-10-11` stays at
`0d21670896d02dda3dc44d18d37f9657935333ed`; it has not been moved.
Nothing was pushed, published or merged into master; neither consumer's pin was changed.

## Delivered contract

Native zero-width `TextMarker` items share the existing FugueMax sequence,
allocator, causal replay and compensation lifecycle. Text deletion/replacement
preserves markers; identity removal detaches them. Explicit before/after gaps
address multiple items at the same text offset. Duplicate insertion does not
revive removal. Undoing removal restores the original marker identity.

Core exposes bounded local undo metadata snapshot/restore and group/retirement
composition without DocumentSession. `ProjectionAllocator` provides a frozen,
domain-separated per-stream actor derivation and checked replay counter. It is
for disposable canonical replay, not a device writer or independent branch.
Consumers version their translators and check actor namespace collisions.

CTBO/0002 and marker CTBO/0003, CTPT/1 and marker CTPT/2 are current writer
formats, with retained literal archive reader tests and forward reader
obligations beginning with this adoption cut. No upgrade obligation for earlier
experimental alpha stores is assumed. Removed obsolete CME1 membership reading,
unbound checkpoint engine section 1 reading and SyncManager's ignored engine
constructor parameter. Current CME2 and checkpoint engine sections 2/3 remain.

UTF-16 edit conversion and marker resolution avoid full-document copies and
per-scalar temporary expansion. Broad edit/rebuild asymptotics are unchanged;
this report makes no comparative throughput or latency claim.

## Implementation revisions

- `0bda34af49da38c81c29d17719543011d0bcb450`: contract/model, markers, codecs,
  history, projection allocator, query allocation cuts, installed trace and docs.
- `a72fca55577a2641ad1eda1bf1d39dbf0d536918`: unknown-future-format transport
  guards updated for the newly supported marker versions.
- `aafe7e702633cce9224652442484cb3a2a9b2de7`: obsolete compatibility removal.
- `e1ff945bd338c526501e244515bcbbdbe3ffc96c`: package evidence format labels
  corrected; this is the final executable implementation revision. The tag's
  subsequent bookkeeping commit changes reports, handoff and roadmap only.

## Correctness evidence

Commands run in the isolated worktree with `COLLABTEXT_VERIFY_JOBS=6`:

```sh
./tools/verify-changed.sh --all --mode debug
./tools/verify-changed.sh --all --mode release
./tools/verify-changed.sh --all --mode asan
```

Debug and Release each passed **99/99** at `a72fca5`. After compatibility removal,
the affected membership/checkpoint/GC/recovery/presence/Core selection passed
**15/15** in each mode at `aafe7e7`. The full ASan/UBSan suite then passed
**99/99** at `aafe7e7`, with LeakSanitizer enabled and no suppressions. The initial
restricted-sandbox leak run could not inspect processes; the successful run
used the approved unrestricted executor. No sanitizer code finding was hidden.

Retained evidence directories, relative to the worktree:

| Check | Evidence |
| --- | --- |
| Full Debug | `build-roadmap-debug/verification/20261011T172840Z-changed-8klo5m65` |
| Full Release | `build-roadmap-release/verification/20261011T172858Z-changed-py45cfak` |
| Cleanup Debug | `build-roadmap-debug/verification/20261011T173515Z-changed-n5ljf_39` |
| Cleanup Release | `build-roadmap-release/verification/20261011T173516Z-changed-dk4711m_` |
| Full leak-enabled ASan/UBSan | `build-roadmap-asan/verification/20261011T173539Z-changed-5lp3lkhq` |

The independent explicit-tree marker model passed before dependent production
ordering changes. Release checks remain active. Seeded retained-order comparison
covers 2,000 steps. The public installed trace includes concurrent Unicode
marker gaps, marker deletion/undo/reopen, strict attachments, literal codec
archives, restored compensation identity and 600 independent projection
transitions. Roadmap validation, strict scope validation and all 14 verification
routing tests passed.

## Relocated installed package evidence

At `e1ff945`, for each `MODE` in `debug`, `release`, `asan`:

```sh
./tools/verify-consumer-package.sh --mode MODE --gate live --components core \
  --producer-build build-roadmap-MODE \
  --output-dir /tmp/ct-wave-MODE-live-e1ff945
```

All three modes passed **4/4** installed CTests, **16/16** strict Wave-shaped
cases and **4/4** Markdown cases. Their `results.json` records the exact producer
revision, manifest v15, hashes and commands. The install was moved to `prefix-b`;
include/link isolation and unknown-component refusal passed. Core-only consumers
configured with optional Qt/zstd/OpenSSL discovery disabled. Both producer and
consumer were instrumented for the leak-enabled sanitizer gate.

The Wave-shaped registry has 18 rows; all 18 observed results passed. The live
strict set requires 16 rows; the two non-strict rows are WP6 concurrent paste and
WP8 durable pin registration, both passing. Physical collection/log pruning is
explicitly disabled during initial adoption. These fixtures do not certify
production consumer acceptance or finish G06/G08.

2026-10-12 evidence correction: earlier wording incorrectly described the two
non-strict rows as unsupported WP8 collection cases. The retained alpha.3
results show WP6 and WP8 passing; this correction changes no historical run or
GC certification claim.

## Wave-owned probe evidence and limit

Read-only Wave checkout revision `6e8f0d928bafda08d677c8b444cc12b0c23bcade`.
Built out of tree under `/tmp`; no Wave files were changed. Both existing
Wave-owned tests passed against the relocated Release installation from
`e1ff945` (the final executable implementation revision):

```sh
cmake -S /home/clinton/dev/wave/tests/collabtext-adoption \
  -B /tmp/wave-ct-wave-core-probe -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/tmp/ct-wave-release-live-e1ff945/prefix-b
cmake --build /tmp/wave-ct-wave-core-probe -j6
ctest --test-dir /tmp/wave-ct-wave-core-probe --output-on-failure --no-tests=error

cmake -S /home/clinton/dev/wave -B /tmp/wave-t406-wave-core-probe \
  -DWAVE_DEV_BUILD=ON -DBUILD_TESTING=ON \
  -DWAVE_ENABLE_COLLABTEXT_ADOPTION_PROBES=ON -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/tmp/ct-wave-release-live-e1ff945/prefix-b
cmake --build /tmp/wave-t406-wave-core-probe --target tst_collabtext_adoption -j6
ctest --test-dir /tmp/wave-t406-wave-core-probe \
  -R '^tst_collabtext_adoption$' --output-on-failure --no-tests=error
```

Standalone **1/1**, Wave-linked reopened EventStore **1/1**.
Logs: `/tmp/wave-ct-wave-core-probe.log`, `/tmp/wave-t406-wave-core-probe.log`.
The EventStore fixture checks preserved event IDs/history hashes and independent
legacy translation; it still uses per-event actors. It does not exercise Wave's
production marker/thread transaction or the new projection allocator.

Q01–Q05 and Q04a are upstream complete. **Q06 remains external and open**:
Wave must adapt the smallest public test into its production adapter/EventStore
coverage, use the new identity contract, verify marker plus first thread
atomicity, and explicitly approve stage-4/T-406 against the exact candidate.
Then Markoff adoption and the final same-candidate engine/consumer checks remain
before explicit master merge authorization. See
[Wave response](../handoff/wave-core-response.md) and
[common adoption handoff](../handoff/consumer-adoption.md).
