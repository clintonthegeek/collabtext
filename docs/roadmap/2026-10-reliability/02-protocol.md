# 02. Identity, decoding, and protocol foundations

Goal: make identifiers, operations, codecs, and delivery outcomes explicit. This phase intentionally changes public types on the isolated branch. The user authorized a fresh API and formats without old-store compatibility. D01 specifies all resolved policies and the constraint to improve the existing native engine before public/protocol changes.

Read [execution rules](EXECUTION.md) and [decisions](DECISIONS.md) before starting. Task dependencies refer to the [master task registry](TASKS.json). New file paths below are intentional; do not assume they already exist.

## D01 — Specify the accepted native-engine and lifecycle constraints

**Status:** see TASKS.json (initially not started)  
**Depends on:** V08  
**Coverage:** R11, R13  
**Design/decision gate:** All product-policy questions resolved. Complete the written design constraints using V08; no additional user backend approval is needed.  

**Files / scope:**

- `docs/roadmap/2026-10-reliability/DECISIONS.md`
- `docs/specs/vnext-design-constraints.md`

**Steps:**

1. Carry forward all four resolved user policies: fresh API/formats without legacy migration; indefinite offline enrollment with explicit retirement and advisory reminders; persistent bounded undo AND redo; improve the existing native C++20 engine without replacing it or rewriting it in Rust.
2. Use V08 to document the intended public behavior, freely revisable source/ABI/wire/store formats, and the existing Buffer/IdList/SumTree/run/undo/transport foundations to retain. No legacy compatibility work is required.
3. Define incremental repair constraints: narrow transformations within current components, failing regressions and independent models for algorithm changes, and measured optimization after correctness. Existing engines may be studied for principles or benchmarks; do not vendor/adopt one, add a backend abstraction for substitution, or plan a parallel replacement engine.

**Acceptance:**

- All Q1–Q4 answers are recorded and applied; no pending backend-policy question blocks implementation.
- The written contract commits to evolving the existing engine and lists retained components plus exact release invariants. Algorithm design gates still require specifications/models; they cannot redirect work into a replacement.

**Verification:**

Review the constraint specification against V08 and the recorded user replies; check that no task requires Rust, Yrs adoption, a backend experiment, or legacy data migration.

## P01 — Specify the vNext type and error contracts

**Status:** see TASKS.json (initially not started)  
**Depends on:** D01  
**Coverage:** F4, F10, R1, R2, R9, R13  
**Design/decision gate:** Public signatures must be finalized here before changing callers; amendments update the new API guide.  

**Files / scope:**

- `docs/specs/vnext-protocol.md`
- `docs/specs/vnext-public-api.md`

**Steps:**

1. Define ActorId as 128-bit persistent random identity and operation counters as checked uint64; stream/document/epoch identity must be part of an operation context. Use per-stream actor counters; never infer cross-stream coverage from a scalar maximum.
2. Define separate BufferOperation and IdListOperation opaque value types, OperationId accessors, ApplyOutcome (applied/duplicate/deferred/rejected), DecodeError, StoreError, and PollOutcome. Use a small C++20 Result type rather than requiring C++23 expected.
3. Allocate separate version fields for operation payload, envelope, manifest, segment, checkpoint, and ack formats. Choose fresh baseline versions with explicit format identifiers; versioning protects future data and does not imply old-format compatibility.
4. Specify non-reentrant single-owner callbacks, replacement/unregistration, object lifetimes, failure atomicity, bounded deferred queues, and notifications only for visible change.

**Acceptance:**

- Every callback and mutation has a precise success/error/ownership contract and examples for both primitives.
- Missing/unknown schema records are retained or quarantined and cannot advance coverage. Counter exhaustion returns an error without mutation.

**Verification:**

Review the specification using the F4/F5/F10 examples and multi-stream independent-counter cases; create compile-oriented API examples for A02.

## P02 — Implement persistent actor identity, writer lease, and checked counters

**Status:** see TASKS.json (initially not started)  
**Depends on:** P01, V07  
**Coverage:** F4, R1  

**Files / scope:**

- `libs/collabtext/include/collabtext/ActorId.h`
- `libs/collabtext/src/identity/ReplicaState.cpp`
- `libs/collabtext/src/crdt/Clock.h`
- `libs/collabtext/src/crdt/Clock.cpp`
- `libs/collabtext/tests/tst_actor_identity.cpp`

**Steps:**

