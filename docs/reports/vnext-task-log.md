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
Implementation commits: PENDING, plus this documentation commit
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
