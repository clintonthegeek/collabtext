# Performance evaluation of the merged engine

Measured 2026-10-19 at `d04f151b3a70b161b72c33a8de6895efbd1c13e2`
(`master`, after the engine integration). This is a new baseline. No engine,
consumer, API, durable format, candidate tag or task status changed for this evaluation.

## Assessment

The engine has useful interactive performance for the small and moderately edited
cases measured here. It is **not yet an efficiently indexed engine for long-lived,
heavily edited streams**. Retained structure matters much more than visible text
size alone. A 10 KB document after 3,000 replacements costs far more to edit than
a much larger document seeded in one insertion. Boundary projection is an
especially expensive public read operation. The optimized marker path avoids
that scalar projection, although it still scans fragments.

The strongest next investment is internal indexing and incremental mutation,
followed by realistic consumer measurements. These findings do not call for an
API redesign or removing causal/scope checks. They do call for avoiding repeated
full projections in consumers, choosing explicit import/undo limits, and testing
aged documents before declaring performance acceptance.

Concrete results on this machine:

- Public 100 KB single-character replacement: median per-run p99 **0.299 ms**.
- Public 10 KB replacement after 3,000 prior replacements: median per-run p99
  **9.643 ms**, ranging **5.205–10.904 ms** across runs. Mean cost increases
  **33.6–44.6 times** relative to the matching zero-prior-history case within
  each run. The byte-engine control shows the same history sensitivity.
- A boundary mint or resolve on 100 KB costs approximately **2.4 ms mean**;
  resolving an existing marker after 1,000 replacements costs **15.4 µs mean**.
- A single-character operation with 512 actors in its causal frontier encodes
  to **28,100 bytes**, versus **506 bytes** with one actor.
- Checkpoint opens beat full replay at both tested sizes. These are warm,
  memory-backed-filesystem measurements, not SSD or cold-start results.

There is no measured ranking against Yjs, Automerge, Loro, or another engine in
this evaluation. Nor is there an apples-to-apples pre-merge speedup claim. Earlier
reports used different workloads, protocols, and/or environments. A general
“faster than other CRDTs” claim would require running them on the same traces and
matching semantics, including undo, identities, causality, persistence and queries.

## Measurement method and limits

Release C++20, GCC 16.2.1, `-O3 -DNDEBUG`, sanitizers off; QtTest 6.11.2 for the
existing benchmark driver. Hardware: Intel i7-1185G7, four cores/eight threads,
3 GHz nominal, up to 4.8 GHz. Linux 6.18.49 / Manjaro, x86-64. See
[environment.json](2026-10-19-performance/environment.json) and
[actual core flags](2026-10-19-performance/core-flags.txt).

Each of 12 selected existing benchmark methods ran in five fresh processes;
each process reported 14 passes including initialization/cleanup, zero failures.
A separate harness ran 30 public/API-control cases in five fresh processes,
checking errors and final results at runtime even in Release. The aggregation
also checks identical case order, sample counts, work counts, and structural
results across repeats. All checks passed. This is benchmark correctness evidence,
not a rerun of the complete correctness, sanitizer or installed-package gates.

Public edit rows time 200 calls individually. Query/list rows have 50 calls;
codec rows have 100. Tables give the **median of five per-run statistics**.
A p99 is the lower empirical order statistic, index `floor(.99*(N-1))`, within
each run. These small samples are descriptive, not reliable extreme-tail/SLA
estimates; the 50-sample p99 is the second-largest sample. Raw evidence contains
per-run summaries, not individual latency samples. Existing benchmarks report
whole-region averages only: their averages must not be relabeled percentiles.

This was a shared laptop environment: no CPU affinity, fixed governor, thermal
control, or host isolation. Recorded load averages varied materially; timing
ranges show that variation. No deliberate concurrent builds ran during the timed
matrix. All repeats are retained; none was discarded as an outlier. The first
public run preceded the recorded matrix; its source hash matches the later runs,
but its load-before/load-after was not recorded. Public always precedes its
byte-control case on separate fresh engines, so allocator/cache ordering can
influence their difference. There is no sampling or allocation profile; source
findings below identify algorithmic work, not percentages of CPU time.

Engine edit timings exclude GUI/layout/paint, serialization, networking and a
durable allocator. A deterministic in-memory checked allocator is used. The
standalone harness excludes reference-string updates and validation from the timed
call. The existing random-edit driver includes RNG/string generation, permits
no-ops, and uses the unchecked `Buffer::apply_local_edit` wrapper, whose refusal
result is an empty operation. Its nominal local-edit counts are attempts, not a
proof that every attempt was accepted. Prefer the checked standalone cases for
public edit latency conclusions. Remote tests additionally check convergence.