1. Generate IDs from OS cryptographic randomness through a narrow platform adapter; accept injected generators for collision/error tests. Persist atomically before creating operations; do not derive uniqueness from presence or 16-bit random allocation.
2. Lock a producer/stream writer across processes. Detect observable duplicate ownership/conflicting actor payloads. Keep private actor state outside shared document stores so ordinary document copying creates a new actor; require an explicit fork-identity operation when copying private device state. Identical disconnected clones cannot always be detected before exchange; document this limit.
3. Recover next counters from durable own log/checkpoint; reserve IDs without reuse and check increments/range arithmetic before mutation. Widen clocks and affected origin/vector fields consistently.

**Acceptance:**

- Offline enrollment of separate devices produces independent actors; restart cannot reuse a durable operation ID.
- Concurrent local writers, corrupted identity state, random-source failure, observable identity conflicts, and uint64 exhaustion produce typed errors without silent fallback to actor 1. Explicitly forked private state uses fresh identity; normal shared-store copies never copy writer identity.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_actor_identity|tst_clock|tst_buffer|tst_idlist'`; inject deterministic identity collisions and max-counter boundaries. Run ASan.

## P03 — Separate coverage intersection from summary aggregation

**Status:** see TASKS.json (initially not started)  
**Depends on:** P01, V02  
**Coverage:** F3, R7  

**Files / scope:**

- `libs/collabtext/src/crdt/Clock.h`
- `libs/collabtext/src/crdt/Clock.cpp`
- `libs/collabtext/src/crdt/Fragment.h`
- `libs/collabtext/src/crdt/IdList.h`
- `libs/collabtext/tests/tst_clock.cpp`
- `libs/collabtext/tests/tst_summary_laws.cpp`

**Steps:**

1. Implement coverage_intersection over the union of actors with absent components equal to zero; canonicalize zero components away. Do not change every existing meet call mechanically.
2. Inventory summary minimum fields. Remove unused fields, or specify a distinct aggregate with a real identity and associative composition; update SumTree dimensions and tests accordingly.
3. Add property cases for empty/disjoint/overlapping vectors, commutativity, associativity, idempotence, and sum-summary identity. Remove the F3 expected-fail marker.

**Acceptance:**

- Intersection of {A:10} and empty is empty; any peer lacking A constrains A coverage to zero.
- Tree summaries pass algebraic tests and visible text remains unchanged across randomized splice patterns.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_clock|tst_summary_laws|tst_sumtree|tst_evaluation_regressions'`; run seeded convergence suites.

## P04 — Implement strict JSON and UTF validation utilities

**Status:** see TASKS.json (initially not started)  
**Depends on:** P01  
**Coverage:** F10, R8  

**Files / scope:**

- `libs/collabtext/src/codec/JsonReader.cpp`
- `libs/collabtext/src/codec/Utf.cpp`
- `libs/collabtext/src/crdt/Utf16.h`
- `libs/collabtext/tests/tst_decode_validation.cpp`
- `libs/collabtext/tests/tst_utf8.cpp`

**Steps:**

1. Build a bounded, schema-focused JSON reader with complete-input consumption, arbitrary object key ordering, duplicate-key rejection, strict separators, and correct escape/surrogate handling. Use a reviewed portable parser dependency only if its adoption is explicitly recorded in P01; do not invent a permissive partial parser.
2. Validate UTF-8 scalar sequences including continuation bytes, overlong forms, surrogates, truncation, and Unicode maximum. Checked conversion reports an invalid offset inside a scalar/surrogate pair rather than manufacturing an out-of-bounds byte position.
3. Define maximum input bytes, nesting, element count, text bytes, and locator depth as configuration with conservative defaults in the spec. Reject before allocating beyond limits.

**Acceptance:**

- Missing syntax, trailing garbage, lone surrogates, invalid UTF-8, duplicate fields, and oversized structures fail with offset/context.
- Equivalent reordered JSON decodes identically; all valid non-BMP escapes round-trip correctly.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_decode_validation|tst_utf8'` and ASan/UBSan; include a fixed malformed-input corpus.

## P05 — Validate semantic operations before engine mutation

**Status:** see TASKS.json (initially not started)  
**Depends on:** P02, P03, P04  
**Coverage:** F10, R9  

**Files / scope:**

- `libs/collabtext/src/crdt/OperationValidation.cpp`
- `libs/collabtext/src/crdt/Buffer.cpp`
- `libs/collabtext/src/crdt/IdList.cpp`
- `libs/collabtext/tests/tst_operation_validation.cpp`

