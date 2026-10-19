# Evaluation artifacts — 2026-10-03

These files support the [full evaluation](../../2026-10-03-library-evaluation.md) of revision `046d375`. No production source changes were made.

## Build and run

Run from the repository root. The examples use a temporary build directory; Qt 6.8+ and zstd are required by the current project build.

```sh
cmake -S . -B /tmp/collabtext-review-release -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/collabtext-review-release -j 6

QT_QPA_PLATFORM=offscreen ctest \
  --test-dir /tmp/collabtext-review-release --output-on-failure -j 4 \
  -E 'tst_benchmark|tst_realistic'

QT_QPA_PLATFORM=offscreen \
  /tmp/collabtext-review-release/libs/collabtext/tst_realistic

QT_QPA_PLATFORM=offscreen \
  /tmp/collabtext-review-release/libs/collabtext/tst_benchmark \
  single_replica_throughput single_replica_large_doc edit_patterns edit_sizes \
  multi_replica_convergence realistic_3_client_throughput reconnect_sync_cost \
  undo_redo_cycle gc_effectiveness
```

Compile and run the additional investigation harnesses:

```sh
c++ -std=c++20 -O2 \
  -I libs/collabtext/src -I libs/collabtext/include \
  docs/reports/2026-10-03-evaluation-artifacts/probes.cpp \
  /tmp/collabtext-review-release/libs/collabtext/libcollabtext.a \
  -lzstd $(pkg-config --libs Qt6Core) \
  -o /tmp/collabtext-review-probes
/tmp/collabtext-review-probes

c++ -std=c++20 -O2 \
  -I libs/collabtext/src -I libs/collabtext/include \
  docs/reports/2026-10-03-evaluation-artifacts/latency.cpp \
  /tmp/collabtext-review-release/libs/collabtext/libcollabtext.a \
  -o /tmp/collabtext-review-latency
/tmp/collabtext-review-latency
```

`probes.cpp` creates a uniquely named directory in the system temporary directory and removes that directory at normal exit. It prints the observed values; it deliberately does not assert that the defects are fixed or return failure for a mismatch. Normal Buffer/IdList undo-redo controls are included. Convert the problematic sequences into Qt regression tests when implementing fixes.

## Measurement conditions

Linux x86_64; Intel Core i7-1185G7; GCC 16.2.1; Qt 6.11.2; system zstd; CMake Release library; standalone harness compilation at `-O2`. The recorded repeat benchmark ran after the build and realistic tests finished. CPU frequency was not fixed. No competing libraries were benchmarked.

Random-insert percentiles reflect a buffer growing more fragmented through 500 edits, rather than a fixed state. The IdList run grows from empty to 1,000 entries. Results are single-run observations; hardware, scheduler activity, and compiler settings affect timings. The high replica ID wire-size and content/recovery defects are behavioral observations independent of benchmark speed.

The realistic test suite prints randomized seeds. The existing test suite does not offer a uniform seed replay argument. The nine selected benchmark functions use their existing code, including the timing/label problems called out in the full report. Initialization and cleanup contribute to QtTest's reported pass counts.

`benchmarks-release.log` is the repeat run used by the report, not the earlier run that overlapped the realistic tests. `latency-release.log` contains the standalone percentile run. `tests-release.log` and `realistic-release.log` contain the executed existing correctness suites. `probes-release.log` contains the exact observed behavioral probe output.
