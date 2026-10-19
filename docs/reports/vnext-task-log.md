# Reliability roadmap implementation log

Historical implementation used `.worktrees/reliability-v2` on
`feat/fuguemax-placement` (earlier `roadmap/reliability-v2`); that worktree was
retired after master integration on 2026-10-19. Current local work targets
`master`. Original source baseline: `046d375`; approved native roadmap: `374f93c`.
Task status and full implementation hashes remain in
[TASKS.json](../roadmap/2026-10-reliability/TASKS.json).

## 2026-10-19 — Performance evaluation and implementation plan

Planning/evaluation only; no X implementation task is complete or in progress.
The merged engine at `d04f151b3a70b161b72c33a8de6895efbd1c13e2` has a new
[measured baseline](2026-10-19-performance-evaluation.md), with five repeats,
checked public traces, native benchmark results, codec and warm recovery evidence.
The user requested a sequenced plan for another executor.

[07-performance.md](../roadmap/2026-10-reliability/07-performance.md) now mirrors
revised registry tasks. Start X01; the Core sequence is X01, X02, X07a, X04a,
X07, X04, X05, X06, X08, X03, X11, X12, X10. X04a is a required design/model
gate. X09 transport scheduling remains separately behind G07. Dependencies on
unfinished physical GC/external acceptance were explicitly removed from Core
optimization/reporting; no GC, reader or consumer gate was marked complete.
All non-X task definitions/statuses and historical candidate tags are preserved.

[The target contract](../specs/vnext-performance-targets.md) defines expected
improvements, source/model references, paired measurements, regression thresholds,
miss dispositions and three correctness/package arcs. Current direction,
execution, decisions and documentation map point to this lane. The frozen
measurement archive has narrowly enumerated verification routes; unknown new
benchmark source still fails routing, and no benchmark was added to CTest.

Verification: registry/Markdown and acyclic serial order validation, strict scope
check, all changed-document local links, preserved non-X task equality and Core
independence from G06/G07/G08/X09/Q06/M06 were checked. The Python verifier suite
passes 15/15 tests, including fail-closed routing for unknown files beside the
archive. The routed `tst_verification_tools` CTest also passes 1/1 with
`--no-tests=error`; evidence is retained in
`build-roadmap-debug/verification/20261019T211245Z-changed-x1u2xgq6`.
No engine suite/sanitizer certification is claimed for these planning and routing
changes.

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


## 2026-10-04 — V08: API inventory and behavioural fixtures

```text
Task: V08
Status: complete
Baseline: fdc6f83 (V07 documentation commit)
Implementation commits: 37b26db, plus this documentation commit
Scope: docs/roadmap/2026-10-reliability/api-inventory.md (new),
       libs/collabtext/tests/fixtures/behavior/*.behavior (new),
       libs/collabtext/tests/tst_behavior_fixtures.cpp (new),
       tests/consumer-smoke/README.md (new), libs/collabtext/CMakeLists.txt
Regression: tst_behavior_fixtures (new) — 9 fixtures with provenance and exact
       expectations, including a red test for finding F8
Verification: 6/6 fixture cases pass; fixtures replayed against revision 046d375
       (8/9 hold there, F8 does not); fast suite 56/56 in Debug, Release, and
       sanitizer mode
Artifacts: docs/reports/2026-10-03-roadmap-kickoff-artifacts/v08-*
API/store changes: documentation only, plus one test executable. The inventory
       records the breaks P01/A01/A06 are expected to make.
Known limits: corruption fixtures live in V07's harness; the installed consumer
       check waits for A01; no fixture uses an old store or format
Next ready tasks: none in phase 1. D01 is next in registry order.
```

### What changed

Eight roadmap tasks have now been implementing against an API nobody had written
down. `api-inventory.md` records, before P01, A01, and A06 start changing it:

- the eight use cases the library must serve, and which of them work today;
- every public header and its intended role, including the internal types that
  leak because `src/` is on the public include path;
- callback ownership, which is implicit today and is finding R9;
- `IdList::local_clear()`'s real semantics, including the trap that it preserves
  the version vector, so replaying the original operation log after a clear is a
  silent no-op rather than a rebuild;
- the CMake targets, including `Qt6::Test` being linked from the shipped
  library because `NetworkSim.cpp` calls `QFAIL`;
- nine API requirements and eight surfaces slated for removal, each with a
  reason;
- the source, ABI, and format breaks a reviewer should expect, all acceptable.

Nine behavioural fixtures freeze logical scenarios as versioned event traces plus
declared expectations, so a fixture is executable evidence rather than a document
that drifts. Each declares name, provenance, scope, and at least one expectation;
the suite fails if one is missing, so a fixture cannot be added without saying
where it came from. A `desired` fixture must hold. A `known-defect` fixture
asserts what *should* happen and is marked expected, so an unexpected pass fails
the suite and forces the marker out.

### Two things this task refused to fake

**Corruption.** A damaged file is not a sequence of decisions, so it cannot be a
replayable trace. Those fixtures live in `tst_store_fault_harness` (V07), which
builds real damaged directories and reads them back with the production reader.
The behaviour suite asserts that harness's presence instead of claiming coverage
it does not have.

**The installed consumer check.** `tests/consumer-smoke/` gets a note explaining
that it cannot exist until A01 makes the core Qt-free and adds install/export
rules. Building it now would encode today's broken packaging as the contract and
then fail loudly when A01 fixes it.

### Baseline reproduction

All nine fixtures were replayed against revision `046d375`, the evaluated source,
which separates "the engine already does this" from "this is what we want":

- Eight hold at the baseline. They freeze current correct behaviour, which is
  what makes them regression tests rather than aspirations.
- `buffer_concurrent_passages_stay_contiguous` does **not**: two producers
  inserting "hello" and "world" at position 0 produce `hweolrllod`, interleaved
  per character. That is finding F8, and the fixture is its red test for S01–S07.
- Nothing else differs between the baseline and this branch, because V01–V07
  changed verification, network simulation, benchmark honesty, and reclamation
  containment — not Buffer or IdList semantics. That is the expected result, and
  it is why freezing these now matters: P01 and S01 are about to edit the same
  code.

Writing the F8 fixture also corrected it twice. The first version inserted
single characters, which do not interleave — the defect only appears with
multi-character passages, which is what "concurrent passages interleave character
by character" actually describes. The second version asserted an exact merged
string, which over-specified: which passage lands first is unspecified, so the
expectation is that each survives as a contiguous run.

6/6 fixture cases pass; the fast suite is 56/56 in Debug, Release, and sanitizer
mode. No external repository was accessed and no consumer was contacted.

### Limits

Fixtures cover Buffer, IdList, Unicode boundaries, restart with undo and redo,
partition and heal, transport loss, and comment anchors. Multi-stream and
`OpStream`-level scenarios are not fixture-covered yet; `tst_opstream_*` covers
them as executables, not as frozen fixtures. The baseline reproduction compiled
today's harness headers against the baseline library and removed two calls to the
V05 link-partition API, which did not exist at `046d375`; no fixture uses that
model.


## 2026-10-04 — D01: the accepted design constraints, made binding

```text
Task: D01
Status: complete
Baseline: 529b80a (V08, closing M0)
Implementation commits: bb0f836, plus this documentation commit
Scope: docs/specs/vnext-design-constraints.md (new),
       docs/roadmap/2026-10-reliability/check-scope.py (new), DECISIONS.md
Regression: none — D01 changes documentation and adds a checker, and
       documentation-only policy/spec tasks need consistency review rather than
       tests that mirror their prose
Verification: check-scope.py --strict clean (62 tasks, 20 hits, 0 to review);
       validate-plan.py valid; all relative links in the touched files resolve
Artifacts: docs/specs/vnext-design-constraints.md, check-scope.py
API/store changes: documentation only. No signature or format is changed here.
Known limits: the release invariants are stated as checkable properties, not as
       verified ones; most become mechanically checkable in later tasks
Next ready tasks: P01 (design gate), then P02
```

### What changed

Four phases had been executing against decisions recorded in prose, where
"improve the existing engine" and "no legacy migration" were sentences rather
than something a task could be held to. `vnext-design-constraints.md` turns them
into numbered constraints a later task can cite:

- **C1–C4** carry the four user answers forward, each stating what it obliges a
  task to do and what it forbids.
- **C5–C7** fix the format rules: per-artifact versioning, explicit rejection of
  unknown versions, documented hard cuts instead of compatibility layers.
- **C8–C14** are the repair discipline: narrow transformations, a failing test
  first, a specification plus an independent model before an algorithm,
  correctness before performance, no substitution layer, external engines as
  reference material only, and containment that stays until it is replaced.
- **R1–R10** are the release invariants as checkable properties — no lost
  acknowledged work, causal ordering, writer identity across restart, both
  history stacks, anchor fidelity, reclamation safety, recovery honesty,
  membership honesty, durability honesty.
- **B1–B8** are the intended public behavior inherited from V08's inventory.

`DECISIONS.md` keeps the record of what the user decided and now carries a table
mapping each answer to the constraints it became, so the two documents cannot be
read as disagreeing.

### The acceptance check is executable

D01's acceptance asks that no task require a Rust rewrite, Yrs adoption, a backend
experiment, or legacy-data migration. That is mechanical, so `check-scope.py`
does it: it scans every task's text for the sensitive vocabulary on word
boundaries — "trusted" is not a Rust rewrite, "migrated fuzz suite" is not data
migration — classifies each hit by its surrounding sentence, and exits nonzero if
anything needs a human decision. Its allowed-phrase list is deliberately visible;
an exception nobody can see is an exception nobody can review.

Result: 62 tasks, 20 hits, all prohibitive or explicitly allowed. The single
allowed phrase is E04's "rewrite current architecture", which is a documentation
rewrite. Run it whenever the registry changes.

### What D01 deliberately does not do

It does not design an algorithm, choose data structures, set performance targets,
or promise a date. C10 keeps S02 and G01 responsible for their own
specifications and executable models, and states that a design gate which cannot
be satisfied records a gap and stops rather than redirecting the work into a
replacement engine. The document is a contract about direction, not a
specification of behaviour — and it says so, so nobody mistakes it for one.


## 2026-10-04 — P01: the vNext type and error contracts

```text
Task: P01
Status: complete
Baseline: e2b9024 (D01 documentation commit)
Implementation commits: f4c0f11, ad5e3d6, plus this documentation commit
Scope: docs/specs/vnext-protocol.md (new), docs/specs/vnext-public-api.md (new)
Regression: none — a design-gate task changes specifications, and its verification
       is the review against the findings plus the examples it hands forward.
       Type-level behaviour becomes regression tests when P02 implements it.
Verification: F4/F5/F10 and the multi-stream cases reviewed and, in a follow-up,
       cited against the current violating lines; six compile-oriented examples
       written for A02; link check clean
Artifacts: docs/specs/vnext-protocol.md, docs/specs/vnext-public-api.md
API/store changes: this *is* the API change, as a specification. The documented
       breaks are listed in the guide's section 9.
Known limits: the types do not exist until P02 and later; the examples are not
       compiled until A02; merge ordering and reclamation stay with their gates
Next ready tasks: P02 (implementation), then P03
```

### What changed

Seven tasks were going to be written against types nobody had written down. P01
fixes them before any caller changes.

**The API guide** defines the C++20 `Result<T>` (no `std::expected`
requirement, `[[nodiscard]]`, no throwing accessor, so an ignored error is a
compile error rather than a runtime surprise); 128-bit `ActorId` with no default
identity, which is what makes "silent fallback to actor 1" impossible to write;
checked per-(actor, stream) `OperationId`; an operation context carrying
document, stream, and epoch; opaque non-copyable `BufferOperation` and
`IdListOperation` whose stable contract is `context()`, `lamport()`, and
encode/decode round-trip, with direct field access named as the habit being
removed; an error taxonomy that separates malformed from unknown-format from
unsupported-version from actor-conflict from wrong-epoch from counter-regression;
and `ApplyOutcome` as applied/duplicate/deferred/rejected, each carrying its
reason.

Callbacks get one contract for the whole library: single owner, replacement on
re-registration, explicit removal, non-reentrant, never invoked holding an
internal lock, fired only for visible change, one engine per thread.

**The protocol contract** fixes per-artifact version identifiers with fresh
baselines; three separate decode steps — syntax, schema/version, semantics —
because conflating them *is* finding F10; complete input consumption and
surrogate-pair decoding; frontier-based contiguous durable coverage that a gap
or a quarantine blocks; cursors that are meaningless without a recovery point;
sender-restart counter resumption above the highest durable value; rebuild before
write on receiver restart; epoch-bumping retirement; and per-(stream, actor)
coverage with no scalar that could be mistaken for cross-stream coverage.

### The review, anchored to code

A review that says "this addresses F4" is worth little, so a follow-up commit
cited the current line that violates each rule: `StreamSync.h:70` and
`StreamSync.cpp:205` for the non-durable sequence, `SegmentReader.cpp:98` and
`:108` for the high-water mark, `SegmentReader.cpp:60` for a cursor with no
recovery point, `Serialization.cpp:572` and `:660` for decode success without
consumption or validation, `apply_ops` returning `void`, and `resolve_anchor`
returning a clamped `uint32_t`.

Six of the seven already have a red test or a known-defect marker. The seventh —
multi-stream coverage — has no violation today only because there is one stream,
which is exactly why the protocol forbids the scalar by construction instead of
warning about it.

### Limits

The specifications are target signature: `Result`, `ActorId`, the opaque
operations, `ApplyOutcome`, and `PollOutcome` do not exist until P02 and later
implement them, and the guide says so rather than implying the examples are
verified. A02 compiles the examples; E03 compiles them against the installed
package. `RecoveryPoint`, `StreamAck`, `ChangeInfo`, `Affinity`, `TextRange`, and
`Anchor` have their *contracts* fixed here and their internals assigned to
L03/L04 and A03.

Merge ordering (S02) and reclamation eligibility (G01) are deliberately
unspecified: D01's C10 assigns them their own gates with their own specifications
and executable models, and a protocol document that quietly designed them would
undermine that.


## 2026-10-04 — P02: persistent actor identity, writer leases, checked counters

```text
Task: P02
Status: complete
Baseline: e489f29 (P01 documentation commit)
Implementation commits: ad37a57, plus this documentation commit
Scope: libs/collabtext/include/collabtext/ActorId.h (new),
       libs/collabtext/src/identity/ReplicaState.cpp (new),
       libs/collabtext/tests/tst_actor_identity.cpp (new),
       libs/collabtext/CMakeLists.txt
Regression: tst_actor_identity (new), 26 cases — one per way identity handling can
       lose or duplicate an operation
Verification: 26/26 identity cases; targeted Debug 4/4 with tst_clock, tst_buffer
       and tst_idlist; fast suite 57/57 in Debug, Release, and sanitizer mode
Artifacts: docs/reports/2026-10-03-roadmap-kickoff-artifacts/v-p02/
API/store changes: new public header ActorId.h (Result, Error, ActorId,
       OperationId, OperationContext, CheckedCounter, ReplicaState) and a new
       private state format `collabtext-replica-state` version 1. No existing
       signature changed; the engines still take uint16_t ids.
Known limits: cross-restart counter-rewind detection needs a second witness;
       engine-wide counter widening is staged, not done
Next ready tasks: P03, P04, L01, S01
```

### What changed

Identity was a 16-bit number drawn at construction, stream entries were keyed by
a counter that restarted at 1 and was never recovered, and a second local writer
could take the same stream without either noticing. That is finding F4's
mechanism, in the code the whole protocol sits on.

`ActorId.h` implements what P01 specified: a C++20 `Result<T>` with no throwing
accessor, a typed error taxonomy that carries context, a 128-bit `ActorId` with
no default identity, checked per-stream `OperationId`/`OperationContext`, and
`CheckedCounter`.

`ReplicaState` owns the private per-device state, and its three properties are
the ones that prevent lost or duplicated operations:

- **Reserve, persist, then hand out.** `next_operation()` writes the reservation
  before returning the id. When the persist fails the reservation is
  deliberately *not* rolled back — a skipped id is a numbering gap, a reissued id
  is a duplicated identity. The first version of this code rolled back and would
  have reissued.
- **Corrupt state is reported, never replaced.** Replacing it would mint a new
  actor while old operations still claim the old one. Parsing is strict: an
  unknown directive or trailing data is malformed, because a skipped line is a
  silently dropped counter. A test asserted a rewound counter would be refused;
  writing that test honestly is what exposed the limit below.
- **Leases are exclusive and are never stolen.** `O_CREAT|O_EXCL` means two
  local writers cannot both hold a stream, and a conflict names the holder. There
  is no timeout takeover, because a lease reclaimed by the clock is a quiet way
  to lose operations — the same reasoning as C2's membership rule.

Private state lives outside shared document stores, so copying a shared folder
copies no identity at all (asserted directly against the copied tree). Copying
*private* state does copy identity, and the only correct answer there is the
explicit `fork_identity()`, which mints a fresh actor and a fresh numbering
space. Two identical clones are detected when they meet: a peer claiming our
actor at a counter we already issued is `actor_conflict`, reported and never
resolved by last-writer-wins.

26/26 identity cases pass; the fast suite is 57/57 in Debug, Release, and
sanitizer mode, with `tst_buffer` and `tst_idlist` unaffected — nothing that
already used an engine identity changed.

### Limits, recorded rather than papered over

**Cross-restart rewind detection is not claimed.** A state file rewound to an
earlier counter is indistinguishable from a fresh one when that file is the only
witness. Within a live object `restore()` refuses to rewind, and that is tested;
across a restart, the second durable witness has to be the own log and checkpoint
(L03/G04). The first version of this test asserted the stronger claim and was
wrong.

**Engine-wide widening is staged, not done.** P02's step asked for clocks and
origin/vector fields to be widened consistently. Doing that means changing
`Global`'s accessors and the wire codec in `Serialization.cpp`, and every engine
caller — files this task's scope does not include, and doing it half-way would
truncate silently. So the widening lands through `CheckedCounter` now, and the
engine adoption belongs to the tasks that already own those files: P06/P08 for
the codec and S03 for the engines. `Buffer(uint16_t)` and `IdList(uint16_t)`
still take a small integer until then.

**Leases cover local processes only**, and stale-lease recovery is deliberately
absent. **Identical disconnected clones** still cannot always be detected before
they exchange operations; the conflict surfaces when they meet.


## 2026-10-04 — P03: coverage intersection is not summary aggregation

```text
Task: P03
Status: complete
Baseline: 9d60827 (P02 documentation commit)
Implementation commits: faa7f5d, plus this documentation commit
Scope: libs/collabtext/src/crdt/Clock.h, Clock.cpp, Fragment.h, IdList.h,
       libs/collabtext/tests/tst_clock.cpp,
       libs/collabtext/tests/tst_summary_laws.cpp (new),
       libs/collabtext/tests/tst_evaluation_regressions.cpp,
       libs/collabtext/CMakeLists.txt
Regression: tst_summary_laws (new, 13 cases) and five new tst_clock cases; the F3
       expected-failure marker is removed and its assertion now passes
Verification: targeted Debug 4/4; summary laws 13/13; clock 23/23; seeded fuzz at
       42 and 20261003; fast suite 58/58 in Debug, Release, and sanitizer mode
Artifacts: docs/reports/2026-10-03-roadmap-kickoff-artifacts/v-p03/
API/store changes: Global gains coverage_intersection() and empty();
       FragmentSummary and IdListSummary lose three unused version-vector
       fields. No wire format changed.
Known limits: NetworkSim::compact_all() still uses meet() as its watermark
Next ready tasks: P04, L01, S01
```

### What changed

`Global::meet()` was answering two questions at once. It folds a nonzero
minimum — *what is the largest counter nothing in this set has gone below?* —
and it was also being used as *what has every peer seen?* A peer that has never
observed replica 1 constrains replica 1's common coverage to zero; `meet()` leaves
it alone. That is finding F3, and the fix is not one line: the two questions need
different operations with different laws.

