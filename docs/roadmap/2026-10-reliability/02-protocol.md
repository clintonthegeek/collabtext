# 02. Identity, decoding, and protocol foundations

Goal: make identifiers, operations, codecs, and delivery outcomes explicit. This phase intentionally changes public types on the isolated branch. Legacy codecs remain in a read-only migration module; do not claim mixed-version live collaboration. D01 captures the user's compatibility choice before protocol work begins.

Read [execution rules](EXECUTION.md) and [decisions](DECISIONS.md) before starting. Task dependencies refer to the [master task registry](TASKS.json). New file paths below are intentional; do not assume they already exist.

## D01 — Finalize compatibility and lifecycle decisions

**Status:** see TASKS.json (initially not started)  
**Depends on:** V08  
**Coverage:** R11, R13  
**Design/decision gate:** Q1–Q4 must be resolved; alternate routes require updating the task registry.  

**Files / scope:**

- `docs/roadmap/2026-10-reliability/DECISIONS.md`
- `docs/specs/vnext-compatibility.md`

**Steps:**

1. Record the user answers to compatibility, offline peer retirement, persisted undo, and the native C++ requirement. If no answer is available, leave dependent tasks blocked rather than treating elapsed time as approval.
2. Enumerate source, ABI, behavior, wire, and on-disk breaks. Specify migration input read-only/output new store and the old/new store isolation rule for the recommended route.
3. Confirm this release keeps a C++20 engine, Buffer and IdList only, Qt as an optional adapter, and one file transport. A backend replacement remains a separate technical decision, not an automatic dependency.

**Acceptance:**

- No unresolved policy is silently embedded in an implementation task.
- Mixed-version collaboration, if required, has its own negotiated bridge plan before P01 proceeds.

**Verification:**

Review the decision register against the user's actual replies and V08 inventory; no tests required for this documentation-only task.

## P01 — Specify the vNext type and error contracts

**Status:** see TASKS.json (initially not started)  
**Depends on:** D01  
**Coverage:** F4, F10, R1, R2, R9, R13  
**Design/decision gate:** Public signatures must be finalized here before changing callers; amendments update migration mapping.  

**Files / scope:**

- `docs/specs/vnext-protocol.md`
- `docs/specs/vnext-public-api.md`

**Steps:**

1. Define ActorId as 128-bit persistent random identity and operation counters as checked uint64; stream/document/epoch identity must be part of an operation context. Use per-stream actor counters; never infer cross-stream coverage from a scalar maximum.
2. Define separate BufferOperation and IdListOperation opaque value types, OperationId accessors, ApplyOutcome (applied/duplicate/deferred/rejected), DecodeError, StoreError, and PollOutcome. Use a small C++20 Result type rather than requiring C++23 expected.
3. Allocate separate version fields for operation payload, envelope, manifest, segment, checkpoint, and ack formats. Confirm existing maximum versions via fixture inventory; document each bump rather than blindly using one shared number.
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

## P06 — Implement versioned sparse codecs and opaque operation values

**Status:** see TASKS.json (initially not started)  
**Depends on:** P05  
**Coverage:** F10, R2, R5, R13  

**Files / scope:**

- `libs/collabtext/include/collabtext/Operations.h`
- `libs/collabtext/include/collabtext/IdListOperations.h`
- `libs/collabtext/include/collabtext/Serialization.h`
- `libs/collabtext/src/codec/OperationCodec.cpp`
- `libs/collabtext/src/codec/LegacyCodec.cpp`
- `libs/collabtext/tests/tst_serialization_public.cpp`

**Steps:**

1. Implement copy/move opaque operation values and stable OperationId/stream accessors; keep Buffer and IdList variants separate. Move construction/details into internal headers.
2. Encode canonical sparse actor/counter vectors and explicit payload version; avoid storing replacement text twice. Encode uint64 values exactly without double conversion.
3. Keep strict legacy decoding only for migration fixtures. A normal vNext store rejects incompatible legacy envelopes instead of warn-and-skip loss. Add a read-only old/new codec mapping document.

**Acceptance:**

- Wire size depends on observed actors/text, not largest numeric legacy actor ID; Buffer/IdList round-trips preserve logical operations.
- Header-only external compilation uses only installed public includes; unknown versions return distinguishable unsupported-version errors.

**Verification:**

Run serialization and public-operation tests; benchmark 1, 250, 65535 legacy IDs through migration and corresponding vNext actors. Verify round-trip stable fixtures.

## P07 — Define transactional transport receipt and commit APIs

**Status:** see TASKS.json (initially not started)  
**Depends on:** P06, V08  
**Coverage:** F2, F5, R2, R9, R13  

**Files / scope:**

- `libs/collabtext/include/collabtext/OpStream.h`
- `libs/collabtext/include/collabtext/StreamSync.h`
- `libs/collabtext/tests/tst_opstream_interface.cpp`
- `docs/specs/vnext-transport.md`

**Steps:**

1. Replace scalar GC fence with per-stream/per-producer durable coverage; supply explicit delivery tokens and commit/retry dispositions. A token identifies exact producer/envelope sequence and epoch, not engine Lamport maximum.
2. Specify push durability modes, result/error return, unknown-stream errors, subscribe/unsubscribe lifetime, poll budgets, and the requirement that commit occurs only after recoverable application state is durable.
3. Expose transport coverage independently from semantic GC certificates. Document an old OpStream adapter for sending only; it must never fabricate durable acknowledgements or destructive-GC eligibility.

**Acceptance:**

- A no-callback or deferred/rejected inbound record cannot be acknowledged as durably applied.
- Different streams using the same actor/counter values never advance one another's frontiers. Third-party adapters have complete porting examples.

**Verification:**

Compile the new interface sample and in-memory test double; run `tst_opstream_interface` and new `tst_transport_commit_contract` tests.

## P08 — Fuzz and bound the complete decoding pipeline

**Status:** see TASKS.json (initially not started)  
**Depends on:** P06, P07, V04  
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
