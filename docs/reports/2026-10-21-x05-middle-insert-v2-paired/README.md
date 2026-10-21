# X05 v2 aged middle-insert paired evaluation

This five-pair Release matrix compares baseline `1b431b1deed2ee847b30fc4825326b031b7cb3cb` with candidate `5abe307279c349a787434dc9cacea6f8a4062c10` using benchmark trace v2 (`7cd89dec0cac1b6f768471c25e91141855f62e260c0aa174ed8c0e4209cae939`). All 56 public cases completed and baseline/candidate state and operation fingerprints matched. The two dedicated workloads each contain 200 individually timed `try_apply_local_edit` calls per matrix pair. Full environment, per-sample records, aggregate, and build/run logs are in [`raw/`](raw/).

| Aged history | Baseline median pair mean | Candidate median pair mean | Median paired ratio | Baseline median pair p99 | Candidate median pair p99 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 128 retained single-scalar runs | 204.9 µs/call | 76.4 µs/call | 0.316 (68.4% lower) | 482.2 µs/call | 180.1 µs/call |
| 3,000 retained single-scalar runs | 3.824 ms/call | 1.563 ms/call | 0.375 (62.5% lower) | 6.364 ms/call | 3.149 ms/call |

The paired ratios are medians of the five per-pair candidate/baseline ratios; displayed absolute values are medians across the five pair-level distributions. These are individual-call latency measurements after histories were built, not cumulative totals. CPU affinity was pinned to logical CPU 2, though the host used the powersave governor and system load varied. Treat the numbers as strong directional evidence for this exact fast path, not a general edit-speed claim.

The 3,000-run p99 remains above X05's 2 ms p99 target, and these cases are pure insertions rather than the specified 10-KB/3,000-prior-replacement acceptance workload. They therefore do not meet X05 acceptance. General replacement, remote middle edits and broader undo coverage remain open. This report does not include native benchmark or recovery suites; the separate full matrix report retains those results for its earlier candidate.