`Global::coverage_intersection()` is the watermark operation: the minimum over
the **union** of both vectors, absence counting as zero, and zero components
canonicalized away. So `{1:10} ∩ {}` is empty, disjoint vectors intersect to
nothing, and overlapping ones take the minimum. Its laws are stated with one
honest gap — there is no identity element in a sparse representation, because the
mathematical identity is "every counter at its maximum", which cannot be
encoded; empty is the *absorbing* element, so folding a peer set starts from the
local vector and never from empty.

`meet()` keeps its behaviour, because the plan says not to change every call site
mechanically, but its documentation now states what it is and that it is not a
watermark.

The tree summaries carried three version-vector fields that nothing read. One of
them aggregated with `meet()`, so a "maybe everyone has seen it" quantity was
cached in every SumTree node and looked like a certainty. Removing all three is
cheaper and removes the false certainty: no dimension consumed them.

### Evidence

`tst_summary_laws` pins the laws of both vector operations and both tree
aggregates — identity, commutativity where meaningful, associativity, and a
property that safety depends on (intersection can only ever understate coverage,
never overstate it). It also checks the thing a user would actually notice:
visible text unchanged across randomized splice patterns, verified against a
reference string rather than against the aggregate under test. `tst_clock` gains
the coverage cases including one across the inline/heap boundary.

The F3 marker is removed and both behaviours are pinned side by side, so the
distinction cannot quietly disappear again. Strict evaluation now exits **5** with
F4–F8 remaining. Seeded fuzz passes at 42 and 20261003; the fast suite is 58/58
in Debug, Release, and sanitizer mode.

### Limits

`NetworkSim::compact_all()` still builds its GC watermark with `meet()`. That is
inert only because V03 made reclamation a no-op, and G06 must replace that call
with the intersection — recorded as P03's first limitation rather than left for
someone to discover.

Existing `meet()` call sites in `tst_gc` and `tst_idlist_gc` were left alone
deliberately: rewriting them mechanically would change expectations rather than
fix a defect. They remain nonzero-minimum aggregates, not watermarks.


## 2026-10-04 — P04: a strict bounded JSON reader and UTF-8 validation

```text
Task: P04
Status: complete
Baseline: b2fb7d0 (P03 documentation commit)
Implementation commits: 9058cc9, plus this documentation commit
Scope: libs/collabtext/src/codec/{Utf,JsonReader}.{h,cpp} (new),
       libs/collabtext/tests/tst_decode_validation.cpp (new),
       libs/collabtext/tests/tst_utf8.cpp (extended), libs/collabtext/CMakeLists.txt,
       docs/specs/vnext-protocol.md (new section 11)
Regression: tst_decode_validation (new, 29 cases, fixed 56-entry malformed corpus)
       and tst_utf8 (extended to 30 cases)
Verification: targeted Debug/Release/ASan 2/2 each; fast suite 59/59 in Debug,
       Release, and sanitizer mode; seeded fuzz at 42 and 20261003
Artifacts: docs/reports/2026-10-03-roadmap-kickoff-artifacts/v-p04/
API/store changes: new internal CollabText::Codec namespace (JsonReader, JsonValue,
       JsonLimits, JsonError, UtfError). No wire format or store format changed.
Known limits: not yet wired into the existing serializers; per-format bounds not
       yet varied; Qt moc rejects raw string literals containing braces
Next ready tasks: P05, L01, S01
```

### What changed

Finding F10 was that `parse_operation()` returned a value for
`{"t":"u"}garbage`, and for objects missing fields the decoder needed. A decoder
that succeeds has to mean something, so now it means: complete consumption, no
duplicate keys, arbitrary key order, strict numbers, and every malformed input
rejected with an offset someone can act on.

There is no reviewed JSON dependency recorded in P01, so the reader is written
here — and the plan's "do not invent a permissive partial parser" is the reason
it is strict rather than lenient. Every bound in `JsonLimits` is checked before
the allocation it governs, and the conservative defaults are now normative in
`docs/specs/vnext-protocol.md` §11. `max_text_bytes` is deliberately in the
*unescaped* domain, so a short frame of `A` repeats cannot ask for a large
allocation.

Duplicate keys are rejected rather than last-wins: two writers that disagree about
a field must not be resolved by whichever happened to be serialized second.
Key order carries no meaning, and two reordered documents decode to equal values
— pinned by a recursive canonical rendering rather than `operator==`, so drift in
one field is visible instead of averaged away.

An unknown `t` identifier or an unsupported `v` is *retained*, not rejected: the
bytes are valid and a later build may understand them. Discarding them as
corruption would lose work, which is why `unsupported_version()` is separate from
`parse()` failing.

UTF-8 validation accepts exactly the Unicode scalar range. Overlong forms,
encoded surrogates, scalars above U+10FFFF, and truncated tails are errors.
Checked conversion reports the offset at the start of a scalar or surrogate pair
rather than manufacturing a position inside one, and a failed conversion leaves
its output untouched — a half-built string is how a caller ends up applying half
an operation. `JsonError` carries the underlying `UtfError` whole so an overlong
encoding and an encoded surrogate stay distinguishable for whoever fixes the
writer.

### Evidence

`tst_decode_validation` (29 cases) carries a fixed 56-entry malformed corpus in
which every entry names the rule it protects and no two entries share a rule, plus
a 200000-deep array proving the depth bound is a bound and not a promise.
`tst_utf8` (30 cases) covers overlong forms, encoded surrogates, truncation
offsets, and a sweep of the whole two-byte range. Targeted Debug/Release/ASan all
pass, the fast selection is 59/59 in three modes, and seeded fuzz passes at 42 and
20261003.

### Four bugs the tests caught in the reader itself

Each is the kind that survives review, which is the argument for having written
them first:

- the one-byte lead mask was derived by the wrong formula and mapped `0x61` to
  `0x21`, so ASCII round-tripped to wrong bytes;
- the escape scanner treated everything after the first escape as escapes, so
  `a	b
c` was rejected;
- `parse()` did not reset its output value, so a caller reusing one `JsonValue`
  across two parses got a spurious duplicate key on the second;
- insignificant whitespace was never skipped, so `{"t":"u"}garbage` and
  ` {"t":"u"} ` behaved identically.

### Limits

The reader is not yet wired into `Serialization.cpp` or `StreamSerialization.cpp`;
those still parse with their own code, and P05/P06 are what adopt it. `JsonValue`
keeps number text verbatim and does no schema validation. The bounds are
per-parse configuration with fixed defaults — nothing varies them per operation
format yet, though §11.1 records the defaults for that purpose. `JsonValue::find`
is a linear scan, which suits envelopes but would need an index for a document
with thousands of members; `max_container_elements` bounds that rather than
optimizing it.

One environment note worth keeping: Qt's moc silently drops every test slot in a
file that contains a raw string literal with braces. The tests therefore use
escaped literals. That cost P04 about an hour and is now recorded in the registry.

## 2026-10-04 — P05: semantic operations are validated before anything mutates

```text
Task: P05
Status: complete
Baseline: 225ad23 (P04 documentation commit)
Implementation commits: 88576ba, plus this documentation commit
Scope: libs/collabtext/src/crdt/OperationValidation.{h,cpp} (new),
       libs/collabtext/include/collabtext/ApplyOutcome.h (new),
       libs/collabtext/src/crdt/ApplyOutcome.cpp (new),
       libs/collabtext/src/crdt/Buffer.{h,cpp}, libs/collabtext/src/crdt/IdList.{h,cpp},
       libs/collabtext/include/collabtext/ActorId.h (three error codes, Error::operator bool),
       libs/collabtext/src/identity/ReplicaState.cpp (to_string for those codes),
       libs/collabtext/tests/tst_operation_validation.cpp (new), libs/collabtext/CMakeLists.txt,
       docs/specs/vnext-protocol.md (sections 4.1, 4.2, 11.1a),
       docs/specs/vnext-public-api.md (section 9.1)
Regression: tst_operation_validation (new, 50 cases)
Verification: targeted Debug/Release/ASan 4/4 each; fast suite 60/60 in Debug,
       Release, and sanitizer mode with leak checking; tst_realistic 13/13 and
       tst_benchmark 21/21 in Release; seeded fuzz at 42 and 20261003
Artifacts: docs/reports/2026-10-03-roadmap-kickoff-artifacts/v-p05/
API/store changes: new public ApplyOutcome/ApplyOutcomeKind; apply_ops returns
       std::vector<ApplyOutcome> instead of void; try_apply_local_edit /
       try_insert_after / try_remove_at return Result; Buffer gains a change
       callback and a deferral budget. No wire or store format changed.
```

### Two undefined behaviours, confirmed rather than asserted

I did not take the existing `assert`s on faith. Both were compiled with
`-DNDEBUG` against the parent commit under AddressSanitizer, which is what any
Release build does:

- `IdList::remove_at` with an anchor naming no entry. The only guard was
  `assert(idx < entries.size())`, after which the code read `entries[idx]`
  anyway. ASan: **heap-buffer-overflow, READ of size 8, "0 bytes after" the
  72-byte entry vector**, at `IdList.cpp:99`.
- `Buffer::apply_local_edit` with two ranges and one replacement string. The only
  guard was `assert(ranges.size() == new_text.size())`, after which the edit walk
  indexed `new_text[i]` regardless. ASan: **heap-buffer-overflow, READ of size 8**,
  at `Buffer.cpp:743`.

So these were not "assertions that might be too weak". They were assertions
covering reads that a Release build performed anyway. Both are now refusals with
a typed error. The reproductions are kept in `v-p05/` so the claim is checkable
rather than something the next reader has to take on faith.

A third, quieter one: `IdList::apply_concrete(IdListRemoveOp)` asserted that a
removal's target was present and then observed the clock and version vector
*anyway*. A removal whose target was missing therefore advanced coverage over
work that never happened — the F3 shape, in a place the evaluation had not
looked. It now returns false, so the operation stays owed.

### The order of the checks was not my choice, and it is the interesting part

I wrote the validator validity-first, causality-second, duplicate-last, because
that is the order the spec's table is in. The tests failed, and the failure was
instructive rather than cosmetic.

`Global::observed` is `get(replica) >= value`, which is **vacuously true for
counter 0**. So an operation stamped with counter 0 — which `Global::observe`
silently discards, and which therefore can never be recorded or deduplicated —
was being reported as an idempotent *duplicate*. Forever. Never applied, never
refused, never surfaced: a malformed record that had become invisible by asking a
question it was not entitled to ask.

"Have I already applied this?" is only a meaningful question of something that
has an identity, so identity now comes first (`validate_operation_identity`,
cheap enough to run on every inbound record including re-deliveries). The
resulting order — identity, duplicate, validity, causality — is written up as
vnext-protocol.md §4.1 because the reasoning is the part worth keeping. The
validator rejects a malformed operation *before* queueing it, so `deferred` means
exactly one thing: well formed, waiting for a dependency.

### What the tests caught in my own code

Worth recording, since it is the argument for having written them first:

- My first cut rejected a counter-0 operation but only *after* the duplicate
  check, so the identity pre-check above is the fix for a bug the test suite
  found rather than a design decision I reached by reading.
- `insertion_origin_interval_overflow_is_rejected` failed on the error *code*,
  not the verdict: I set the origin to `UINT32_MAX` and the length to 4, so the
  length/scalar mismatch fired before the interval arithmetic. The test had been
  asserting two things and checking one.
- `each_rejection_reason_leaves_engine_state_untouched_data_driven` compared a
  snapshot taken before payloads were generated, while generating a payload
  meant a local edit on the buffer under test. It would have passed for the wrong
  reason. The payloads now come from a separate buffer.

### Deliberate scope calls

**Returning a value rather than renaming the method.** `apply_ops` now returns
`std::vector<ApplyOutcome>`. It does not become `apply_remote` returning one
`ApplyOutcome`, which is what vnext-public-api.md §9 asks for. Doing the rename
and the return-type change together would have made every call site in the tree
change for two unrelated reasons, and it is not P05's job to decide that.
Legibility of the diff mattered more than a cosmetic match to the table.

**Keeping the old local-mutation signatures.** Here the reason was not tidiness.
`insert_after` and `remove_at` cannot report a refusal, so they delegate to
`try_*` variants and return an empty value; that is a real improvement, but the
spec's `Result`-returning signature is still owed and is recorded in §9.1.

**Not inventing an epoch.** Step 1 asks for epoch validation. The engine's
operation value has no epoch in it: replicas are still 16-bit Lamport replica
ids, not the `ActorId` P02 built. There is nothing to compare, so this is recorded
as a gap for P06/P07/L04 rather than satisfied by a field that exists only to be
checked. §9.1 states this plainly, because a validator that appears to check the
epoch and does not is worse than one that says it cannot.

### Verification notes

LeakSanitizer ran with detection enabled (`ASAN_OPTIONS=detect_leaks=1`), and the
handoff's warning about this sandbox blocking LSan did not reproduce — the
sanitizer suite passed. I confirmed instrumentation rather than trusting the
green result: `-fsanitize=address,undefined` and `-fno-omit-frame-pointer` are in
the core compile command recorded in the run metadata, and the test binary
carries 45 distinct sanitizer symbols.

The fast selection excludes `tst_benchmark` and `tst_realistic` for runtime, and
P05 changes the apply path both exercise. Rather than inherit the exclusion I ran
them: 13/13 and 21/21 in Release.

One discrepancy worth flagging for whoever reads the handoff next: strict
evaluation mode exits **5**, not the 6 the kickoff handoff records. That is not
P05. P03 removed F3's expected-failure marker when it fixed the
coverage-intersection defect, and the handoff predates it. F4, F5, F6, F7, and F8
all still fail with the fixing tasks the handoff names.

## 2026-10-04 — P06a gate: the identity order, proven before the engine changes

```text
Task: P06a (design gate only; steps 2–5 remain)
Status: in_progress
Baseline: 2fd5a76 (P05 documentation commit)
Implementation commits: 4ece11a
Scope: docs/specs/vnext-identity.md (new),
       libs/collabtext/tests/tst_identity_ordering_model.cpp (new),
       libs/collabtext/CMakeLists.txt, 02-protocol.md (P06 split),
       TASKS.json, validate-plan.py (split-suffix headings)
Verification: model 14/14; Debug fast suite 61/61; 3 of 3 deliberate comparator
       mutations caught
Artifacts: the two new files above; git history carries the mutation results
API/store changes: none yet. The engine is deliberately untouched by this commit.
```

### The ordering problem was the whole reason for the gate

I asked whether the identity migration could precede the codecs and initially
proposed that it could. Measuring it said no, and the reason is worth recording
because it is not obvious from the API.

The engine orders fragments by `(locator, origin)`, and equal locators are a
**routine** case, not a corner: `normalize_fragments` exists specifically to
handle runs of fragments from *different replicas* sharing one locator, and the
split-relocation path assigns `max(new_locator, sf.locator)`, which leaves two
locators equal on purpose. When locators tie, origin decides. So the identity
order is load-bearing for convergence.

That rules out the obvious way to keep identities small. Assign each new ActorId a
dense local index and store the index: replica A meeting X then Y gives X=0,Y=1,
while replica B meeting Y then X gives Y=0,X=1. Equal-locator fragments from X and
Y then tie-break in opposite directions and the two documents diverge
permanently. `locally_assigned_slots_diverge` demonstrates exactly this so §2.1
of the spec is a shown counterexample rather than an assertion.

The second reason is serialization: `encode_lamport` writes `replica_id` as an
integer and `encode_global` writes the version vector, so both the wire format and
the store format carry the identity. Identity-first would mean changing those
formats outside the task that owns them, or writing a representation the codec
task immediately rewrites. Hence the split into P06a/P06b rather than a reorder.

### Testing a contract rather than testing code

The model reimplements the order from the spec text, comparing ActorId bytes
directly and never calling `ActorId::compare` — the roadmap forbids a reference
model from calling the helper whose behaviour it validates, so agreement between
the two is the only way that comparison is evidence rather than tautology.

Three mutations confirm the suite bites: actor-first instead of counter-first, the
actor comparison truncated to 4 bytes, and a cyclic counter comparison that breaks
transitivity. Each produced failures; the unmutated file passes 14/14.

I also caught a weakness in my own first draft. `make_set` originally gave every
identity a distinct counter, which meant the counter alone decided every pairwise
and triple comparison and the actor tiebreak — the path that can actually diverge —
was never exercised. The fixture now collides counters (`counter = i % 3`), and
the transitivity test asserts it has enough ties rather than leaving that to the
reader's trust. A green law test over a fixture that cannot fail the interesting
way is worse than no test, because it looks like coverage.

### The number the optimization phase inherits

Measured before any change: `Lamport` 8→24 bytes, `UndoMapKey` 8→24,
`FragmentSummary` 40→56, `Fragment` 104→120. `FragmentSummary` is the one that
matters, because the SumTree aggregates one per edit; 1.4x is affordable for
correctness. The spec names the only acceptable lever if it later stops being
affordable — stop storing an identity in the summary at all — and explicitly rules
out the tempting one, since shrinking the identity is what reintroduces divergence.

### Continuation

P06a steps 2–5 are not done and this commit does not pretend otherwise: the laws
are proven, but they are proven against a standalone reference implementation,
not yet against the engine's types, because the engine's types have not changed.
What remains is the wide part — `Clock.h`, `UndoMapKey`, `Buffer(ActorId)`,
`IdList(ActorId)`, opaque values, then re-measuring — across 56 files and roughly
140 `Lamport` construction sites in tests. L01 and S01 remain independently ready
and neither depends on this.

## 2026-10-04 — L01: durable writes that mean what they say

```text
Task: L01
Status: complete
Baseline: 2988a19 (P06a gate documentation commit)
Implementation commits: 04182ed
Scope: libs/collabtext/src/storage/DurableFile.{h,cpp} (new),
       libs/collabtext/tests/tst_durable_file.cpp (new),
       libs/collabtext/include/collabtext/ActorId.h (unsupported_operation),
       libs/collabtext/src/identity/ReplicaState.cpp (to_string), libs/collabtext/CMakeLists.txt
Regression: tst_durable_file (new, 28 cases)
Verification: 28/28; targeted 2/2 with tst_store_fault_harness in Debug and ASan;
       fast suite 62/62 in Debug, Release, and ASan/UBSan/LSan with leak detection;
       5 of 5 deliberate mutations caught
Artifacts: the three new files; mutation results in git history
API/store changes: new internal CollabText::Storage namespace (DurableFile,
       DurableAppendFile, FileOps, FileOpCounts, ProcessLease, StepHook).
       No wire or store format changed.
```

### F9 stated plainly

`SegmentWriter::flush()` incremented a counter named `fsync_count` after calling
`std::ofstream::flush`, which moves bytes from user space into the kernel and no
further. No `fsync` was ever issued. `SegmentWriter::seal_open_()` renamed a
temporary over the destination while discarding the `std::error_code`, having
fsynced neither the temporary nor the containing directory. So a seal that failed
was indistinguishable from one that succeeded, and the counter named after a
guarantee counted something else.

`ReplicaState.cpp` already did this correctly in-tree — temp, fsync, rename,
directory fsync — which is the analogue to mirror rather than a pattern to invent.

### What it promises, and what it refuses to

Four promises: short writes are retried and never silently truncate; the
destination is never unlinked before its replacement exists; the order
write → file sync → rename → directory sync is fixed and observable; a failed step
names itself and changes nothing.

One refusal. There is no power-loss claim, because nothing in user space can make
one — `fsync` returns once the kernel has accepted the bytes, and a later power cut
can still lose them without a stronger platform barrier. What is promised is that a
*process* crash cannot tear a committed write, which is real and testable. The
forked-writer test says this in a comment, because the tempting misreading is
exactly the mistake F9 already made at a larger scale.

Windows reports `unsupported_operation` for a directory sync rather than
pretending. Verified on Linux/GCC only; A07 owns the real platform matrix.

### The tests were wrong first, and this is the part worth keeping

I injected faults at the *step* boundary and asserted the step order from a hook.
Then I mutated the implementation five ways to check the tests could actually fail.
**Four of the five passed.** Skipping the real file sync before the rename — the
precise F9 mistake — was invisible. So was unlinking the destination first,
skipping the directory sync, and not retrying short writes.

