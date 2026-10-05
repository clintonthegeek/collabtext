# Independent review evidence — 2026-10-04

See the [review report](../2026-10-04-reliability-progress-review.md).
Reviewed HEAD was fac1ecbb428c92acca8ed805d6a45fcc42d32951. Baseline runs used that
unmodified source. Final runs used it plus the review source patch; the
[manifest](manifest.json) records exact final source hashes. Implementation commit
IDs are `b3f4f20` (validation) and `bbe8434` (storage/identity), also recorded
in the handoff/task log and registry. Function-list artifacts confirm the named
review regressions are registered in each of the four focal QtTest executables.

Logs retain the CTest portion and invocation header, omitting repetitive build
progress. Each metadata file retains the exact selected test names, compiler,
configuration, representative core command, regex, and reproduction commands.
Generated build-tree logs are not required to read this evidence.

| Run | CTest executables passed | Exit | Portable evidence |
| --- | --- | --- | --- |
| baseline-debug | 62/62 | 0 | [baseline-debug.log](baseline-debug.log) / [metadata](baseline-debug.metadata.json) |
| baseline-release | 62/62 | 0 | [baseline-release.log](baseline-release.log) / [metadata](baseline-release.metadata.json) |
| final-debug | 62/62 | 0 | [final-debug.log](final-debug.log) / [metadata](final-debug.metadata.json) |
| final-release | 64/64 | 0 | [final-release.log](final-release.log) / [metadata](final-release.metadata.json) |
| final-asan | 62/62 | 0 | [final-asan.log](final-asan.log) / [metadata](final-asan.metadata.json) |

Final Debug and ASan exclude **only tst_benchmark and tst_realistic**; Release
includes both, selecting all 64 CTest executables. All three modes use one worker
after a stalled earlier Release attempt was stopped; none infer success from the
stopped build. ASan runs core code with address/undefined instrumentation and
leak checking enabled outside restricted process inspection; no ASAN/LSAN/UBSAN
options or suppressions were set. Default fuzz seed is 20261003.

Reproduce from the worktree with `COLLABTEXT_VERIFY_JOBS=1`, using each metadata
file's recorded regex with `./tools/verify-roadmap.sh debug|release|asan`.
Release uses `'^tst_'`; fast selections are the authoritative CTest name list
minus the two excluded names, not a silent exclusion of failing tests.

Red evidence:

- [validation](red-validation.log): four failing baseline-defect slots.
- [identity/storage](red-storage-identity.log): ten failing baseline-defect slots.
- [quarantine](red-quarantine.log): one failing baseline-defect slot.
- [state link and boundary control](red-state-link-and-boundary-control.log):
  one additional baseline defect and one positive control for a regression in
  the review's own UTF-8 lookup. The first attempted test compile used the wrong
  injection signature; only the subsequent executable failures count as evidence.
- The review's initially wrong seek bias failed seven original CTest executables
  in [Debug](intermediate-debug-boundary-regression.log) and
  [ASan](intermediate-asan-boundary-regression.log). Those failures were corrected;
  the original assertions remain. [Corrected integration](targeted-corrected-integration.log)
  passes all nine selected executables before the final full runs.
- [Sanitizer timing failure](sanitizer-load-failure.log) passed all functional
  assertions and had no sanitizer/leak diagnostic, but two wall-clock assertions
  exceeded their unchanged bounds during Release compilation (10.244/10 s and
  1.009/1 s). The final full sanitizer run occurs after that build, with the
  same source, seed, thresholds, and selection; no failing case is excluded.

[Default evaluation](evaluation-default.log) has five visible XFAILs F4–F8;
[strict evaluation](evaluation-strict.log) uses
`COLLABTEXT_STRICT_EVALUATION=1`, exits 5, and exposes exactly those five failures.
F1/F2 pass through inert GC containment; F3 tests coverage_intersection. A green
CTest total therefore does not mean those future guarantees have landed.

This is Linux/GCC correctness evidence, not general convergence proof, a
performance comparison, power-loss certification, or Windows/macOS verification.
