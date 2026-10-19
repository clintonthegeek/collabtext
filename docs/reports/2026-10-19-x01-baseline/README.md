# X01 paired performance baseline

This baseline freezes the checked performance matrix for the optimization
sequence. The engine baseline is `d04f151b3a70b161b72c33a8de6895efbd1c13e2`;
the measured candidate is `1701d35fe9f88a4071d4fb9e71224cd5d8a429bd` on
`feat/fuguemax-placement`. The candidate changes in this run add the harness,
runner and documentation only. `git diff d04f151..1701d35 --
libs/collabtext/src libs/collabtext/include` is empty, so these paired ratios
measure run-to-run noise, not an engine speedup.

The five alternating Release pairs completed 54 public cases per run, 12
selected native benchmark methods (14 QtTest passes including setup/cleanup),
and checkpoint recovery at 512 and 2,048 operations. All 45 build and run
commands exited successfully. The runner validated exact public result and
operation fingerprints, attempted/accepted/refused/no-op counts, native work
and state totals, and recovery fixture dimensions.

The median of the five within-case mean-latency ratios, summarized across cases,
was 1.009 for the 54 public workloads; 31 case medians were slower and 23 were
faster. The corresponding median across the 43 native measurement names was
0.998. At 2,048 operations, checkpoint-open and full-replay median ratios were
1.067 and 1.004. The matrix is intentionally a baseline, not evidence of a
performance change. Public case medians ranged from 0.862 to 1.291, and one
2,048-operation full-replay pair reached 1.371; this spread shows why a single
run or aggregate headline is not useful. Later work must compare against these
per-run distributions and explain material regressions.

Reproduce from the repository root with the archived baseline source and a new
output directory:

```sh
git archive d04f151b3a70b161b72c33a8de6895efbd1c13e2 \
  | tar -xf - -C /tmp/collabtext-x01-baseline
python3 tools/run_performance_matrix.py \
  --baseline-root /tmp/collabtext-x01-baseline \
  --output /tmp/collabtext-x01-reproduction
```

The complete raw samples, per-command logs, manifest and aggregates are in
[`raw/`](raw/). `raw/environment.json` records host, compiler, filesystem,
governor, command load, source/input hashes and exit status. `raw/aggregate.json`
contains each run and paired ratio; the JSONL files retain every individual
latency sample. The artifact set occupies about 6.4 MiB. Do not edit these
results in place; later candidate work gets a new output directory and report.

## Limits

This pass measures engine APIs, the private byte-buffer control, codec work,
selected existing native cases and checkpoint recovery. It does not measure
durable allocator persistence separately, consumer adapter latency, allocation
profiles or memory peaks. Those belong to later tasks. The host reported the
`powersave` CPU governor. Its 1-minute load average ranged from 1.92 to 6.27
(median 3.47) on an eight-logical-CPU system during the 45 commands, so
background load and CPU frequency were not controlled. Treat small ratios as
noise until a specific optimization has a mechanism, a measured effect and
clean correctness gates.