Two distinct reasons, both worth naming:

- A hook fired *around* a step observes **intent**. An implementation that
  announces a step and does not perform it is exactly the defect under test, so a
  hook-only test is structurally incapable of catching it.
- Injected step failures fire *before* the real operation, so the unlink mutation
  never executed at all. My `failure_at_each_step_preserves_old_contents` test
  looked thorough and was checking the error path, not the filesystem.

The fix was `FileOps` — the real calls injectable — plus `FileOpCounts` that
increments only when a call returned success. Then "the file was synced before the
rename" became checkable, and intercepting `remove()` shows *directly* that the
destination is never unlinked. Watching only the outcome could never show that,
since an unlink followed by a rename leaves byte-identical results.

All five mutations are now caught, each by the test that should catch it.

A note on my own method: the first version of the fifth mutation placed an
increment outside a null guard and produced a segfault. I had it bracketed as
"caught", which would have been claiming evidence I did not have — a crash is a
non-zero exit, not a demonstration that the tests check counter accuracy. Redone
correctly (double-counting an operation), it is caught by the exact-count
assertions, as it should be.

### Still live

`SegmentWriter` and `FileSync` are not converted yet; that is L02. Until then F9 is
fixed *in the new primitive* and still live in the two call sites that increment
the counter without syncing. Saying otherwise would be the same category of
mistake this task exists to fix.

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
units, timed regions, and asserted convergence, V07 added store fault injection
with a subprocess crash harness, V08 inventoried the intended API and froze
behavioural fixtures, D01 made the design constraints binding, P01 fixed the type
and error contracts, P02 built persistent actor identity with writer leases and
checked counters, P03 separated coverage intersection from summary aggregation,
P04 built the strict bounded reader, and P05 made operations validate themselves
before anything mutates. Phases 1 and M0 are complete and P01–P05 are done.

**Next in serial order is P06** (versioned sparse codecs and opaque operation
values). L01 is also structurally ready. Note that P06 is where the largest
recorded gap closes: the engine still identifies replicas with a 16-bit Lamport
replica id rather than the persistent `ActorId`, which is why P05 could not check
epoch/document/stream context.

Two stale statements in the handoff, both corrected above rather than silently
carried forward: strict evaluation exits 5 known failures, not 6, because P03
fixed F3; and `tst_operation_validation` is now part of the fast selection, so
the fast suite is 60 executables rather than 59.

Do not interpret any single task as completing the remaining roadmap.


## 2026-10-04 — Independent progress review and Wave consumer amendment

```text
Status: review corrections complete; future tasks remain incomplete
Baseline: fac1ecbb428c92acca8ed805d6a45fcc42d32951
Implementation commits: b3f4f20 (P04/P05 validation), bbe8434 (P02/L01 ownership/durability)
Regression: 16 failing baseline-defect slots, plus positive controls and a corrected review-introduced boundary regression
Verification: Debug fast 62/62; Release all 64/64; ASan/UBSan/LSan fast 62/62 without suppressions
Seed: fixed default 20261003
Strict evaluation: exit 5; F4-F8 remain; default has five XFAILs
Artifacts: docs/reports/2026-10-04-review-artifacts/
Next: finish P06a's allocation-domain gate and engine adoption; S01 independently ready
```

The [independent report](2026-10-04-reliability-progress-review.md) records the
focal review, exact failures, repairs, scope, and remaining gates. Baseline
Debug/Release already passed 62/62: green totals missed private-state counter
ownership, duplicate/injectable directives, state-link replacement, literal JSON
limits, UTF-8 source boundaries, validation bypass on blocked actors, fictitious
pending bytes, unbounded overflow retention, short-write/count/close/cleanup
errors, missing first-append directory barriers, uncertain publication, and an
unsafe PID/stale-lease protocol. New regressions and controls now cover them.

Review testing caught an initially wrong SumTree seek bias in the review's own
UTF-8 patch. Its broad failures and the subsequent positive control remain in
the artifacts; the original assertions were preserved and the lookup corrected.
An overlapping-build sanitizer run exceeded two unchanged wall-clock limits;
the final full sanitizer run after Release passed. No failing suite was excluded.

Historical L01 entry correction: fsync requests stable storage from the
filesystem/device stack; it does not mean merely “accepted into the kernel.”
Containing-directory sync is a distinct name barrier. Process-kill tests are
not power-loss certification. A post-rename directory failure now returns
commit_uncertain because complete new contents are already visible. PID records
are diagnostic; persistent-inode OS locks establish local ownership. The old
private-state helper was not entirely correct (shared temporary/ownership gaps)
and now uses the repaired L01 primitive. SegmentWriter/session conversion and
end-to-end F9 remain L02–L08, rather than complete at L01.

The [Wave reply draft](../handoff/2026-10-04-reply-to-wave.md) is saved for the user
to relay; no Wave edit or external message occurred. [W1–W6](../specs/vnext-external-position-contract.md)
extend 13 existing dependency-gated tasks with stable encoded tokens, exact
affinity/scoped boundaries, explicit errors, durable external pins and consumer
frontiers, historical projections, and installed headless tests. Formatting is
explicitly consumer-owned. No backend, replacement engine, or rich-text CRDT was
added. P06a separately gained an allocation-domain gate: comparator laws alone
do not establish safe run/character identities or abandoned-reservation coverage.

Registry statuses remain honest: V01–V08, D01, P01–P05, L01 complete; P06a in
progress; 63 active tasks after its earlier split. Updated executor orientation
and API/protocol guides supersede stale kickoff status. Plan checks pass with
F1–F10/R1–R14/W1–W6 coverage and zero unreviewed scope hits. Source stays isolated
on roadmap/reliability-v2; master receives discovery docs only.

## 2026-10-05 — P06a allocation-domain gate (step 2 of 6)

```text
Task: P06a (step 2 of 6 only; the task remains in_progress)
Status: gate complete; engine migration pending
Baseline: c150086
Scope: docs/specs/vnext-identity.md sections 3.1-3.8; CheckedCounter::reserve_run;
  libs/collabtext/tests/reference/IdentityAllocationModel.h;
  libs/collabtext/tests/tst_identity_allocation_model.cpp; CMake registration
Regression: none in production code -- this step adds a contract, a model, and a
  checked counter primitive. The two failures found while writing the fixtures
  were fixture bugs (undo parity asserted backwards; a frontier expectation that
  did not account for the merged range), both fixed and re-run.
Verification: Debug fast 63/63; Release 65/65 including tst_benchmark and
  tst_realistic; ASan not run for this step
Seed: fixed default 20261003; the randomised model comparison uses
  std::mt19937_64(20261004)
Artifacts: build-roadmap-debug/verification/20261005T014431Z-3098282;
  build-roadmap-release/verification/20261005T014613Z-3101343
Next ready tasks: P06a steps 3-6; S01 remains independently ready
```

The review was right that comparator evidence does not authorize replacing the
character clock with one counter per operation, and the specification now says
exactly what replaces it. A local operation reserves a **run interval**: `n`
item identities followed by its own operation identity, from one checked
`reserve_run(n + 1)`. Two consequences worth keeping in view.

The layout is not cosmetic. Legacy sets `op.timestamp = m_clock.value - 1`
(`Buffer.cpp:900`), so an insert operation and the last character of its own run
carry the *same* identity today, and `deletion_ts` costs a second tick
(`Buffer.cpp:586`) only because the operation's own timestamp cannot serve twice.
Separating the domains removes both, and makes "which operation inserted this
character" a range-containment question instead of a guess.

The obvious alternative was rejected rather than skipped. Giving each operation
one counter and each scalar a packed index stops consuming a counter per
character, but it changes the order: `(counter, index)` puts all of one
operation's items before the next operation, where the per-scalar clock
interleaves them. Equal locators tie-break on exactly that order, so the change
would be invisible in the type and visible in every concurrent-passage fixture.
The interval scheme reproduces the legacy order value for value, which is why it
was chosen and why S01's F8 fixtures stay meaningful after the migration.

Three findings from writing the gate, in the order they mattered:

- **The gap obligation is real and had nowhere to live.** Durability-first
  allocation means a crash between the reservation and the log append burns the
  whole interval, and a maximum observed counter is not proof that the values in
  between ever existed. The contract now requires recovery to publish an explicit
  gap record covering the abandoned interval, so a peer's contiguous frontier
  crosses a *known* absence instead of stalling forever. Publication belongs to
  P06b (carry the interval length and the gap kind), refusal to a mismatched
  interval to P07, and framing plus the reopen path to L02. The gate fixed the
  obligation; it did not implement any of it.
- **A mutation survived the first version of the fixtures.** Deleting the
  frontier's "nothing from the beginning is known" requirement still passed
  16/16, because every fixture started at counter 1 and so never reached that
  branch. A late-joining replica case was added; the mutation is now caught.
  Recording this because the honest reading of "16/16 green" before that addition
  was "the green run proves less than it appears to", which is the same category
  of mistake the review caught in the engine.
- **Comparing against production needs a production primitive to compare with.**
  `CheckedCounter` could only reserve one value, so the interval rule would have
  been unverifiable against anything real. `reserve_run` is additive to P02's
  checked counter, deliberately not to the engine's identity types, and its
  overflow-safe bound is the one thing the model and production are allowed to
  share a test for.

What this step does **not** prove: that a migrated engine converges, that
`sizeof(FragmentSummary)` grows by the projected 1.4x, or anything about durable
allocation across a power loss. Steps 3–6 own the migration and the measurement.

## Roadmap amendment — Wave ask2 early consumer integration

**Date:** 2026-10-04. **Planning baseline:** `4983bc4`.
**Status:** roadmap amended; no implementation task promoted.

The user authorized resequencing the worktree plan after reviewing Wave's
follow-up. Nine tasks were added: W01–W03, A02a, A06a, S04a–S04b and E07a–E07b.
The registry has 72 tasks. A01/header/install work no longer waits for the full
identity/codec/recovery milestones; the normal route finishes the running P06a
first. Live boundary implementation has its own specification/model gate and
does not wait for S03/L08. S04 later certifies repaired ordering and real recovery;
S05/G-series preserve durable pin/GC gates. Supported published token readers,
persistent undo/redo, explicit retirement, optional sessions/transports and the
native plaintext/IdList engine remain required.

The [consumer lane](../roadmap/2026-10-reliability/00-consumer-integration.md)
contains the serial route, eight-probe ownership, exact verification-script
contract, stage capabilities and alpha publication procedure. The
[executor handoff](../handoff/2026-10-04-wave-sequencing-plan.md), AGENTS.md,
phase tasks, API/position contracts and execution guidance were synchronized.
E07a/E07b prepare concrete candidates; no tag/package was published, external
repository edited, message sent or master merged.

Verification: `validate-plan.py --ready` passes for 72 tasks, topological/acyclic
ordering, Markdown/JSON agreement, F1–F10/R1–R14/W1–W6 coverage, transitive early
consumer gates and WP1–WP8 ownership. `check-scope.py --strict` reports 0 hits
requiring review. Preservation checks confirm all 63 existing statuses and every
completed/in-progress task record are unchanged; all nine new tasks are
`not_started`. Four deliberate invalid registries were rejected: adding L08 to
A01, removing P06a from S04a, removing L08 from final S04, and removing WP7
ownership. Local-link review found 96 valid links across 20 orientation/roadmap/
spec/handoff documents before this log entry; `git diff --check` passes.
No new engine tests/results are claimed by this documentation/validator change.

## P06a step 3 — the engine's identity types adopt the persistent actor

**Task:** P06a (still in progress; this is the engine-migration step, not the
whole task)
**Status:** in progress — steps 3 and 4 done, 3b/5/6 remaining
**Baseline:** `25c4eee`
**Implementation commits:** this entry's commit
**Scope:** `Lamport {uint32 value; uint16 replica_id}` is gone. `Clock.h` now
defines `Crdt::Identity {ActorId actor; uint64_t counter}`, compared
counter-first then actor byte-wise. `Global` is keyed by `ActorId` and lost its
dense view entirely; `Fragment`, `FragmentSummary`, `FragmentOrderDim`,
`UndoMapKey`, `UndoTreeKey`, `Anchor`, `IdListEntry` and the public operation
structs carry identities; `Buffer`/`IdList`/`CrdtEngine`/`CollabDocument` take an
`ActorId`; the JSON codecs, the version-summary codec, the cursor/comment/chat
anchor codecs and `collabedit`'s replica-id file all follow. `Interval::fits`
overflow checks moved to the 64-bit width. `schema_version` 3 -> 4.

**Regression:** `tst_identity_types.cpp`, written before the migration. It did
not compile — `Crdt::Identity` did not exist and `Anchor` was still
`{uint16_t, uint32_t}` — which is the honest form of "red" for a type migration:
there was nothing to fail at runtime yet.

**Verification:**
- Debug: 64/64 CTest executables pass.
- Release: 23/23 via `./tools/verify-roadmap.sh release` (identity + models,
  Buffer, IdList, convergence, serialization, validation, undo, anchors, seed,
  public API).
- ASan/UBSan/LSan: 17/17 via `./tools/verify-roadmap.sh asan`, no suppressions.
  This run is also the evidence that `Global`'s new raw-byte inline storage is
  sound.
- Measured: `Identity` 24, `UndoMapKey` 24, `FragmentSummary` 56 (baseline 40),
  `Fragment` 120 (104), `Anchor` 32 (12), `Global` 64 (40).
- Seed: fixed default `20261003`; the randomised model comparison uses
  `std::mt19937_64(20261004)`.
- Artifacts: `build-roadmap-release/verification/20261005T050208Z-3316833`,
  `build-roadmap-asan/verification/20261005T050845Z-3322359`.

**API/store changes:** `op_lamport()` -> `op_identity()`,
`idlist_op_lamport()` -> `idlist_op_identity()`, `get_idlist_op_timestamp()` ->
`get_idlist_op_identity()`; `Lamport::counter()` -> `Identity::counter` field;
`Lamport::replica_id` -> `Identity::actor`; `Buffer::replica_id()` ->
`actor_id()`; `Anchor::max()` -> `Anchor::end()` plus an explicit `at_end` flag;
`CrdtEngine`/`CollabDocument` lost their defaulted replica argument. All three
public headers state the superseded contract explicitly rather than leaving a
reader to infer it.

**Known limits:**
- `Global` grew more than the pair arithmetic predicted (40 -> 64), because the
  inline capacity fell from 4 pairs to 2 and the heap pointer costs 8 bytes even
  when the vector never spills. It is off the per-edit path — P03 removed the
  per-summary Globals — so the cost lands per peer, not per edit.
- The seed actor and `collabedit`'s private state are content/replica-derived in
  different ways, and neither is a *device* identity. That is correct (a seed
  operation must be reproducible), but it means neither path is covered by
  `ReplicaState`'s durability guarantees.
- `StreamEntry::replica_id`, `ChatMessage::replica_id` and
  `Comment::replica_id` are still 16-bit producer ids and are **not** this
  device's actor. The ack-frontier file key is still a decimal replica id.
  These are L04's, recorded as inherited obligations.

**Three things worth keeping in view.**

*The narrow thing that caused four failures.* Rewriting the deletion-run encoder
for the new type dropped the run's opening bracket, so every edit carrying a
deletion run encoded to `"dr":[["actor",3],5]]` instead of `"dr":[[["actor",3],5]]`.
The decoder refused it, which is the good outcome: a decoder that accepted it
would have turned a malformed record into a plausible one. It is worth noticing
that the *tests* found it and not the diff review.

*The projection was incomplete, not wrong.* `Global` and `Anchor` were not in the
original cost table at all, and both grew (1.6x and 2.7x). §5 now carries the
measured numbers including those two rows and says why neither lands on the
per-edit path. The `FragmentSummary` figure the projection did predict — 40 -> 56,
1.4x on the SumTree's hottest structure — is exactly what the migration cost.

*Two anti-patterns had to die rather than be widened.* `collabedit` drew a random
16-bit id, scanned peers' presence files for unused values and wrote the winner
to a text file; `SeedOp` used replica 0, which was also the *default* replica of
every engine that did not configure one. Both would have kept compiling with a
16-bit field bolted onto the side and would have kept minting colliding
identities. The first is now `ReplicaState::open()`, the second derives its actor
from the seed content — same determinism requirement, no shared counter range.

**Next ready tasks:** A01, W01, S01 (all independently ready). Within P06a:
run-interval allocation, then the construction-time rejection of an invalid
ActorId, then opaque operation values.

## P06a step 3b — run-interval allocation from a borrowed counter

**Task:** P06a (still in progress; this is the allocation step)
**Status:** in progress — steps 3, 3b and 4 done; 5 and 6 remaining
**Baseline:** `788254e`
**Implementation commits:** this entry's commit
**Scope:** a local operation now costs exactly one checked reservation of
`n + 1` counters instead of one tick per inserted scalar. `Buffer`,
`IdList`, `CrdtEngine` and `CollabDocument` take a borrowed
`IdentityAllocator` instead of an `ActorId`; `ReplicaAllocator` binds one
(document, stream) of a `ReplicaState` to it. A deletion marks its tombstones
with the deleting operation's own identity, which removes the second
`deletion_ts` tick. `Buffer`'s and `IdList`'s local Lamport clocks are gone —
the durable counter is the clock, whose actor half is now persistent.

**Regression:** `tst_identity_allocation_engine.cpp`, 20 cases, written first.
It did not compile: `Buffer` had no constructor that accepted an allocator.

**Verification:**
- Debug 65/65 executables.
- Release 24/24 via `./tools/verify-roadmap.sh release`.
- ASan/UBSan/LSan 18/18 via `./tools/verify-roadmap.sh asan`, no suppressions.
- Artifacts: `build-roadmap-release/verification/20261005T062636Z-3388724`,
  `build-roadmap-asan/verification/20261005T064034Z-3395786`.

**API/store changes:** `Buffer(const ActorId&)` and friends are gone, replaced
by `Buffer(SharedAllocator)`. `Buffer::allocator()` is new. `ReplicaState`
gained `reserve_run(stream, document, epoch, count)` — one durable write per
interval instead of one per value — and `observe_remote`, which raises a counter
without persisting because §3.4 makes the raise re-derivable.

**Known limits:**
- The reopen case is a clean close/open, not a crash. Durable behaviour across
  a real power loss is L02/L08's evidence, and it now matters more than it did: a
  reservation that is never published burns a whole *interval*, not one value.
- `NetworkSim`, `tst_*` and `testapp` run on volatile counters, deliberately and
  by name. Persisted simulator counters would outlive the scenario that made them
  and a rerun would start where the last one stopped.
- `StreamEntry::replica_id`, `ChatMessage::replica_id`, `Comment::replica_id`
  and the ack-frontier key are still 16-bit and are *not* this device's actor.
  L04's, recorded as an inherited obligation.

**The finding that mattered most was a test failure I caused myself.**

V08's `comment_anchor_tracks_edits` fixture froze a raw `(1, 4)` Lamport pair
with a note explaining that "the clock advances twice per edit". Run-interval
allocation changes exactly that, and the fixture stopped resolving. The tempting
move was to restate the numbers — recompute the new counter, keep the green.

Instead the fixture now names the position ordinally: *the second item this
replica inserted*. That is what it always meant; the raw pair was an
implementation detail that happened to be stable when it was written. The
behavioural expectation (offset 1) is untouched and still asserted. A frozen
fixture that has to be unfrozen by an unrelated change is a fixture encoding the
wrong thing.

**One more place a refusal would have been silent.** `undo()` and `redo()`
decrement `m_undo_cursor` before doing anything else. Adding a fallible
reservation in front of the body without moving it earlier would have meant an
exhausted counter returned `nullopt` *after* the cursor had already moved — an
undo entry vanishing with no error anywhere. Both now reserve first. Nothing
failed to warn me about that one; it showed up while writing the change, which
is the only reason it is fixed.

**What this does not claim.** That a migrated engine converges across a
partition, that identity survives a crash, or that the abandoned-interval gap
path works. The first is the convergence suites (green here, and re-run by S06
against the model); the second is L02/L08; the third does not exist yet and is
P06b/P07/L02's obligation, recorded in TASKS.json.