All sizes below use decimal bytes unless explicitly marked KiB/MiB. Physical
collection/log pruning was not enabled or evaluated. Counter-gap and tree-delta
performance were not isolated. Repeated callback/projection patterns in real
Wave/Markoff adapters remain unmeasured.

## Public editing: size, history and coordinate conversion

Seed: one insertion of ASCII `a`, then fixed-length one-character replacements
at deterministic random positions, always changing the selected character.
“Prior replacements” excludes the seed and the following 200 measured edits;
the sample window itself accumulates history. All cases validate the complete
final text. Matching byte-control cases use exactly the same trace and assert
identical encoded final operations, not just equal visible text.

| Visible text | Prior replacements | Public mean, ms | Public p50, ms | Public p99, ms | Range of per-run p99, ms | Byte-control mean, ms |
|---|---:|---:|---:|---:|---:|---:|
| 1 KB | 0 | 0.093 | 0.088 | 0.302 | 0.185–0.577 | 0.106 |
| 10 KB | 0 | 0.114 | 0.118 | 0.301 | 0.201–0.718 | 0.090 |
| 100 KB | 0 | 0.147 | 0.136 | 0.299 | 0.224–0.560 | 0.104 |
| 1 MB, raised budget | 0 | 0.978 | 0.924 | 1.670 | 0.978–2.963 | 0.238 |
| 10 KB | 500 | 0.766 | 0.733 | 1.193 | 0.922–3.474 | 0.915 |
| 10 KB | 3,000 | 4.551 | 4.079 | 9.643 | 5.205–10.904 | 4.582 |

The large-document control identifies material UTF-16 facade overhead:
at 1 MB public mean is 3.59–4.11 times byte-control mean within each run.
At 3,000 prior replacements the two paths are similarly expensive, pointing to
shared retained-structure work rather than coordinate conversion as the main
problem in that case. Small-case differences are noisy; do not infer a constant
facade surcharge by subtracting independently aggregated medians.

A separate Unicode case starts with 10,000 repetitions of `a😀é` (70,000 UTF-8
bytes / 40,000 UTF-16 units), then appends 200 emoji through the public API.
Mean **0.073 ms**, p99 **0.138 ms**; final text and 40,400-unit length checked.
This exercises surrogate-aware offsets, not arbitrary large Unicode pastes.

The default undo depth is 1,000; the 3,000-prior-edit case does **not** retain
3,000 active undo entries. Limiting active undo does not eliminate retained
sequence/history costs.

### Import/undo limit that consumers should choose explicitly

Only the 1 MB public and byte-control rows raise `max_undo_bytes` to **256 MiB**.
All other standalone cases use defaults. A separate fresh-engine probe confirms:

| Single ASCII insertion, default settings | Result |
|---|---|
| 256 KiB | Accepted |
| 512 KiB | Accepted |
| 1,000,000 bytes | Refused: `local edit exceeds configured undo byte limit` |

The refusal preserved empty text and did not advance the checked allocator.
The default 16 MiB budget charges inserted-scalar undo identity metadata as well
as text; it is not a 16 MiB paste allowance. These three probes do not establish
the exact cutoff. Consumers should choose explicit undo/import policy and handle
`limit_exceeded`. Chunking changes undo grouping and retained structure; it is
not a semantically free workaround. Raising this budget does not promise a
corresponding RSS bound or solve aged-document performance.

## Position and list reads

100 KB ASCII text, after the indicated prior replacements, with an existing
marker and boundary at byte 50,000. No edits occur inside the query window.
Marker insertion cost is excluded. All resolved offsets are checked.

| Operation | Prior replacements | Mean, µs | p99, µs |
|---|---:|---:|---:|
| Mint boundary | 0 | 2,391 | 4,278 |
| Resolve boundary | 0 | 2,370 | 3,742 |
| Resolve marker | 0 | 0.159 | 0.150 |
| Mint boundary | 1,000 | 2,896 | 3,938 |
| Resolve boundary | 1,000 | 2,945 | 4,699 |
| Resolve marker | 1,000 | 15.381 | 23.494 |

Mean can exceed p99 when a few extreme samples dominate; the sub-microsecond
marker row is also sensitive to clock overhead. The robust conclusion is the
large difference between scalar projection and fragment scanning. Marker resolve
is **not constant-time** in retained fragments. Boundaries and markers have
different semantics and mutation/retention costs; this does not justify replacing
all boundaries with markers. Repeated reads in a render/cursor loop deserve
measurement at the consumer level.

Ordered list projection, all application values deliberately equal to `7`, with
full distinct insertion identities and a known last identity:

| Visible list entries | `visible_entries()` mean, µs | p99, µs |
|---:|---:|---:|
| 100 | 4.944 | 5.271 |
| 1,000 | 52.285 | 60.338 |
| 5,000 | 347.354 | 480.583 |

This measures full enumeration, not insertion or anchor lookup. A full copy
naturally scales with output size. Code additionally shows
`try_anchor_of(identity)` enumerating the entire visible list before searching.
Calling that separately for every entry can create quadratic application work.
Consumers can enumerate once and reuse full identities; an indexed engine lookup
is a sensible later improvement. No identity or ordering contract change is
needed to address this.

## Existing mixed workloads: fragmentation, tombstones, undo, merge

These are median **region averages**, including the driver overhead described
above, not per-edit p99s. Document seeding is chunked: 100-byte chunks up to
100 KB and 1,000-byte chunks at 1 MB. Thus both 100 KB and 1 MB start with 1,000
fragments. The 1 MB row looking faster does not mean larger documents are faster,
and these rows are not directly comparable to the one-insertion public table.

| Mixed local workload | Mean per attempted edit, ms | Final fragments | Final tombstone fragments |
|---|---:|---:|---:|
| 10 KB seed, 200 edits | 0.129 | 576 | 168 |
| 100 KB seed, 200 edits | 0.632 | 1,488 | 181 |
| 1 MB seed, 100 edits | 0.606 | 1,244 | 82 |
| 5 KB seed, no initial deletions, 200 edits | 0.103 | 518 | 177 |
| 5 KB seed, initially delete 50% byte-by-byte, 200 edits | 1.831 | 4,062 | 2,824 |
| 5 KB seed, initially delete 90% byte-by-byte, 200 edits | 2.370 | 5,168 | 4,934 |
| 8 KB seed, initially delete 50% byte-by-byte, 50 edits | 3.297 | 6,076 | 4,091 |

Deletion percentages are fractions of original text, not final fragment ratios.
The deletion procedure changes both fragmentation and live text; it does not
isolate the cost of tombstones alone. Nevertheless, a tiny visible document can
have millisecond edit costs. A benchmark keyed only by visible document bytes
would conceal that important behavior.

The 10 KB + 2,000 single-character insertion workload grows from 1,073 fragments
after 500 insertions to 3,759 after 2,000. Cumulative mean grows from **0.199 ms**
to **0.853 ms** per insertion. These are cumulative averages, not latency of the
500th/2,000th call or separate interval measurements.

Undo region averages on a 10 KB seed: **0.039 / 0.268 / 0.941 ms** per successful
undo after 100 / 500 / 1,000 attempted edits. In the separate 500-edit cycle,
mean edit / undo / redo costs are **0.253 / 0.290 / 0.361 ms**. All three phases
in that cycle report final structural state, not separate state snapshots.

Concurrent independent editing and all-to-all application:

| Replicas, 50 source edits each | Timed remote applications | Mean per application, µs |
|---|---:|---:|
| 2 | 100 | 28.12 |
| 5 | 1,000 | 54.94 |
| 10 | 4,500 | 96.16 |

That scaling combines state growth, causal/order handling and retry passes; it is
not a fixed-state actor-count experiment. Offline catch-up with 200 edits per
online peer (400 source edits) applies **416** deliveries in the timed region:
median **30.47 ms total / 73.24 µs per application**. Normalizing by source edits
would give the wrong operation cost. Convergence is checked. This excludes real
network, serialization and durable append latency.

### What memory measurements establish

After 5,000 mixed edits from empty, the existing driver ends with **8,496 visible
bytes, 6,332 fragments, 1,631 tombstone fragments**. Cumulative mean is **1.693 ms**
per attempted edit. This is substantial retained structure relative to live text.

Its measured process RSS delta is zero in every run: previous benchmark cases
already grew the process allocator enough to satisfy this case. That result is
**not zero engine memory growth**. Likewise, the standalone harness records
absolute process RSS after multiple prior cases, not isolated engine allocation.
The evidence does not establish bytes per character, bytes per edit, peak
allocation or a memory-leak verdict. Use fresh isolated cases and an allocation
profiler before selecting a memory target or judging a representation change.

## Causal metadata and operation codec

One-character insertion after observing the specified number of distinct actors.
Frontiers are established before timing; encoding/decoding and exact encoded
bytes / decoded operation identity are checked. This is codec-only work.

| Actors represented | Encoded bytes | Encode mean, µs | Decode mean, µs |
|---:|---:|---:|---:|
| 1 | 506 | 7.328 | 5.618 |
| 32 | 2,180 | 30.106 | 20.238 |
| 128 | 7,364 | 108.554 | 74.975 |
| 512 | 28,100 | 435.558 | 304.851 |

