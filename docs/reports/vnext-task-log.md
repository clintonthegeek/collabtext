# Reliability roadmap implementation log

Worktree: `/home/clinton/dev/collabtext/.worktrees/reliability-v2`.
Branch: `roadmap/reliability-v2`. Source baseline: `046d375`; approved native
engine roadmap: `374f93c`. Task status and full implementation hashes are in
[TASKS.json](../roadmap/2026-10-reliability/TASKS.json).

## 2026-10-03 — V01: reproducible verification

Commit `9d5541b` adds an opt-in ASan/UBSan CMake configuration and
`tools/verify-roadmap.sh`. The runner configures the correct worktree, asks
CTest to select tests, builds their corresponding targets, executes them with
offscreen Qt, and retains commands/compiler/flags/counts/revision/logs/status.
Default build concurrency is two. Unsupported sanitizer setups fail explicitly;
an empty test selection cannot succeed.

Verification: the original fast suite passed **51/51** in both Debug and
Release. The Buffer/IdList/clock sanitizer smoke selection passed **3/3** with
ASan, UBSan, and LeakSanitizer enabled. An isolated temporary CMake fixture using
the actual runner returned exit **8** for an intentionally failing executable
and **3** for a zero-test selection. Restricted-sandbox sanitizer runs passed
test assertions but failed LeakSanitizer process inspection; the unrestricted
rerun passed with no suppressions. This is an environment restriction, not a
leak finding.

The runner now supplies the planned commands; Qt/zstd are still required until
the later packaging tasks. No public engine API changed in V01.

## 2026-10-03 — V02: evaluation regressions

Commit `cebabc3` registers `tst_evaluation_regressions`. Named tests encode
F1–F8, with both Buffer and IdList for insertion redo and remote deletion undo.
Ordinary remote undo/redo, fresh timestamps, fixture setup, decoding, and exact
content controls are normal passing assertions. No timing sleeps are required.
Expected-failure messages link each defect to its finding and fixing task.

Verification: Debug, Release, and ASan/UBSan/LeakSanitizer runs passed this
executable. The initial default Debug run reported **16 QtTest passes and 11
XFAIL assertions**. With `COLLABTEXT_STRICT_EVALUATION=1`, the same assertions
became **11 actual failures**, process exit **11**. The local IdList redo case
already passed; the remote deletion test clears A's incidental local insertion
pin while B retains deletion undo, exposing the actual compaction defect.

These are explicit known failures, not a claim that all lifecycle behavior is
correct. A fixing task removes its own marker; unexpected passes fail normal
QtTest execution. F9/F10 belong to later durability and validation work.

## 2026-10-03 — V03: contain unsafe reclamation

Commit `3f4a210` implements containment; its full hash is recorded in TASKS.json. `collect_garbage()` and
`compact(Global)` now retain every fragment/entry and return zero in both
primitives. The unsafe sweep/coalescing path was removed. Public comments
describe temporary retention and transport acknowledgements as receipt
information that cannot authorize deletion. Existing reclamation expectations
were replaced with retention checks; undo, convergence, and anchor-context
assertions remain, including remote undo after local history trimming.

Before the fix, removing F1/F2's five expected-failure markers caused exactly
**five actual restoration failures**; both revised GC executables also failed
their retention assertions against the old implementation. After the fix, all
six primitive restoration rows pass in strict mode (**8 QtTest passes** counting
initialization/cleanup). The full regression executable has **16 passes and six
remaining XFAIL assertions**, F3–F8; its entire strict run exits **6**.

Verification: the targeted Debug GC/regression selection passed **3/3**. The
final fast suite passed **52/52** in both Debug and Release, excluding only
`tst_benchmark` and `tst_realistic`. Targeted ASan/UBSan/LeakSanitizer GC and
regression checks passed **3/3**, unrestricted and without leak suppression.
These are Linux/GCC runs; historical randomized suites have not yet been made
replayable (V04), and no new performance claim follows from this work.