**Next ready tasks:** A01, W01, S01. Within P06a: rejecting an invalid `ActorId`
at the public boundary (step 5), then opaque operation values (step 6).

## P06a step 5 — an invalid actor is refused, in Release too

**Task:** P06a (still in progress; this closes step 5)
**Status:** in progress — steps 3, 3b, 4 and 5 done; step 6 remains
**Baseline:** `4d5ca29`
**Implementation commits:** this entry's commit
**Scope:** `Buffer`, `IdList`, `CrdtEngine` and `CollabDocument` take a borrowed
allocator, expose `actor_id()`, and have no legacy integer constructor. Every mint
site now refuses an all-zero `ActorId` with `ErrorCode::invalid_identity`.
`IdList::insert_after_resolved` / `remove_at_resolved` became `Result`-returning
because a refusal inside them was being reported as success. The public API guide
gains §2.1 for the allocator contract.

**Regression:** one more case in `tst_identity_allocation_engine` — an engine over
an allocator holding the all-zero actor refuses edits, undo and redo with
`invalid_identity`, reserves nothing, and leaves the text empty.

**Verification:** Debug 65/65; Release 16/16
(`…/20261005T072729Z-3439371`); ASan/UBSan/LSan 12/12
(`…/20261005T074436Z-3464803`), no suppressions.

**The finding.** I had written the constructor as
`assert(m_allocator->actor().valid())`. That is a developer tripwire, not a
check: it disappears in Release, where the engine would then mint operations for
an engine with no author — unattributable, unorderable against any real identity,
and accepted by nothing. The test caught the real problem by *not being able to
run*: the assert fired before the case could assert that the refusal is reported.

The distinction the fix turns on: a **null** allocator is a programming error and
is asserted on; an **invalid actor** is a state — an engine may legitimately exist
before its private state has an identity — so it is refused at the reservation
site, where the refusal can be returned to a caller. Those two look identical in
the constructor and want opposite treatment.

The same reasoning caught `IdList`: `try_insert_after` delegates to
`insert_after_resolved`, which returned a *default-constructed* `IdListOperation`
on failure. `try_insert_after` therefore reported success with an empty
operation — indistinguishable from a real one whose fields happen to be zero.
Making the resolved bodies `Result`-returning is the fix, and it is the second
time in this task that a "cannot report failure" convenience turned out to be
hiding a refusal.

**Environment note.** `/tmp` filled up mid-run (7.7G tmpfs, 100%), and an ASan
build failed with `No space left on device` writing a compiler temporary. Nothing
in this worktree caused it — `/tmp/opencode` holds ~5G of build directories from
other sessions. The ASan re-run used `TMPDIR` pointed inside the worktree, which
is where EXECUTION.md wants build temporaries anyway.

**Step 6 needs a decision, not an implementation.** "Move operation field layout
out of the public header" cannot be done for a `std::variant` of by-value structs
without either handle types — which is the A02 rename, and would collide with it
— or leaving the layout visible and routing engine access through internal
accessors, which is weaker than the step asks for. That tension is recorded in
TASKS.json rather than resolved unilaterally; it belongs with whoever owns the
public boundary.

**Next ready tasks:** A01, W01, S01. Within P06a: step 6, once the above is
settled with A02.

## A01 — split CMake targets; Core builds with nothing else installed

**Task:** A01 **Status:** in progress **Baseline:** `897c95e`
**Implementation commits:** this entry's commit
**Scope:** four exported components — `CollabText::Core` (25 sources, C++20
only), `::FileTransport` (zstd, segments, file sync, StreamSync, sidecar
manifest), `::QtAdapter` (SyncManager), `::Widgets` (CollabDocument + 11 widget
sources) — plus `::TestSupport`, which is where NetworkSim and QtTest live.
Four new top-level options. `QCryptographicHash` replaced by a dependency-free
FIPS 180-4 SHA-256 in Core. A plain-C++ `collabtext_core_smoke` that runs with
Qt6, zstd and OpenSSL discovery all disabled.

**Regression:** `tst_sha256` (FIPS vectors, agreement with Qt's implementation
over 512 randomised inputs, the padding boundaries, embedded NULs) and
`collabtext_core_smoke`, which did not build before the split.

**Verification:**
- Core-only (Qt, apps, tests, transport all off; `CMAKE_DISABLE_FIND_PACKAGE_Qt6/zstd/OpenSSL=ON`):
  configures, builds, `collabtext_core_smoke` exits 0.
- FileTransport without Qt: builds; the only Qt6 entry in its cache is the
  disable flag we passed.
- Full Debug 67/67 — the 65 existing suites plus the two new ones. No test dropped.
- Release 10/10 (`…/20261005T094103Z-3618442`); ASan/UBSan/LSan 6/6
  (`…/20261005T095533Z-3645776`), no suppressions.
- Link interfaces inspected across the whole build tree: no production target
  links `collabtext_testsupport`, no non-test target mentions `Qt6::Test`.
- Runner contracts intact: a zero-match regex still exits 3, and the
  instrumentation assertion now samples `collabtext_core` and passes.

**Decisions worth recording.**

*Where `DurableFile` went.* Into Core, not FileTransport. `ReplicaState` needs
portable lease and durable-write primitives for private-state ownership, so
putting them in FileTransport would make Core link *upwards* to reach them —
precisely the trap the task's first step warns about.

*Why the hash is in-tree rather than OpenSSL.* OpenSSL::Crypto is the better
answer for a consumer who already links it. It cannot be the only answer:
`DerivedActor` needs a digest to derive a seed operation's and a simulated
replica's actor, so either the crypto dependency comes back into Core or
`DerivedActor` cannot hash at all. An in-tree implementation also has nothing to
export, which is the static-consumer problem the task names. The output is
byte-identical to what it replaced, and that is *checked* against Qt's
implementation rather than asserted — the digest is inside every stored segment
header and every manifest, so a subtly wrong hash would silently invalidate
them.

*What is still leaking.* `src/` remains on Core's build interface because public
headers genuinely include from it (`<collabtext/Operations.h>` includes
`<crdt/Clock.h>`). That is A02a's self-contained-header work. What this task
fixes is the *link* interface, which is the half a consumer has to install.

**The smoke test caught me writing a wrong assertion.** I asserted that two
engines inserting at position 0 would produce contiguous `hello `/`world`. It
produced `hweolrllod ` — every character, interleaved. That is finding F8:
convergent, and wrong for prose. It is S01/S02/S03's subject, and it is *not* a
packaging defect, so the honest fix was to assert the multiset of characters and
say why in a comment. Asserting contiguity there would have quietly turned a
component smoke test into an ordering gate that is meant to stay red for several
more tasks — the sort of well-meaning assertion that makes a suite lie.

**Environment note, repeated because it will bite again:** `/tmp` is full
(7.7G tmpfs). The ASan runs use `TMPDIR` inside the worktree.

**Next:** A02a — self-contained public headers over the components that now
exist. That is Wave's ask 2, and it is the gate on A06a/W01/W02/E07a.

## A02a — a public surface that stands on its own, and a manifest that admits what it cannot do

**Task:** A02a **Status:** complete **Baseline:** `ea751c7`
**Implementation commits:** `a77f852`
**Scope:** the engine's value types promoted to `collabtext/Engine.h`; `src/`
removed from every component's public interface; a new `IdListEngine` facade;
`StreamSync`'s header relocated out of the public tree; a versioned capability
manifest; a plain-C++ alpha consumer; and header independence enforced by one
translation unit per advertised header.

**Regression:** `collabtext_alpha_consumer` and the 16 header TUs, neither of
which could build before. The alpha consumer is the acceptance criterion as a
program rather than a sentence: it edits text and list content, moves operations
between two peers, and includes only `<collabtext/...>`.

**Verification:** full Debug 68/68; Core-only with Qt/zstd/OpenSSL discovery
disabled builds and both consumer programs exit 0; Release 14/14
(`…/20261005T134354Z-3785495`); ASan/UBSan/LSan 4/4
(`…/20261005T134918Z-3787056`), no suppressions;
`tools/check-public-headers.sh` in sync.

**Why the types had to move rather than the include path grow.** Six public
headers included `crdt/Clock.h`, `crdt/Locator.h` or `crdt/Anchor.h`. That was
not sloppiness — it is why A01 had to leave `src/` on the build interface. A
consumer of `<collabtext/Operations.h>` was reaching into the implementation
tree to get `Identity`, and there is no include-path arrangement that makes that
self-sufficient. So the values moved to `collabtext/Engine.h` and the four
`crdt/` headers became one-line shims, the same promotion `crdt/Operations.h`
already went through. About sixty engine translation units kept compiling
unchanged, which is the argument for doing it this way rather than renaming.

**The defect worth remembering.** `CrdtEngine::applyRemoteOp` returned `true`
unconditionally — it computed the `ApplyOutcome` and threw it away. A consumer
writing the obvious `if (!engine.applyRemoteOp(op)) retry();` would never retry,
and would believe a rejected operation succeeded. A bool that is always true is
worse than no bool, because it invites exactly that code. It now returns true for
applied-or-duplicate and false for deferred-or-rejected, and the header says
plainly that a bool cannot distinguish those two. The typed answer is A02's
`apply_remote(...) -> ApplyOutcome` rename, and the manifest records it as
*partial* rather than letting the fix imply the whole thing is done.

Two more of my own wrong assertions, both caught by the consumer and both fixed
by reading the implementation rather than by relaxing the check: `ids()` is
visible-only (my new facade doc claimed it included tombstones), and two peers
inserting at offset 0 produce `hweolrllod ` rather than two contiguous runs —
F8, which belongs to S01/S02/S03.

**What I did not do.** `StreamSync`'s header could have been made self-sufficient
with a pimpl. I did not, because that is a real answer to a question A06a owns
— what the transport's exported form is — and A02a inventing an ownership shape
nobody has reviewed is the same mistake as promising an API that does not exist.
The header moved to `src/transport/` instead, and the manifest records the
transport as implemented but not advertised.

**The manifest is the point of this task.** `collabtext/ALPHA_CAPABILITIES.md`
states as precisely what the alpha *cannot* do: affinity is stored and not read,
an unresolvable anchor still falls back to end-of-document, there is no stable
position-token encoding, undo does not survive a reopen, GC is inert, and there
is no epoch on an operation. Wave's requirement R3 — a detectable failure rather
than a silent relocation — is *not met yet*, and a consumer reading this file
learns that before writing code rather than after losing a reply's position.

**Next:** A06a — real `install(TARGETS)`/`install(EXPORT)`, a package config, and
a producer/consumer check that relocates the install tree before building against
it. That is the last packaging task, and it unblocks W01, W02 and E07a.

## A06a — Install and export a relocatable alpha package (2026-10-05)

Commits `666297b`, `b1cfa6c`, `f3360c0`.

**Why this was not packaging busywork.** A02a made the alpha surface usable, but
only from inside this repository. A consumer cannot consume a repository. More
importantly, a library whose only user is its own test suite has never been asked
the questions a consumer asks, and three of those questions failed the moment an
outside project tried to answer them.

**What the install now does.** Each component exports in its own export set
rather than sharing one. That is stricter than a single combined export file
requires — an unused imported target naming `Qt6::Widgets` is usually harmless —
and the strictness is the point: a consumer that asked for Core should not have
Qt visible anywhere in its configure output, not even as a string it never reads.
Static dependency records ride along per component, so `find_dependency(zstd)`
only happens when someone asked for the file transport.

The consumer fixture is a separate CMake project on purpose. It finds the package
by prefix path, includes headers as `<collabtext/X>`, links an imported target,
and compiles one translation unit per installed header. That last part is a real
check rather than a gesture: a header that needs an include path the consumer
does not have is not self-sufficient, and only compiling it alone proves
otherwise. `StreamSync` fails exactly this test — its header names zstd-reaching
segment types — so it stayed in `src/transport/` and the manifest says the
transport is implemented but not advertised. Splitting those types out of the
public header is what would let it ship, and that is a decision for whoever owns
the transport, not something to paper over with a pimpl.

**The relocation is the check.** `verify-consumer-package.sh` installs to prefix
A, moves it to prefix B, and only then configures the consumer against B. Not
because moving is clever, but because a consumer inside the source tree compiles
happily against whatever it can reach — an inherited include path, a source-tree
`-I`, a link against the build directory. Every one of those produces a green
build that proves nothing about the install tree.

**Three defects, one shape.** Every interesting failure in this task was a check
that reported success after failing, and I want them recorded rather than
quietly fixed:

- `finish()` read `$?` instead of its argument. Every explicit `finish 3` path
  therefore exited with the status of the preceding `echo` — zero. A failed check
  printed its own diagnostic and then reported `PASS`. It survived review because
  the first version of the gate had no failing path to take; I added the leakage
  check, watched it fail, and saw `PASS` printed underneath its own error message.
- The consumer emitted no `compile_commands.json`, so the leakage check had
  nothing to inspect and silently could not run. A check that cannot execute is
  indistinguishable from a check that passes.
- `results.json` recorded `step_status` rather than the status the caller saw.
  They disagree on paths that fail before any `run_step`, so the evidence file
  could disagree with the exit code.

The general lesson, now in `AGENTS.md`: a verification script that cannot exit
nonzero is worse than no script, and a new verification path must be tested by
making it fail. `--no-tests=error` is not optional; CTest exits 0 on a
zero-test selection, which reads exactly like success.

**Verification.** `verify-consumer-package.sh`, `--gate package`, components
`core,file-transport,qt`, in Debug, Release and ASan, each from a from-scratch
producer, each installing to A and moving to B, each configuring the consumer
with Qt/zstd/OpenSSL discovery disabled and asserting that the consumer's only
include path is `prefix-b/include` and that it links only from prefix B. Also
asserted: requesting a component the producer did not build fails with a message
naming the option that would enable it; requesting an unknown component fails;
an unbuilt tree passed via `--producer-build` exits 3.

Full Debug sweep: 69/70. The one failure is `tst_benchmark::memory_growth`
tripping the QtTest watchdog at 307s under a parallel sweep — a Debug-build
timing limit on an uninstrumented measurement, no engine source changed here.
The user asked for it to be skipped rather than tuned.

**The test ritual, rewritten.** Separately: the full sweep had come to default to
two workers and was taking over an hour, which is most of why it had stopped being
run routinely. `tools/verify-changed.sh` now routes on the diff — each changed
path maps to the tests that can observe it, and it builds only those in the warm
tree. The packaging change above verifies in about 30 seconds. Two properties
matter more than the speed: a path matching no route is reported rather than
dropped, and a zero-test selection is an error. A routing table that quietly
stops matching a file is exactly the regression a fast subset would hide.

**Not done, and not faked.** P06a step 6 (opaque operation values) remains open
and blocked on the recorded A02 boundary decision; S04a does not need it.
`ReplicaAllocator` is still source-tree only — making it public would commit to
its persistence format, which is a separate decision. The manifest now says so
rather than naming A06a as the owner of a thing A06a did not do.

**Next:** S04a, the design gate for scoped boundary identities. It is on Wave's
critical path — affinity, typed unresolvable anchors, and a persistable position
token are the three items the manifest lists as not implemented, and they are
the ones a consumer cannot design around.


## 2026-10-05 — Faster correctness iteration and completed S01 corpus

Baseline `8496194`; implementation `0db31aa`. Kept all changes in the
isolated reliability worktree. No engine performance optimization or benchmark
run; the user explicitly deferred that work.

Benchmarks now default OFF, are standalone explicit targets and are absent from
CTest even when enabled. They cannot run with normal/all/fresh correctness
selection. Verification reuses warm configuration, builds a single aggregate,
uses available bounded local ccache, and builds only enabled libraries for the
installed package gate. Corrected unknown-path/docs mixing, incomplete test
family routes, invalid-baseline handling, missing-option loops, empty/stale
selection checks and failure-status propagation. Twelve verifier regressions pass.

Completed **S01** with a documented fixture schema and seventeen histories:
forty-six operations, twenty-nine arrival permutations. Exact content and item
order, causal dependencies, compensation identities, scoped mint directives,
left/right and collapse offsets, scalar boundaries and later comparisons are
recorded. A Python validator and eleven parser regressions run via CTest without
compiling a static-data parser. This implementation choice is reflected in the
phase scope and task registry. Corpus SHA-256: `8722cb24e7fea9465615b1e355ae10f3d9f7a6ebdcc7f17339347183e8b0130b`.
Parser validation is not a repaired CRDT interpreter; S02/S04a must execute the
semantic histories. F7/F8 expected-fail markers and production behavior remain.

Evidence: bounded Debug **70/70** CTest tests passed (25.62 s CTest), relocated
Core consumer **1/1** plus dependency isolation and negative component checks
passed; combined gate 153.178 s. Same warm three-test selection measured
12.833 s before versus 4.421 s after; that measures orchestration only. Final
fixture/verifier selection **2/2**, **1.599 s**, including all 23 parser/verifier
unit cases. Configure-only checks confirm benchmarks ON never registers a
benchmark and Core-only tests/Qt/apps/transport/benchmarks OFF needs no Qt/Python
lookup. No fresh full build, Release/ASan/realistic sweep or clean-build cache
speedup claim. Details, portable results and fixture hashes are in the
[throughput review](2026-10-05-development-throughput-review.md) and its JSON.

**Handoff correction:** the previous entry/orientation prematurely claimed W01
and S01 were finished and S04a ready. S01 is completed by this commit. W01 is
still not started; its WP1–WP8 corpus is missing. P06a step 6 is unfinished, and
S04a's registry depends on **all** of P06a. P01 already requires opaque operation
values; no new policy answer is required. Finish W01 and P06a, then follow the
actual S04a/S04b/probe/ordering dependencies. Benchmarks and speculative engine
optimization must not occupy the current consumer delivery lane.

## 2026-10-05 — W01: the eight consumer probes, with their provenance kept honest

```text
Task: W01
Status: complete
Baseline: fcb7e71 (docs: record S01 evidence and correct consumer implementation handoff)
Scope: docs/specs/vnext-consumer-probes.md (new),
       libs/collabtext/tests/fixtures/external-position/*.json (14 new),
       tests/consumer-smoke/wave-contract/probes/{probe_contract.h,README.md} (new),
       tools/validate-consumer-fixtures.py (new),
       docs/reports/vnext-consumer-probe-provenance.md (new),
       docs/specs/vnext-external-position-contract.md, tools/verify_changed.py
Verification: corpus 14 fixtures / 32 operations / 28 tokens / 58 typed expectations,
       WP1-WP8 covered; --self-test 31 corruption kinds applied 351 times, all rejected;
       determinism re-run identical; probe_contract.h standalone on gcc 16 + clang 22.1.8;
       tools/verify-changed.sh PASS (debug/package); Debug suite 71/71
Known limits: specifies cases only; collapse placement and undo-restore of a referent
       are design_pending (S04a); offset units are our decision; no third-party figure
       was reproduced because the cited harnesses do not exist locally
Next ready tasks: W02 (installed baseline probes); P06a step 6 remains resumable
```

### The finding that shaped the task

The consumer's table has a column headed **"What it found"**, and every entry in
it describes Automerge, yrs or Loro — not collabtext. Reading that column as a
table of expected results would have inverted the entire task: eight fixtures
asserting that collabtext silently relocates 400/400 times, which is the defect
being reported about somebody else's engine.

So every expectation carries a machine-readable provenance tag, and the validator
enforces it rather than trusting it. `consumer_asserted` and
`wave_protocol_normative` must cite an external document; `roadmap_inference` must
cite the internal requirement it derives from. A derived expectation citing a Wave
document is rejected as a fabricated citation.

**That rule immediately caught me.** Seventeen of my own expectations were tagged
`consumer_asserted` while citing only our own W1–W6 contract — I had credited the
consumer with requirements I had written. They are now `roadmap_inference`. The
honest ratio is roughly 40% consumer-asserted; the number I started with was much
higher, and the difference was entirely self-congratulation.

### What the consumer did not say, and what we did about it

