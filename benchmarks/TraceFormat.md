# Benchmark trace schemas v1 and v2

`traces/workloads-v1.json` pins the deterministic workload definitions. Its
SHA-256 is recorded by the paired matrix runner; changing a seed, size, operation,
window, budget or accounting rule creates a new trace version/hash. Do not edit
an old trace after recording results.

`traces/workloads-v2.json` preserves the v1 workload grid and adds a 200-call
aged random-boundary single-character insertion case at 128 and 3,000 retained
single-scalar runs. Pass it with `tools/run_performance_matrix.py --trace
benchmarks/traces/workloads-v2.json`. The runner compiles one harness source
against both the baseline and candidate engines, validates final text and
encoded-operation fingerprints, and records every call separately. The v1
default and its archived hash remain unchanged.

The C++ harness writes JSON Lines using schema `collabtext-performance-samples/1`:

- `header` identifies schema, full/quick profile, seed, percentile rule and
  whether compile-time performance diagnostics are enabled, with a separate
  diagnostics schema identifier when they are enabled.
- `sample` records one timed checked call in integer nanoseconds, with a stable
  workload name, sample index and unit `call`. With diagnostics enabled it also
  records the per-call `fragment_snapshots` counter delta, allowing fallback
  frequency to be localized without using diagnostic timings as performance
  results. Edit workloads may include `byte_offset` to identify the sampled
  position in the fixture. Diagnostic edit samples also include
  `middle_insert_refusals` (`guard`, `nonterminal_predecessor`,
  `missing_successor`, `ambiguous_ancestry`) and `partial_replace` (attempts,
  failures, boundary-rank refusal, then failures at stages 1–5). The stages are
  input guard; fragment/scalar validation; origin/run lookup; placement proof;
  and staged index/tree update. These counters classify fallback work only;
  diagnostic timings remain excluded from performance results.
- `case` records attempted/accepted/refused/no-op counts, setup dimensions,
  visible bytes and available structure counts, plus deterministic final-state
  and encoded-operation fingerprints.
- `limit` records the one intentional default-budget refusal separately from
  accepted-work throughput.

When configured with `COLLABTEXT_ENABLE_PERF_DIAGNOSTICS=ON`, each `case`
record also has a `diagnostics` object containing the fixed counter set from
`crdt/Diagnostics.h`; `diagnostics_schema` versions that counter set. Counters
are reset per case and increment only inside timed calls; setup, expected-result
checks and JSON output are excluded. The
default build compiles each counter update to a no-op and emits
`diagnostics_available: false`. Diagnostic and default runs must preserve case
counts and logical/operation fingerprints. Diagnostic timings are overhead
measurements, not publishable engine timings.

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
