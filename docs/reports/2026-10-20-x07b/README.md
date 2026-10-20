# X07b — measured no-change disposition

**Disposition: keep X07's exact-width summary; revert the compact-summary production prototype.** The prototype reduced tree-node storage, but it did not bring the cumulative matched byte-control regression within the target. Relative to X07 itself, the byte-control changes were only about 3%, while the larger public-text win remained X07's. No production summary representation change is retained from X07b.

## Cost attribution

The opt-in diagnostics schema is `collabtext-perf-diagnostics/4`. Added counters measure summary compositions, UTF-16 index builds, ASCII-probe bytes and checkpoint entries. They compile to no-ops in ordinary builds and are not latency evidence.

In the quick diagnostics-enabled profile, each five-call 1-MB public or matched-byte replacement case recorded 84 summary compositions, five UTF-16 index builds, five ASCII-probe bytes, zero decoder steps and zero checkpoint entries. The calls use ASCII replacement fragments; each one-byte fragment required one probe byte. Cache construction therefore does not explain the matched byte-control regression. The compact-summary prototype produced the same work counts. The diagnostic outputs are [`candidate-diagnostics-quick.jsonl`](candidate-diagnostics-quick.jsonl) and [`candidate-diagnostics-compact.jsonl`](candidate-diagnostics-compact.jsonl).

On this 64-bit ABI, measured sizes were:

| Representation | FragmentSummary | Fragment | SumTree leaf | Internal node payload | SumTree node |
|---|---:|---:|---:|---:|---:|
| d04f151 baseline | 8 B | 176 B | 2,216 B | 296 B | 2,240 B |
| X07 exact-width summary | 16 B | 200 B | 2,600 B | 392 B | 2,632 B |
| compact prototype | 12 B | 200 B | 2,552 B | 344 B | 2,576 B |

The prototype stored modulo-2^32 visible bytes in the tree and maintained an exact byte total in Buffer at fragment-tree publication and certified collection. That reduced the Summary by 4 bytes and each measured SumTree node by 56 bytes, while preserving overflow refusal. It passed focused summary, Unicode, SumTree, position and diagnostics tests. Its extra exact-total scan was folded into the existing visibility loop before final paired measurements.

## Paired results

The final candidate snapshot (`b17f62e` + `23ebdf1`, with the compact summary and fused exact-total accumulation) was compared to both d04f151 and X07 using the full 54-case public profile, five paired runs, diagnostics disabled, and public processes pinned to CPU 1. All public results, encoded-operation fingerprints and work counts matched in both comparisons. Raw files and per-command environment records are in [`release-pinned-d04/`](release-pinned-d04/) and [`release-pinned-x07-correct/`](release-pinned-x07-correct/).

| Case | Compact / d04f151 median mean ratio | Compact / X07 median mean ratio |
|---|---:|---:|
| Public 1-KB replacement | 1.203 | 1.046 |
| Matched byte 1-KB replacement | 1.212 | 0.926 |
| Public 10-KB replacement | 1.162 | 1.018 |
| Matched byte 10-KB replacement | 1.158 | 0.935 |
| Public 100-KB replacement | 0.820 | 1.099 |
| Matched byte 100-KB replacement | 1.138 | 1.031 |
| Public 1-MB replacement | 0.306 (3.27× faster) | 0.973 |
| Matched byte 1-MB replacement | 1.112 | 1.031 |
| 70-KB Unicode append | 0.338 | 0.909 |

The acceptance guard was at most 1.10 for both 100-KB and 1-MB matched byte cases versus d04f151. The prototype missed both (1.138 and 1.112). Its comparison against X07 reduced the 100-KB byte-control mean by about 3.1%, but increased the 1-MB byte-control mean by about 3.1%; that modest attribution does not pay for the added separate exact-total state and update obligation.

The CPU-pinned runs still had changing system load. For the d04f151 comparison, load averages around public runs ranged from 7.84 down to 2.58; the X07 comparison ranged from 7.70 down to 1.73. The 1-MB byte ratio includes a 1.85 first-pair outlier, but its five-pair median remains 1.112. The earlier unpinned compact run also passed both byte thresholds at 100 KB/1 MB but exceeded 10% on 1-KB/10-KB controls; the subsequent fused run was stopped after host load rose above 12.9. These exploratory datasets are retained separately in `release-matrix-fixed/` and `release-matrix-fused/`.

The initial `release-matrix/` attempt exposed a runner defect: it compiled the baseline harness against candidate private headers, causing an ABI mismatch and a baseline crash after Buffer layout changed. The runner now compiles Buffer/Fragment/SumTree against baseline headers and overlays only the header-only diagnostics file. The corrected smoke profile passed all 54 public cases in `smoke-verify/`; both complete pinned matrices then passed their correctness checks.

## Verification and disposition

Debug `tst_summary_laws`, `tst_unicode_seek`, `tst_sumtree`, and `tst_positions` passed 4/4 after restoring X07's exact-width summary. Diagnostics-enabled `tst_perf_diagnostics` passed 7/7. `test_run_performance_matrix.py` passed 4/4, `test_verify_changed.py` passed 16/16, and the plan/scope validators passed.

Keep the X07 representation and its recorded 3.11× median public 1-MB improvement. Revert the X07b compact-summary production prototype because it fails the cumulative byte-control guard. Retain the opt-in diagnostic counters and runner support for pinned public-only comparisons and preceding-candidate baselines. X04 can proceed against the existing X04a model/design gate.