Nine things the four external documents simply do not state, recorded rather than
filled in: offset units, WP1's sweep corpus, **WP5's expected results** (ask2:204
offers "the exact expected results we need" and never writes them), WP6's target
result, WP8's answers, WP2's collapse placement, WP3's ownership split, the error
type names, and the Yjs version.

WP2 is the case worth naming. The consumer requires that deletion never be silent;
it does **not** say what a deleted referent should resolve to. Inventing a
fallback integer would have produced a fixture that looks authoritative and
settles S04a's design gate by accident. Instead the mandatory observable outcome
is recorded — a strict attachment surfaces detachment, a caret may resolve with an
explicit collapse marker — and the offset rule is tagged `design_pending` with
owner S04a. Same for whether undo restores a referent.

### The provenance report says the harnesses do not exist

The alternatives report states its harnesses are "under `/tmp/opencode/`" and
cites `loro-internal/src/state.rs:2122-2152` and
`crates/loro/tests/contracts/cursor_recovery.rs:164-171`. **Neither file is
present.** Nor is any identifiable yrs/Loro/Automerge probe harness; the
`probe*.cpp` files in that directory are unrelated SQLite/GTFS timing code. Only
an Automerge `LICENSE`/`Cargo.toml` and some extracted Loro snippets survive.

So 400/400, 1000/1000, 256/256, the 84-case sweep and exit 134 are carried as the
consumer's reported observations with `attribution:
reported_by_wave_not_reproduced_here`, and the validator **rejects any
third-party observation without that attribution**. Reproducing a number we cannot
re-run would be exactly the failure this roadmap has been auditing for.

Also recorded: the consumer twice cites
`docs/reports/2026-10-03-evaluation-artifacts/probes.cpp` as containing its
probes. That path is relative to collabtext, not Wave; the file exists and holds
*our* 13 evaluation probes, exactly one of which is probe 1 of the eight. Anyone
following that citation finds one block and stops.

### Three defects the validator found in my own work

- **Token observation excluded the parent operation.** A token minted by the
  latest operation referenced an item that operation had created, and was refused.
- **The neighbour-derived offset oracle was unsound.** My first version computed a
  boundary token's offset from its neighbours, which is right only when nothing was
  inserted at that boundary. Where content *was* inserted, the offset is
  affinity-dependent and the rule belongs to S04a/S04b. Tightening it to "the
  offset must lie between the end of the left neighbour and the start of the
  right one" constrains what can be claimed without inventing the rule that is
  still an open design gate.
- **The list-origin interval rule was wrong for lists.** A list element's identity
  *is* its insertion operation, so the two counters are equal by construction; the
  "item counter is below the operation counter" rule applies to text runs only.

Two more came from compiling the header: `offset = 0` is the start of a document,
not "unset" — my check was wrong, not the contract — and `PositionState::Unavailable`
with no error was passing `well_formed()`. A default-constructed resolution now
fails, because a value that reads as "a position was produced" before anything ran
is precisely the shape that reports success after failing.

### The self-test was wrong about itself, twice

The first version applied each corruption to one fixture and treated a mutation
returning `None` as "not applicable". Most of my mutations edited the case in
place and returned nothing, so **sixteen of thirty-one were silently skipped** and
the self-test reported success while covering almost nothing. It now detects
applicability by comparing against a snapshot of the original, and fails if any
mutation never applied anywhere.

Two mutations were also not corruptions at all. Replacing a neighbour with
`START` produces a *legal alternative history*, and reversing the arrival order of
two concurrent operations violates nothing. A self-test full of mutations that
cannot fail is decoration; both were rewritten to break a rule the validator
actually enforces.

To confirm the self-test can go red at all, the arrival-causality rule was
disabled: it then reported nine corruptions it had been accepting. Restoring the
rule returned it to green.

### Scope, stated plainly

W01 specifies cases. Nothing here is evidence that collabtext passes a probe, and
the fixture validator's own output says so on every run. The harness in
`tests/consumer-smoke/wave-contract/probes/` is a contract header with no `main`,
no engine call and no assertion, because a green run here before an adapter exists
would be the exact thing this workstream keeps catching. W02 builds the installed
adapter and the result matrix; S04a/S04b supply the live API behind it.

`PositionResolution::well_formed()` is the one piece of real enforcement shipped
here, and it encodes the rule the probes keep asking for: a resolution carries an
offset **or** a typed error, a failure state must say why, and `Collapsed` is not
`Resolved`.


## 2026-10-05 — Complete P06a's opaque values and resume native boundary design

Baseline `2157cfb`; implementation `01614af`. Public text/list operation
variants and payload fields moved behind distinct immutable values. The private
payload header is not installed. Builders freeze before publication and private
access is const-only. Copyable shared const ownership revises P01's move-only
draft to retain existing independent queue/callback/transport lifetimes without
mutable aliases or a new clone protocol. The API guide records that choice.

New lifetime tests cover retained copies, builder mutation, decoder/input
destruction and empty/moved-from rejection. Public codec tests now exercise real
engine outputs using public headers. A first timestamp-only type predicate missed
legacy variants; the final variant-exclusion predicate demonstrably fails for
both baseline operation types and accepts opaque headers. This is compile-time
API evidence, not behavioral ordering evidence. Identity remains 24 bytes,
FragmentSummary 56; both opaque handles are 16 bytes plus private heap storage.

Verification: **Debug bounded 72/72**, relocated installed Core package/consumer
passes; **Release selected 9/9**, **ASan/UBSan/LSan selected 9/9**. The restricted
sanitizer run passed assertions but could not perform LSan's process inspection;
only the unrestricted run with leak checks enabled is accepted. Exact selection
and paths are in TASKS.json. No fresh full rebuild, realistic simulation or
benchmarks; allocation/order laws are unchanged and their models run in all
three configurations.

W01's 31 corruption kinds/351 applications reproduced. Competitor figures remain
attributed observations. Direct Wave source review found an affinity-comment
mismatch and a UTF-8 conversion requiring text-safe token wrapping; the review
report gives paths and adapter obligations. A pending strict-referent fixture
also conflates a visible range with a caret gap; S04a will define those precisely.

P06a is complete; **S04a is now structurally ready**, alongside W02/P06b. Proceed
with scoped boundary semantics and independent model before S04b production code.
Publication/master/consumer edits remain outside this local implementation.


## 2026-10-05 — S04a: scoped boundary gate accepted

Baseline d52dceb; implementation 45fd306. A complete native boundary contract,
implementation map, candidate canonical codec and compile-checked target signatures
now precede S04b. Positions use scoped stable item fences, mint causal coverage
and retained logical order, with separate caret collapse and strict selected
referent loss. A valid undo restores the original token when its item becomes
visible; physical history loss remains unavailable. Local clear invalidates
position history even for old empty-edge tokens. S02 must preserve the relative
order of integrated retained items and generate the supplied orders itself.

The independent model first failed on WP2's original range mislabeled as a gap.
WP8 also selected a surviving character while asserting detachment of another.
Both fixtures are corrected with explicit selected referents and provenance.
WP2's pending rules are replaced by exact byte-1 collapse and same-token undo
restoration. The corpus validator now rejects these mistakes: 37 corruption
kinds applied 362 times, all rejected. WP6 prose follows S01's exact native tie
abcXYZ, without attributing that choice to the consumer.

Verification: `tools/verify-roadmap.sh {debug,release,asan}
'^tst_(boundary_model|sequence_fixtures)$'` passed 2/2 in each mode. Final oracle
checks passed `verify-changed.sh --target tst_boundary_model` in Debug/Release;
a narrow unrestricted final ASan/UBSan/LSan ctest passed 1/1 after the broader
approval review timed out. Leak checking stayed enabled. Model harness links only
QtTest/QtCore, not production CollabText. Tests: 21 fixture histories, 50 minted
gaps, 48 placements, 24 comparisons; 472 supplied text/list orders, 75,328 states,
602,624 result assertions and 944 missing-state assertions. Three reduced model
mutation violations are detected. Seed 20261003 repeats with identical traces,
SHA256 1cbd4c70d0e27a700551ac0da36d07c3771344b54b6b1dab36f6f0d350ca6a5c.
Exact logs/paths belong to TASKS.json. Verification routing checks pass 13/13;
model-header edits now select their own executable, and external fixture edits
run model/anchor checks. No benchmarks or engine optimization.

S04a is complete. P06b is next before S04b; W02/S02 are separately ready.
No production F7/F8 marker or installed position capability was promoted.


## 2026-10-05 — P06b partial: repair actual public syntax/Unicode decoding

Baseline fa3e11a; partial implementation 3b19e22. P06b remains in_progress: the versioned Result API and
allocation/gap codecs are still pending, with an exact implementation contract
in vnext-operation-codec.md. This is a real native decoder repair, not a completed
wire-format task or a store-recovery claim.

Regression 1: the public decoder accepted an actual operation followed by garbage.
Regression 2: a valid escaped emoji became invalid surrogate UTF-8 bytes. Each
new test failed before its corresponding repair. Public operation/list/global
paths now use bounded complete-input syntax validation; root v is treated as a
causal vector in syntax-only mode. String decoding reuses the existing checked
Unicode implementation. Invalid/oversize/duplicate input is refused and escaped
non-BMP content preserves the exact logical operation on replay. Public header
warn-and-skip advice is removed; caller/legacy transport retention still needs
its actual transactional implementation.

Verification: Debug bounded **72/72** plus installed relocated Core/package gates,
**81.572 s total**, **9.36 s CTest**; Release **7/7**, **66.765 s total**,
**0.67 s CTest**; already-built unrestricted ASan/UBSan/LSan **7/7**, **11.75 s**,
leak checks on and no suppressions. Exact paths/selections are in TASKS.json.
No benchmark, optimization campaign or long realistic simulation. Manifest v3
records implemented syntax/Unicode and keeps typed/versioned codec and legacy
file transactions explicitly unavailable. S04b remains dependency-blocked by
unfinished P06b; W02 and S02 remain independently ready.

## 2026-10-05 sprint — P06b complete: versioned native codecs

Replaced interim optional/bare-JSON codecs directly with Result-returning version
1 CTBO/CTLO/CTGV/CTGP frames; no legacy readers or compatibility wrappers. All
integer fields use fixed-width exact hex strings, and text allocations partition
one checked actor interval ending at the operation identity. CounterGap codecs
own unused-interval metadata; publication and terminal integration remain P07/L02.
One bounded JsonReader tree replaces the old duplicate parser. The writer applies
the same byte/string/tree budgets, refusing invalid/default/moved-from values.

A runtime red regression first demonstrated that optional decoding could not
report an unsupported version. Seven independently assembled golden frames then
matched actual engine output for every variant, sparse vectors and gaps. Cases
cover every truncated prefix, wrong family/version/tag, strict fields, identity,
interval arithmetic, Unicode, actor/string/frame limits and exact values above
2^53 through UINT64_MAX-1 (UINT64_MAX application values). Historical synthetic
fixtures were corrected to P06a's valid allocation domains; validation was not
weakened. All codec callers use checked Results. Installed examples exercise
both primitives, typed versions, vectors and gaps using public headers only.

FileSync and StreamSync now retain a failed batch and freeze that reader before
any application/callback/cursor commit; later records cannot pass it on the next
poll. Typed errors and original bytes are inspectable. This is containment,
not a certified recovered state/cursor transaction; generic stream payloads may
remain opaque. Original operation ownership stays with a refused encoder's caller.

Verification: Debug **72/72** bounded suites, **9.82 s** CTest; Release **14/14**,
**0.38 s**; ASan/UBSan/LeakSanitizer **14/14**, **10.89 s**, leak checking active.
Relocated installed Core package/header isolation/negative-component gates pass.
The first new installed list case used an unsupported legacy end anchor; changed
that codec case to a valid start insertion, with no engine semantic change.
Logs: `/tmp/collabtext-sprint-codecs-{debug-full,package,release,asan}.log`;
exact build/package artifacts are retained in TASKS.json. No benchmark or long
realistic simulation ran. **Next is S04b's production positions.**

## 2026-10-05 sprint — S04b complete: native scoped positions

Added immutable scope-bound text boundaries, strict content attachments, list
boundaries and stable insertion-identity list elements through the existing
native Buffer/IdList facades. Checked traversal projects retained native items;
no new sequence engine. Exact affinity, real empty/start/end tokens, UTF-8 scalar
boundaries, explicit collapse and strict detach/undo restoration are implemented.
Foreign context is refused before item lookup; local_clear invalidates even edge
history. Reads reserve no identities and emit no callbacks or operations.

CTPT v1 owns binary bytes with bounded canonical scope/frontier/fence fields.
All four kinds have golden/ownership/enum/cap checks, every truncated prefix and
all 256 trailing-byte values. Tests explicitly reject success before inspecting
an error code; default Error.code alone is not negative evidence. The initial
incorrect corruption offsets were fixed, and the broad build's ambiguous private
namespace reference was qualified without changing assertions.

Debug **74/74**, **7.11 s** CTest; Release **7/7**, **0.55 s**; leak-enabled
ASan/UBSan/LSan **7/7**, **22.94 s**. Relocated installed Core/header/isolation
checks pass. Native/model differential coverage: **1,050 text**, **550 list**
resolutions plus concurrent/hidden-mint histories. Independent fresh processes
reuse original operation/token bytes (artifact SHA256
`a0b7cb07d590643bc9621f9a9064829fff0243ec7c38b53c6f761c2487a7eb8a`).
Warm arc rebuilds took 140.518/164.72 s; routine checks stay targeted. No benchmarks.

Manifest v5 advertises actual live APIs and labels token bytes experimental and
unpublished. Installed strict consumer certification is W03. Repaired ordering,
real store recovery, persistent undo and durable retention remain outstanding.
**Next: W02 → W03 installed consumer gates.**

## 2026-10-05 sprint — W02 complete: installed consumer baseline

Added an installed-only, consumer-owned Document fixture with block identity,
local/remote operation routing, exact text/list position forwarding and
Resolved/Collapsed/Stale/Gone mapping. It uses no Wave checkout, source-tree
headers, Qt widgets, event log or engine-owned block map. The case table executes
all fourteen W01 fixtures under WP1–WP8 and compares worker JSON to independent
reviewed expectations; workers cannot classify their own results.

The relocated Core baseline has 17 cases in Debug and Release: 15 pass, the
exact WP6 `aXbYcZ` passage-order defect is `known_failure` owned by S02/S03, and
durable pins are `unsupported` owned by S05/G01/G06/G08. The no-GC smoke remains
containment evidence only. Every fixture/result retains case ID, capability,
format, seed, fixture hash, revision, stdout/stderr, exit status and signal.

Negative checks are active: a deliberately bad expectation fails, an injected
SIGABRT fails with signal 6, and a Debug producer is refused for a Release gate.
The optional Qt installation exposed and fixed an export dependency omission:
QtAdapter's enabled FileTransport target is now loaded when requested. Core-only
consumers still discover no Qt, zstd or OpenSSL. Local workflow YAML/shell checks
pass; remote CI is pending branch publication.

Evidence: `build-consumer-verify/debug-baseline-20261006T010146Z-2`,
`build-consumer-verify/release-baseline-20261006T010013Z-2`,
`build-consumer-verify/debug-baseline-20261006T010107Z-2`,
`build-consumer-verify/w02-negative/`, and
`docs/reports/vnext-consumer-probe-baseline.md`.

**Next: W03 strict installed live-position probes.**

## 2026-10-06 sprint — W03 complete: strict installed live-position gate

```text
Task: W03
Status: complete
Baseline: ad0ae59
Implementation commits: (this commit)
Scope: tests/consumer-smoke/wave-contract (WP5 two-process replay, phase-aware
  runner), tests/consumer-smoke/markdown-contract (new independent consumer),
  tests/consumer-smoke/CMakeLists.txt, tools/verify-consumer-package.sh
  (--gate live plus retained-evidence assertions), .github/workflows/
  consumer-alpha.yml, capability manifest v6, report and specs.
Regression: a wrong strict expectation injected into cases.json fails
  consumer_wave_contract and the gate exits 3; the retained-evidence checker fed
  a baseline result refuses it; wrong Markdown expectation and injected SIGABRT
  each exit 1 with signal 6 recorded.
Verification:
  tools/verify-consumer-package.sh --mode debug   --gate live --components core
  tools/verify-consumer-package.sh --mode release --gate live --components core
  tools/verify-consumer-package.sh --mode asan    --gate live --components core
  All 3/3 CTest, 16/16 strict wave rows of 18, 4/4 Markdown, manifest v6.
  W02 baseline gate re-checked at its own 2/2 contract.
Artifacts: build-consumer-verify/debug-live-20261006T015403Z-347884,
  build-consumer-verify/release-live-20261006T015432Z-348930,
  build-consumer-verify/asan-live-20261006T015505Z-349802,
  /tmp/opencode/w03-live-negative.log,
  docs/reports/vnext-consumer-live-position-gate.md
API/store changes: no public API or format change. ALPHA_CAPABILITIES.md v6 adds
  the installed strict probe capability and narrows the crash-recovery row.
Known limits: no baseline allowlist entry removed — WP6 stays the exact aXbYcZ
  known failure (S02/S03) and durable pins stay unsupported (S05/G01/G06/G08).
  The no-GC row is containment evidence, not certified reclamation. The
  two-process replay is codec/token evidence, not L08 crash recovery. CTPT/1 is
  experimental and unpublished. Remote CI pending branch publication.
Next ready tasks: E07a, S02
```

The strict set is WP1–WP5/WP7 plus the WP8 no-GC containment smoke: 16 rows, and
a strict row that is not a pass fails the gate rather than being reclassified.
The complete 18-row matrix and both baseline exceptions stay in the results, so
the pending work is visible instead of absent.

WP5 gained what the probe actually asks for. One process mints a left boundary, a
right boundary, a strict attachment and a list element and persists six records
through the fixture's own length-prefixed file; a second process builds three new
engines — two physically independent `CrdtEngine` instances and one
`IdListEngine` — replays the recorded CTBO/CTLO operations, takes a remote edit,
and resolves the *original* token bytes after every input frame has been
overwritten. Observed `aXb` in both replicas at `[1,2,2]` and `list_index=0`.
Nothing is reminted after reopen.

The Markdown consumer is deliberately a second, independent composition: it owns
the Markdown bytes, a sibling block map and an optional transport, and shares no
adapter, support header or expectation table with the Wave-shaped fixture. It
round-trips 51 exact bytes including a non-ASCII scalar, assembles a peer purely
from routed opaque frames, routes exactly one frame per edit while leaving an
unattached document's edits off the wire, and shows a collapsed caret at offset 5
beside a strict attachment that is `Stale`/`detached` with no offset.

Worth recording: the exact-comparison runner caught a real bug in this task's own
new worker — the Markdown `exact_bytes` case emitted an unclosed JSON array. It
read as plausible output when eyeballed and was only caught because the
expectation lives outside the worker. That is the whole argument for keeping
expectations hand-written.

CTest passing is not treated as the assertion. After CTest, `--gate live`
re-reads both retained result files and refuses the run unless the wave result is
genuinely `gate: live`, covers exactly WP1–WP5/WP7, has every strict row at
`pass`, agrees with its own recorded counts, and the Markdown gate passed every
case strictly.

**Next: E07a/E07b alpha preparation, and S02 for ordering repair.**

## 2026-10-06 sprint — S02 complete: the ordering repair is specified and modelled

```text
Task: S02
Status: complete
Baseline: b5de626
Implementation commits: (this commit)
Scope: docs/specs/vnext-sequence-algorithm.md (new),
  docs/reports/vnext-ordering-design.md (new),
  libs/collabtext/tests/reference/SequenceModel.h (new),
  libs/collabtext/tests/tst_sequence_model.cpp (new), BoundaryModel.h named-status
  helpers, CMake registration, changed-path routes. No production code changed.
Regression: productionAtomizingRuleIsRejected reproduces today's atomizing rule
  and shows the S01 oracle rejects it on the four comparable fixtures.
