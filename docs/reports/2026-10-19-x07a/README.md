# X07a retained-run position reads

X07a replaces the public text-boundary and strict-attachment read path that
copied every retained fragment and expanded every scalar into a temporary
`PositionItem` vector. The new path walks the native fragment tree by retained
run, using contiguous identity counter intervals and existing visibility. It
does not change public signatures, token bytes, scope checks, history policy,
ordering, undo or marker semantics.

Implementation: `3e3d4c141afe90cafc4f25898559d478523c6555`. The paired harness
compatibility adjustment is `eb9176156ec83d5f1514094d630669d787add6d0`; the
benchmark environment capture is `1a1cbe48d949433e4aa005b1477d7dd76e84492a`.
Baseline engine: pinned `d04f151b3a70b161b72c33a8de6895efbd1c13e2`. The
candidate has no uncommitted source diff. The harness and trace hashes, exact
build flags, commands, per-command load, fingerprints, raw samples and paired
aggregates are preserved in `raw/`.

## Paired results

The full 54-case public harness, 12 native benchmark methods and 512/2,048-item
checkpoint/replay measurements completed in three five-pair batches. Each batch
alternated baseline/candidate order, used fresh processes, and preserved all
result and operation fingerprints and logical work counts. The primary
controlled report is `raw/pinned-cpu2/aggregate.json`; the unpinned initial
batch and full recheck are retained at `raw/aggregate.json` and
`raw/recheck/aggregate.json`.

The table reports medians of the five paired per-run mean ratios and medians of
per-run means. Queries used 50 fresh, varying offsets or tokens per case at 100
KB; the 1,000-history cases had 1,000 fixed-width prior replacements. Ratios
compare identical workload and statistics. Speedup is the reciprocal of the
median paired ratio.

| Query | History | Baseline mean | Candidate mean | Candidate / baseline | Speedup |
|---|---:|---:|---:|---:|---:|
| Boundary mint | 0 | 1.825 ms | 0.000291 ms | 0.000159 | 6,271× |
| Boundary resolve | 0 | 1.934 ms | 0.000097 ms | 0.000051 | 19,499× |
| Boundary mint | 1,000 | 2.357 ms | 0.015996 ms | 0.006482 | 154× |
| Boundary resolve | 1,000 | 2.583 ms | 0.042287 ms | 0.015586 | 64× |
| Attachment mint | 0 | 1.903 ms | 0.000276 ms | 0.000148 | 6,738× |
| Attachment resolve | 0 | 1.997 ms | 0.000099 ms | 0.000049 | 20,392× |
| Attachment mint | 1,000 | 2.343 ms | 0.016952 ms | 0.007237 | 138× |
| Attachment resolve | 1,000 | 2.344 ms | 0.037758 ms | 0.015759 | 63× |

The ≥10× target is met on all four boundary cases, including varied tokens and
offsets, and on all four strict-attachment cases. The interleaved 100-KB
replacement/boundary workload also fell from a 1.82–2.10 ms baseline mean to
0.00067–0.00110 ms in the pinned run. All 54 public cases retained exact result
and encoded-operation fingerprints.

The pinned run used CPU affinity `[2]`, powersave governor, eight logical CPUs,
GCC 16.2.1, Release `-O3 -DNDEBUG`, and tmpfs. Recorded load averages ranged
from 1.31 to 4.21 during the run. The pinned full matrix recorded a 1.684
median ratio for 100-KB bulk insert, the sole non-target public case over 1.10.
That path does not call boundary or attachment reads, so I built a focused
public-API harness and ran five alternating baseline/candidate pairs pinned to
CPU 2, with 25 fresh 100-KB inserts per process. Its paired per-process mean
ratios were 0.999, 0.998, 1.029, 0.877 and 1.052 (median 0.999). The full-matrix
outlier was not reproduced. Across the two unpinned batches, other unrelated
microcases crossed 1.10 inconsistently as load varied from 1.51 to 9.19. Their
full results remain archived; the isolated test found no reproducible edit-path
regression.

## Work and correctness evidence

The diagnostics-enabled exact fixture confirms public mint plus resolve performs
zero scalar `PositionItem` materializations, zero fragment snapshots, and two
tree-item visits for the one-run fixture. ASCII runs use the existing equality
of byte length and scalar length to map offsets directly; non-ASCII runs scan
only the relevant run prefix when converting between UTF-8 bytes and scalar
identity offsets. Runtime remains linear in retained fragment count; resolving
a non-ASCII fence can also scan within its run. No per-scalar retained projection
or copied fragment-text vector is constructed.

Before the arc gates, these focused checks passed: `tst_positions` (including
1,050 native/model text resolutions), `tst_boundary_model` (602,624 independent
result checks), `tst_position_token_reader`, `tst_text_markers`,
`tst_marker_model`, `tst_anchor_retention`, and diagnostics-enabled
`tst_perf_diagnostics`. The diagnostics test needs `QT_QPA_PLATFORM=offscreen`
in this headless environment. The first full-run harness compile failed because
the pinned baseline predates the header-only diagnostics include; the runner fix
and original failure log are preserved under `raw/compatibility-failure/`. The
bulk-insert follow-up source and all 250 raw timings are under
`raw/pinned-cpu2/`; its source SHA-256 is
`3dc4e24962459722d62cfd0f958a69d3d350a9c4abf98f3c7a7cacc5724b2f15`.

Arc A gates are complete: fresh Debug and Release each passed 101/101 tests;
ASan/UBSan with leak detection enabled passed 101/101 outside the sandbox's
ptrace restriction, without suppressions. Relocated live installed-Core package
gates passed in Debug, Release and ASan/UBSan. Each mode passed all 5 consumer
CTest cases, Wave 16/16 strict cases, and Markdown 4/4. Evidence is archived at:

- Debug: `build-consumer-verify/debug-live-20261020T000028Z-2`
- Release: `build-consumer-verify/release-live-20261020T000125Z-2`
- ASan/UBSan with leaks: `build-consumer-verify/asan-live-20261020T000310Z-2792992`
- Full Debug: `build-roadmap-debug/verification/20261019T233515Z-2`
- Full Release: `build-roadmap-release/verification/20261019T234009Z-2`
- Full ASan/UBSan with leaks: `build-roadmap-asan/verification/20261019T235708Z-2783379`

`python3 docs/roadmap/2026-10-reliability/validate-plan.py` and
`check-scope.py --strict` pass. The plan validator also exposed pre-existing
X01/X02 phase/registry drift; those phase mirrors were synchronized before
closing X07a. The next performance task is X04a, subject to its unchanged
independent-model gate. Package gate transcripts and CTest logs are retained in
the build evidence directories above.
