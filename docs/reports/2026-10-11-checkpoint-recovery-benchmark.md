# Checkpoint recovery timing sample

This is an early, synthetic comparison for G05. Correctness is checked on every
measured open: both startup paths must reconstruct the exact expected text and
report a ready session. Each number is the median of five alternating opens of
cloned durable state.

## Workload and environment

- 512 or 2,048 sequential local edits, each appending one ASCII byte.
- The checkpoint and full-replay branches use copies of the same shared log and
  private actor state. The checkpoint branch also has a validated published
  checkpoint; the control has no configured checkpoint store.
- Timed region: `DocumentSession::open` only. Workload creation, checkpoint
  publication, text verification, and close are outside that region.
- Release build: GCC 16.2.1, `-O3 -DNDEBUG`, x86_64, Intel Core i7-1185G7,
  8 reported CPUs.
- Five repeats per branch, alternated to reduce fixed branch-order bias.

## Results

| Operations / text bytes | Checkpoint median | Full replay median | Checkpoint / full |
|---:|---:|---:|---:|
| 512 | 6.98 ms | 24.78 ms | 0.282 |
| 2,048 | 29.96 ms | 382.29 ms | 0.078 |

Reproduce with:

```sh
cmake -S . -B build-checkpoint-benchmark \
  -DCMAKE_BUILD_TYPE=Release \
  -DCOLLABTEXT_BUILD_TESTS=OFF \
  -DCOLLABTEXT_BUILD_BENCHMARKS=ON \
  -DCOLLABTEXT_BUILD_QT=OFF \
  -DCOLLABTEXT_BUILD_APPS=OFF
cmake --build build-checkpoint-benchmark \
  --target tst_checkpoint_recovery_benchmark -j8
./build-checkpoint-benchmark/libs/collabtext/tst_checkpoint_recovery_benchmark 512
./build-checkpoint-benchmark/libs/collabtext/tst_checkpoint_recovery_benchmark 2048
```

These measurements show a useful reduction for this local append workload; they
are not a release performance guarantee. They do not cover remote interleavings,
large tombstone histories, undo-heavy sessions, cold storage, or high producer
counts. Timing distributions beyond five-run medians remain to be collected.
