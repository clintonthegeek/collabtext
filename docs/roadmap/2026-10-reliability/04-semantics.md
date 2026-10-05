# 04. Ordering and anchor semantics

Goal: agree and execute precise sequence semantics, independent of transport and GC. Algorithm work is gated by a complete specification and reference interpreter. Do not fix concurrent paste by special-casing equal locators or by merely changing a comparator.

Read [execution rules](EXECUTION.md) and [decisions](DECISIONS.md) before starting. Task dependencies refer to the [master task registry](TASKS.json). New file paths below are intentional; do not assume they already exist.

The selected route is incremental repair of existing Buffer/IdList integration, anchors, and fast paths. Keep their working foundations; complete specification/model gates prevent unprincipled changes without introducing a replacement production engine.

## S01 — Specify sequence ordering and observable edit behavior

**Status:** see TASKS.json (initially not started)  
**Depends on:** P01, V04  
**Coverage:** F8, R4  

**Files / scope:**

- `docs/specs/vnext-sequence-semantics.md`
- `libs/collabtext/tests/fixtures/sequence-semantics/`
- `docs/specs/vnext-external-position-contract.md`

**Steps:**

1. State exact properties: convergence, duplicate safety, causal integration, contiguous concurrent passages, intended left/right insertion affinity, selective local undo, and explicit behavior for concurrent replacements.
2. Create finite fixtures for whole-run paste, per-character forward typing, reverse typing, nested insertions, deleting a neighbor concurrently, undo and reinsert, and editing inside a merged run. Include Buffer and IdList ties; stable expected order derives from actor/operation order and the chosen algorithm.
3. Keep plain text and IdList only. Do not claim arbitrary semantic merge intent or maximal non-interleaving unless the selected algorithm/spec establishes it.
4. Add W2 fixtures named boundary_affinity_ab_insert_X, scoped_empty_start_end_tokens, shared_token_concurrent_boundary_inserts, and deleted_neighbors_explicit_collapse. Record the same minted token, its causal state, affinity, exact offsets, and later token comparisons; include all scalar boundaries and list lookup misses. Semantic paragraph recognition after arbitrary replacement is outside the contract.

**Acceptance:**

- F8 fixture accepts only abcXYZ or XYZabc according to the specified tie rule, never aXbYcZ.
- Every example has operation IDs, causal edges, and exact expected content, not just equality between replicas.
- External-position fixtures specify exact left/right and collapse expectations, including empty/end boundaries; equality of separately minted tokens is insufficient.

**Verification:**

Review fixture histories against the evaluation and original engine; run fixture parsing tests. No production algorithm change in this task. Verify the W-series named cases assigned above at this task's implementation layer and record their evidence; later dependent cases remain pending.

## S02 — Specify the ordering repair within the existing C++ engine

**Status:** see TASKS.json (initially not started)  
**Depends on:** S01, P06b, L08  
**Coverage:** F8, R12  
**Design/decision gate:** Requires a complete reviewed incremental repair specification and independent executable model. Unresolved ordering invariants block dependent code changes; replacement engines are outside scope.  

**Files / scope:**

- `docs/specs/vnext-sequence-algorithm.md`
- `docs/reports/vnext-ordering-design.md`
- `libs/collabtext/tests/reference/SequenceModel.h`
- `docs/specs/vnext-external-position-contract.md`

**Steps:**

1. Trace each S01 failing history through existing Buffer/IdList insertion integration, origins, run splitting, Locator ordering, and undo visibility. Identify the first violated invariant and specify the smallest representation/integration repair that satisfies the complete fixtures. Retain current engine components and storage structures where their invariants hold.
2. Write changed operation fields (if any), integration pseudocode, tie-break rules, run splitting/identity, causal deferral, undo interaction, and anchor boundary interpretation. Include a current→repaired transformation map with exact helper/file scope and justify each changed invariant. Existing origin-neighbor algorithms may inform the repair; do not implement a replacement CRDT/backend.
3. Implement a deliberately simple independent vector/reference interpreter for the repaired semantics. Validate the full fixture and bounded-history corpus. If it fails, record the smallest counterexample and refine this repair specification; do not launch a Rust/backend experiment or ask a routine executor to invent missing rules.
4. Specify W2 boundary interpretation in the independent model: scoped empty/start/end values, same-token concurrent inserts under every causally valid delivery order, deleted-neighbor collapse reporting, later comparison, and separate UTF-8 byte/list-index units. Review these fixtures before unlocking S03/S04; do not infer semantic attachment intent from convergence.

**Acceptance:**

- The repair includes implementable pseudocode, a transformation map within current components, and an independent reference interpreter passing S01.
- Technical review checks nested/concurrent runs, reverse insertion, post-merge edits, undo, and reuse of existing storage. A superficial comparator tweak or an incomplete specification does not unlock S03.
- The independent interpreter passes all W2 named fixtures and specifies representable boundary context and typed placement results; missing rules keep the implementation gate closed.

