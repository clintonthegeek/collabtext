# Installed consumer checks

This is an independent CMake project that finds the installed collabtext package.
Core tests link only CollabText::Core and public installed headers. The package
verifier installs to prefix A, moves it to B, builds this project, checks include
and link isolation, and runs nonempty CTest selections.

`consumer_wave_core` adds the alpha.3 Wave trace: typed zero-width markers,
Unicode/exact gaps, concurrent delivery, fresh replay, restored local undo,
deterministic canonical projection and retained literal CTBO/0002–0003/CTPT/2
archives. The same source is `tst_wave_core` in producer correctness builds.
The older Wave-shaped and Markdown-shaped probes retain their separate contracts.

```sh
./tools/verify-consumer-package.sh --mode release --gate live --components core
```

These upstream-shaped traces do not approve Wave's production adapter or its
EventStore transactions. Wave reruns its own installed probes and records its
stage-4 acceptance against the exact new candidate. See
[the handoff](../../docs/handoff/wave-core-response.md).
