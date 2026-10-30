# X05 multi-run replacement and deletion evaluation

Five pinned Release pairs compare baseline `16cc48f4969abc2940a7fb332b72af018075dab7`
(the published `collabtext-perf-checkpoint-2026-10-21`) with the candidate tree
at `8019a8b` using trace v2
(`7cd89dec0cac1b6f768471c25e91141855f62e260c0aa174ed8c0e4209cae939`). The
measured candidate was that commit's content with a working-tree diff SHA-256 of
`2e1fb9fb2c07f03b4008ab83969f76f78a8c58f5c382706be3c6461dcd6155e5`; the diff is
the commit itself, so the pair is reproducible from `8019a8b` directly.

All 56 public state and operation fingerprints matched, all five native suites
passed 14/14, and recovery cases completed at 512 and 2,048 operations. Public
benchmark processes were pinned to one CPU (`--pin-cpu 2`); the host governor is
`powersave`. Raw samples, aggregate, environment and build logs are in
[`raw/`](raw/).

## What changed

A replacement or deletion whose visible range crosses retained-fragment
boundaries now stages every covered run in one transaction:

- each covered run is split at its own covered edges, so a kept prefix and kept
  suffix stay in place;
- only the covered scalars are tombstoned, with the operation's deletion id
  appended to exactly one piece per run;
- the new run's FugueMax slot is proved against the old state before mutation
  (inside a run from the retained prefix successor, at a boundary from the
  indexed predecessor/successor);
- tree edits run highest retained rank first, node paths are refreshed across the
  affected span, the staged order is verified before publication, and
  origin/deletion/run rows are maintained incrementally.

Deletion reuses the same staging without an inserted run, so a partial-run
deletion no longer copies and rebuilds the whole fragment list.

## Measured result

Native tombstone scaling, per-edit mean ratio (candidate/baseline), median of
five pairs:

| Workload | Baseline ns/edit | Candidate ns/edit | Ratio | Speedup |
|---|---:|---:|---:|---:|
| 5-KB, 50% tombstones (3,746 frags) | 3,336,970 | 889,618 | 0.263 | **3.8x** |
| 3-KB, 50% tombstones (2,250 frags) | 2,009,750 | 686,646 | 0.342 | 2.9x |
| 8-KB, 50% tombstones (6,005 frags) | 5,530,630 | 979,317 | 0.177 | 5.6x |
| 5-KB, 200-edit, 50% tombstones | 3,806,820 | 740,050 | 0.198 | 5.0x |
| 5-KB, 200-edit, 90% tombstones | 4,676,500 | 1,412,780 | 0.298 | 3.4x |
| 5-KB, 200-edit, 0% tombstones | 215,619 | 136,368 | 0.621 | 1.6x |

The 5-KB/50%-tombstone row clears X05's `2x` mixed-deletion target at 3.8x; the
worst pair in that row was 0.283 (3.5x), so the margin is not a single-pair
outlier. The 0%-tombstone control also improves rather than regressing.

Public aged-edit rows (mean ratio, median of five pairs): the aged 10-KB /
3,000-history replacement is 0.957 and its matched byte control 0.924; the
already-complete aged-insertion rows are 0.985 (`middle_insert_3000_runs`) and
1.020 (`middle_insert_128_runs`). The no-history replacement rows are 0.975–1.011
mean with p99 between 0.922 and 1.024, and the boundary-mint rows are faster
still (0.655 and 0.902 mean), so nothing in the light-history set crosses X05's
10% guard.

## Work-count evidence

In the diagnostics-enabled native 5-KB/50%-tombstone fixture, the 50 timed edits
now record 7 fragment snapshots (from 14 before this change) and publish 0
splice failures; the 3-KB case records 9 (from 16) and the 8-KB case 4 (from
16). Every replacement that reaches the splice path succeeds; the remaining
snapshots are pure middle insertions whose indexed slot proof is still
unavailable, which is X07/X04 territory rather than this task.

## Limits

- This does not make edit complexity logarithmic, and it does not change the
  installed API, package label or manifest.
- The reference reconstruction path remains as the fallback for unproven
  insertions, multi-range edits and any splice refusal.
- The paired matrix is public/native/recovery only; arc B's post-X08 rerun and
  the relocated package gate are tracked separately.