**Steps:**

1. Check operation/actor/epoch identities, counter intervals without overflow, insertion length versus Unicode scalar count, locator/boundary constraints, causal relationships, deletion intervals, and edit range/text cardinality.
2. Distinguish missing dependencies (deferred) from invalid input (rejected). Validate local offsets, negative public positions, invalid anchors, and overlapping ranges before mutating any tree, clock, or undo history.
3. Enforce configurable deferred-operation count/byte limits with explicit backpressure; do not silently discard dependencies. Return ApplyOutcome and avoid duplicate/deferred visible-change notifications.

**Acceptance:**

- Invalid operation/local edit leaves serialized engine state, version, undo depth, and callbacks unchanged.
- Out-of-order valid operations still converge; unknown dependencies can be deferred within budget and reported when the budget is exhausted.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_operation_validation|tst_opqueue|tst_convergence|tst_idlist_convergence'`; run boundary corpus under ASan.

## P06a — Adopt persistent actor identity in operations and make values opaque

**Status:** see TASKS.json (in progress)
**Depends on:** P05
**Coverage:** F4, R1
**Design/decision gate:** Ordering and identity-allocation contracts and independent models must exist and run before engine types change; comparator-only evidence leaves allocation/recovery obligations incomplete.

**Files / scope:**

- `docs/specs/vnext-identity.md`
- `libs/collabtext/src/crdt/Clock.h`
- `libs/collabtext/src/crdt/Clock.cpp`
- `libs/collabtext/src/crdt/UndoMap.h`
- `libs/collabtext/include/collabtext/Operations.h`
- `libs/collabtext/tests/tst_identity_ordering_model.cpp`
- `libs/collabtext/tests/reference/IdentityAllocationModel.h`

**Steps:**

1. Write the identity contract first: what a Lamport identity becomes (persistent ActorId plus a checked 64-bit counter, per R1), what total order the engine relies on, and which of those uses are convergence-critical rather than merely identifying. Prove the ordering laws against an independent model before touching the engine.
2. Complete vnext-identity.md section 3.1 before production adoption: specify operation/item/causal-clock domains, checked durable run-interval reservation or explicit subidentity, split/deletion/undo mapping, and unused-reservation recovery under contiguous coverage. Add independent multi-scalar/split/undo/restart, multi-document/stream, remote-clock, gap, and exhaustion fixtures. A comparator-only model does not authorize mechanically replacing character timestamps with one counter per operation.
3. Replace the 16-bit replica_id and 32-bit counter in the engine's identity types. The ordering must remain a deterministic total order that every replica computes identically, because origin breaks ties between fragments that share a locator and normalize_fragments exists to handle exactly that case. A locally assigned slot would diverge here and must not be used.
4. Measure the cost. Every Lamport is embedded in FragmentSummary, which the SumTree aggregates on every edit, so widening the identity widens the hot path. Record bytes-per-identity before and after so the optimization phase inherits a number rather than a surprise.
5. Make Buffer and IdList take an ActorId and expose it. Keep the legacy integer constructor only if it cannot silently mint an identity that looks valid; an operation carrying a default-constructed (invalid) ActorId is rejected, per P02.
6. Opaque operation values: move construction and field layout out of the public header so the fast paths that touch fields do so through internal accessors.

**Acceptance:**

- The independent model and the engine agree on ordering for every generated pair of identities, including equal-locator ties from different actors.
- Two distinct ActorIds never share an identity, and a default-constructed ActorId is rejected as invalid_identity rather than treated as actor 0.
- The recorded before/after size of an identity is stated in the task log, and no convergence test regresses.
- The identity-allocation contract and independent fixtures prove non-aliasing across character runs/splits/deletions/undo and specify reservation/log failure semantics; P06b/P07/L02 inherit those obligations. A checked scalar counter by itself does not satisfy this gate.

**Verification:**

Run the identity model and Buffer/IdList convergence suites in Debug, Release, and ASan; state the measured sizeof(Lamport) and sizeof(FragmentSummary) before and after.

## P06b — Implement versioned sparse codecs for the new identity and opaque values

**Status:** see TASKS.json (initially not started)  
**Depends on:** P06a  
**Coverage:** F10, R2, R5, R13

**Files / scope:**