Physical reclamation is intentionally unavailable until G06. This fixes the
F1/F2 data-loss sequences by preserving state; it does not establish bounded
memory or implement persistent history, recovery, membership, retirement, or a
GC certificate. Engine state may grow in the meantime.

## 2026-10-03 — V04: replayable, reducible randomized failures

```text
Task: V04
Status: complete
Baseline: cbe72eafd7f8bb324d4056339808ce31489ff160
Implementation commits: e5eb6f5, 97a306c, plus this documentation commit
Scope: libs/collabtext/tests/TestSeed.h (new), libs/collabtext/tests/TraceHarness.h
       (new), libs/collabtext/tests/tst_trace_reduction.cpp (new),
       tools/reduce-trace.cpp (new), CMakeLists.txt, libs/collabtext/CMakeLists.txt,
       tst_fuzz.cpp, tst_idlist_fuzz.cpp, tst_realistic.cpp
Regression: tst_trace_reduction (new) — same-seed trace equality, faithful replay,
       file round-trip, injected lost delivery that reduces to a smaller valid
       reproduction; plus the previously hidden tst_realistic
       networksim_compact_all failure
Verification: seed 42 twice → byte-identical traces for tst_fuzz, tst_idlist_fuzz
       and tst_realistic; all 28 recorded scenarios replay and converge;
       Debug 3/3, Release 3/3, sanitizer 3/3 targeted; Debug 53/53, Release 53/53,
       sanitizer 53/53 fast; strict evaluation executable still exits 6
Artifacts: docs/reports/2026-10-03-roadmap-kickoff-artifacts/v04-*
API/store changes: none in the library. Trace format v1 is a test-only artifact
       format described in TraceHarness.h.
Known limits: reduction preserves the failure predicate, not the mechanism; a
       smaller trace may diverge for a different reason. Two tst_fuzz scenarios
       are not traced yet (multi-range local edits; timed pinpoint measurement).
Next ready tasks: V06, V07, V08 (V05 now has its V04 prerequisite)
```

### What changed

The three randomized suites drew a fresh `std::random_device` seed per test, so a
failing run printed a seed that could not reproduce it and left nothing else
behind. They now derive each scenario's seed from `COLLABTEXT_TEST_SEED`, parsed
as a checked uint64 (or the word `random`) with the documented fixed default
`20261003` used by ordinary verification and CI, and print the campaign seed,
the effective per-scenario seed, and whether the source was fixed or random.

Every edit, delivery, disconnect, reconnect, undo, redo, restart, and GC request
in those suites is recorded in a versioned event trace. The trace records
decisions — replica, byte range, visible index, delivered operation index — and
never entropy, so replay re-executes the same choices and the engine re-derives
identical Lamport identities. Three models are supported: `buffer`, `idlist`, and
`networksim`. With `COLLABTEXT_TRACE_OUTPUT` set, each scenario emits its trace,
which is what makes two runs comparable byte-for-byte; a failing scenario also
saves its trace even when only the failure path is reached.

Delta reduction removes contiguous event chunks and keeps a removal only when the
candidate still replays to completion, still delivers to every replica the failing
trace did, and still fails the chosen predicate. Validity is what preserves
identity and causality: a candidate whose remaining edit ranges, IdList visible
indices, or delivery references no longer apply is rejected, never clamped.
`tools/reduce-trace` exposes this as a CLI with `--replay`, `--predicate`, and
`--out`, and fails loudly (nonzero exit, distinct codes) rather than reducing a
passing trace.

### Acceptance evidence

Two runs at seed 42 produced byte-identical traces for all three suites
(`v04-seed42-run-a/` vs `v04-seed42-run-b/`, `v04-seed42-identical.txt`); a third
run at the default seed differs in every scenario (`v04-seed42-differs.txt`).
All 28 recorded scenarios replay through `tools/reduce-trace --replay` to
completion and converge.

The injected-failure proof is in `v04-injected-failure/`: a 42-event trace with
one delivery marked lost diverges on replay; reduction removes 26 events; the
16-event reduction still replays to completion and still diverges; both the
in-test path and the CLI produce it, and the unreduced trace is saved first.

