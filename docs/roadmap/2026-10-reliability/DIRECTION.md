# Current direction

As of 2026-10-11. Branch `feat/fuguemax-placement`, worktree
`/home/clinton/dev/collabtext/.worktrees/reliability-v2`. This file selects the
active lane; TASKS.json owns exact status and dependencies. Historical task
steps and reports do not override these instructions.

The user has authorized finishing Wave's Core requirements, including deep
changes and optimization, before Markoff starts. Q01–Q05 and Q04a are complete; do not repeat them. The next task is external Q06 in
[09-wave-core.md](09-wave-core.md). The normative capability contract is
[wave-core-adoption.md](../../specs/wave-core-adoption.md); the consumer-facing
instructions are [wave-core-response.md](../../handoff/wave-core-response.md).
Q06 is external Wave acceptance, not something an upstream fixture can certify.

Production ordering remains native FugueMax for text and IdList. P-series,
L-series and S-series tasks are complete; E07a/E07b are complete. Do not repeat
those tasks or resurrect Locator. Recovery, local undo and public pin composition
exist. G03/G06 remain in progress; G07/G08 and remaining A/X/E work are pending.
Certified source-level collection is not a turnkey installed consumer service.
Initial Wave/Markoff adoption retains history and disables physical GC/pruning.

The earlier candidate is the immutable local tag
`collabtext-adoption-2026-10-11` at `0d21670896d02dda3dc44d18d37f9657935333ed`.
The new candidate is `collabtext-wave-core-2026-10-11`, package `0.2.0-alpha.3`. Neither local tagging nor package tests imply publication or approval
by Wave. C++ ABI remains alpha; durable readers for supported CTBO/CTPT bytes are
separate obligations. CTBO/0002 and CTPT/1 stay readable; marker operations use
CTBO/0003 and marker tokens CTPT/2. Pre-vNext CTBO/0001 stays unsupported.

One executor. Complete specification/model gates first, preserve independent
oracles, use installed public headers for consumer traces, and record nonempty
Debug/Release/sanitizer/package results. Benchmark separately before performance
claims. Optimizations must preserve operation identity, ordering, compensation,
coordinates, callback behavior and retained archive semantics.

The candidate report and handoff provide the exact pin and smallest installed test. Wave must
rerun its own adapter and EventStore probes; Q06 stays open until its evidence
exists. Do not edit Wave/Markoff, message consumers, merge master or publish
without explicit instruction. Markoff's later acceptance remains a merge gate
unless the user explicitly changes that policy. Remaining G/A/X/E tasks follow
their actual registry dependencies; Q05 does not imply those tasks are complete.


2026-10-11 clean-cut amendment: the user requires no compatibility for earlier
alpha stores. Superseded readers and source-compatibility parameters are removed
once current replacements pass. CME1 and unbound checkpoint engine section 1 are
unsupported; presence-only SyncManager no longer takes an ignored engine pointer.
CTBO/0002–0003 and CTPT/1–2 remain current writer formats; the reader obligation
above starts with this adoption candidate for newly durable Wave data. This is
not an obligation to upgrade earlier experimental stores.
