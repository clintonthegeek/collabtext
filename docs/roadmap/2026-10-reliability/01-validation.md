# 01. Evidence, containment, and verification

Goal: establish reliable tests and measurements before changing semantics. Keep valid legacy behavior available as a baseline. Known failures are temporary explicit expected-fail cases, never invisible skips. This phase does not implement a new CRDT algorithm.

Read [execution rules](EXECUTION.md) and [decisions](DECISIONS.md) before starting. Task dependencies refer to the [master task registry](TASKS.json). New file paths below are intentional; do not assume they already exist.

## V01 — Create reproducible verification entry points

**Status:** see TASKS.json (initially not started)  
**Depends on:** none  
**Coverage:** R4  

**Files / scope:**

- `CMakeLists.txt`
- `libs/collabtext/CMakeLists.txt`
- `tools/verify-roadmap.sh`
- `docs/roadmap/2026-10-reliability/EXECUTION.md`

**Steps:**

1. Add opt-in Debug, Release, and ASan/UBSan configure modes without changing the current default library API. The verification script takes a mode and anchored CTest regex, runs configure/build/test from the worktree, and propagates failures.
2. Use build directories under this worktree; default to two build workers; set offscreen Qt only for test execution. Make sanitizer setup explicitly unsupported on an unavailable compiler, not a successful empty run.
3. Record base commit, compiler flags, test count, and commands. Avoid running all expensive benchmarks on every core check.

**Acceptance:**

- Debug and Release fast suites reproduce the report baseline; script exits nonzero when a deliberately failing test is selected.
- Sanitizer smoke tests execute both Buffer and IdList paths, with leak behavior documented for Qt.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_clock|tst_buffer|tst_idlist'` and the same selection in `release` and `asan` modes. Retain logs outside source files.

## V02 — Convert the report probes into lifecycle regressions

**Status:** see TASKS.json (initially not started)  
**Depends on:** V01  
**Coverage:** F1, F2, F3, F4, F5, F6, F7, F8, R4  

**Files / scope:**

- `docs/reports/2026-10-03-evaluation-artifacts/probes.cpp`
- `libs/collabtext/tests/tst_evaluation_regressions.cpp`
- `libs/collabtext/CMakeLists.txt`

**Steps:**

1. Create one named test per F1–F8 sequence; include both primitives for restoration failures and exact text/anchor expectations. Use independent temp directories and no timing sleeps.
2. Use narrowly placed QEXPECT_FAIL only for confirmed outstanding defects; controls for ordinary undo/redo must pass normally. Each fixing task removes its own expected-fail marker.
3. Give every known failure an ID linked to its fixing task. Treat an unexpected pass as requiring removal of the marker, not weakening the assertion.

**Acceptance:**

- All confirmed report failures are visible by task/finding ID; existing successful controls stay green.
- The test executable cannot silently succeed after removing an assertion or skipping an entire failure case.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_evaluation_regressions'`; inspect expected-fail counts and confirm each case matches the report.

## V03 — Disable unsafe physical reclamation until a certificate exists

**Status:** see TASKS.json (initially not started)  
**Depends on:** V02  
**Coverage:** F1, F2, R10  

**Files / scope:**

- `libs/collabtext/src/crdt/Buffer.cpp`
- `libs/collabtext/src/crdt/IdList.cpp`
- `libs/collabtext/include/collabtext/OpStream.h`
- `libs/collabtext/tests/tst_gc.cpp`
- `libs/collabtext/tests/tst_idlist_gc.cpp`

**Steps:**

1. Make legacy collect_garbage/compact retain tombstones and restoration metadata by default; allow only separately tested coalescing that preserves identities, undo, and anchors. Do not expose an unsafe opt-in.
2. Document that a scalar transport acknowledgement is receipt information and cannot authorize physical deletion. Preserve source signatures provisionally, returning zero removals until G06 replaces the interface.
3. Update tests that assumed reclamation succeeds; add local and remote undo/redo preservation tests. Do not claim a bounded-memory solution yet.

**Acceptance:**

- F1/F2 restoration probes pass for Buffer and IdList; existing text and remote convergence do not change.
- No application or public scalar-ack path can invoke unsafe destructive reclamation.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_gc|tst_idlist_gc|tst_evaluation_regressions'`; run the fast suite after changing historical expectations.

## V04 — Make randomized failures replayable and reducible

**Status:** see TASKS.json (initially not started)  
**Depends on:** V01  
**Coverage:** R4  

**Files / scope:**

- `libs/collabtext/tests/TestSeed.h`
- `libs/collabtext/tests/TraceHarness.h`
- `libs/collabtext/tests/tst_fuzz.cpp`
- `libs/collabtext/tests/tst_idlist_fuzz.cpp`
- `libs/collabtext/tests/tst_realistic.cpp`
- `tools/reduce-trace.cpp`

**Steps:**

1. Define COLLABTEXT_TEST_SEED as a checked uint64 seed and COLLABTEXT_TRACE_OUTPUT as an optional artifact directory. Use a documented fixed default for CI; retain opt-in random campaigns.
2. Record edits, delivery order, disconnects, undo/redo, store restarts, and GC requests in a versioned trace. Replay without rerolling strategy choices.
3. Add delta reduction that removes events only when identity/causality remain valid and the same failure predicate persists. Save the unreduced trace first.

