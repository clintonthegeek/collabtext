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

## Current X04 work-count and memory telemetry

After the delta update was implemented, a Release `--quick` run recorded all 54
benchmark cases with diagnostics schema 6. All attempted operations were
accepted and produced result fingerprints. Among 74 sampled
`set_fragments` publications, 0 fell back to a full origin/deletion row rebuild.
The sample performed 249 origin-row insertions, 66 origin-row erasures and 66
deletion-incidence insertions. In the 10-KB/3,000-prior-edit byte-control case,
all five sampled publications avoided index rebuild fallback, while the existing
full structural work remained visible: 37,521 fragment snapshot items and
37,532 tree rebuild items.

At 5,000 entries in each map, the AVL node payload sizes were 88 bytes per
origin interval, 136 bytes per deletion incidence and 64 bytes per RunDirectory
entry: 1,440,000 bytes total for 15,000 nodes (288 bytes per retained run at one
deletion incidence per run). This excludes `shared_ptr` control blocks and
allocator overhead; additional deletion incidences add 136 bytes of node payload
each. The 5,000-run diagnostic inspected 8 SumTree items on the indexed path
versus 5,000 for the scan reference.

The telemetry raw data is in `index-telemetry-quick.jsonl`; its host and source
records are in `index-telemetry-environment.json`. The host load averaged 13.60
over one minute and 12.38 over five minutes before the run on an 8-logical-CPU
machine using `powersave`. Use this artifact for work and fallback counts only;
exclude all latency measurements. It does not close the required low-load paired
timing comparison against the pre-delta X04 commit.