The current full causal frontier imposes meaningful transport and CPU overhead
at high historical actor cardinality, even for a tiny payload. It does not follow
that 512 currently online users are needed: actors remembered by the stream are
what matter. These are a specific operation/scope fixture, not universal wire
sizes. Compression, batching and causal summaries should be considered if real
frontiers grow this large; causal refusal/coverage must remain correct. Unlike
internal tree/index work, changing a durable operation encoding requires explicit
reader/format planning for currently durable consumer data.

## Checkpoint recovery

The existing durable recovery benchmark creates one-byte append histories, then
alternates five warm opens per mode with identical expected state and checks
readiness and exact text. Times cover open/reconstruction, excluding source
creation and close. Each reported mode statistic is its five-open median; this
is not five independent source-history repetitions.

| Source operations / text bytes | Checkpoint open, ms | Full replay, ms | Full replay / checkpoint |
|---:|---:|---:|---:|
| 512 / 512 | 15.91 | 64.81 | 4.07× |
| 2,048 / 2,048 | 66.15 | 940.37 | 14.22× |

`/tmp` is **tmpfs** on this host. These results isolate warm reconstruction
through the durable code paths on a memory-backed filesystem; they do not measure
SSD durability, cold cache, or crash-time I/O. The workload has tiny text and many
operations, not a realistic Markdown file. Checkpointing meaningfully avoids
replay work; even checkpoint loading grows with retained state. Do not compare
these absolute times to the October 11 report as a regression without rerunning
both revisions under identical conditions.

## Code-backed priorities

These priorities follow measurements plus production code, not documentation
claims. They are recommendations, not task-registry completion or authorization
to alter consumer contracts.

1. **Remove scalar materialization from common boundary reads.**
   `CrdtEngine::boundary_at/resolve` call `Buffer::position_items()`, which copies
   all fragments and emits an item per retained scalar, including hidden text.
   Use run-aware traversal and identity/rank indexes with the existing boundary
   semantics. Gate on the independent position model, Unicode, hidden referents,
   affinity, scopes and detached/history-unavailable cases. Rerun the query table
   with history and larger text; preserve the existing marker path's advantage.
2. **Make mutation incremental and add UTF-16 aggregates.** `get_fragments()`
   copies the collection; `set_fragments()` recomputes visibility and reconstructs
   the tree across all fragments. Both local edits and remote application use
   these paths. `utf16_range()` separately visits visible fragments and scans
   text for conversion. The tree's existence does not make these callers
   logarithmic. Profile copying, ordering, visibility and undo retirement before
   choosing the implementation sequence; replace whole-collection operations
   while retaining checked allocation, transactional refusal and FugueMax order.
   Verify both bulk-seeded size cases and fixed-visible-size aging cases. Do not
   blame debug consistency checks: those are not the normal edit path here.
3. **Index full list identities and avoid repeated projections.** Keep
   `visible_entries()` as an honest full enumeration. Make repeated identity
   lookup cheap without weakening duplicate application-value/full-identity
   semantics. Measure list mutation and aged/deleted lists too; the current
   projection table alone does not establish those costs.
4. **Measure isolated allocation and real adapter workloads before GC decisions.**
   Capture per-operation allocations/peak resident memory, age a realistic
   Wave block and Markoff document, and include callbacks, tree-delta application,
   durable identity reservations and real storage. History retention is visible
   in the timings; this does not justify enabling physical GC or pruning ahead
   of their correctness/consumer acceptance gates.
5. **Address causal metadata if measured consumer actor cardinality warrants it.**
   Track actor count and wire bytes in actual streams. Compare batching/transport
   compression before inventing a new durable encoding. Then benchmark any codec
   change with checked decode, malformed inputs and the existing reader duties.

Useful source entry points:
[boundary calls](../../libs/collabtext/src/CrdtEngine.cpp),
[fragment/scalar and mutation paths](../../libs/collabtext/src/crdt/Buffer.cpp),
[undo defaults](../../libs/collabtext/src/crdt/Buffer.h),
[list projection and lookup](../../libs/collabtext/src/IdListEngine.cpp),
[operation codec](../../libs/collabtext/src/codec/OperationCodec.cpp).

## Evidence and reproduction

[Raw results, harnesses and scripts](2026-10-19-performance/README.md) include all
five runs, environment, compile flags, checkpoint results, refusal probe,
[aggregated medians/ranges](2026-10-19-performance/summary.json), and an
aggregator that checks repeat consistency. The standalone harness includes a
private `Buffer` byte-control; consumer-facing cases use public engines. Neither
is real consumer approval. This evaluation does not certify the roadmap's broad
performance goals, complete X tasks, or change physical-GC/adoption policy.