**Acceptance:**

- Two runs with the same seed produce identical event traces; every migrated fuzz suite prints an effective seed.
- An injected convergence failure produces a replayable artifact and a smaller valid reproduction.

**Verification:**

Run seeded fuzz twice with seed 42; compare traces. Run reducer tests and replay the reduced injected failure. Use Debug and ASan.

## V05 — Correct simulator partitions and ordering configuration

**Status:** see TASKS.json (initially not started)  
**Depends on:** V04  
**Coverage:** R4  

**Files / scope:**

- `libs/collabtext/src/crdt/NetworkSim.h`
- `libs/collabtext/src/crdt/NetworkSim.cpp`
- `libs/collabtext/tests/tst_network_sim.cpp`
- `libs/collabtext/tests/tst_realistic.cpp`

**Steps:**

1. Define disconnect as blocking both inbound and outbound delivery while allowing local edits. Queue offline-produced edits until reconnection; retain duplicate and causal-reordering scenarios.
2. Implement allow_reorder=false using per-producer/per-stream monotone scheduling; allow cross-producer reorder. Add explicit link partition support for asymmetric-network tests instead of overloading disconnect.
3. Test a partitioned sender edit remains invisible to online peers until heal, and that queued operations eventually deliver exactly or redundantly as configured.

**Acceptance:**

- Symmetric disconnect behavior matches the documented contract; allow_reorder influences delivery order.
- Old realistic scenarios are updated to the corrected network model and converge after healing.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_network_sim|tst_realistic|tst_fuzz'` with fixed seeds 42 and 20261003.

## V06 — Repair benchmark timing, units, correctness, and memory labels

**Status:** see TASKS.json (initially not started)  
**Depends on:** V01  
**Coverage:** R5  

**Files / scope:**

- `libs/collabtext/tests/tst_benchmark.cpp`
- `libs/collabtext/tests/BenchmarkSupport.h`

**Steps:**

1. Start reconnect timing before reconnect/tick and stop after actual application plus deferred retry. Use steady_clock throughout.
2. Make every convergence benchmark assert equality. Report source edits, remote applications, wall duration, and duration per defined unit separately. Remove fake zero memory measurements; unavailable metrics are explicitly absent.
3. Run GC before/after comparisons on independently recreated identical traces and state snapshots. Distinguish active engine memory from process RSS.

**Acceptance:**

- A controlled catch-up test measures work inside its timed region; the total is nonzero without asserting a speed threshold.
- Wrong content fails a benchmark; JSON or text reports have unambiguous units and actual metric availability.

**Verification:**

Run selected single-replica, multi-replica, reconnect, and GC benchmark functions in Release; inspect reports against operation counts. Do not compare the corrected outputs directly to old mislabeled values.

## V07 — Add store fault-injection and subprocess crash harnesses

**Status:** see TASKS.json (initially not started)  
**Depends on:** V01  
**Coverage:** F5, F6, F9, R4  

**Files / scope:**

- `libs/collabtext/tests/StoreFaultHarness.h`
- `libs/collabtext/tests/tst_store_fault_harness.cpp`
- `libs/collabtext/tests/store_crash_worker.cpp`
- `libs/collabtext/CMakeLists.txt`

**Steps:**

1. Provide injectable hooks before/after write, file sync, rename, directory sync, cursor publication, segment seal, and checkpoint publication. Hooks identify operation and path; production default is a no-op.
2. Use subprocess termination to test loss of user-space buffers without relying on destructor cleanup. Distinguish kill/restart from power-loss durability evidence.
3. Provide filesystem fixtures for missing/out-of-order segments, partial tails, disk-full simulation, failed rename, repaired corruption, and concurrent writer lock attempts.

**Acceptance:**

- Each fault can be deterministically triggered and replayed; terminated workers cannot hang the test runner.
- The harness itself verifies parent cleanup, retained source data, and independent temp paths.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_store_fault_harness'`; run harness-only ASan tests without treating deliberate worker termination as a library memory defect.

## V08 — Inventory consumers and freeze legacy compatibility fixtures

**Status:** see TASKS.json (initially not started)  
**Depends on:** V02  
**Coverage:** R2, R3, R11, R13, R14  

**Files / scope:**

- `docs/roadmap/2026-10-reliability/compatibility-inventory.md`
- `libs/collabtext/tests/fixtures/legacy-v1/`
- `tests/consumer-smoke/`

**Steps:**

1. Inventory public headers, actual aliases, callbacks, OpStream methods, CMake target/includes, serialized operations, manifests, anchors, and IdList local_clear behavior. Include Markoff contracts from existing handoff docs without accessing external repositories.
2. Freeze valid operations/stores with Buffer, IdList, chat/comments, Unicode, multiple producers, restart ID collisions, and known corrupt cases. Record expected legacy visible content using the frozen old engine.
3. Create a compile-only legacy consumer sample; identify behavior changes independently from source and ABI changes.

**Acceptance:**

- Inventory explicitly names every planned compatibility break and old guarantee; fixtures have licenses/provenance and exact expected outputs.
- No external consumer is modified or contacted. Legacy source is retained for migration comparison.

**Verification:**

Build the legacy sample against the original checkout; validate fixture hashes and expected content. Record the base revision `046d375`.