**Verification:**

Run the reference interpreter on all S01 fixtures and bounded enumerations; retain exact traces and implementation-independent expected outputs. Verify the W-series named cases assigned above at this task's implementation layer and record their evidence; later dependent cases remain pending.

## S03 — Apply the ordering repair to existing Buffer and IdList paths

**Status:** see TASKS.json (initially not started)  
**Depends on:** S02  
**Coverage:** F8  

**Files / scope:**

- `libs/collabtext/src/crdt/Buffer.cpp`
- `libs/collabtext/src/crdt/IdList.cpp`
- `libs/collabtext/src/crdt/SequenceOrder.cpp`
- `libs/collabtext/tests/tst_sequence_semantics.cpp`

**Steps:**

1. Apply the S02 transformation map to current Buffer/IdList integration helpers. Use their existing general/slow paths as the initial correctness baseline; preserve logical identities and run compression. Do not create a new production engine alongside them. Production decisions must match the independent reference interpreter.
2. Adjust existing shared-locator order assumptions and split relocations only where the reviewed repair requires it. Keep unaffected storage/tree/undo behavior; update codecs/schema fixtures for explicitly changed fields and reject obsolete formats.
3. Disable old fast paths when their invariants no longer apply; do not optimize this task. Remove the F8 expected-fail marker only after all exact-content fixtures pass.

**Acceptance:**

- Production Buffer and IdList match the reference model on every S01 history and delivery permutation.
- All existing valid local editing, duplicate, undo, and convergence tests pass or have explicitly justified semantic expectation updates.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_sequence_semantics|tst_convergence|tst_idlist_convergence|tst_evaluation_regressions'` plus seeded differential traces under ASan.

## S04 — Define and implement stable boundary anchors

**Status:** see TASKS.json (initially not started)  
**Depends on:** S03  
**Coverage:** F7, R2, R13  

**Files / scope:**

- `libs/collabtext/include/collabtext/Anchor.h`
- `libs/collabtext/src/crdt/Anchor.h`
- `libs/collabtext/src/crdt/Buffer.cpp`
- `libs/collabtext/src/crdt/IdList.cpp`
- `libs/collabtext/tests/tst_anchor.cpp`
- `docs/specs/vnext-external-position-contract.md`
- `libs/collabtext/tests/tst_anchor_codec.cpp`
- `libs/collabtext/tests/tst_idlist_anchor.cpp`

**Steps:**

1. Represent boundaries and affinity according to S02 using sufficient left/right identity context; cover start/end, empty document, deleted neighbors, and subsequent concurrent insertions.
2. Return explicit unresolved/wrong-epoch results rather than silently returning document end. Specify IdList element anchors separately from text insertion boundaries.
3. Encode anchors with their own version, document/stream/primitive/epoch context, and checked affinity. Reject unknown or unsupported formats explicitly. Keep readers for supported published vNext token versions; no pre-vNext anchor conversion is required.
4. Implement W1/W2 via the public opaque token codec and typed position results carrying UTF-8 byte offsets or list indices plus specified collapse placement. Add checked IdList lookup. Bind document/stream/primitive/epoch/affinity, reject foreign/malformed/unknown tokens without mutation, and round-trip original bytes after reopen and replica replay. Freeze the published token version and its supported-reader contract; pre-vNext anchors require no conversion.

**Acceptance:**

- In ab→aXb, preexisting left/right anchors at 1 resolve to 1/2; start/end affinity and remote exact-position tests pass.
- Deleted-neighbor anchors remain meaningful before reclamation; foreign-stream/epoch anchors cannot resolve accidentally. F7 marker is removed.
- Named cases external_token_roundtrip_reopen, boundary_affinity_ab_insert_X, scoped_empty_start_end_tokens, shared_token_concurrent_boundary_inserts, deleted_neighbors_explicit_collapse, foreign_or_unsupported_token_is_explicit, and missing_list_id_is_not_an_end_anchor pass through library-owned encode/decode. Resolution returns an actual typed position or error, never an application outcome without an offset.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_anchor_codec|tst_anchor|tst_idlist_anchor|tst_sequence_semantics|tst_evaluation_regressions'`; assert exact positions through undo/redo and concurrent inserts.

## S05 — Define and implement retained external anchors and history policy

**Status:** see TASKS.json (initially not started)  
**Depends on:** S04, L06  
**Coverage:** F7, R10  

**Files / scope:**

- `docs/specs/vnext-anchor-retention.md`
- `libs/collabtext/src/DocumentSession.cpp`
- `libs/collabtext/src/crdt/Comment.cpp`
- `libs/collabtext/src/crdt/ChatMessage.cpp`
- `libs/collabtext/tests/tst_anchor_retention.cpp`
- `docs/specs/vnext-external-position-contract.md`
- `libs/collabtext/tests/fixtures/external-position/`