- `libs/collabtext/include/collabtext/Serialization.h`
- `libs/collabtext/src/codec/OperationCodec.cpp`
- `libs/collabtext/tests/tst_serialization_public.cpp`

**Steps:**

1. Encode canonical sparse actor/counter vectors and an explicit payload version. The actor set is what the wire size must depend on, never the magnitude of the identity values.
2. Encode 64-bit counters exactly, with no conversion through double. A counter above 2^53 is a legal value and must round-trip bit for bit.
3. Define and test only the new codecs. An old or unknown format identifier produces an explicit incompatible-format error before mutation; diagnostic input is retained and no record is skipped silently. No legacy decoder or conversion mapping is required.

**Acceptance:**

- Wire size depends on the number of observed actors and the text, not on the magnitude of identity values; Buffer and IdList round-trips preserve logical operations.
- Header-only external compilation uses only installed public includes; unknown versions return distinguishable unsupported-version errors.

**Verification:** Compare equal-cardinality small/large-valued actor sets and exact uint64 bounds; verify new-format golden round trips and explicit rejection of old and unknown formats.

### Why P06 was split

Split out of P06 on 2026-10-04. P06 asked for opaque values and versioned sparse codecs in one step, but the actor identity is serialized by both the wire codec and the store, so the identity change cannot land before the codecs without either changing those formats outside this task's ownership or introducing a representation the codec task immediately rewrites. The identity work is therefore sequenced first *within* P06 rather than before it, which keeps it a discrete reviewable step without an impossible ordering.
The identity work is also the larger of the two: it touches 56 files and 140
construction sites in tests alone, and its risk is concentrated in one place — the
ordering gate above.

## P07 — Define transactional transport receipt and commit APIs

**Status:** see TASKS.json (initially not started)  
**Depends on:** P06b, V08  
**Coverage:** F2, F5, R2, R9, R13  

**Files / scope:**

- `libs/collabtext/include/collabtext/OpStream.h`
- `libs/collabtext/include/collabtext/StreamSync.h`
- `libs/collabtext/tests/tst_opstream_interface.cpp`
- `docs/specs/vnext-transport.md`

**Steps:**

1. Replace scalar GC fence with per-stream/per-producer durable coverage; supply explicit delivery tokens and commit/retry dispositions. A token identifies exact producer/envelope sequence and epoch, not engine Lamport maximum.
2. Specify push durability modes, result/error return, unknown-stream errors, subscribe/unsubscribe lifetime, poll budgets, and the requirement that commit occurs only after recoverable application state is durable.
3. Expose transport coverage independently from semantic GC certificates. Provide a new-contract reference adapter; no old OpStream wrapper is required and no adapter may fabricate durable acknowledgements or destructive-GC eligibility.

**Acceptance:**

- A no-callback or deferred/rejected inbound record cannot be acknowledged as durably applied.
- Different streams using the same actor/counter values never advance one another's frontiers. Third-party transport implementers have complete new-contract examples.

**Verification:**

Compile the new interface sample and in-memory test double; run `tst_opstream_interface` and new `tst_transport_commit_contract` tests.

## P08 — Fuzz and bound the complete decoding pipeline

**Status:** see TASKS.json (initially not started)  
**Depends on:** P06b, P07, V04  
**Coverage:** F10, R4  

**Files / scope:**

- `libs/collabtext/tests/fuzz_decode.cpp`
- `libs/collabtext/tests/tst_decode_validation.cpp`
- `libs/collabtext/src/crdt/ZstdUtil.cpp`
- `libs/collabtext/src/crdt/SegmentFormat.cpp`

**Steps:**

1. Fuzz operation, envelope, manifest, ack, cursor, and segment decoders using one checked boundary. Apply only values accepted by semantic validation to fresh engine instances.
2. Cap decompressed bytes and records; reject incomplete zstd frames, malformed base64 padding, unexpected segment kinds/flags, checksum failures, and size mismatches.
3. Seed corpus from V08 fixtures plus F10 probes; record max input/output budgets and run bounded deterministic sanitizer campaigns.

**Acceptance:**

- Malformed input cannot cause out-of-bounds access, unchecked overflow, indefinite loops, or uncontrolled allocation.
- Corrupt/unsupported records retain their original bytes and do not produce a committed delivery token.

**Verification:**

Run ASan/UBSan corpus tests and a time-bounded fuzz campaign; publish compiler/options, duration, corpus hash, and findings. No unbounded fuzz run in routine CI.
