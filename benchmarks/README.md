# Performance evaluation

The checked public-engine harness is opt-in. It is not registered with CTest
and does not run in the default build. `BenchmarkRunner.cpp` measures individual
public UTF-16 facade calls, a private byte-buffer control, boundary/attachment
and marker reads, identity-list operations, codec work, and bulk edits with explicit undo
budgets. Each case checks its resulting state and emits integer-nanosecond
JSONL samples. See [TraceFormat.md](TraceFormat.md) and
[`traces/workloads-v1.json`](traces/workloads-v1.json) for the frozen workload
recipe.

To reproduce X01, extract the pinned baseline without changing the checkout,
then run the paired matrix from the candidate checkout:

```sh
mkdir -p /tmp/collabtext-x01-baseline
git archive d04f151b3a70b161b72c33a8de6895efbd1c13e2 \
  | tar -xf - -C /tmp/collabtext-x01-baseline
python3 tools/run_performance_matrix.py \
  --baseline-root /tmp/collabtext-x01-baseline \
  --output /tmp/collabtext-x01-results
```

The full run alternates five baseline/candidate pairs by default. It builds the
standalone harness for both revisions, runs the selected native QtTest
benchmark cases and checkpoint-recovery fixtures, validates operation/state
fingerprints and work counts, then writes `aggregate.json` and
`environment.json` alongside raw JSONL and test logs. The output directory
must be new. `--smoke` runs one quick public-harness pair to validate setup; it
is not performance evidence. A benchmark build needs Qt 6.8 Core and Test,
zstd, CMake and a C++20 compiler. The runner places ccache data under the
candidate checkout's `.cache/ccache`.

Build the public harness alone with:

```sh
cmake -S . -B build-perf -DCMAKE_BUILD_TYPE=Release \
  -DCOLLABTEXT_BUILD_TESTS=OFF -DCOLLABTEXT_BUILD_BENCHMARKS=ON \
  -DCOLLABTEXT_BUILD_QT=OFF -DCOLLABTEXT_BUILD_APPS=OFF \
  -DCOLLABTEXT_BUILD_FILE_TRANSPORT=ON
cmake --build build-perf --target collabtext_performance_benchmark
build-perf/libs/collabtext/collabtext_performance_benchmark --quick
```

Do not use these measurements to claim a speedup until the full paired matrix
passes its correctness gates. Compare per-run distributions and retain the raw
output directory with the evaluation report.
