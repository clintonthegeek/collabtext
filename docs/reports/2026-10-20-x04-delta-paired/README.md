# X04 incremental-index comparison

The candidate at `b2c47d6c90f5ba56b4f216c190e40f98ec049432` adds path-copied
OriginIndex/DeletionIndex updates to the pre-delta X04 source at
`eef0d15f6759aa7ac753a07e6f489cf4052b586a`. The first five-pair run is retained
in this directory with its build logs and full environment record. All 54
workload state and operation fingerprints match. Its host load was too high for
latency interpretation.

A second five-pair set in [`steady-five-pair/`](steady-five-pair/) reused those
Release binaries and also matched all 54 workload fingerprints in every pair.
The first two pairs encountered shared-host load spikes (up to 15.88 on the
one-minute average), so exclude them from timing interpretation. Pairs 3–5 ran
as load fell: pair three's one-minute average was 8.49 before baseline and 5.63
before candidate; pair four was 3.88 before candidate and 3.07 before baseline;
pair five was 2.57 before baseline and 2.17 before candidate. Five-minute
averages for those paired runs ranged from 4.67 to 7.12 on the 8-CPU host.
Treat the last three as comparable directional evidence, not a broad claim.

Across pairs 3–5, the median candidate/baseline mean ratio for the 10-KB byte
replacement was 1.231 at history 0, 1.174 at history 500, and 0.881 at history
3,000. The corresponding p50 ratios were 1.203, 1.263, and 0.946. The 1-KB
bulk-undo-after-remove mean ratio was 0.825; the 100-KB case was 1.142. The
results are mixed: delta-maintained indexes help aged replacement and the small
undo case, while some light-history and larger undo cases regress. The current
candidate is the baseline for X05, which must remove the remaining whole-tree
edit work and meet its explicit light-history regression bound. These
measurements do not establish a general end-to-end speedup.

## Direct indexed lookup

The opt-in `tst_benchmark indexed_run_lookup_latency` case constructs a fixed
5,000-run origin/deletion fixture before timing. It alternates five batches of
5,000 queries between the full fragment scan and `OriginIndex` +
`DeletionIndex` + `RunDirectory`; both paths return the same results. The median
batch cost was 27,913 ns/query for scanning and 538 ns/query for indexed
resolution (51.8x). This isolates target lookup from fixture construction and
whole-edit tree publication. It includes one-result vector creation in both
paths and does not claim end-to-end edit latency.

Raw measurements and provenance:

- `aggregate.json`, `environment.json`, and five baseline/candidate JSONL pairs:
  the full checked matrix run, including build provenance and load before/after
  each command.
- [`steady-five-pair/aggregate.json`](steady-five-pair/aggregate.json),
  [`steady-five-pair/environment.json`](steady-five-pair/environment.json), and
  its five baseline/candidate JSONL pairs: the follow-up set used above.
- `direct-index-lookup.txt`: the direct lookup output. It used the current
  Release build with GCC 16.2.1; full host and toolchain details are in the
  first matrix environment record.
