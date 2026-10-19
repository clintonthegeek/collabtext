# Benchmark trace schema v1

`traces/workloads-v1.json` pins the deterministic workload definitions. Its
SHA-256 is recorded by the paired matrix runner; changing a seed, size, operation,
window, budget or accounting rule creates a new trace version/hash. Do not edit
an old trace after recording results.

The C++ harness writes JSON Lines using schema `collabtext-performance-samples/1`:

- `header` identifies schema, full/quick profile, seed and percentile rule.
- `sample` records one timed checked call in integer nanoseconds, with a stable
  workload name, sample index and unit `call`.
- `case` records attempted/accepted/refused/no-op counts, setup dimensions,
  visible bytes and available structure counts, plus deterministic final-state
  and encoded-operation fingerprints.
- `limit` records the one intentional default-budget refusal separately from
  accepted-work throughput.

A failure exits nonzero with an explanatory message. A failed process may emit
partial samples; its paired run must be retained and the case is not a passing
measurement. Runners must reject duplicate/missing cases, sample-count changes,
nonzero unexpected refusal/no-op counts, and different logical/operation
fingerprints across the pinned baseline and candidate.

Timing starts immediately before and ends immediately after the named checked
engine/codec call. Seed construction, expected-string updates, result checks,
projection comparison and output formatting are outside the sample. The exact
number of timed work items is the `samples` value in the corresponding `case`
record. No means are inferred from total process time.
