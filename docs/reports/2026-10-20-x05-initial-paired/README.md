# X05 initial paired measurement

This first X05 comparison measures the narrow append fast paths before adding
the remote append publication optimization. It compares the frozen X04 baseline
`0fbd15bbad5a8488e78cce56fa0ddb133f225b35` with candidate
`eb79216127091555c1755d25e64fcc5dfd8cb370`, using the checked five-pair full
matrix and its pinned workload trace. The runner reports 54 cases and confirms
that baseline and candidate operation/state fingerprints match in every pair.
Raw samples, command logs, host observations, and the aggregate are under
[`raw/`](raw/).
Qt's native console logs had trailing horizontal whitespace removed for a clean
patch; the timing JSONL, aggregate, and environment records are unchanged.

The candidate's proven local append path delivers a large gain on the exact
sequential case: the median of five candidate/baseline mean ratios is 0.009
(about 111x faster). The public 70-KB Unicode append ratio is 0.046 (about
22x faster). These are narrow append cases; they do not establish a general
edit improvement.

The X05 aged replacement target remains unmet. For public 10-KB replacement
after 3,000 prior edits, the median paired candidate/baseline ratios are 0.893
for mean, 0.891 for p50, and 1.017 for p99. Median per-pair p99 is 16.62 ms
for the candidate and 16.33 ms for baseline, far above the 2-ms target. The
private byte-control ratios are 0.926, 0.945, and 0.868 respectively. In the
native workloads, median ratios are 1.027 for 10-KB random-position edits,
1.097 for 5-KB edits with 50% tombstones, and 1.129 for 90%-tombstone edits.
Those results confirm that appends improved while general edits remain
history-sensitive and can regress.

The shared host was not idle: public-run one-minute load averages ranged from
3.18 to 10.14, and baseline/candidate order varied across pairs. Use the paired
ratios as directional evidence, not as a broad latency claim. The full raw
records retain load before/after each command. No latency threshold is added to
CTest.

Disposition: keep the local tail fast path for its proven work and measured
append benefit. Continue X05; this result does not satisfy its acceptance.
Next, use the same checked publisher for proven remote tail replay, then replace
the vector reconstruction path for middle edits and deletion-heavy cases with
the specified transactional tree/index operations. Re-measure after those
algorithmic changes under a lower-load paired run.