The fast suite grew from 52 to 53 executables (`tst_trace_reduction` is new) and
passes 53/53 in Debug, Release, and the sanitizer mode. Leak checking was active
and unsuppressed: this environment is not the restricted sandbox V01 hit, which
was confirmed by watching LeakSanitizer report a deliberate leak from a throwaway
program. `tst_realistic` is excluded from the fast selection and was run
separately (11/11, about 160 s in Debug).

### A pre-existing failure this migration exposed

`tst_realistic::networksim_compact_all` asserted that compaction removed
tombstones (`removed > 0`). That has been false since V03 made `compact()` retain
everything, and it stayed hidden because the long-running suite is excluded from
the fast CTest selection. It now asserts the containment contract instead —
tombstones retained, invariants and convergence intact after `collect_garbage()`
and `compact_all()` — and `long_partition_with_gc` checks the same retention
property across both replicas.

### Limits

Reduction preserves the failure predicate, not the failure mechanism: a smaller
trace may diverge for a different reason than the original did, and the retained
delivery-target rule is a heuristic guard against fully degenerate reductions
rather than a proof. Reduction is also conservative about causality by design —
removing an edit invalidates the delivery references that named its operation —
so a trace whose deliveries all point at late operations cannot shrink far.

`restart` is modelled as replay-based recovery from the replica's own decision
journal. That reproduces the clock, the undo *and* redo stacks, and the document,
but it is a harness model: durable segment and checkpoint recovery remain L03 and
L05, and a scenario that seeds a replica out of band must record that seeding as
trace events or its replay will not match.

Trace format v1 expresses one byte range per local edit, so
`rapid_multi_range_edits` (2–3 disjoint ranges in one operation) is not yet
traced; `large_document_pinpoint_edits` keeps its timing threshold until V06.
Both remain seeded and announce their seed. Nothing here measures performance,
certifies another operating system, or completes the remaining roadmap.


## 2026-10-03 — V05: correct simulator partitions and ordering configuration

```text
Task: V05
Status: complete
Baseline: 3ec89c2addc34fe54a1a63200a5af497cc9f0710
Implementation commits: 5f9f759, plus this documentation commit
Scope: libs/collabtext/src/crdt/NetworkSim.h, NetworkSim.cpp,
       libs/collabtext/tests/tst_network_sim.cpp (new),
       libs/collabtext/tests/tst_realistic.cpp, tests/TraceHarness.h,
       libs/collabtext/CMakeLists.txt
Regression: tst_network_sim (new) — the partition and ordering contract; plus the
       realistic scenarios, which previously passed through a partition without
       asserting anything about it
Verification: seeds 42 and 20261003 → byte-identical traces on two runs each, all
       11 recorded scenarios replay and converge; tst_network_sim 16/16; Debug
       54/54, Release 54/54, sanitizer 54/54 fast; tst_realistic 13/13
Artifacts: docs/reports/2026-10-03-roadmap-kickoff-artifacts/v05-*
API/store changes: NetworkSim gained block_link/unblock_link, is_online,
       is_link_blocked, queued_inbound, queued_outbound, and delivery_log.
       Trace format v1 gained `block`/`unblock` events.
Known limits: the harness models latency, duplication, and partitions, not a
       real transport; monotonicity is per (peer, producer); drain() heals
       partitions by design
Next ready tasks: V06, V07, V08
```

### What changed

`disconnect(r)` used to stop deliveries *to* `r` while still delivering `r`'s own
operations to every online peer immediately. A replica could broadcast while
"offline", and a partitioned sender's work was visible to online peers before it
healed — an asymmetry no transport exhibits. Disconnects are now symmetric:
local editing keeps working, operations produced offline are queued and released
on reconnect, and operations already scheduled for the replica stay in its inbox
until it is online again.

`allow_reorder` was declared and never read, so every delivery took an
independent random latency and a producer's own operations could overtake each
other. It now governs per-(peer, producer) monotonicity: with `allow_reorder`
false an operation never becomes due before the previous one from the same
producer, latency stays random, and different producers still interleave.
Retransmissions are scheduled on the same lane, so a duplicate cannot overtake
the operation it duplicates.