Verification:
  tst_sequence_model 8/8 in Debug, Release and leak-enabled ASan
  tst_boundary_model 8/8 (BoundaryModel.h gained named-status helpers)
  tools/verify-changed.sh -> 75/75 selected plus the package gate
Artifacts: docs/specs/vnext-sequence-algorithm.md,
  docs/reports/vnext-ordering-design.md
API/store changes: none. No public API, format or store change.
Known limits: no production code changed — the engine still interleaves concurrent
  passages and WP6 remains an exact known failure. No maximal non-interleaving is
  claimed. Reverse typing depends on Buffer::locator_between producing a strictly
  decreasing locator, which S03 will touch. Equal-boundary IdList ordering is
  lightly evidenced (2 fixtures vs 6 text fixtures). No benchmark ran; no fresh
  acceptance sweep.
Next ready tasks: S03, E07a
```

The defect is named exactly rather than described. `Buffer::normalize_fragments`
(`Buffer.cpp:498`) atomizes any same-`Locator` multi-actor run into single scalars
and sorts the pieces by per-scalar identity. Item counters are per-actor, so two
three-character pastes own counters `1,2,3` each, counter-first ordering ranks
`A/1,B/1,A/2,B/2,A/3,B/3`, and the result is `aXbYcZ`. Reproduced against the
installed build before writing anything.

The repair keeps whole operations contiguous and orders same-position operations
by their first inserted item — counter ascending, then actor bytes ascending.
Document order is the pre-order walk of the insert-after tree, with an explicit
precedence rule for groups inserted immediately before a sibling. That rule is
what makes reverse typing reverse; the branch rule is the F8 fix.

What makes this more than a comparator tweak, and what the design record makes
S03 falsify first: the comparator was never the defect. `FragmentOrderDim`'s
`<=>` is already `(locator, origin)` and `origin` is already the per-operation
key — feeding it atomized scalars was the error. `Fragment` already *is* the
group. The transformation map touches one function body and adds no type, field
or wire format.

The independent interpreter links no production ordering, `Locator`, identity,
`Buffer`, `IdList`, `Fragment`, anchor or codec code. It reproduces all 17 S01
fixtures exactly, all 29 arrival orders converge to identical outcomes, and the
order it computes is handed to the independent `BoundaryModel` for 22 named token
positions and 14 comparisons.

The oracle was checked for teeth, because a model agreeing with itself proves
nothing. `productionAtomizingRuleIsRejected` reconstructs today's rule and shows
the fixtures reject it on exactly the four comparable fixtures — `aXbYcZ`,
`aXbYcZ!`, `cZbYaX`, `aXbYcZ`. Restricting to four was deliberate: a naive
atomizing sketch also "disagrees" with the nine seeded fixtures because it
ignores seeds and deletes, which would have made the assertion look stronger
while proving less.

Three real bugs in the model surfaced during this work and are written up in the
design record: undo looked up an item id where the fixture names an operation,
redo cleared the record but not the per-item visibility flag, and the S01
`expected.order` field is the *visible* order — a distinction now asserted
explicitly by checking that visible plus hidden equals the total order, so no item
can be quietly dropped.

Nothing here is production behavior. The engine still interleaves concurrent
passages, the installed WP6 probe is still an exact known failure, and the
capability manifest still records F8 as `not implemented`.

**Next: S03 applies the algorithm; E07a prepares the alpha candidate.**

The paragraph above is the state at the end of S02. The 2026-10-06 entry below
is what happened next.

## 2026-10-06 — S03 complete: FugueMax placement, registry closed after the interrupted session

```text
Task: S03
Status: complete
Branch: feat/fuguemax-placement
Recorded revision: 0984d93
Implementation commits: 44cd0d6, a1f9218, 6ce7956, 337179d, e21ff06, cc7d8cc, 0984d93
Scope: FugueMax placement in Buffer and IdList; Locator removed; CTBO/CTLO
  advanced to 0002. Spec section 0 of vnext-sequence-algorithm.md.
Verification retained on disk:
  ASan 18/18 exit 0
    build-roadmap-asan/verification/20261006T063331Z-599681
  Release 30/30 exit 0 (CTest real time 1.32s, warm tree)
    build-roadmap-release/verification/20261006T063935Z-606072
  Debug installed live gate PASS (wave 16/16 strict, markdown 4/4, manifest v7,
    WP6 observed abcXYZ at CTBO/2 and CTLO/2)
    build-consumer-verify/debug-live-20261006T063910Z-605372
  Debug installed baseline gate PASS (WP6 observed abcXYZ; that result still
    labels CTBO/1 and CTLO/1)
    build-consumer-verify/debug-baseline-20261006T063828Z-604623
  Fixture validator self-test: 37 corruption kinds, 362 applications, all
    rejected. Corpus: 14 fixtures, 32 operations, 28 tokens, 57 expectations.
Not retained: a session-reported Debug 78/78 pass. No evidence directory.
Closeout commit 0984d93: probe labels CTBO/2 and CTLO/2, newer-version probe
  CTBO0003, WP6 baseline exception for aXbYcZ removed. The live and baseline
  gates above ran before that commit and were not re-run.
Known limits: WP8 durable pins unsupported. Placement is O(n). Tombstones are
  not reclaimed. No benchmark. No fresh full-suite sweep. No remote CI.
Next: E07a, then E07b.
```

The session that finished the Release gate stopped before writing `TASKS.json`
or this log. `0984d93` then committed the fixture labels and removed the WP6
exception, and this entry marks S03 complete. Details are in
`docs/handoff/2026-10-06-s03-progress.md`.

Entries above this one are dated records. Several say the engine still
interleaves or that WP6 is a known failure. That was true on the day they were
written. Current instructions are `docs/roadmap/2026-10-reliability/DIRECTION.md`.

## 2026-10-06 — E07a complete: the 0.2.0-alpha.1 package alpha candidate is prepared

```text
Task: E07a
Status: complete
Branch: feat/fuguemax-placement
Baseline: add5cf1
Candidate commit: bdbae5c3d310f2388b1d445a254b4ac35474d215
Implementation commits: 6c56306, 1e6de60, 4bf732f, bdbae5c
Scope:
  CMakeLists.txt and libs/collabtext/CMakeLists.txt version metadata
  cmake/collabtext-config.cmake.in (COLLABTEXT_PRERELEASE, _VERSION_STRING)
  libs/collabtext/include/collabtext/ALPHA_CAPABILITIES.md v7 -> v8
  docs/releases/integration-alpha.md (new)
  docs/USING_VNEXT.md, docs/specs/vnext-alpha-capabilities.md
  tools/prepare-integration-alpha.sh (new)
  tools/verify_changed.py route + self-test for the new script
Regression: no failing-test-first step, because this task changes no algorithm.
  Four defects were found by running the gates and the preparation script rather
  than by writing a test:
    1. wave-contract/run.py recorded CTBO/1 and CTLO/1 while its own cases.json
       declared CTBO/2 and CTLO/2, so every retained probe result disagreed with
       the codec it exercised. S03's log entry had noticed the mislabel and left
       it. Now derived from the case file; a missing family fails the run.
    2. verify-consumer-package.sh took a relative --output-dir, installed
       successfully, then failed the consumer configure with "Could not find a
       package configuration file provided by collabtext" -- blaming the package.
    3. prepare-integration-alpha.sh exited 0 after a failed step: the dependency
       record died on an unbound $CXX under set -u, the traceback printed, and
       the script still printed "wrote alpha-candidate.json".
    4. "${producer_flags[*]}" reached the JSON as literal text, because a
       parameter reference inside a nested $(...) inside a heredoc is never
       expanded. The output parsed and looked plausible.
  Also: SHA256SUMS verification failed on the archive's own entry (verified twice
  from the two directories now), and the format check compared lists as strings
  and rejected an identical set in a different order.
Verification:
  package gate  Debug    PASS 11 steps, manifest v8, exit 0
    build-consumer-verify/e07a-debug-package
  package gate  Release  PASS 11 steps, manifest v8, exit 0
    build-consumer-verify/e07a-release-package
  package gate  ASan     PASS 11 steps, manifest v8, exit 0, no sanitizer or leak report
    build-consumer-verify/e07a-asan-package
  baseline gate Debug    PASS 18 rows, 17 pass, WP8 durable pins unsupported
  baseline gate Release  PASS 18 rows, 17 pass, same single unsupported row
    build-consumer-verify/e07a-{debug,release}-baseline
  All five at bdbae5c. WP6 observed abcXYZ. No known failure in the matrix.
  prepare-integration-alpha.sh PREPARED (not published), accepting all three modes
    build-alpha/integration-alpha-20261006T174015Z-43813
  archive sha256 356f639526ef146ecb653efd3e93e25f333318e3622a744a2a4e029b41396f9f,
    identical across two independent runs, 24 checksum entries all re-verified
  clean clone at the candidate SHA: built, installed, and consumed by a separate
    CMake project with public headers only; printed text=aXb byte_offset=1
    token_bytes=104
  find_package: 0.2.0 accepted, 0.2.1 refused, 0.1.0 accepted
  negative paths, each observed with its exit status: wrong branch 3, dirty tree
    3, existing tag 3, version mismatch 3, evidence from another SHA 3, evidence
    from another gate 3, evidence dir without results.json 3, output dir outside
    the worktree 2, unknown option 2, injected unexpanded ${...} 3
API/store changes: no engine API change. The installed manifest is now v8 and
  names the package version. Three version variables are added to the installed
  config file; no public header changed. CTPT/1 remains experimental and no
  format is declared supported for durable storage.
No external action: no push, no remote tag or release, no package publication, no
  merge to master, no consumer message. The preparation script performs no network
  access and has no code path that could. Remote CI is recorded as pending
  because nothing triggered it.
Known limits:
  - Nothing is published. The candidate commit is in a local worktree, so no
    consumer can fetch it yet; reachability on the remote is a publication step.
  - docs/specs/vnext-operation-codec.md still describes the pre-bump version 1
    frame table. prepare-integration-alpha.sh fails a candidate whose notes
    disagree with the codecs, so the release notes cannot drift, but the spec
    text is still stale and E04 owns it.
  - Anchors across a durable reopen, recovery, persistent undo, durable pins and
    GC are all still pending. WP8's durable pin registration is `unsupported`.
Next ready tasks: E07b (recommended next), P07.
```

CMake's `project(VERSION)` accepts only a numeric version, so a SemVer prerelease
cannot live there. The release line is `0.2.0` and the prerelease label is
`COLLABTEXT_PRERELEASE`; `COLLABTEXT_VERSION_STRING` carries `0.2.0-alpha.1`.
The consequence — a `find_package` request cannot name the prerelease label — is
documented in the manifest, `USING_VNEXT` and the release notes rather than left
to be discovered.

## 2026-10-06 — E07b complete: position alpha candidate prepared

```text
Task: E07b
Status: complete
Branch: feat/fuguemax-placement
Candidate: 0.2.0-alpha.2; exact commit recorded in generated alpha-candidate.json
Proposed tag: v0.2.0-alpha.2 (not created)

Strict installed live gates at the final clean candidate commit:
  - Debug: PASS, consumer 3/3; Wave 16/16 strict (18 rows, manifest v9); Markdown 4/4
  - Release: PASS, consumer 3/3; Wave 16/16 strict (18 rows, manifest v9); Markdown 4/4
  - ASan/UBSan: PASS, same strict counts, with LSAN_OPTIONS=detect_leaks=0
    because LeakSanitizer cannot run under this environment's ptrace; no leak
    checking is claimed for this run.
  - Evidence: build-consumer-verify/e07b-{debug,release}-live and
    build-consumer-verify/e07b-asan-live-lsanoff

Preparation:
  - tools/prepare-integration-alpha.sh --gate live accepted all three retained
    result sets at the candidate revision.
  - build-alpha contains the generated candidate record, relocated install
    archive, checksums and unexecuted publication checklist.
  - Release notes now record the exact gate outcomes and sanitizer limitation.
  - No tag, publication, merge, push, or consumer message was made.

Previously completed E07b implementation evidence remains: CTPT/1 supported
reader fixtures contain 16 golden cases for all four token kinds;
tst_position_token_reader passes 8/8 with all four negative controls rejecting.
The manifest and consumer guide keep recovery, persistent undo, durable pins and
GC as unfinished capabilities. WP8 durable pins remain unsupported.
```

Representation evidence recorded:
  - AnchorCodec.cpp written by S04b, byte-identical through S03 to HEAD.
  - No token field names a parent, side, right origin, or locator.
  - S04b golden bytes still assert unchanged post-S03.
  - tst_position_token_reader replay check re-mints all 16 goldens on current
    engines and compares bytes exactly.

Defects found and fixed during E07b:
  - First draft of goldens shared one actor counter across cases, making them
    not independently reproducible. The replay check caught it; each case now
    uses a fresh counter.
  - A length-only layout assertion PASSED when 24 bytes were injected into a
    golden (total stayed consistent with "two item fences"). The literal reader
    rejects it. Header comment corrected.
  - Literal reader initially had a premature at_end() check before reading
    frontier entries, and did not handle kind 4 (TextAttachment) fences.
  - specified_length double-counted the selection byte for attachments (off by
    one). Fixed.

Negative controls verified (each fails with a diagnostic):
  - Injected 24-byte field into a golden -> literal reader rejects.
  - Corrupted fence tag to 0x05 -> literal reader rejects.
  - Fixture declaring CTPT/2 -> load_goldens refuses.
  - Recorded byte_length off by one -> length comparison fails.

## 2026-10-06 — P07 complete: transactional transport contract

```text
Task: P07
Status: complete
Branch: feat/fuguemax-placement
Baseline: d3733971614eb578be0948d9237891ac46695d95
Implementation: 700415d6676d36b525b06c8be9233b8eef302e0e

Delivered:
  - Replaced OpStream's void payload push, callback-only inbound delivery and
    scalar lowest-peer Lamport fence with stable envelope tokens, explicit
    queued/durable push modes, bounded poll, commit/retry/quarantine decisions,
    recovery points, scoped receipts and subscription lifetimes.
  - Added normative docs/specs/vnext-transport.md and aligned the public API
    guide. The stream key follows the current bounded ASCII StreamId contract.
  - Added tst_transport_commit_contract with a memory-only reference adapter.
    It exercises missing handlers, retry, quarantine, durable recovery points,
    per-stream/per-producer receipt isolation, ordering, byte budgets,
    idempotent push, callback unsubscribe/reentrancy and typed unknown scopes.
  - StreamSync had already moved to src/transport during A02a, not the planned
    public include path. It no longer implements OpStream: its legacy cursor and
    scalar observed-operation ack cannot prove the durable recovery-point
    transaction. Existing convergence suites now call that internal legacy
    adapter directly; its file recovery remains L03/L08 work.

Verification at the implementation commit:
  - Debug and Release: tst_opstream_interface + tst_transport_commit_contract,
    2/2 each (verification logs under build-roadmap-{debug,release}).
  - Installed Debug package gate: pass; relocated Core consumer 1/1, installed
    headers compile independently, unknown component request refused.
  - Full changed-path sweep on the implementation worktree: 79/79 correctness
    tests and installed Debug package gate passed.
  - Six existing legacy StreamSync integration suites passed in Debug/Release.
  - validate-plan.py: 72-task dependency graph and consumer sequencing valid.

No production transport capability is claimed by the test double. Legacy
StreamSync remains unadvertised until its durable receiver transaction lands.
No merge, push, tag, publication or consumer message was made.
```

## 2026-10-06 — P08 complete: bounded decoding and fuzz coverage

```text
Task: P08
Status: complete
Branch: feat/fuguemax-placement
Baseline: 15ae0f8
Implementation: 4714067

Delivered:
  - Bounded zstd input/output, rejected truncated or concatenated frames, and
    added no-progress protection. Segment headers now reject unsupported kinds,
    flags and excessive record counts; payload/checksum/size validation remains
    all-or-nothing.
  - Base64 decoding now requires canonical terminal padding and rejects
    non-zero unused bits. Segment payload encoding/decoding has explicit byte
    and record budgets.
  - Extracted exact-size cursor and bounded legacy ack decoders; persisted ack
    reads now reject oversized files and overflowing counters.
  - Added the opt-in Clang libFuzzer target fuzz_decode. It exercises operation,
    id-list, stream envelope, sidecar manifest, global, counter-gap, legacy ack,
    segment header/cursor/records, base64 and zstd decoders. Successful
    operation decodes pass through semantic validation before any consumer
    could apply them.

Verification:
  - Clang 22.1.8 Debug build with COLLABTEXT_ENABLE_SANITIZERS=ON and target
    flags -fsanitize=fuzzer,address,undefined.
  - ASan/UBSan: tst_decode_validation, tst_segment_format, tst_segment_writer,
    tst_segment_reader passed 4/4.
  - Bounded campaign: 55,831 runs in 11 seconds, max_total_time=10,
    timeout=2, max_len=1 MiB; no sanitizer findings. LeakSanitizer was disabled
    because it terminates in this container with a ptrace-related runtime error.
  - Seed corpus: 51 tracked fixtures under libs/collabtext/tests/fixtures;
    aggregate SHA-256 22b629e31ebdb14dd419862e313d060b46d87d711103d5e374262faf47dc3f71.
  - validate-plan.py: 72-task dependency graph and consumer sequencing valid.

The opt-in campaign is bounded and not part of routine CI. Decoder budgets are
1 MiB per fuzz input, 64 MiB decompressed output, 100,000 segment records, and
1 MiB for legacy ack JSON. No merge, push, tag, publication or consumer message
was made.
```

```text
Task: L02
Status: complete
Branch: feat/fuguemax-placement
Baseline: da1abb8
Implementation: f8aa728cca7fb52d4998b452b0ce8043cfc2e392

Delivered:
  - SegmentWriter now reports checked lifecycle and append results, durably
    commits encoded open-tail records, counts successful sync calls, bounds
    pending/tail bytes, and applies backpressure without discarding queued data.
  - Seal publishes a durably written temporary segment atomically before
    removing the covered tail. Recovery validates interrupted seals and open
    records, restores header counter ranges, and preserves ambiguous/corrupt
    data for explicit repair.
  - FileSync and StreamSync propagate writer failures; explicit close reports
    seal errors. Added subprocess crashes after temp sync and after publish.

Verification:
  - Debug: tst_segment_writer and tst_durable_file passed 2/2; crash cases
    included.
  - Release: tst_segment_writer and tst_durable_file passed 2/2; crash cases
    included.
  - Debug affected callers: tst_filesync, tst_segment_reader,
    tst_evaluation_regressions and tst_opstream_streamsync passed 4/4.
  - validate-plan.py passed for all 72 tasks.
  - No merge, push, or publication performed.
```

```text
Task: L03
Status: complete
Branch: feat/fuguemax-placement
Baseline: 22d72d6
Implementation: 0771f018aba8ebe17e455a2bd009a07587c3c53e

Delivered:
  - Segment reads stop at missing or invalid predecessors and expose the gap.
    They return bounded batches with reader-owned delivery tokens; reads do not
    advance coverage, and stale or foreign tokens cannot commit.
  - Open and sealed progress carry a consumed-prefix hash. Rewritten/truncated
    tails replay safely, open-to-sealed transitions skip only verified prefixes,
    and large sealed segments resume across bounded batches.
  - Cursor files use L01 atomic durable replacement and are bound to a consumer
    checkpoint ID. Without a matching recovered checkpoint, readers replay from
    the beginning. FileSync and StreamSync accept progress only in memory.
  - Removed F6's expected-failure marker. F5's receiver-restart fixture also
    passes now that legacy readers do not persist coverage without recoverable
    consumer state; L05 still owns the full session lifecycle.

Verification:
  - Debug selected suites passed 5/5: tst_segment_reader,
    tst_evaluation_regressions, tst_filesync, tst_opstream_streamsync, and
    tst_decode_validation.
  - All six arrival permutations of the three-segment fixture passed.
  - validate-plan.py passed for all 72 tasks.
  - No merge, push, or publication performed.
