# collabtext: design, architecture, implementation, and performance evaluation

**Date:** 2026-10-03  
**Evaluated revision:** `046d375` (working tree initially clean)  
**Scope:** C++ CRDT primitives, public API, file transport, persistence/recovery, Qt integration, tests, benchmarks, and suitability for the documented use case.  
**Deliverables:** This report and the [reproduction sources and raw results](reports/2026-10-03-evaluation-artifacts/).

## 1. Overall assessment

collabtext has a worthwhile product direction and substantial working infrastructure. An ordinary Qt editor that merges edits through Syncthing-compatible sidecars addresses a concrete problem. The engine/transport separation, stable operation identities, deterministic seed operation, per-replica storage, selective undo machinery, and simulated-network tests are useful foundations.

**The current implementation is a promising prototype, but I would not yet entrust irreplaceable documents to its advertised guarantees.** Small, valid API sequences expose broken redo, divergent replicas after compaction, lost edits after a transport restart, skipped late-arriving segments, ignored anchor bias, and interleaved concurrent passages. These problems matter more than the remaining throughput gap.

The strongest differentiator is the combination of native C++, Qt integration, and file-sync-oriented workflow. Offline operation, transport independence, selective undo, and stable positions are not unique to this library. Its own competitive reports overstate both the uniqueness of those features and the certainty of correctness.

| Area | Assessment | Practical implication |
|---|---|---|
| Intended workflow | Strong and coherent | Retain the file-sync-first product direction |
| Core representation | Capable, but complicated by run splitting and relocation | Specify merge semantics before further optimization |
| Convergence without reclamation | Encouraging existing test evidence | Passing randomized tests is not a proof or sufficient release gate |
| GC and undo/redo together | Confirmed correctness defects | Redesign reclamation conditions before enabling GC in production |
| Persistence and restart | Incomplete and unsafe in current integration | Make restart/replay the first delivery milestone |
| Merge quality | Concurrent bulk insertions can interleave characters | Unacceptable default for the prose/manuscript use case |
| Interactive performance | Fast append; millisecond random edits; worsening with history and peers | Adequate for some small workloads, not a demonstrated large-document guarantee |
| Library packaging | Algorithm code largely framework-independent; shipped target is Qt-bound | Split build targets and prove standalone consumption |
| Qt integration | Useful advanced editor work alongside obsolete/incomplete wrappers | Consolidate around one supported adapter |
| Engineering evidence | Broad scenario coverage, important lifecycle blind spots | Extend tests around actual files, restarts, GC, and semantic expectations |

My recommendation is to continue development, with a correctness and recovery milestone ahead of new features or an engine rewrite. Preserve the UI and transport direction. Keep the option of replacing the CRDT backend open if achieving acceptable ordering semantics requires a substantial redesign.

## 2. Method, evidence, and limits

I read the README, orientation document, architecture and engine/list/sync/transport specifications, implementation plans and performance reports, and the relevant C++ sources and tests. I examined the core trees, clocks, anchors, operation handling, GC, codecs, segment transport, public wrappers, and dogfood editor wiring. External comparisons use primary project documentation and research papers, linked below.

I configured and built the unmodified implementation in `/tmp/collabtext-review-release` with CMake Release settings. The environment was Linux x86_64, Intel Core i7-1185G7, GCC 16.2.1, Qt 6.11.2, and system zstd. The library used `-O3 -DNDEBUG -std=gnu++20`; the extra standalone harnesses were compiled with `-O2` and linked to that library. Internal C++ assertions were therefore disabled, while QtTest assertions remained active. Headless Qt tests used `QT_QPA_PLATFORM=offscreen`.

Validation performed:

- Complete project build, including both applications: succeeded.
- CTest excluding `tst_benchmark` and `tst_realistic`: **51/51 executables passed**, about 1.1 seconds with four workers.
- `tst_realistic`: **11 QtTest passes**, including initialization and cleanup; about 19.8 seconds.
- Nine selected existing benchmark functions: completed, with **11 QtTest passes** including initialization and cleanup.
- Standalone behavioral probes: reproduced the failures described in section 5.
- Standalone latency/serialization harness: collected mean, p50, p95, p99, maximum, layout sizes, and wire sizes.

The benchmark run reported here was repeated after the build and realistic tests had finished to reduce contention from review work. This is still a laptop measurement, without fixed CPU frequency, dedicated machine isolation, or enough repetitions for formal confidence intervals. Treat the numbers as reproducible observations and scaling signals, not universal performance claims.

I did not run every benchmark function, sanitizer builds, a physical power-loss test, an actual Syncthing deployment, a long-lived production workload, or competing engines on matching traces. I did not prove the CRDT formally or validate every UI interaction. Static findings are identified separately from observed behavior. Production sources were not modified.

The probe executable prints observations and exits normally; it is an investigation harness, not a replacement for failing regression tests. Positive controls confirm normal Buffer and IdList undo/redo use distinct timestamps and converge. An initial suspicion about clock advancement was disproved by execution and is not a finding.

## 3. Intended use case and what it requires

The [README](../README.md) describes plain-text editing across intermittently connected machines, using file synchronization rather than a required server. Examples include notes, manuscripts, research, air-gapped exchange, and editing the same document on a desktop and laptop.

That changes the order of priorities:

