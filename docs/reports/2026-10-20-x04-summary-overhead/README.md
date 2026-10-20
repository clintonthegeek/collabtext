# X04 versus X07 byte-control comparison

This diagnostic comparison measures the current X04 implementation against
the immediately preceding X07 commit, using the public benchmark matrix's
matched byte-replacement cases. It is evidence about the combined X04 change
(indexes, stable run handles, visibility targeting, and summary growth); it does
not isolate the cost of any one component.

The harness recorded five paired Release runs and matched all 54
baseline/candidate state and operation fingerprints. For matched byte-control
replacements, the median of the five per-pair mean ratios was 2.054 at 100 KB
and 1.531 at 1 MB. The median p50 ratios were 2.131 and 1.529, respectively.
These values are a diagnostic warning, not a performance conclusion: the host
was substantially loaded (load averages reached 17.57 on an 8-logical-CPU
machine), and the CPU governor was `powersave`. The run was pinned to CPU 1, but
pinning does not remove contention. Do not use these timings as an acceptance
claim or as proof that the X04 structures caused the full difference.

The work-count evidence remains useful independently of timing: the focused
5,000-run diagnostic resolves one origin/deletion target to one run and
inspects 8 SumTree leaf items versus 5,000 items in the scan reference. The
current X04 mutation path still snapshots and rebuilds the fragment tree and
derived indexes. The next timing comparison should be repeated in a comparable,
low-load environment, then structural edits should move to the planned X05
transactional splice path before attributing or accepting end-to-end latency.

## Recorded artifacts

- `aggregate.json`: five-pair summaries and exact fingerprint/work-count
  agreement.
- `baseline-public-{1..5}.jsonl` and `candidate-public-{1..5}.jsonl`: raw
  per-run public measurements.
- `environment.json`: revisions, compiler, CPU/governor, commands, and load
  records.
- `*-configure.log`, `*-build.log`, `baseline-harness-compile.log`: build
  provenance. Build directories are intentionally excluded from the report.

Candidate: `eef0d15f6759aa7ac753a07e6f489cf4052b586a`.
Baseline X07: `eefcf9d0a2166974d6b8ab393d3750bddd26c89b`.