```

```text
Task: L04
Status: in progress (partial implementation committed locally)
Baseline: 7b1704a
Scope delivered so far: StreamSync now acquires a per-replica process lease,
  scans each durable producer stream to restore its next legacy envelope
  sequence, avoids unchanged ack-file replacements, and leaves read progress
  uncommitted when there is no inbound callback or when the callback throws.
Regression: tst_opstream_replay now covers a sender restart while the receiver
  remains alive, no-callback replay/no-ack, idle ack-file stability, and retry
  after a throwing callback. F4's obsolete expected-failure marker is removed.
Verification: Debug 11/11 across transport contract, replay, idempotence,
  evaluation regressions, multi-stream convergence, FileSync and SegmentReader;
  validate-plan.py passed all 72 tasks.
  Owned direct-entry pushes also reject gaps/regressions and sequence exhaustion
  before append, while exact same-token retries are idempotent.
Artifacts: build-roadmap-debug/verification/20261006T211710Z-2/
  Full changed-path sweep also passed 79/79 at
  build-roadmap-debug/verification/20261006T211912Z-changed-b68u_063; stale
  StoreFaultHarness cursor usage and pre-L03 gap expectations were updated, and
  legacy ack observations now require an installed callback.
Remaining: production StreamSync still exposes only the legacy 16-bit
  replica/void-callback transport. P07 ActorId/epoch DeliveryToken handling,
  explicit durability and commit/retry/quarantine outcomes, and per-stream
  producer receipts remain necessary before L04 completes. L05 remains locked
  behind L04. No merge, push, publication or consumer message was made.
```

```text
Task: L05
Status: complete
Branch: feat/fuguemax-placement
Baseline: 2ac0510
Implementation: 1304dc2, 39eae85772f5143da8a363094ed77a6b8c06bbf2

Delivered:
  - Completed the optional full-replay DocumentSession over actor-owned state,
    FileOpStream, and Buffer, including recovery readiness and pending intent
    reconciliation.
  - Added a source-private test accessor for SegmentWriter failure injection;
    it does not expose transport configuration through the installed API.
  - Injected FileSync failure during durable local publication. The session
    reports failure and becomes unusable, then reopen reconciles the intent,
    applies the operation once, restores its undo group, and successfully
    undoes it.

Verification:
  - Required Debug selection passed 3/3: tst_session_recovery,
    tst_evaluation_regressions, tst_opstream_replay; GCC 16.2.1;
    build-roadmap-debug/verification/20261010T035139Z-2/.
  - Earlier L05 restart, mixed-peer, copied-store, outbox, and historical
    projection cases remain recorded in
    build-roadmap-debug/verification/20261006T214356Z-2/.
  - validate-plan.py passed for all 72 tasks.
  - Durable position pins and collector enforcement remain assigned to S05/G08.
```

```text
Task: L06
Status: in progress (partial implementation committed locally)
Commit: 9356f3e feat(idlist): expose checked undo through public facade
Scope update: IdListEngine now exposes try_undo()/try_redo() with
  Result<std::optional<IdListOperation>>, forwarding allocator errors while
  preserving the legacy optional-return methods. The public API guide records
  the empty-history versus refused-reservation distinction.
Verification: Debug public-header and filetransport targets compiled. Runtime
  suites were not run in this turn.
Remaining: pending retirement memory budgeting and consumer-owned event policy
  remain open under L06.
```

```text
Continuation — 2026-10-10

Task: L08
Status: complete
Baseline: d1a7b28
Scope: Added separate-tree transfer tests for real Buffer and IdList operation
  codecs, newest-before-oldest delivery, incomplete coverage, exact replay after
  arrival, corruption refusal and repair, restart, and competing writer lease.
  Added editor manifest-before-seed integration coverage. Existing OpStream,
  segment-writer, session, transport-contract and fault-harness suites complete
  callback, durable commit, restart, history, seal and bounded fault coverage.
  GC remains disabled. See `docs/reports/vnext-recovery-milestone.md` for fixture
  hashes and matrix details.
Verification: Six recovery suites passed 6/6 in Debug, Release and ASan/UBSan;
  the bounded `tst_store_fault_harness` passed under ASan/UBSan. LeakSanitizer
  cannot run in this environment and exits after the Qt cases with its ptrace
  diagnostic, so ASan/UBSan was run with `ASAN_OPTIONS=detect_leaks=0`.
Evidence: Debug `build-roadmap-debug/verification/20261010T055202Z-2`, Release
  `build-roadmap-release/verification/20261010T055215Z-2`, ASan/UBSan
  `build-roadmap-asan/verification/20261010T055234Z-2`, bounded fault corpus
  `build-roadmap-asan/verification/20261010T055317Z-2`.

Task: L06
Status: in progress (retirement budget implemented; runtime verification pending)
Commit: 81e1cfa feat(undo): bound pending retirement history (L06)
Scope: Bound combined active/pending history to 100,000 groups and pending
  retirement snapshots to 8 MiB of estimated metadata. Local edits check the
  projected retirement queue before allocating operation identities; limit
  changes refuse atomically when trim records would exceed the cap. Restore
  validates the same bounds, and acknowledgments release the charged bytes.
Verification: GCC 16.2.1 Debug compiled core, filetransport, public headers, and
  the existing undo, IdList GC, Buffer GC, evaluation, realistic, and session
  recovery test executables. The tests were not run in this turn.
Remaining: runtime backpressure coverage; byte charges estimate object and
  identity-array storage; Markoff/Wave own their event encodings and
  multi-block transaction policy.
```


```text
Task: S04
Status: complete
Candidate: 733097c4d042a4c257939e07f7f85b114ff6b48a
Scope: Added durable original-token recovery through FileOpStream and fresh
  scoped replicas, including the repaired WP6 `abcXYZ` passage order. Hardened
  the token codec test's byte mutation helper to use checked string indexing.
  Recorded exact named cases, CTPT/1 fixture digest, verification modes and
  remaining retention limits in docs/reports/vnext-boundary-certification.md.
Verification: Seven local position/recovery suites passed in Debug, Release and
  ASan/UBSan. Installed strict live-position gates passed Debug, Release and
  ASan/UBSan at the candidate revision: Wave 16/16 strict cases, Markdown 4/4.
  LeakSanitizer is unavailable under this runner's ptrace setup; sanitizer
  evidence disables leak detection. WP8 durable pins remain unsupported and
  remain assigned to S05/G-series.
Evidence: Debug build-roadmap-debug/verification/20261010T060612Z-2; Release
  build-roadmap-release/verification/20261010T060623Z-2; ASan/UBSan
  build-roadmap-asan/verification/20261010T060640Z-2; installed gates
  build-consumer-verify/{debug,release,asan}-live-20261010T060707Z-2,
  20261010T060729Z-2, and 20261010T060739Z-2 respectively.