1. Edits must survive ordinary close/reopen, crashes, file arrival gaps, and reconnection.
2. Replica identities and operation IDs must remain unique across sessions and offline enrollment.
3. Reclamation must respect offline peers, future undo, and durable bootstrap state.
4. Concurrent paragraphs should remain intelligible, not merely equal on every machine.
5. Catch-up must avoid freezing the editor, even if immediate keystroke processing is fast.
6. Long editing histories must have a bounded storage and recovery strategy.

The README's “No lost work. Ever.” is stronger than the current code supports. A CRDT can provide conditional convergence when valid operations are eventually delivered; it does not by itself guarantee delivery, durability, useful merge semantics, or correct GC.

The existing argument that offline workflows make throughput less important needs qualification. Human typing is slow, but a reconnect can deliver thousands of historical operations at once. File-sync-first systems depend heavily on efficient, reliable replay. The Eg-walker research specifically addresses loading and merging diverged histories, including peer-to-peer use, so offline support is not an excuse to ignore those costs. [Gentle and Kleppmann, Eg-walker](https://arxiv.org/abs/2409.14252).

## 4. Design and architecture

### 4.1 Good choices worth retaining

**Operations are independent of transport.** Buffer and IdList produce operations and consume remote operations. `OpStream` supplies a small delivery boundary. This allows file exchange and future direct channels without embedding networking in the CRDT algorithm. The small interface is easy to implement and test.

**Per-replica storage avoids multiple writers to the same log.** Segments, local cursors, and separate presence files are the right general shape for externally synchronized directories. Sealed immutable segments plus an open tail reduce small-file overhead and provide a place for compression and integrity checking.

**The engine owns undo semantics.** Qt undo is disabled where the CRDT owns history. The undo parity representation, local stack, and coalescing API provide a usable conceptual basis for collaborative undo. The principal defects are in interaction with GC, rather than ordinary clock uniqueness.

**The tree supports multiple useful measurements.** SumTree's summaries and dimensions make byte-offset and ordering queries possible within one storage abstraction. Structural sharing and splice-oriented construction can avoid rebuilding entire documents on common paths.

**The editor has a surgical update path.** `edits_since()` and `CollabPane::applyEditsPreservingScroll()` are preferable to replacing a whole QTextDocument on every inbound update. The cursor overlays, scroll anchors, and grouped typing show attention to real editor behavior.

**Deterministic seeding is valuable.** `SeedOp` and the sidecar manifest avoid giving independently opened copies unrelated CRDT identities for the same initial content. Seed integrity checking is useful, although enrollment and lifecycle handling still need work.

### 4.2 Representation complexity and ordering semantics

The representation combines character identity ranges, multi-character text fragments, fractional locators, split relocation messages, deterministic ordering by locator/origin, visibility derived from undo state, and several auxiliary structures. This is powerful but gives correctness a large surface area: local mutation, remote fast paths, fallback normalization, replay, GC, and undo must all agree.

The most important design issue is shared-locator atomization. When concurrently inserted fragments share a locator, normalization can order their individual characters by Lamport origin. The resulting `aXbYcZ` merge is deterministic, but it destroys passage boundaries. It is particularly damaging when collaborators work offline on prose.

There should be an explicit ordering specification covering concurrent pastes, forward typing, reverse insertion, insertion inside a concurrently deleted span, replacements, and subsequent edits inside merged runs. The Fugue/FugueMax paper defines non-interleaving properties and demonstrates why replica agreement alone is insufficient. It is an appropriate reference for this work, not evidence that collabtext already has those properties. [Weidner and Kleppmann, The Art of the Fugue](https://arxiv.org/abs/2305.00583).

The earlier optimization report's claim that per-character locators and a complete rewrite are the only path to further improvement is not established. Logical per-character identity and physical storage runs are separate choices. Targeted indexes, removal of full-tree validation, Unicode summaries, run-level deletion bookkeeping, and batched notification all offer improvement opportunities. Fixing insertion ordering may still require an operation-format change; that decision should follow a semantic specification and representative traces.

### 4.3 Acknowledgement is underspecified for safe GC

The four-method `OpStream` interface is a useful delivery adapter, but its scalar GC-fence method overpromises. [OpStream.h](../libs/collabtext/include/collabtext/OpStream.h) says operations below the returned Lamport counter are safe to collect.

In [StreamSync.cpp](../libs/collabtext/src/crdt/StreamSync.cpp), acknowledgements are updated after payload decoding and optional callback invocation, not after a confirmed durable engine commit. A missing callback still permits an ack. A callback can defer an operation on unsatisfied causal dependencies, but the transport cannot distinguish that from successful application. IdList payloads are not accounted for: ack extraction uses `decode_operation()`, not `decode_idlist_operation()`.

The acknowledgement state is keyed by producer ID, not stream. A maximum counter observed in one stream is not proof of complete coverage in another, especially when consumers compose independent Buffers using the same replica ID. Per-stream delivery order does not establish a cross-stream frontier.

Peer membership also matters. The implementation excludes peers without an ack entry and only advances the cached fence. A previously silent peer can subsequently enroll below that fence. If old history has been reclaimed, a bootstrap snapshot or epoch/membership protocol is required. Some tests explicitly require silent-peer exclusion; that makes the behavior deliberate, not necessarily safe.

Separate three concepts in the contract: transport receipt, durable local coverage, and semantic eligibility for CRDT reclamation. Only the last, combined with a defined peer/epoch set and undo policy, supports deletion of required state. A producer-local transport fence cannot be substituted directly for Buffer's multi-replica `Global` watermark.

### 4.4 The actual layers differ from the advertised layers

There are at least three integration paths:

| Path | Actual behavior | Assessment |
|---|---|---|
| Public `CrdtEngine` + `CollabDocument` + `SyncManager` | Lightweight engine wrapper, document copying, obsolete sync stubs | Not a complete supported collaborative stack |
| Dogfood `CollabPane` + Buffer + FileSync | Actual text editing, sidecar transport, undo, surgical remote patches | Most complete application path; recovery remains incomplete |
| Public `OpStream` + StreamSync | Byte transport with registered streams, callbacks, and ack files | Useful boundary, but identity/replay/GC contracts need hardening |

[SyncManager.cpp](../libs/collabtext/src/SyncManager.cpp) still says serialization support is pending, does not wire engine operations, and advances remote cursors without applying updates. [CollabDocument.cpp](../libs/collabtext/src/CollabDocument.cpp) does not connect ordinary QTextDocument edits back to the engine or subscribe to remote engine changes. Its suppression flag is not an implemented binding mechanism. The dogfood application bypasses these wrappers.

Choose one supported document adapter and retire or clearly label obsolete wrappers. Keep presence, transport, and document binding separate, but ensure the public path actually demonstrates their composition.

### 4.5 Framework independence and distribution

The principal Buffer/IdList/clock/locator algorithms are ordinary C++ and can be separated from Qt. The delivered library target cannot currently be consumed that way:

- Both CMake levels require Qt Core, Gui, Widgets, and Test.
- `collabtext` publicly links all four Qt components and bundles engine, transport, UI, identity, and NetworkSim.
- `SegmentFormat.cpp` uses QCryptographicHash; NetworkSim uses QtTest.
- zstd is a required dependency of the bundled target.
- No install/export package configuration is provided.
- Public include directories expose `src`; public operation and StreamSync headers include internal `crdt/` headers.
- The “public surface” tests inherit that same public `src` include path, so they do not prove that only the installed public headers are sufficient.

The public PImpl wrapper is a useful start, but transitive internal includes and comments asking consumers not to depend on visible fields do not create a strong physical API boundary.

Provide separate core, file-transport, Qt-adapter, widget, and test-support targets; make QtTest a test dependency; add install/export rules; and compile an external sample against the installed package. Windows portability also needs validation: the current code contains POSIX-specific `gmtime_r()` and the mandatory benchmark uses `unistd.h` and `/proc`.

## 5. Correctness, durability, and lifecycle findings

Severity below reflects impact on the documented workflow. “Confirmed” means a standalone probe reproduced the behavior, or the relevant implementation is directly inspectable. The reproduction source is [probes.cpp](reports/2026-10-03-evaluation-artifacts/probes.cpp), with [observed output](reports/2026-10-03-evaluation-artifacts/probes-release.log).

### F1 — GC removes insertion state still needed by redo

**Severity: critical. Confirmed.**

[Buffer.cpp](../libs/collabtext/src/crdt/Buffer.cpp), `collect_garbage()` around line 160, protects deletion IDs but not inserted character IDs. An undone insertion is invisible and has an empty deletion list; the eligibility loop therefore accepts it.

Reproduction: insert `abc`; undo; collect garbage; redo. One fragment is removed, and redo leaves the document empty. This fails even with the default undo stack still retaining the insertion.

There is a distributed variant: A inserts; B receives; A undoes; B receives the undo and collects garbage; A redoes; B receives the redo. A becomes `abc`, B remains empty. A local-only undo stack cannot identify what another replica can still redo.

IdList includes local insertion protection, unlike Buffer, but its remote undo/redo eligibility deserves the same scrutiny: remote protected state is still absent from the local stack.

**Remedy:** Immediately prohibit removal of undone insertions while restoration remains possible. Define distributed undo horizons or make restoration create sufficient new content/identity. Add local and remote regression cases for both primitives.

### F2 — Observing a deletion does not make it permanently irreversible

**Severity: critical. Confirmed.**

`Buffer::compact()` around line 180 checks whether the watermark covers deletions and whether the local stack protects them. It cannot see the deleting replica's retained undo history.

Reproduction: A inserts `abc`; B receives and deletes it; A receives the deletion; A compacts at the meet of both versions; B undoes the deletion; A receives that undo. A stays empty and B becomes `abc`.

Both replicas really had seen the deletion. The defect is that acknowledgement of an operation is not acknowledgement that the operation can never be undone.

**Remedy:** Use a published undo-retirement horizon, coordinated checkpoint/epoch, retained restorable content, or another explicitly safe policy. Merely replacing the watermark computation is insufficient to fix this case.

### F3 — `Global::meet()` is unsafe as a common-observation watermark

**Severity: critical when used for reclamation. Confirmed.**

[Clock.cpp](../libs/collabtext/src/crdt/Clock.cpp), `Global::meet()`, leaves a component unchanged when the other vector has no nonzero value. `{1:10}.meet({})` yields `{1:10}`. A peer that has never observed replica 1 must instead constrain that common-coverage component to zero.

The engine spec itself describes retaining nonzero values, so this is a semantic design problem preserved by implementation, not merely a typo in the sparse refactor. `NetworkSim::compact_all()` uses this meet as a GC watermark.

There may be a legitimate need for a nonzero-minimum aggregation in a tree summary, but it is a different operation from intersection of replica coverage. Conflating them creates false certainty. Also, starting the current summary minimum from an empty `Global` never adds components, making those minimum fields questionable as useful aggregates.

**Remedy:** Separate coverage intersection from summary aggregation and specify their identities and algebraic laws. Test absence, disjoint replicas, empty vectors, and lagging peers; verify GC never reclaims an unseen dependency.

### F4 — Sender restart causes a distinct new edit to be dropped

**Severity: critical. Confirmed.**

`StreamState::next_seq` starts at 1 in [StreamSync.h](../libs/collabtext/include/collabtext/StreamSync.h). `StreamSync::push()` constructs entry IDs as `replica_name + "-" + seq`. The counter is not recovered on restart.

Reproduction: A pushes insertion `a`, flushes, and B polls. Recreate A's transport with the same name while retaining A's Buffer; A inserts `b`, pushes and flushes; B polls. A is `ab`, B is `a`. The new CRDT operation has a valid fresh timestamp, but AppendOnly's `merged.try_emplace()` rejects the reused stream-entry ID before invoking the engine callback.

The [transport spec](CRDT_TRANSPORT_SPEC.md), “Known limitation: seq reset on restart,” acknowledges collisions but incorrectly characterizes the effect as duplicate log entries handled by CRDT idempotence. The observed effect is suppression of a different operation.

**Remedy:** Recover/persist unique producer sequence identity, or include an explicit persistent session identity. Define crash ordering between identity allocation and append. Test the new operation reaching an already-running peer after sender restart.

### F5 — Persisted read cursors outlive the state they supposedly cover

**Severity: critical. Confirmed in transport probe; application problem also evident in code.**

SegmentReader persists consumption positions. StreamSync's merged entries and CRDT state are in memory. Restarting a receiver with the same local cursor but a new Buffer yields no replay: first session receives `abc`; second session receives zero entries and has empty text.

Incremental consumption is valid only if its covered application state is restored durably. The current interface does not enforce that pairing.

The dogfood startup in [CollabPane.cpp](../app/collabedit/CollabPane.cpp) applies the seed, starts FileSync, and polls once. FileSync skips its own replica directory; no snapshot or own-history replay is performed. Already-consumed remote history is skipped via persisted cursors. Thus ordinary reopen cannot reconstruct the complete document from the sidecar. A materialized plain-text file is not a CRDT snapshot containing identities and causal history.

**Remedy:** Implement full replay of own and remote logs as a correct baseline, or durable snapshots/checkpoints with recovery cursors tied to their coverage. Commit transport progress only after the application can recover the covered state. Test close/reopen, receiver crash, sender crash, and copied stores.

### F6 — Late sealed segments are permanently skipped

**Severity: critical. Confirmed.**

[SegmentReader.cpp](../libs/collabtext/src/crdt/SegmentReader.cpp), `read_new()`, uses one `m_last_sealed` high-water mark. If segment 2 exists while segment 1 is missing, it processes 2 and sets that mark to 2. Segment 1 later arrives and is ignored forever. The same issue applies when an earlier segment initially fails validation but a later one succeeds.

The probe seals records `first` and `second`, temporarily withholds segment 1, reads and commits segment 2, then restores segment 1. The later read returns zero records.

External file synchronization need not present files in segment order. Causal deferral inside Buffer cannot recover an operation that never reaches it.

**Remedy:** Track gaps explicitly or advance a contiguous frontier only through validated coverage. Establish how open-tail observation interacts with missing sealed segments, seal replay, partial transfers, and file replacement. Test arrival permutations and repaired corruption against real SegmentReader instances.

### F7 — Anchor bias is stored but does not control resolution

**Severity: high. Confirmed.**

`Buffer::anchor_at()` and `resolve_anchor()` around lines 347 and 367 identify the character at the offset and resolve before that character, without considering `bias`.

Reproduction: `ab`; create left- and right-biased anchors at offset 1; insert `X` at 1. In `aXb`, both anchors resolve to 2. The documented behavior is left 1, right 2. End anchors also need explicit insertion-at-end semantics.

Additionally, removing a referenced tombstone loses its ordering context; unresolved anchors fall back to document end. Persistent comments, selections, and viewport positions need a specified policy through GC, not just through normal edits.

**Remedy:** Define anchors as boundaries with explicit affinity, including start/end and deleted-neighbor behavior. Use stable origin/position lookup and retain or translate necessary anchor information through compaction. Strengthen tests to assert exact expected positions, not merely that they are within bounds.

### F8 — Concurrent passages interleave character by character

**Severity: high for product suitability. Confirmed; this is a merge-quality failure, not divergent replicas.**

A and B independently paste `abc` and `XYZ` into empty buffers, then exchange operations. Both produce `aXbYcZ`. The content bytes survive, but neither passage remains intact.

This undermines the claim that the engine is particularly solid on merge quality. A notes/manuscript editor needs contiguous concurrent passages under at least this elementary case.

**Remedy:** Make non-interleaving part of the algorithm's explicit target and test it independently from convergence. Assess a run-aware ordering fix or a backend using a well-specified sequence algorithm before optimizing the existing behavior further.

### F9 — “Force-fsync” does not call fsync

**Severity: high. Confirmed by implementation inspection; power-loss effects not experimentally tested.**

[SegmentWriter.cpp](../libs/collabtext/src/crdt/SegmentWriter.cpp), `flush()` and `seal_open_()`, flush C++ streams and increment `fsync_count`, but do not invoke OS durable-sync operations. Seal writes a temporary file, renames it, and removes the open tail without the specified durable ordering. Stream I/O errors are not reliably checked before pending records are cleared or the tail is removed.

The headers and transport spec promise force-fsync. That promise is not implemented. Atomic rename and flushing user-space buffers solve different problems from ensuring recovery after power loss. The default destructor also does not drain pending append records; callers must explicitly flush/close, and that lifecycle needs a clear contract.

The application Save path writes the materialized text but does not explicitly flush the CRDT logs. Its helper removes the destination before renaming the temporary file, creating a failure window rather than a single atomic replacement.

**Remedy:** Implement platform-specific durable writes and directory synchronization where required; distinguish append, flush, durable commit, and seal; check failures; and expose errors to callers. Make Save's promised boundary explicit and use a proper atomic-save primitive. Validate failures at each step with injection, plus crash tests.

### F10 — Decoder success does not establish a valid operation

**Severity: high. Confirmed acceptance cases; broader risks from inspection.**

[Serialization.cpp](../libs/collabtext/src/crdt/Serialization.cpp) accepts `{"t":"e"}` without required fields and `{"t":"u"}garbage` without rejecting trailing input. It requires `t` to be the first object key, but does not generally enforce JSON grammar or semantic constraints.

Inspection also shows no full validation of fragment character count against content, origin/counter relationships, locator validity, paired ranges/text, or overflow-prone intervals. `parse_global()` uses a 16-bit index without an array-length bound. Unicode `\u` decoding encodes individual surrogate code units rather than combining surrogate pairs; raw invalid UTF-8 is not validated. [Utf16.h](../libs/collabtext/src/crdt/Utf16.h) assumes sequence widths from lead bytes and can return offsets past truncated input.

The public `applyRemoteOp()` documentation relies on the decoder rejecting malformed operations. That assumption is currently false. Even in trusted shared folders, truncation, corruption, and incompatible versions need actionable handling. Decoding failures should not be silently skipped while advancing durable coverage.

**Remedy:** Separate syntactic parsing, schema/version handling, and semantic validation. Require complete input consumption, bounded lengths/counts, valid text, checked arithmetic, valid identities and positions, and consistent insertion metadata. Preserve/quarantine unsupported records for retry or upgrade. Add parser fuzzing and malformed-operation tests under sanitizers.

## 6. Implementation quality and maintainability

The implementation is readable enough to investigate: naming generally explains intent; the public wrapper is small; data structures have local contracts; fast paths and fallback paths are recognizable. Sparse Global storage is a sensible response to replica IDs with large numeric values. The work already invested in convergence and regression tests is useful.

Several implementation patterns now increase maintenance risk:

- **Duplicated semantic machinery.** Buffer and IdList share concepts but separately implement undo, GC, deferral, and ordering. Buffer's missing insertion protection while IdList has it demonstrates policy drift. Share carefully defined policy/helpers, without forcing all storage behavior into one oversized generic engine.
- **Large methods and obsolete scaffolding.** Buffer.cpp is about 1,537 lines and SumTree.h about 1,242. `edits_since()` contains an unused first attempt and redundant accumulator setup. Comments in Buffer.h still characterize mutation as extract/rebuild despite later fast paths. Remove dead code and make invariants visible around smaller transformations.
- **Several sources of state truth.** Visibility, origin indexes, fragment trees, version state, and undo metadata must stay synchronized. The insertion index is rebuilt on fallback, but anchor methods scan fragments instead of using it; it is not a reliable current acceleration path. Either maintain and use it or remove the maintenance cost.
- **Assertions where public errors need handling.** Some invalid anchors/ranges rely on `assert`, leaving different behavior in Release. Document preconditions for internal trusted APIs and return recoverable errors at public/wire boundaries.
- **Manual allocation in Global.** `malloc` results are used without checking failure. The small-buffer optimization is reasonable, but standard allocation/error conventions and property tests would simplify confidence in copy/move/growth behavior.
- **Silent operations and ambiguous callbacks.** Unknown streams are dropped by void `push()`; remote apply returns true even when deferred; CrdtEngine change notifications fire even for duplicate/deferred operations. Consumers need documented thread affinity, callback lifetime/reentrancy, batching, and failure semantics.
- **Finite identity space and counters.** Replica IDs are 16-bit and internal counters 32-bit despite a public 64-bit accessor. No explicit exhaustion policy is visible. This is a limitation to define, especially when characters consume IDs, not a claim that ordinary typing immediately exhausts the clock.

The dogfood enrollment allocator chooses IDs from only 1–250 and infers occupied IDs from currently visible presence summaries. Two machines enrolling while disconnected can choose the same ID; the fallback chooses 1. This undermines a foundational CRDT assumption. Use collision-resistant persistent identities or an explicit offline-safe identity mapping and collision recovery strategy.

Signing is explicitly a stub: `verify_profile()` always returns true. That is acceptable only as a clearly trusted-folder prototype contract. It must not be presented as implemented identity authentication. This review did not perform a security audit.

## 7. Performance evaluation

### 7.1 Existing benchmark results on this checkout

The reported figures below come from [benchmarks-release.log](reports/2026-10-03-evaluation-artifacts/benchmarks-release.log), using existing benchmark functions unchanged.

| Workload | Observed result | Interpretation |
|---|---:|---|
| 1 KB document, 200 random edits | 0.760 ms/edit; 1,315 edits/s | Small-document response is practical |
| 10 KB document, 200 random edits | 1.070 ms/edit; 934 edits/s | Already above README's older average |
| 100 KB document, 200 random edits | 3.621 ms/edit; 276 edits/s | Random editing has material CPU cost |
| 1 MB document, 100 random edits | 3.704 ms/edit; 270 edits/s | Similar fragment count explains similar time |
| 100 KB sequential append | 0.086 ms/edit; 11,632 edits/s | Common fast path is much stronger |
| 100 KB hotspot edits | 2.262 ms/edit; 442 edits/s | Operation position/pattern matters |
| 100 KB random insertion, 1–1,000 characters | 4.4–5.2 ms/edit | Metadata/splitting dominates these cases |
| Three-client realistic workload, 300 local operations | 1.075 s total; 279 local ops/s | Includes simulator work and remote application |
| 10 KB, 500 undos / 500 redos | 0.852 / 0.954 ms each | Full visibility walks are not free |
| Single-replica GC benchmark | 97 → 267 edits/s; 4.7 ms GC | Useful speedup, but unsafe GC is not a production solution |

These are not controlled comparisons with the historical README: hardware, compiler, engine changes, and methodology differ. The README's 3,100/1,950/450 edits/s should be labeled with the exact revision and retained benchmark procedure, rather than treated as present guarantees.

The 100 KB and 1 MB initial documents both contain approximately 1,000 insertion runs: the builder uses 100-byte chunks for the former and 1,000-byte chunks for the latter. Their similar times do not establish byte-size-independent scaling. Historical fragmentation is a more important cost driver than visible document length.

### 7.2 Latency distribution and adapter overhead

The additional [latency harness](reports/2026-10-03-evaluation-artifacts/latency.cpp) measures whole API calls with a steady clock. Random insert workloads grow their fragment count over 500 operations, so percentiles describe a changing editing history, not repeated queries on a fixed state.

| Workload | Mean | p50 | p95 | p99 | Maximum |
|---|---:|---:|---:|---:|---:|
| Buffer random single-byte insert, initial 10 KB in 100-byte runs | 2.11 ms | 2.10 ms | 4.40 ms | 5.31 ms | 5.50 ms |
| Buffer random single-byte insert, initial 100 KB in 100-byte runs | 6.23 ms | 6.03 ms | 9.89 ms | 11.97 ms | 12.80 ms |
| Buffer append, initial 100 KB in one run | 0.070 ms | 0.069 ms | 0.098 ms | 0.230 ms | 0.314 ms |
| Public CrdtEngine append, same initial size | 0.169 ms | 0.161 ms | 0.259 ms | 0.415 ms | 0.529 ms |
| IdList head insertion, growing from 0 to 1,000 entries | 0.867 ms | 0.706 ms | 2.257 ms | 2.814 ms | 3.127 ms |

There is substantial variation between the best append path and random mid-document insertion. The public wrapper adds work by reconstructing the text and scanning UTF-8 for every UTF-16 conversion. These figures exclude Qt document layout, paint, file I/O, and real transport delay. A roughly 12 ms engine call can consume a substantial part of a 60 Hz frame, but this measurement is not a direct input-to-photon latency result.

### 7.3 Offline merge scaling

The existing multi-replica benchmark cross-applies 50 independent edits from each replica. Observed total merge times were approximately:

| Replicas | Original edits | Remote applications implied by all-to-all exchange | Total measured merge time |
|---:|---:|---:|---:|
| 2 | 100 | 100 | 25.8 ms |
| 5 | 250 | 1,000 | 887.7 ms |
| 10 | 500 | 4,500 | 8.02 s |

Peer count, fragment state, and causal deferral vary together, so this table is not a clean complexity experiment. It nevertheless shows that small offline histories can become expensive with many peers. A synchronous poll processing such a batch on the GUI thread can stall the application.

The benchmark prints its total merge time under `ns/op`, and normalizes throughput by original edits rather than remote applications. Those labels must be corrected before publishing comparisons.

### 7.4 Wire-format cost still depends on numeric replica ID

Global is sparse in memory, but `encode_global()` preserves a dense array through the largest replica ID. The second one-character edit serialized to:

| Replica ID | Operation bytes | Mean encoding time over 100 calls |
|---:|---:|---:|
| 1 | 125 | 0.57 µs |
| 250 | 629 | 3.78 µs |
| 65,535 | 131,205 | 1,153 µs |

A single small edit with a high ID is therefore about 131 KB before compression. Compression may hide zeros on disk but cannot remove the encoding, decoding, buffering, and transport-envelope costs. This also helps explain the application's small-ID allocator, which trades protocol inefficiency for collision risk.

Use a versioned sparse wire representation with explicit `(replica_id, counter)` pairs. Preserve compatibility through negotiated schema handling rather than allowing numeric identity to determine payload size indefinitely. Text is also serialized in both `new_text` and inserted-fragment content, increasing payload duplication.

### 7.5 Main cost drivers

Inspection supports these specific optimization opportunities:

1. **Whole-tree ordering validation on local edits.** Even a splice-built fast path runs a fragment walk to check ordering. Its cost grows with historical fragments.
2. **Fallback relocation/normalization.** Mid-run inserts trigger split relocation and sort/rebuild work. Frequency is workload-dependent; a historical “2.3%” observation is not a general bound.
3. **Remote edits outside the fast path.** Full-path deletion/relocation scans and rebuilds remain expensive.
4. **Undo/redo visibility recomputation.** Both engines walk all stored items rather than only affected origins/deletion references.
5. **Anchor creation and resolution.** Both currently walk the fragment tree; the actual implementation does not provide the advertised logarithmic resolution.
6. **Delta computation.** `edits_since()` scans all fragments, reducing the advantage of surgical Qt updates at long histories.
7. **Unicode conversion.** CrdtEngine and CollabPane repeatedly materialize whole strings; painting and remote cursor conversion also scan prefixes. Add UTF-16 counts to summaries and seek using them.
8. **IdList rebuilding.** Ordinary insert/remove operations extract vectors and rebuild the tree. This is acceptable for tiny structural lists, but not a scalable implementation simply because SumTree is present.
9. **Per-poll directory work.** Readers enumerate segments; ack files are rewritten even without a frontier change; merged entries retain payloads indefinitely. Long-term history needs indexing/checkpointing as well as compression.
10. **Memory layout.** On this build, Fragment is 104 bytes, FragmentSummary 160 bytes, Global 40 bytes, and a FragmentTree node 3,352 bytes before owned strings/maps. Leaf item summaries and repeated version vectors contribute overhead. These are layout measurements, not a complete RSS estimate.

Local undo retention is bounded by entry count, not byte count. Coalesced entries can cover many characters. UndoMap itself is not reclaimed by the tombstone sweeps. Removing fragments therefore does not demonstrate bounded total memory or disk usage.

## 8. Test and benchmark evaluation

The test suite is a useful asset: separate primitive tests, Unicode-aware random edits, duplicate/reorder handling, undo scenarios, partitions, serialization, file segments, widgets, and deterministic benchmark seeds. Passing all executed tests is positive evidence about those exercised scenarios.

The missing tests align closely with the confirmed failures:

| Missing or insufficiently asserted property | Needed test |
|---|---|
| Local insertion redo after GC | Insert → undo → GC → redo retains exact text |
| Remote restoration after reclamation | Different replicas retain different undo histories; compact/GC; later undo/redo; compare exact content |
| Absence in watermark meet | Empty/disjoint peer vectors constrain coverage to zero |
| Sender session continuity | Restart only sender transport; a fresh edit still reaches a live peer |
| Receiver recovery | Restart with persisted cursors; restored engine equals pre-restart engine |
| Own-log replay | Close/reopen the actual sidecar editor without another peer |
| File arrival gaps | Deliver segment 2 before 1; repair an earlier invalid segment; no permanent skip |
| Exact anchor affinity | Same-offset insertions before/after left/right anchors, including document ends |
| Passage integrity | Concurrent whole-word/paragraph insertions remain contiguous |
| Decoder validity | Required fields, trailing bytes, invalid Unicode, overflow, bounded resource use |
| Actual durability ordering | Inject failed writes, failed renames, crash points, and durable-commit checks |

The simulator tests operation delivery, not the storage lifecycle. `NetworkConfig::allow_reorder` is unused. `disconnect()` blocks receipt but does not prevent a disconnected replica broadcasting new edits to online peers; it does not implement a symmetric network partition. These observations do not invalidate all existing scenarios, but reduce confidence in blanket “realistic network” claims.

Many fuzz tests use `std::random_device`, print a seed, then seed mt19937. Seeds are visible but a common command-line/environment replay mechanism is missing. Add deterministic CI seeds, a longer rotating corpus, failing-trace persistence, and automatic reduction. Check both internal invariants and user-facing semantics; some anchor tests only assert a result is within document bounds.

Two benchmark problems need immediate correction:

- `reconnect_sync_cost()` calls `net.tick(10000)` before starting its timer. The bulk remote application occurs outside the measured region, explaining the reported 0.0 ms catch-up time. That result is invalid as a reconnect performance claim.
- `multi_replica_convergence()` logs divergence and still passes rather than asserting equality, and mislabels total duration as per-operation duration.

Most benchmark `mem_delta=0` fields are default values, not measurements. GC before/after benchmarks mutate the same buffer, so they illustrate improvement but do not isolate GC from changes in state. Match traces, bytes, operation counts, fragment histories, replicas, and output verification before making cross-engine comparisons.

## 9. Comparison with other approaches

The previous competitive reports mix distinct workloads and translate trace-replay throughput into supposed single-edit latencies. That is not a valid ranking. This evaluation does not repeat their “orders of magnitude behind” numbers because I did not run matching workloads across libraries.

The useful current comparison is architectural and feature-based:

| Project | Relevance to collabtext | Adoption/comparison caveat |
|---|---|---|
| Yjs / Yrs | Network-independent updates, selective undo, established editor ecosystem; Yrs exposes a native C interface | Rust-backed native integration changes toolchain/FFI needs; file transport and Qt binding still need implementation |
| Automerge | Explicit local-first/offline design, retained history, network-independent exchange, C API, versioned persistence | General structured-document model is broader than this library's scope; native bindings and undo policy must be assessed for the application |
| Loro | Local-first structured state, collaborative undo and cursor handling, text/list primitives | Broader model; migration and native API fit need an experiment |
| cola | Focused native text CRDT; offsets returned for integration with an application's own text buffer | Rust; application owns text storage; not automatically a drop-in equivalent for all current features |
| diamond-types / Eg-walker | Strong references for text history, load/merge efficiency, and logical identity compressed into runs | Research and repository APIs need careful evaluation; shipping status and feature completeness cannot be inferred from headline speed |
| Zed | Concrete native editor CRDT/SumTree integration to study | Its production editor assumptions and implementation are not a ready-made replacement library |

Yjs documents itself as network-agnostic and suitable for decentralized/local-first applications. Its selective UndoManager includes scoped origins and grouping. Yrs's C interface explicitly supports offline replicas. These are direct counterexamples to the README's implication that major alternatives fundamentally require a WebSocket or lack useful undo. [Yjs introduction](https://docs.yjs.dev/), [Yjs UndoManager](https://docs.yjs.dev/api/undo-manager), [Yrs C interface](https://github.com/y-crdt/y-crdt/tree/main/yffi).

Automerge's documentation explicitly includes sending a file by email or USB drive and exposes a C API. Its 3.0 announcement describes a major in-memory history reduction, which is relevant to long-lived offline documents. [Automerge design principles](https://automerge.org/docs/hello/), [Automerge 3.0](https://automerge.org/blog/automerge-3/).

Loro provides local collaborative undo with grouping, depth limits, and transformed cursors. cola deliberately separates CRDT metadata from the application's text buffer. Both are useful design comparisons even if a C++-only implementation remains a hard requirement. [Loro undo](https://www.loro.dev/docs/advanced/undo), [cola repository](https://github.com/noib3/cola).

For data-structure and algorithm study, examine diamond-types' run/identity representation, the Eg-walker paper, and Zed's editor discussion. They show that compression, history management, and integration behavior deserve attention alongside operation throughput. [diamond-types](https://github.com/josephg/diamond-types), [Eg-walker](https://arxiv.org/abs/2409.14252), [Zed CRDT architecture](https://zed.dev/blog/crdts).

A native C++ source base is still a meaningful engineering preference: it avoids an FFI and Rust toolchain, simplifies Qt debugging, and offers direct control over data structures. It is not a reason to discount mature alternative semantics. I did not establish that no other C++ text CRDT exists; a claim of historical or present uniqueness needs a separately scoped survey.

## 10. Recommended sequence of work

### Milestone 1 — Reliable document lifecycle

**Release gate:** A document can be edited offline, saved, closed, reopened, crashed/restarted, and synchronized with late files without losing or suppressing operations.

Implement own/remote log replay or snapshots; tie read cursors to recoverable state; persist stream identity; handle segment gaps; implement checked durable writes; define Save; make offline enrollment IDs unique. Convert F4–F6 and F9 into regression tests. Temporarily keep reclamation disabled in application flows until its safety policy exists.

This milestone preserves the current product direction and can deliver immediate value without changing the sequence algorithm.

### Milestone 2 — Explicit merge, anchor, and reclamation semantics

**Release gate:** Small adversarial histories satisfy exact expected text, affinity, and restoration behavior across all replicas.

Define the ordering/non-interleaving target. Fix anchor boundary representation. Separate watermark intersection from summary aggregation. Decide the lifetime of remote undo/redo and persistent anchors, then implement a reclamation protocol compatible with it. Specify peer membership and epoch/bootstrap behavior. Convert F1–F3, F7, and F8 into regression cases and add reduced history enumeration.

Do not declare distributed GC complete merely because all peers have seen a deletion. It must be safe against permitted future operations.

### Milestone 3 — A supported reusable library

**Release gate:** An external consumer can install a Qt-free core and connect a single documented document adapter to OpStream.

Split targets, remove QtTest from runtime linkage, implement install/export rules, establish a real public-header boundary, consolidate adapters, document callback/threading and failure behavior, and complete validated/versioned decoding. Distinguish supported features from stubs and future plans. Update architecture status and README guarantees.

### Milestone 4 — Performance based on representative histories

**Release gate:** Published results measure engine, replay, transport, and Qt paths separately with exact state and percentile data.

Fix benchmark timing/units/assertions first. Use steady clocks, realistic manuscript traces, long tombstone histories, Unicode, many-peer catch-up, high replica IDs, and persistence restart. Record fragment counts, allocations, RSS, disk size, wire size, and p50/p95/p99. Publish compiler flags, hardware, revision, and repeated-run variance.

Then optimize full-tree walks, summary layout, origin/deletion indexes, run bookkeeping, sparse serialization, and Unicode conversions. Batch catch-up work and notifications; use a single ownership model for engine state if moving work off the GUI thread. Avoid introducing concurrent mutation of Buffer merely to hide stalls.

### Backend decision after milestones 1 and 2

Run a small backend experiment only if the ordering and reclamation fixes become disproportionately complex. Use the same traces against collabtext and one mature native-accessible alternative, retaining the file-sync transport and Qt presentation.

Choose continuing collabtext when C++ ownership is a firm constraint and the corrected semantics stay tractable. Choose a mature backend if its persistence/ordering guarantees substantially reduce maintenance cost and the FFI is acceptable. A replacement is not automatically necessary, and a full rewrite is not justified by the current average-throughput numbers alone.

## 11. Reproduction and retained evidence

The artifact folder contains:

- `probes.cpp` and `probes-release.log`: behavioral failures and normal undo/redo controls.
- `latency.cpp` and `latency-release.log`: percentile measurements, layout sizes, and replica-ID serialization costs.
- `tests-release.log`: 51-executable CTest result.
- `realistic-release.log`: realistic scenario result and printed random seeds.
- `benchmarks-release.log`: selected existing benchmarks from the repeat run.
- `README.md`: exact build/run commands and measurement limits.

The essential probes intentionally use very small histories. Their importance does not depend on statistical fuzz coverage or reproducing a particular CPU speed. Performance results are environment-dependent; the observed content mismatches and skipped records are deterministic under the tested sequences.