Asymmetric loss has its own API rather than overloading disconnect:
`block_link(from, to)` holds one direction in the receiver's inbox while the
reverse keeps flowing, and `unblock_link` releases the held operations without
rescheduling them.

Writing the tests also exposed a real bug: `drain()` reset the clock to a fixed
value, so anything scheduled after an earlier drain was stranded and never
delivered. The clock now only moves forward. That bug was reachable before this
task too, by any scenario that called `drain()` twice with operations in flight.

### Acceptance evidence

`tst_network_sim` adds 16 cases pinning the documented contract: inbound and
outbound blocking, invisibility until heal, exactly-once delivery of queued
work, redundant delivery when `duplicate_probability` is 1.0, monotone lanes with
and without duplicates, per-seed determinism, and one-directional link blocking.

`tst_realistic` no longer passes through a partition unnoticed. Each partition
scenario pins the disconnected replica's text from the moment it went offline;
`long_partition_with_gc` asserts that no delivery crossed the partition in either
direction (its own documents do change — an edit strategy can delete most of a
document during 500 edits — so the assertion is about delivery, not length). Two
scenarios were added for an asymmetric link partition and a monotone transport.
13/13 pass at seeds 42 and 20261003, with byte-identical traces across two runs
at each seed and all 11 recorded scenarios replaying to completion and
converging.

The fast suite is now 54 executables (`tst_network_sim` is new) and passes 54/54
in Debug, Release, and the sanitizer mode.

One investigation worth recording: an early replay of a realistic trace aborted
inside a Buffer fragment assertion. It was a stale `reduce-trace` binary linked
against the pre-V05 library, not an engine defect — an incremental replay
compared against the recorded run matched every one of 813 intermediate states.
The lesson is that a partially rebuilt tool can look like a correctness bug;
rebuild the tool before believing a crash from it.

### Limits

NetworkSim is a test harness, not a transport. It models latency, duplication,
partitions, and ordering; it says nothing about TCP, Syncthing, or any real link.
Monotonicity is a per-(peer, producer) lane property, with no cross-producer
guarantee. `drain()` heals every partition by design, so it can assert only
post-heal convergence, never convergence while partitioned. These are Linux/GCC
runs and make no performance or cross-platform claim.


## 2026-10-04 — V06: honest benchmark timing, units, correctness, memory labels

```text
Task: V06
Status: complete
Baseline: 1a51d44 (V05 documentation commit)
Implementation commits: 74732ca, plus this documentation commit
Scope: libs/collabtext/tests/BenchmarkSupport.h (new),
       libs/collabtext/tests/tst_benchmark.cpp
Regression: measurement_reports_carry_units_and_honest_availability (new) for the
       report contract; catch_up_timed_region_contains_the_work (new) for the
       timing repair; the divergence and reconnect assertions inside
       multi_replica_convergence, multi_replica_hotspot,
       realistic_3_client_throughput, and reconnect_sync_cost
Verification: 21/21 benchmark cases pass in Release (~60 s) and Debug; fast suite
       54/54 in Debug, Release, and sanitizer mode
Artifacts: docs/reports/2026-10-03-roadmap-kickoff-artifacts/v06-*
API/store changes: none. BenchmarkSupport.h is test-only infrastructure.
Known limits: no corrected figure is comparable to the old mislabelled values;
       process RSS is not engine memory; reclamation remains disabled, so no GC
       speedup can be measured
Next ready tasks: V07, V08
```

### What changed

The benchmarks emitted numbers that could not be read as measurements.

**Units.** `multi_replica_convergence` and `multi_replica_hotspot` wrote the
*total* convergence time into a field labelled `ns/op`, so anyone comparing them
against the genuine per-edit figures was comparing different quantities. Every
result now reports its wall duration, the named work count it came from, and
`source_edits` and `remote_applications` as separate counts. "Ops/sec" is
derived from those two numbers and never printed alone.

