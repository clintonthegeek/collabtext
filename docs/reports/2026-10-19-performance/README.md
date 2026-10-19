# Evidence for the October 19 performance evaluation

Interpretation, limitations and conclusions: [report](../2026-10-19-performance-evaluation.md).
Engine revision: `d04f151b3a70b161b72c33a8de6895efbd1c13e2`.
No production code was changed.

## Recorded artifacts

- `public_api.cpp`: exact checked harness, including private byte-control cases.
  SHA-256 is recorded in `environment.json` and verified against this copy.
- `public-1.jsonl`, `public-2.log` through `public-5.log`: five runs of that
  harness. The first run was performed before the matrix; its empty stderr is
  `public-1.err`. Only runs 2–5 have process load/timing metadata in the runner.
- `native-1.log` through `native-5.log`: five QtTest processes, each with 12
  selected methods plus initialization/cleanup. Benchmark wall times in rows
  are measured regions, not process wall time.
- `recovery-512.log`, `recovery-2048.log`: one source-history experiment each,
  five alternating warm opens per mode internally.
- `bulk_limit.cpp`, `bulk-limit.log`: untimed default-budget refusal probe,
  including unchanged text/counter checks on refusal. Compiled and run after
  the timing matrix, with the same flags/library.
- `environment.json`, `core-flags.txt`, `configure.log`: revision, environment,
  matrix commands/status/load, and build settings. `/tmp` was tmpfs. Recovery
  temporary directories use the host temporary directory as well.
- `run-recorded.py`, `progress.jsonl`: original machine-specific matrix runner
  and its command results. Preserved as provenance; use `repeat.py` to reproduce.
- `summary.json`: all 30 public and 43 existing benchmark rows, with the median,
  minimum and maximum of the five per-run measurements. Invariants are checked
  by `aggregate.py`. Null RSS means unavailable, not zero.

The public logs contain per-run latency summaries, not raw individual samples.
Process RSS fields include allocator reuse and prior cases and are not an engine
allocation measurement. The report explains the native driver's weaker local
attempt accounting. All five runs are retained without outlier selection.

## Reproduce

Requirements: Linux, C++20 compiler, CMake, Qt6 Core/Test, Python3, and the normal
project build dependencies. Use the pinned engine revision in a separate checkout
for a historical reproduction; record a new revision for future comparisons.
Do not build concurrently with timed runs. These commands create binaries and
results outside the repository; keep `CCACHE_DIR` inside the checkout.

From the repository root:

```sh
export CCACHE_DIR="$PWD/.cache/ccache"
cmake -S . -B /tmp/collabtext-perf-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCOLLABTEXT_BUILD_BENCHMARKS=ON \
  -DCOLLABTEXT_BUILD_TESTS=OFF \
  -DCOLLABTEXT_BUILD_QT=OFF \
  -DCOLLABTEXT_BUILD_APPS=OFF
cmake --build /tmp/collabtext-perf-build \
  --target tst_benchmark tst_checkpoint_recovery_benchmark -j4
python3 docs/reports/2026-10-19-performance/repeat.py \
  "$PWD" /tmp/collabtext-perf-build /tmp/collabtext-perf-repeat
```

The output directory must not already exist. `repeat.py` compiles both harnesses
before timing, records machine/load/revision/compiler information, runs five
native/public pairs and the two recovery sizes, runs the limit probe, and
aggregates. It stops on nonzero exit or timeout. A different machine, compiler,
filesystem, power state, run order or load can change results. The repeat runner
puts the first public run after native-1; the original first public run preceded
native-1. That order difference is explicit, not an exact environmental replay.
Build type and sanitizer settings must be verified in the recorded core flags;
the script does not turn an arbitrary existing build into Release.

Recompute the checked aggregation of committed evidence without running timings:

```sh
python3 docs/reports/2026-10-19-performance/aggregate.py \
  docs/reports/2026-10-19-performance
```

Benchmarks are opt-in executables, never CTest selections. No correctness test
suite or sanitizer certification is implied by this measurement-only procedure.