**Steps:**

1. Classify ephemeral cursors separately from durable internal and external comments/bookmarks/replies/undo selections. Implement explicit durable anchor pins and their typed outcomes; pins are released through the specified durable lifecycle, never through a device presence timeout.
2. Specify which versions edits_since may query and how a checkpoint/GC floor invalidates older queries. Return HistoryUnavailable instead of silently producing an incorrect delta.
3. Make comment/chat anchor ingestion reject wrong document/epoch and preserve unresolved anchors for later causal resolution. Define anchor translation for epoch transitions before G06 can remove identities.
4. Implement W3 external durable pin registration/lookup/release keyed by stable consumer reference IDs, independent of author device lifetime. Specify idempotent replay, concurrent release and release-undo policy, recovery-before-GC, and the external canonical-log frontier/orphan protocol. A decoded or externally stored token does not register a pin. Preserve supported original token bytes through approved epoch translations. Document current-state reconstruction separately from isolated historical playback and HistoryUnavailable below the engine floor.

**Acceptance:**

- A durable comment on deleted text survives permitted undo and restart; stale ephemeral cursors do not pin history indefinitely.
- Querying a version older than the supported retention floor reports an explicit error.
- Tests cover externally stored reply tokens, durable pin recovery before GC, partial consumer-log/pin commits, release/replay, and retiring the author while its reply remains live. Pin obligations and translation requirements are executable inputs to G01; historical projections resolve against their own epoch/state, and live pins do not promise unlimited historical text.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_anchor_retention|tst_comment|tst_chat_message|tst_edits_since|tst_persistent_undo'`. Verify the W-series named cases assigned above at this task's implementation layer and record their evidence; later dependent cases remain pending.

## S06 — Boundedly enumerate sequence/undo histories against the model

**Status:** see TASKS.json (initially not started)  
**Depends on:** S05, V04  
**Coverage:** F7, F8, R4  

**Files / scope:**

- `libs/collabtext/tests/tst_sequence_model.cpp`
- `libs/collabtext/tests/reference/SequenceModel.h`
- `libs/collabtext/tests/fixtures/reduced-traces/`
- `docs/specs/vnext-external-position-contract.md`

**Steps:**

1. Enumerate 2–3 actors and small histories under all causally valid delivery orders, duplicates, deferred retry, forward/reverse insertion, deletion, undo/redo, and anchors.
2. Compare production visible content, resolved boundaries, operation coverage, and future edit behavior to the S02 interpreter. Reduce and retain every discrepancy.
3. Provide bounded CI limits and a longer deterministic campaign; clearly state that bounded enumeration is evidence, not a general proof.
4. Include W2 same-token boundary histories in differential enumeration, with encode/decode, restart, duplicate delivery, scoped empty/end values, and later-token comparisons. Compare exact resolved offsets and collapse outcomes as well as text; retain reduced counterexamples.

**Acceptance:**

- No mismatch in the checked domain; histories reproduce from artifacts and have exact resource/runtime bounds.
- New ordering/anchor regression traces are included permanently; model does not call production ordering helpers.
- Every checked W2 history agrees with the independent model for the same persisted token across delivery orders and restart, with exact domain/count/runtime limits.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_sequence_model|tst_sequence_semantics'` and a bounded ASan campaign; retain enumeration limits and counts. Verify the W-series named cases assigned above at this task's implementation layer and record their evidence; later dependent cases remain pending.

## S07 — Reintegrate safe fast paths with equivalence checks

**Status:** see TASKS.json (initially not started)  
**Depends on:** S06  
**Coverage:** F8, R6  

**Files / scope:**

- `libs/collabtext/src/crdt/Buffer.cpp`
- `libs/collabtext/src/crdt/IdList.cpp`
- `libs/collabtext/tests/tst_sequence_fast_paths.cpp`

**Steps:**

1. Re-enable one fast path per small change only when its preconditions are written next to the code. Keep the slow production path and model available for differential testing. Remove the obsolete first attempt and redundant accumulators in edits_since; split ordering/delta transformations into named helpers with explicit preconditions and update stale extract/rebuild comments. Reuse shared policy helpers where semantics match, keeping Buffer and IdList storage distinct.
2. For each path, exercise split boundaries, same-position concurrency, mixed Unicode, undo visibility, and origins retired from the local undo stack but retained globally.
3. Record fast/fallback counters so later performance work can measure which path is used rather than assuming historical frequency.

**Acceptance:**

- Fast and slow paths produce identical logical state and anchors on differential corpus.
- No blanket normalization bypass or altered comparator is accepted as a performance shortcut.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_sequence_fast_paths|tst_sequence_model|tst_fuzz|tst_idlist_fuzz'`; run Release benchmark smoke without speed assertions.