**Correctness as an assertion.** Both multi-replica benchmarks printed
`converged: NO` and still passed. A benchmark that reports a correctness failure
as a log line is worse than no benchmark, because it reads like evidence. They
now fail on divergent content, as do the realistic 3-client session and the
reconnect benchmark.

**Timing regions.** `reconnect_sync_cost` reconnected, ticked, and *then* started
the clock — it measured the leftovers of work already done, which is why a bulk
catch-up reported a near-zero "sync cost". The timed region now contains
reconnect, delivery of everything that piled up while partitioned, and the
deferred retry, and the benchmark asserts that operations were applied inside
the region. A new controlled case (`catch_up_timed_region_contains_the_work`)
recreates an identical partitioned network from a recorded trace, times the
catch-up, and asserts nonzero work and nonzero duration with **no speed
threshold** — a threshold on shared CI hardware measures the hardware.

**Memory.** `mem_delta` was zero in every row because nothing ever set it, and
the RSS helper returned 0 when `/proc` was unreadable, so "no data" printed as
"0 KB". Metrics are now optional: absent prints as `absent` and `null` in JSON.
RSS is labelled `process_rss` and never presented as engine memory; engine size
is reported structurally as fragment/entry, tombstone, and visible/deleted byte
counts. A sampled RSS difference is labelled a delta, because "no change" is not
the claim "0 KB of memory".

**Comparisons that compared the wrong things.** The GC before/after benchmark
measured one buffer before and after the call, conflating the call with a warm
allocator and a different fragment layout. Both sides now execute the same
recorded workload on independently recreated engines, and the rebuild is
verified to reproduce the recorded tombstone and fragment counts — a rebuild that
silently dropped the tombstones would have compared an empty document with
itself, which is exactly what happened on the first attempt.
`gc_under_sustained_editing` was rebuilt the same way: one recorded session, two
recreated runs.

Timing moved to `steady_clock` throughout.

### Acceptance evidence

A new test asserts the report format itself: units spelled out, work counts
named, absent metrics null rather than zero. It costs milliseconds and must not
be weakened to accommodate a measurement.

`gc_effectiveness` had been failing since V03 — it asserted that compaction
removed tombstones. Like `tst_realistic` before it, it was hidden by the
fast-suite exclusion. It now asserts the containment contract: nothing is
reclaimed, fragment and tombstone counts are unchanged, and the benchmark says
out loud that no reclamation speedup is claimed.

21/21 benchmark cases pass in Release (~60 s) and Debug; the fast suite is 54/54
in Debug, Release, and sanitizer mode. `v06-report-table.md` holds the corrected
53-row report table with every unit spelled out, and
`v06-benchmark-release.log` the raw output.

### Limits

No corrected figure may be compared with the old mislabelled values — the units
differ, so the comparison would be meaningless rather than merely unflattering.
Process RSS includes Qt, the allocator, and the harness; engine memory is only
ever reported as structural counts, not as bytes. Because reclamation is disabled
until G06, no GC speedup can be measured at all: the benchmark compares two
identical states and says so. `tst_benchmark` remains outside the fast CTest
selection because it is a long-running suite in Debug (~8 minutes); its Release
run is about a minute.


## 2026-10-04 — V07: store fault-injection and subprocess crash harnesses

```text
Task: V07
Status: complete
Baseline: 1dd6e7c (V06 documentation commit)
Implementation commits: 5a18f8c, plus this documentation commit
Scope: libs/collabtext/tests/StoreFaultHarness.h (new),
       libs/collabtext/tests/tst_store_fault_harness.cpp (new),
       libs/collabtext/tests/store_crash_worker.cpp (new),
       libs/collabtext/CMakeLists.txt
Regression: tst_store_fault_harness (new), 24 cases covering fault reachability,
       what each fault leaves behind, filesystem fixtures read by the production
       reader, and fourteen process-crash points
Verification: 24/24 in Debug, Release, and sanitizer mode (no sanitizer
       findings); fast suite 55/55 in Debug, Release, and sanitizer mode
Artifacts: docs/reports/2026-10-03-roadmap-kickoff-artifacts/v07-*
API/store changes: none in production code. The harness is test-only; the
       production writer is deliberately untouched, because making its write
       sequence fault-injectable is L01/L02's job alongside the real fix.
Known limits: this is not durability evidence; process death is not power loss;
       F6's skip behaviour is pinned, not fixed
Next ready tasks: V08
```

