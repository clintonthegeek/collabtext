# X05 exact-run deletion paired measurement

This five-pair full matrix compares the frozen X04 baseline
`0fbd15bbad5a8488e78cce56fa0ddb133f225b35` with candidate
`5c28746a3ee507a9d0b4a06f4c6ab7699dc84808`. The candidate contains the local
and remote exact-run deletion fast paths, in addition to the previously
measured local append path. The runner completed 57 public workload cases and
confirmed matching result and operation fingerprints in every pair. Raw
samples, command logs, environment, and aggregate data are in [`raw/`](raw/).
Native Qt log trailing whitespace was stripped for a clean patch; structured
JSONL, aggregate, and environment records are unchanged.

Ratios below are medians of the five paired candidate/baseline ratios. A value
below 1 is faster. The measurements show a strong narrow deletion win, while
general aged edits remain far from the X05 acceptance target.

| Workload | Mean ratio | p50 ratio | p99 ratio | Interpretation |
| --- | ---: | ---: | ---: | --- |
| Sequential local append | 0.009 | — | — | About 111x faster in this native case |
| Public 70-KB Unicode append | 0.039 | 0.037 | 0.039 | About 26x faster |
| Whole-run bulk remove, 100 KB | 0.0046 | 0.0043 | 0.0048 | About 217x faster |
| Whole-run bulk remove, 1 MB | 0.0033 | 0.0031 | 0.0032 | About 307x faster |
| Public 10-KB replacement after 3,000 edits | 0.957 | 0.966 | 0.920 | 4.3% lower mean and 8.0% lower p99 |
| Native 5-KB edits, 50% tombstones | 0.965 | — | — | 3.5% lower mean |
| Native 5-KB edits, 90% tombstones | 0.822 | — | — | 17.8% lower mean |

For the aged public replacement, the median per-run p99 was 15.57 ms on the
baseline and 14.42 ms on the candidate. This is still over seven times the
2-ms limit, and the candidate is nowhere near the target of a 5x reduction in
mean. The mixed 5-KB tombstone case also misses its 2x target. Exact whole-run
deletion is a meaningful win for its covered shape, not evidence that arbitrary
deletion or middle replacement has constant cost.

The host used the powersave governor. One-minute load before commands ranged
from 2.86 to 16.04, so these ratios are directional paired evidence rather
than a low-load latency claim. The raw manifest retains per-command loads and
all five pairs. No wall-clock assertion was added to CTest.

Disposition: keep the exact-run deletion paths and the local append path; their
checked work reductions now have paired evidence. Continue X05. Stable
Buffer-owned locators, common middle replacements, partial/multi-interval
remote edits, remote undo, and the aged-edit and mixed-tombstone acceptance
targets remain open.