```

Task: S05
Status: in progress (retention contract specified; implementation pending)
Scope: Added `docs/specs/vnext-anchor-retention.md` and linked its lifecycle
rules from W3. Reference IDs are immutable, release is terminal and
idempotent, incomplete consumer projections and unreconciled orphans block
collection, and history queries below the retained floor must report
`history_unavailable`. Current-state reconstruction and isolated historical
playback have separate obligations.
Remaining: The public API has no token scope inspection or durable pin
projection/persistence hooks. `DocumentSession::edits_since` has no typed
history-floor result, and legacy comment/chat anchors lack document/epoch
scope. These are implementation gaps, not supported features; GC remains
inert. Add the public API and crash/recovery tests before marking S05 complete.
Verification: `python3 docs/roadmap/2026-10-reliability/validate-plan.py` passed
(72 tasks, acyclic graph and coverage checks); `git diff --check` passed.

Task: S05
Status: complete
Commit: b450139 feat(retention): add durable external anchor pins (S05)
Scope: Added a headless caller-persisted `AnchorPinRegistry` with scoped token
validation, canonical event replay, contiguous-frontier gating, idempotent
release/lookup, terminal reference tombstones and live pin enumeration. Added
typed DocumentSession history-floor errors, scoped CTPT payloads and expected-
scope comment/chat ingestion. Updated CollabEdit to surface history query
errors, and made installed Wave WP8 assert pin registration, lookup, release
and restart recovery. The crash test simulates a published event with an
uncertain durability result, then proves recovery restores the pin before its
frontier gate opens.
Verification: S05's seven suites passed in Debug, Release and ASan/UBSan;
84/84 change-routed Debug correctness tests passed. The installed live gate at
the candidate passed Debug/Release/ASan: Wave 16/16 strict cases and Markdown
4/4 in each mode. See `docs/reports/vnext-anchor-retention-milestone.md` for
commands, evidence paths and limits. LeakSanitizer is unavailable in this
runner; sanitizer evidence disables leak detection.
Remaining: Pin projection does not enable or perform GC. G01/G06 own
certificate checks, epoch translations and physical reclamation; G08 measures
retained pin metadata. Historical playback remains bounded by its own
projection/archive floor.

Task: S06
Status: complete
Commit: 185e813 test(sequence): expand bounded causal replay model
Scope: Expanded the independent model from 12 insertion shapes to every
causally valid interleaving of two/three three-scalar concurrent typing
groups, including forward/reverse typing. Checked 13,520 interleavings,
duplicate replay for every order, and reversed delivery with defer/drain for
each of the 12 shapes. Added a deletion undo/redo regression that verifies
hidden content returns while item identity/order remain stable. Carried the
original fixture token values through fresh model reconstruction for all 11
declared token-bearing arrival paths: 28 position outcomes, 17 comparisons,
and 17 duplicate deliveries.
Verification: Debug model/production semantics 2/2 passed in 16.17 s
(20261010T073845Z-2). Release 2/2 passed in 1.13 s
(20261010T074335Z-2). The bounded ASan/UBSan model case passed directly with
`ASAN_OPTIONS=detect_leaks=0` in 86.132 s; the full verification wrapper's
LeakSanitizer shutdown is unsupported by this runner's ptrace environment.
Production S01 fixtures replay through fresh Buffer/IdList engines in all
fixture arrival orders. CTPT encoding and fresh-process recovery remain
evidenced by S04's position/recovery gate. No counterexample was found.
Limits: Exhaustive only for the stated 13,520 small insertion histories and
listed fixture states/orders; deletion undo/redo has a focused synthetic
regression, not an exhaustive delete-history matrix. This is bounded
evidence, not a proof for arbitrary histories.

Task: S07
Status: complete
Commit: pending
Scope so far: Removed the dead first-pass accumulator and redundant resets in
`Buffer::edits_since`, leaving one old-position walk and one changed-text
accumulator. Added a regression that splits a mixed-Unicode fragment, inserts
at scalar boundaries, deletes a neighbor, undoes that deletion, and proves the
reported byte edits reconstruct exact current text. Added a narrowly gated
`SequenceOrder` append path for a right child at END whose parent is the final
known item (or an empty root). All other cases use the general algorithm,
retained as `integrate_slow` for comparisons. Per-thread counters report append
fast-path and general/fallback calls. Direct tests compare Fragment and
IdListEntry outputs field by field for root append, tail append, fragment
splitting, and list-head insertion. The suite also compares same-position
concurrency, hidden and immediately retired origins, and boundary offsets plus
collapse outcomes at valid UTF-8 scalar boundaries.
Verification so far: Debug focused model, Buffer/IdList fuzz and delta tests
passed 4/4 (20261010T074553Z-2); production ordering also passed with the S06
Debug gate. The delta test passed independently in Debug
(20261010T074536Z-2). Release delta/production sequence tests passed 2/2
(20261010T074659Z-2). Release benchmark target built after enabling the
opt-in benchmark option; its measurement-format smoke passed with the Qt
offscreen platform and no speed assertion. The focused delta test passed
ASan/UBSan with LeakSanitizer disabled (20261010T074713Z-2).
Verification: Direct cases cover concurrency, hidden/retired origins, split
fallback, and exact anchor outcomes. Debug full
gate passed 7/7 in 17.09 s (20261010T075418Z-2); Release 7/7 in 1.36 s
(20261010T075439Z-2); ASan/UBSan 6/6 in 36.00 s with leak detection off
(20261010T075452Z-2). After the final precondition-helper refactor, the fast
path plus production differential passed in Debug 2/2 (20261010T075615Z-2),
Release 2/2 (20261010T075627Z-2), and ASan/UBSan 2/2 (20261010T075635Z-2).
Release benchmark-format smoke passed with no speed assertion. No benchmark
claim is made; counters are available for later measurement.

## A02 — complete and certify the supported public API boundary

**Task:** A02 **Status:** complete
**Baseline:** `6b17304`
**Scope:** Finished the installed text facade's typed remote apply surface:
`apply_remote` returns `ApplyOutcome` and `apply_ops` preserves one ordered
outcome per offered operation. Added checked `try_undo` / `try_redo`, undo depth
and byte queries, and checked limit setters while keeping the convenience bool
undo/redo methods. Updated Markoff and Wave adapters to consciously map typed
outcomes onto their existing receive contracts. Updated existing direct tests to
assert accepted or non-rejected outcomes according to the behavior they exercise;
retry policy tests assert exact kinds.

The engine regression covers applied, duplicate, deferred, rejected, batch result
order, checked undo/redo, and depth/byte pruning. The relocated installed consumer
now supplies its own `OpStream` implementation from the public contract, covers
text/list facades, exercises single and batch apply plus undo bounds, and runs
with Qt/zstd/OpenSSL package discovery disabled. Public capability manifest is
version 10 and advertises these available features. The public API history and
consumer inventory now describe the landed API instead of an obsolete bool
signature.

**Verification:** Debug full CTest 86/86 passed with
`QT_QPA_PLATFORM=offscreen` (20261010; this platform setting is required by the
headless runner). Fresh Release acceptance passed 86/86
(20261010T081248Z-2). Header inventory check passed. Debug relocated package gate
passed (20261010T080647Z-2): all 17 installed core headers compiled as separate
translation units, consumer include paths contained only the relocated install
prefix, and consumer test passed. Debug live installed-consumer gate passed
(20261010T080823Z-2), as did Release live gate
(20261010T081338Z-2): consumer main, Wave 16/16 strict cases, and Markoff 4/4;
manifest v10. The final 14-case CrdtEngine API test passed in Debug, Release,
and ASan/UBSan. Leak detection was disabled for ASan because this runner's
LeakSanitizer shutdown is unsupported under ptrace; no ASan/UBSan finding was
reported. `tools/check-public-headers.sh` passed.

## G01 — specify membership epochs and reclamation certificates

**Status:** complete
**Baseline:** `14c374f`

Added `docs/specs/vnext-membership-gc.md` as the normative protocol and an independent slow eligibility model in `libs/collabtext/tests/reference/GcModel.h`. Membership is immutable within each epoch; enrollment and retirement require explicit durable transitions, silent peers remain members, and returning retired peers retain an exportable unsynced branch. Certificates require exact common per-stream/per-producer delivery and operation coverage, undo-retirement floors, validated complete checkpoints, and complete external pin projections/translations. The model rejects stale or missing acknowledgements, deferred/cross-stream work, unknown actors, unresolved concurrent proposals, retained relevant undo/redo, corruption, and missing translation. Live pins survive retirement of their author; orphan metadata accounting saturates safely.

The independent QtTest target exercises F1/F2/F3 histories, membership and crash-window adversaries, checkpoint and pin requirements, and a bounded exhaustive undo/redo-retention matrix. This task specifies and models eligibility; it does not enable production reclamation.

**Verification:** `tst_gc_model` passed 15/15 in Debug, Release, and ASan/UBSan (`ASAN_OPTIONS=detect_leaks=0`). `validate-plan.py`, `check-scope.py --strict`, and `git diff --check` passed. LeakSanitizer remains unavailable in this runner because its shutdown is incompatible with ptrace; ASan/UBSan reported no issue.

## G02 — durable membership and undo-retirement publication (in progress)

**Baseline:** `4f17c5f`

Added `MembershipStore`, a producer-scoped append-only journal for explicit enrollment/retirement facts with strict sequence, owner, checksum, corruption, and torn-tail validation. Added `RetentionState`, a per-stream monotonic set of retired undo groups. Its publication method requires a caller-provided durable history-trim commit before journaling. If append durability fails after bytes were written, both journals reconcile from disk before accepting another sequence, preventing a retry from reusing an already-published sequence. Added restart, incomplete-tail, callback-order, idempotence, and injected fsync-failure regressions.

**Verification:** Debug and Release selected gates each passed `tst_membership`, `tst_undo_retirement`, and `tst_persistent_undo` (3/3). ASan/UBSan passed the same 3/3 with `ASAN_OPTIONS=detect_leaks=0`; LeakSanitizer shutdown is unsupported under this runner's ptrace environment. This is the durable journal foundation only. G02 remains open for protocol-validated epoch transitions, session/history integration, typed retirement preview and explicit commit, and returning-device export/reconciliation.

**Follow-up:** Added typed retirement preview/commit. Preview digest binds the parent descriptor hash, sorted membership, per-document/stream epoch and consequence counts, and recoverable branch hashes. Commit recalculates the preview against current caller state, requires explicit confirmation and the exact set of remaining-member acknowledgements, and delegates durable-record/signature validation to a required callback. It then journals the retired event with the reviewed evidence. Membership and impact counts have explicit bounds. Focused Debug, Release, and ASan/UBSan gates passed 3/3 in each mode. Remaining G02 work is G01 transition validation, session/history wiring, and returning-device export/reconciliation.

Added typed enrollment preview/commit with the same parent-descriptor binding and durable acknowledgement verifier. It requires acknowledgement from every current member and the joining device before recording the next epoch. Tests cover missing participant acknowledgement, stale parent descriptor, verification count, and persisted epoch event. Debug, Release, and ASan/UBSan focused gates passed 3/3; plan and strict-scope validators passed.

**Full regression:** After both enrollment and retirement APIs landed, all 87 `tst_*` CTest cases passed in Debug and Release. The same 87 cases passed ASan/UBSan with `ASAN_OPTIONS=detect_leaks=0` (152 seconds total); the two consumer smoke targets (`collabtext_alpha_consumer`, `collabtext_core_smoke`) also passed in all three modes. Thus the complete 89-test set passed in Debug, Release, and ASan/UBSan. The longer editor-recovery test passed in the parallel Debug and Release runs. LeakSanitizer remained disabled due the runner's ptrace limitation.

Review then found that a caller could replay a successfully committed preview while still supplying the old descriptor. Both enrollment and retirement commit now reject a transition when its next epoch is already present in the journal. Regressions confirm replay leaves the journal sequence unchanged. `tst_membership` passed in Debug, Release, and ASan/UBSan; the plan and strict-scope validators passed again.

A follow-up malformed-input review found an invalid `MembershipEvent::Kind` value was serialized as `Retired` by the enum-to-tag conversion. `append` now rejects unknown enum values before writing. The regression confirms sequence zero is retained so a valid event can follow; `tst_membership` passed in Debug, Release, and ASan/UBSan.

Added a deterministic stateful churn regression covering 48 membership epochs. It mixes enrollment and retirement, validates complete acknowledgement sets, injects missing/corrupted acknowledgements and stale descriptor forks, includes three document consequences for retirement, and reopens the journal after every successful transition. Each failed attempt must leave the sequence unchanged. `tst_membership` passed in Debug, Release, and ASan/UBSan; roadmap validators passed.

The churn and contention review found that concurrent open handles could otherwise race on the same recovered sequence. `MembershipStore` and `RetentionState` now hold OS writer leases for their lifetimes and return `writer_conflict` to a competing open. Tests prove contention is rejected and a fresh handle can recover after the original releases its lease. Both journals use filesystem-native lock-path construction. The three focused G02 gates passed in Debug, Release, and ASan/UBSan; roadmap validators passed.

Added a cross-process lease regression by starting a second copy of `tst_membership`, waiting until it holds the journal, and verifying the parent process is refused. After the child exits, opening and recovering succeeds. This case passed in Debug, Release, and ASan/UBSan.

Added the matching uncertain-fsync test for `RetentionState`: its trim callback commits, the complete retirement record is written, the injected sync fails, and the store reconciles the record before accepting a second group. Both groups survive reopen with sequence continuity. `tst_undo_retirement` passed in Debug, Release, and ASan/UBSan.

The full ASan/UBSan run then exposed scheduler sensitivity in the editor-recovery test: per-keystroke coalescing exceeded its 500 ms window under sanitizer load. The recovery test now applies explicit editor changes with the expected word boundary, isolating recovery persistence from time-window coalescing; lower-level tests cover undo-group merging. The complete 89-test CTest set passed in Debug (20261010T094354Z-12), Release (20261010T094417Z-1474), and ASan/UBSan with `ASAN_OPTIONS=detect_leaks=0` (20261010T094433Z-2). LeakSanitizer remains unavailable in this runner's ptrace environment.

Epoch-chain review found typed transitions only rejected already-used next epochs. Enrollment and retirement now also require the latest persisted event epoch to equal the supplied current descriptor's parent epoch, preventing a skipped or stale epoch transition. A regression covers both APIs and verifies no new sequence is consumed on refusal. `tst_membership` passed in Debug (20261010T094740Z-12), Release (20261010T094749Z-182), and ASan/UBSan (20261010T094759Z-2). This is an epoch-number continuity check; binding current descriptor hashes, transition events, and validated checkpoints/frontiers remains part of full G01 integration.

Added an end-to-end session/history retirement regression using the real `DocumentSession` and `RetentionState` APIs. The test saves the trimmed local history before publication, injects a retention journal sync failure, verifies the pending retirement survives restart, then injects a second sync failure during retry. That retry must not clear the session obligation. `RetentionState` now re-syncs an already recovered record before reporting durable success; a later idempotent retry clears the session obligation only after the fresh barrier succeeds. `tst_session_recovery` passed in Debug (20261010T162653Z-2), Release (20261010T162712Z-12), and ASan/UBSan (20261010T162735Z-2). The directly affected `tst_undo_retirement` suite passed in Debug (20261010T162805Z-12), Release (20261010T162809Z-155), and ASan/UBSan (20261010T162812Z-2). This verifies the persistence ordering seam; a production-level coordinator and returning-device export remain open. The full 89-test run predates these latest changes.

Added returning-device local branch export. `FileOpStream::replay_producer()` scans only one producer's original stream/epoch without advancing a durable receive cursor. `DocumentSession::export_local_branch()` streams opaque deliveries to a caller visitor, checks sequence continuity and operation identity, includes a durable pending local intent, deduplicates an exact uncertain append, and refuses conflicts or gaps. It does not rebase into a new epoch, and visitors must stage output until the complete call succeeds. `tst_session_recovery` passed in Debug (20261010T163904Z-2), Release (20261010T163923Z-12), and ASan/UBSan (20261010T163942Z-2), including a transport write failure before any segment record and a sync failure after the operation was written. Public header inventory passed. A relocated FileTransport consumer invoked the installed API and passed (20261010T164104Z-12); the installed Wave gate passed 16/16 and the Markdown/Markoff gate 4/4 (20261010T164112Z-2), both with manifest v11. The full test sweep predates the new API. Membership-gated old-epoch mutation refusal and explicit reconciliation into a newer descriptor remain open G02 work.
# 2026-10-10 — Membership journal epoch continuity

Closed a gap in G02's lower-level journal API: `MembershipStore::append()` and recovery now reject repeated or skipped transition epochs, matching the continuity requirement already enforced by typed transition commits. The first journaled transition may begin at an existing epoch; subsequent records must advance exactly one epoch. Added an append/reopen regression. `tst_membership` passed Debug (`20261010T164258Z-2`), Release (`20261010T164314Z-12`), and ASan/UBSan (`20261010T164349Z-2`, `ASAN_OPTIONS=detect_leaks=0`). A first ASan attempt failed only because LeakSanitizer cannot run in this ptrace-limited runner; rerunning with leak checks disabled passed.

## 2026-10-10 — Returning branch export retry safety

Added a regression proving `DocumentSession::export_local_branch()` returns a visitor's typed failure, stops at the first failed item, and can be retried to emit the same ordered source deliveries. Visitor exceptions now return `io_error` through the API's `Result` contract. `tst_session_recovery` passed Debug (`20261010T164450Z-2`), Release (`20261010T164526Z-12`), and ASan/UBSan (`20261010T164549Z-2`, leak detection disabled for the ptrace-limited runner).

## 2026-10-10 — Reject corrupt membership epoch history

Added a checksummed journal fixture with a skipped epoch, proving recovery rejects semantically inconsistent records even when their record checksums and sequences are valid. `tst_membership` passed Debug (`20261010T164716Z-2`), Release (`20261010T164729Z-12`), and ASan/UBSan (`20261010T164738Z-2`, leak detection disabled for the ptrace-limited runner).

## 2026-10-10 — Reconcile uncertain membership transition commits

Enrollment and retirement commits now recognize an exact already-appended transition after a sync error, force a fresh journal durability barrier, and return the existing event only after that barrier succeeds. Conflicting replays remain rejected. Added injected sync failure and exact/conflicting retry coverage; `tst_membership` passed Debug (`20261010T165022Z-2`), Release (`20261010T165036Z-12`), and ASan/UBSan (`20261010T165049Z-2`, leak detection disabled for the ptrace-limited runner).

The integrated G02 gate (`tst_membership|tst_undo_retirement|tst_persistent_undo|tst_session_recovery`) passed 4/4 in Debug (`20261010T165207Z-12`), Release (`20261010T165224Z-214`), and ASan/UBSan (`20261010T165236Z-2`, leak detection disabled for the ptrace-limited runner).

Extended membership retirement commit failure injection across `Step::Write`, `Step::FileSync`, and `Step::DirSync`. Each failure recovers safely, an identical retry reaches durable success, and a retry with different confirmation is refused. `tst_membership` passed Debug (`20261010T165551Z-2`), Release (`20261010T165605Z-12`), and ASan/UBSan (`20261010T165615Z-2`, leak detection disabled for the ptrace-limited runner).

Revalidated installed consumers against revision `aafb159`: the relocated FileTransport package gate passed (`build-consumer-verify/debug-package-20261010T165735Z-2`); the installed live gate passed Wave 16/16 strict cases and Markdown/Markoff 4/4 (`build-consumer-verify/debug-live-20261010T165746Z-2`). Both builds also compiled the consumer's installed public-header translation units without source-tree leakage.

Ran the full 87-test CTest set after the membership publication and branch export changes. Debug passed (`20261010T165917Z-2`), Release passed (`20261010T170025Z-2`), and ASan/UBSan passed (`20261010T170125Z-2`, `ASAN_OPTIONS=detect_leaks=0`).

Implemented canonical content-addressed `MembershipDescriptor` values carrying parent and transition hashes, the base checkpoint hash, sorted members, per-stream operation and delivery frontiers for every current member (including explicit zero) plus historical producers, supported token versions, and the translation-manifest hash. Sealing normalizes ordered fields; validation rejects malformed or modified contents and bounds the frontier matrix. Added field-by-field tamper and canonicalization tests. `tst_membership` passed Debug (`20261010T171921Z-12`), Release (`20261010T171933Z-182`), and ASan/UBSan (`20261010T171948Z-2`, leak detection disabled for the ptrace-limited runner). This validates descriptor self-integrity; binding its transition hash and checkpoint hash to recovered durable artifacts remains open.
Against committed revision `93b2255`, the relocated FileTransport package consumer passed (`debug-package-20261010T172207Z-2`) and the live consumer gate passed Wave 16/16 strict cases plus Markdown/Markoff 4/4 (`debug-live-20261010T172222Z-2`), using manifest v11.


## 2026-10-10 — Bind membership descriptors to journal events

Membership journal writes now use CME2 records that persist the parent descriptor hash; recovery still reads CME1 history, but legacy events without a parent binding cannot validate a new descriptor transition. Added canonical event hashing and `MembershipStore::validate_transition_link()`: it requires a valid parent and child descriptor, finds exactly one matching durable journal event, checks the parent hash and contiguous epoch, and verifies that the event produces the child's exact enrolled-member set. Tests cover a valid enrollment link and mismatched event and parent links. `tst_membership` passed Debug, Release, and ASan/UBSan (`ASAN_OPTIONS=detect_leaks=0`) on 2026-10-10. The complete 89-test CTest set passed in Debug (serial, 48.33 s) and Release (serial, 7.81 s). ASan/UBSan passed 87/89 in the full serial run; two binaries were incomplete after an interrupted parallel build, then were removed, rebuilt, and passed individually (`tst_comment`, `tst_seed_op`). Combined, all 89 tests passed under ASan/UBSan with leak detection disabled. Qt tests used `QT_QPA_PLATFORM=offscreen`. This does not verify that `base_checkpoint_hash` names a durable, complete checkpoint whose contents match the descriptor frontiers; G02 remains open for that artifact verifier, retired-epoch mutation refusal, production session/history orchestration, and returning-branch reconciliation.


## 2026-10-10 — Gate local session mutations on membership permits

`DocumentSessionConfig` now accepts an optional membership authorizer that receives the document, stream, epoch, and producer before local edit, undo, redo, or undo-group coalescing. A configured authorizer must return a nonnull RAII lease; the session holds it through the in-memory change and its durable publication. A filesystem StepHook observed the permit alive during the actual write and released after publication returned, allowing the consumer's membership coordinator to prevent a concurrent retirement transition. Refusal, callback exceptions, and a missing lease leave text and undo state unchanged. The regression passed `tst_session_recovery` in Debug, Release, and ASan/UBSan, and the integrated G02 gate (`tst_membership|tst_undo_retirement|tst_persistent_undo|tst_session_recovery`) passed 4/4 in all three modes. The hook is optional for compatibility: callers without one remain uncoordinated. A consumer still must wire a real durable membership coordinator and use the lease for atomic transition serialization; checkpoint artifact validation and returning-branch reconciliation remain open.

The installed live consumer gate was rerun against this candidate after the public `DocumentSessionConfig` change. The relocated installation compiled the `DocumentSession` public header without source-tree leakage; the live gate passed Wave 16/16 strict cases and the Markoff-shaped Markdown fixture 4/4 (`build-consumer-verify/debug-live-20261010T175017Z-2`, manifest v11).

A deterministic concurrency regression now models the consumer coordinator with a mutex-backed membership lease. It blocks the storage write, starts retirement concurrently, proves retirement cannot complete while the write holds its permit, then confirms later local edits are refused after retirement. `tst_session_recovery` passed Debug, Release, and ASan/UBSan; the ASan/UBSan session target passed 20 consecutive runs (`--repeat until-fail:20`, leak detection disabled).


## 2026-10-10 — Require checkpoint verification for full transitions

Added `MembershipStore::validate_transition()` as the full validation entry point. It checks the child descriptor against the unique durable CME2 event, then requires both a caller-supplied evidence verifier and checkpoint verifier; missing verifiers return `awaiting_state`, verifier failures propagate, and thrown exceptions become `io_error`. Evidence validation runs first and checkpoint validation is skipped when evidence fails. Tests assert the exact persisted event and checkpoint reference supplied, ordering, refusal, and exception cases. `tst_membership` passed Debug, Release, and ASan/UBSan. The repository still has no shareable checkpoint codec/store or standalone persisted-evidence decoder, so these are fail-closed integration contracts rather than proof that the evidence or checkpoint artifacts have yet been independently validated; G04/G05 and the consumer acknowledgement verifier must provide those implementations.
## 2026-10-10 — A03: Qt paste observes the committed document state

Clipboard paste exposed a gap in the Qt adapter: `QTextDocument::contentsChange` can fire before `toPlainText()` reflects the change. The adapter consequently rejected the reported range and restored the old text. The adapter now observes `contentsChanged`, computes one UTF-16 replacement by diffing the last backend text against the committed document text, and retains surrogate-pair boundary checks. Added regressions for clipboard paste through both engine-backed and durable-session-backed `CollabPlainTextEdit` instances, plus whole-document replacement with non-BMP text. The adapter suite passed 11/11 in Debug, Release, and ASan/UBSan with leak detection disabled; the five focused editor/recovery tests passed 5/5 in Debug. IME commit testing remains open under A03.

## 2026-10-11 — G04: retain rejected delivery bytes for checkpoint capture

`DocumentSession` now exposes exact inbound delivery tokens, payload bytes, and rejection reasons through `quarantined_deliveries()`. Repeated scans with the same token and payload bytes deduplicate while retaining the first reason. Configurable count and byte limits (defaults 1,024 records / 16 MiB) make the snapshot fail with `limit_exceeded` when incomplete, while rejected deliveries remain uncommitted and keep recovery incomplete. Regression coverage proves malformed bytes and reason survive reopen, initial replay/poll/recovery deduplicate the snapshot, quarantine leaves the poll commit count at zero, and both zero-record and zero-byte budgets refuse incomplete snapshots. Debug, Release, and ASan/UBSan passed `tst_session_recovery` and `tst_checkpoint_codec`; the Core public-header object target passed in each mode. The installed FileTransport header compiled in the Debug package gate. The relocated Debug FileTransport package gate passed, and its installed consumer calls the API (`build-consumer-verify/debug-package-20261011T022235Z-2`). The installed live gate passed Wave 16/16 and Markoff-shaped Markdown 4/4 (`build-consumer-verify/debug-live-20261011T022309Z-2`). G04 remains in progress: session snapshot composition, provider membership/pin validation, artifact import atomicity and restore-after-replay are still open.

The current Clang ASan/UBSan checkpoint decoder corpus completed a further 50,000 mutations (`-max_len=4096`, `ASAN_OPTIONS=detect_leaks=0`) without findings. An initial invocation completed its mutations but exited during LeakSanitizer shutdown because this runner restricts ptrace; the rerun disabled leak detection and exited successfully.

## 2026-10-11 — G05: bind local companions to shareable frontiers

CheckpointStore now checks cross-artifact consistency before publishing a bundle or accepting it during recovery. The local identity reservation must exceed the owner’s shareable causal frontier; the local durable delivery frontier must exactly match shareable delivery coverage; and all active or retired local undo-history identities must occur in the shareable causal vector. A regression verifies each mismatch is refused before publication while a consistent bundle still recovers. The complete 10-case `tst_checkpoint_recovery` suite passed Debug, Release, and ASan/UBSan (`ASAN_OPTIONS=detect_leaks=0`). Provider comparison against actual durable owner state remains required; an artifact’s counter is never allocator authority.

## 2026-10-11 — G05: preserve the prior bundle on local-binding refusal

Extended the G05 cross-artifact regression to prove that rejected local companions do not replace or damage the prior complete generation. `tst_checkpoint_recovery` passes all 10 cases in Debug, Release, and ASan/UBSan (`ASAN_OPTIONS=detect_leaks=0`).

## 2026-10-11 — G04: capture session state and committed delivery coverage

Added a source-private `DocumentSession` shareable text checkpoint capture seam. It assembles Buffer fragments, causal version and UndoMap, operation coverage, and exact outstanding deferred/quarantined records from the current engine/session state. Delivery coverage is derived by scanning durable logs and only advancing across records represented as committed in the session; unresolved deferred and quarantined records remain in the checkpoint's deferred section and stop that producer's committed prefix. Represented committed inputs retain SHA-256 digests for token/payload reuse checks rather than duplicating replay payloads in memory. Tests cover valid deferred input before/after dependency arrival, an unseen durable peer envelope, exact malformed quarantine round-trip, local committed coverage and canonical core encode/decode. `tst_session_recovery` passed Debug (`build-roadmap-debug/verification/20261011T030910Z-2`), Release (`build-roadmap-release/verification/20261011T030925Z-12`), and ASan/UBSan (`build-roadmap-asan/verification/20261011T030933Z-2`, leak detection disabled for the runner ptrace restriction). G04 remains in progress: provider state validation, public coordination, artifact restore/bootstrap and G05 atomic publication remain open.

## 2026-10-11 — G04: expose shareable session checkpoint capture

Added public `DocumentSession::capture_shareable_checkpoint` with public provider input types for parent descriptor, canonical pin projection, retained pins, supported token versions and translations. It returns the encoded shareable artifact, CheckpointStore-compatible full artifact hash, operation/delivery coverage, pin projection hash, and canonical translation manifest hash. Incomplete pin projections and a mismatched expected translation manifest fail closed. The caller remains responsible for reconstructing and validating its canonical pin log and membership descriptor before publication. The public-header check and `tst_session_recovery` pass; the session suite passes Debug (`build-roadmap-debug/verification/20261011T033537Z-12`), Release (`build-roadmap-release/verification/20261011T033439Z-12`), and ASan/UBSan (`build-roadmap-asan/verification/20261011T033459Z-2`, leak detection disabled for the runner ptrace restriction). The relocated installed FileTransport package gate passed and exercised the public API (`build-consumer-verify/checkpoint-capture-api-45f09d2 (revision 45f09d2807af9ab2cf34fea77141be96ebb4a1f0)`). Restore/bootstrap, store publication orchestration and replay from checkpoint coverage remain open.

## 2026-10-11 — G04/G05: compose and store the local checkpoint companion

`DocumentSession::capture_checkpoint` now returns a shareable CTCK artifact and a separate CTCL companion. The local companion binds to the exact shareable digest and captures current undo/redo history, durable next-identity reservation, and the local producer's committed delivery frontier. The session regression decodes both artifacts, checks the writer fields, publishes them through `CheckpointStore`, and recovers the stored pair under the expected context. `tst_session_recovery` passes Debug (`build-roadmap-debug/verification/20261011T033955Z-12`), Release (`build-roadmap-release/verification/20261011T034010Z-200`), and ASan/UBSan (`build-roadmap-asan/verification/20261011T034029Z-2`, leak detection disabled for the runner ptrace restriction). The standalone public-header check passes. Startup restore and replay from checkpoint coverage remain the next G05 work. The relocated installed FileTransport package gate passes against revision 5c870049e51c84dfb41d203d4c4d156c3af73423 and exercises both artifact outputs (`build-consumer-verify/checkpoint-bundle-5c87004`).

## 2026-10-11 — Q01–Q05/Q04a: Wave Core adoption completion

Native typed zero-width markers, current-format archive readers, text-owned undo
metadata and a deterministic per-stream projection allocator are implemented.
Removed obsolete CME1/unbound checkpoint readers and the ignored presence engine
parameter after proving current replacements. Full Debug/Release passed 99/99;
post-removal affected checks passed 15/15 each; full leak-enabled ASan/UBSan passed
99/99. Relocated Core-only live package gates passed in all three modes. Both
existing Wave-owned probes passed; production marker/thread and new-allocator
acceptance remains external Q06. Full revisions, commands, counts and artifacts:
[Wave Core candidate report](2026-10-11-wave-core-candidate.md).

New annotated candidate `collabtext-wave-core-2026-10-11`; prior adoption tag is
unchanged. No push, consumer edit, publication or master merge. Broad performance
and GC certification remain governed by their incomplete registry tasks.

## 2026-10-12 — M01–M05: Markoff adoption requests

Ordered full list identities and exact identity lookup, scoped terminal counter
gaps with retained conflict/checkpoint evidence, and durable CTLO/0002/CTGP/0001
reader commitments are implemented. The unused-counter compensation/history
regression was reproduced and fixed. Final full Debug/Release and serial
ASan/UBSan/LeakSanitizer suites passed 101/101; all three relocated installed
Core gates passed 5/5, Wave 16/16 strict and Markdown 4/4. Literal CTLO archives
passed against alpha.3 and alpha.4. Markoff's actual probe compiles but fails its
own noncanonical block-1 fixture on both packages; the handoff names its existing
canonical helper and leaves M06 external. Q06 is also open.

[Markoff candidate report](2026-10-11-markoff-core-candidate.md) records exact
revisions, commands, retained timing failure/rerun, source digest and artifacts.
New annotated local tag collabtext-markoff-core-2026-10-11; neither earlier tag
moved. No consumer edit, pin change, push, publication or master merge.


## 2026-10-19 — X01 started

Work starts from commit `0d9977c`; engine comparison baseline stays pinned to
`d04f151b3a70b161b72c33a8de6895efbd1c13e2`. Start checks pass and no failures
were inherited. X01 is marked `in_progress`; build/trace/harness work and its
paired measurements are pending.