### What changed

A store is durable only if every step of its write sequence can fail without
leaving something worse than "the write did not happen". Nothing in the suite
could make a step fail on demand, so a torn record or a cursor that advanced past
missing data was not a state any test could reach.

`StoreFaultHarness.h` adds a hook before and after each step — write, file sync,
rename, directory sync, segment seal, cursor publication, checkpoint publication
— with a default plan that injects nothing, so ordinary use is a no-op. Each hook
records its arrival with the operation name and the path it touched. Faults can
pass, fail, abort the process, or truncate a write, and a skip count delays one,
so a test can say "fail the third write" without keeping state of its own. The
suite asserts that all fourteen points are reachable and that two runs of the
same fault produce byte-identical hook logs — determinism and replayability as
testable properties rather than as an aspiration.

Building the harness corrected the write sequence itself. An append-only tail is
appended in place; the temp-and-rename discipline belongs to the steps that
*replace* a file, which is where the rename and directory-sync hooks live. The
first version renamed the tail on every append and silently discarded the
previous records — a bug the fixtures caught immediately.

Filesystem fixtures build a stream directory in a known damaged state: missing
segment, out-of-order arrival, partial tail, corrupt segment, repaired segment,
failed rename, simulated disk full, contended writer lock. They use the
production sealed-segment encoding, so the production `SegmentReader` reads them
back — the fixtures are checked against the real reader, not a convenient one.

`store_crash_worker` is a separate process that dies mid-write at a named point
with `_exit`, so nothing unwinds and no destructor cleans up. Fourteen
point/phase combinations are exercised; in every case the surviving tail
consists of whole newline-terminated records, and a surviving temp file is never
mistaken for visible content.

### Acceptance evidence

24/24 cases pass in Debug, Release, and sanitizer mode with no sanitizer
findings. The terminated workers are deliberate and are not treated as library
memory defects — the sanitizer run reports nothing, which is the claim that
matters here. The fast suite is 55/55 in Debug, Release, and sanitizer mode.

Two fixture tests pin today's behaviour for finding F6 — a late earlier segment
is skipped, and a corrupt segment is skipped — with comments saying that skipping
must become "blocks coverage advancement". That red test belongs to L03; writing
it here is how the later fix gets a target rather than a hope.

### Limits

This is not durability evidence. The production writer still flushes a
`std::ofstream` and increments a counter named `fsync_count` without calling
`fsync` — finding F9, owned by L01/L02, and the reason production code was left
untouched by this task. A terminated worker proves what happens when user-space
buffers vanish; it says nothing about losing the page cache, because the kernel
keeps writing the file. The worker says so on stderr before it dies.

Fault injection covers the write sequence only. Checkpoint validation,
membership, and quota policy belong to G01/G02.


## Evidence and continuation

[Portable artifacts](2026-10-03-roadmap-kickoff-artifacts/README.md) retain CTest
output, direct QtTest traces, and configuration metadata. Full build logs remain
in ignored worktree verification directories. Metadata names the HEAD at test
time; task changes were then uncommitted, so TASKS.json's final implementation
commit identifies the tested code. The V03 red runs used the revised tests
against V02's production code.

The [handoff](../handoff/2026-10-03-reliability-kickoff.md) gives exact commands,
known failures, and settled product decisions. V04 has since made the randomized
suites seeded, traced, and reducible; V05 has since corrected the simulated
network's partition and ordering model, V06 made the benchmarks report honest
units, timed regions, and asserted convergence, and V07 added store fault
injection with a subprocess crash harness. V08 is the last structurally ready
task in phase 1. Do not interpret any single task as
completing the remaining roadmap.
