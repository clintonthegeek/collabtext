# Current direction

As of 2026-10-20. Performance work is executing in the isolated
`.worktrees/reliability-v2` worktree on `feat/fuguemax-placement`; preserve root
`master` and its unrelated changes. TASKS.json owns exact status/dependencies;
completed phase procedures are history, not work to repeat.

The selected local lane is **[07 — measured performance](07-performance.md)**.
X01, X02, X07a, X04a, X07, and X07b are complete. X07 delivered a 3.11× median
improvement on public 1-MB replacement, with a measured byte-control regression
above the 10% guard at 100 KB and narrowly at 1 MB. X07b's compact-summary
prototype missed the cumulative 1.10 byte-control thresholds and was reverted;
the exact-width X07 representation remains. See the
[X07b report](../../reports/2026-10-20-x07b/README.md). The next task is **X04**:
implement the modeled origin and deletion indexes with checked updates.
The user requested an implementable optimization plan after
[the new evaluation](../../reports/2026-10-19-performance-evaluation.md).
The frozen source baseline is `d04f151b3a70b161b72c33a8de6895efbd1c13e2`.
Follow the exact serial order and measurement/disposition rules in
[the performance target contract](../../specs/vnext-performance-targets.md).
The frozen source baseline is not a substitute for the paired harness: X01 has
now established that checked harness, and each optimization must retain its
correctness and measurement gates.

Wave Q01–Q05/Q04a and Markoff M01–M05 are upstream complete. Q06/M06 remain
external production acceptance on the exact candidate, independent of local
performance work. Earlier alpha.3/alpha.4 probe findings are historical; current
consumer evidence is recorded in the
[master integration review](../../reports/2026-10-19-master-integration-review.md).
Do not edit consumers or infer their acceptance from upstream fixtures.

Current integrated package: **0.2.0-alpha.5**, capability manifest **v17**,
`collabtext-integration-2026-10-19`. All earlier candidate tags remain immutable.
No package, manifest or tag change is needed to start performance work.

Current normative extensions:
[Wave contract](../../specs/wave-core-adoption.md),
[Markoff contract](../../specs/markoff-core-adoption.md).
Consumer responses:
[Wave](../../handoff/wave-core-response.md),
[Markoff](../../handoff/markoff-core-response.md).
Evidence: [Markoff candidate report](../../reports/2026-10-11-markoff-core-candidate.md).

Native FugueMax remains the text/IdList production ordering. Markers share the
text sequence. Ordered list projections expose full insertion identities;
terminal counter gaps are scoped host-committed metadata, not placeholder edits.
Current durable readers: CTBO/0002–0003, CTLO/0002, CTGP/0001 and CTPT/1–2.
No earlier experimental-store migration obligation is assumed. Obsolete CME1,
unbound checkpoint engine section 1 and ignored Qt constructor paths are gone.
Checkpoint current writers use engine sections 2/3 or gap-bearing section 4;
general private-store upgrade compatibility is not promised. Alpha ABI is not stable.

P/L/S and E07a/E07b tasks are complete. G03/G06 remain in progress; G07/G08 and
remaining A/E tasks remain governed by their incomplete registry entries.
The revised X graph explicitly separates Core optimization from G06/G08 and
X09 transport scheduling; removing an edge does not complete those gates.
Initial adoption retains history and disables physical collection/log pruning.
Source collection composition is not turnkey installed consumer GC. Do not
claim broad performance certification or logarithmic edit complexity.

One executor. Preserve independent models, typed refusal and allocator authority.
Consumer-owned fixtures must use installed public headers. Record nonempty
Debug/Release/leak-enabled sanitizer/package evidence. Do not weaken tests or
suppress findings; time-based checks need an uncontended verification run.

The previous master integration/publication is complete. Do not edit consumers,
message them, push, publish or perform another merge without a new explicit
instruction. Provide local evidence; consumers supply real adapter/transaction
acceptance. Q06/M06 are not complete merely because upstream-shaped tests pass.
