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
- `tools/validate_sequence_fixtures.py`
- `tools/test_sequence_fixtures.py`
- `CMakeLists.txt`

**Steps:**

1. State exact properties: convergence, duplicate safety, causal integration, contiguous concurrent passages, intended left/right insertion affinity, selective local undo, and explicit behavior for concurrent replacements.
2. Create finite fixtures for whole-run paste, per-character forward typing, reverse typing, nested insertions, deleting a neighbor concurrently, undo and reinsert, and editing inside a merged run. Include Buffer and IdList ties; stable expected order derives from actor/operation order and the chosen algorithm.
3. Keep plain text and IdList only. Do not claim arbitrary semantic merge intent or maximal non-interleaving unless the selected algorithm/spec establishes it.
4. Add W2 fixtures named boundary_affinity_ab_insert_X, scoped_empty_start_end_tokens, shared_token_concurrent_boundary_inserts, and deleted_neighbors_explicit_collapse. Record the same minted token, its causal state, affinity, exact offsets, and later token comparisons; include all scalar boundaries and list lookup misses. Semantic paragraph recognition after arbitrary replacement is outside the contract.
5. Register tst_sequence_fixtures to validate the versioned fixture schema, causal graph and exact expected content/positions. Separate desired repaired-order fixtures from current-engine baseline observations; fixture validation does not require the legacy engine to pass F8. Hand boundary-only fixtures to S04a before the complete ordering algorithm is designed.

**Acceptance:**

- F8 fixture accepts only abcXYZ or XYZabc according to the specified tie rule, never aXbYcZ.
- Every example has operation IDs, causal edges, and exact expected content, not just equality between replicas.
- External-position fixtures specify exact left/right and collapse expectations, including empty/end boundaries; equality of separately minted tokens is insufficient.

**Verification:**

Run ./tools/verify-roadmap.sh debug tst_sequence_fixtures and review histories against the evaluation and original engine. No production algorithm change in this task. Record exact-content and W-series fixture review separately from later implementation evidence.

## S02 — Specify the ordering repair within the existing C++ engine

**Status:** complete; see TASKS.json, [algorithm](../specs/vnext-sequence-algorithm.md) and [design record](../reports/vnext-ordering-design.md)  
**Depends on:** S01, P06a, S04a\
**Coverage:** F8, R12  
**Design/decision gate:** Requires a complete reviewed incremental repair specification and independent executable model. Unresolved ordering invariants block dependent code changes; replacement engines are outside scope.  

**Files / scope:**

- `docs/specs/vnext-sequence-algorithm.md`
- `docs/reports/vnext-ordering-design.md`
- `libs/collabtext/tests/reference/SequenceModel.h`
- `docs/specs/vnext-external-position-contract.md`
- `docs/specs/vnext-boundary-semantics.md`
- `libs/collabtext/tests/reference/BoundaryModel.h`

**Steps:**

1. Trace each S01 failing history through existing Buffer/IdList insertion integration, origins, run splitting, Locator ordering, and undo visibility. Identify the first violated invariant and specify the smallest representation/integration repair that satisfies the complete fixtures. Retain current engine components and storage structures where their invariants hold.
2. Write changed operation fields (if any), integration pseudocode, tie-break rules, run splitting/identity, causal deferral, undo interaction, and conformance to the S04a boundary contract. Include a current→repaired transformation map with exact helper/file scope and justify each changed invariant. Existing origin-neighbor algorithms may inform the repair; do not implement a replacement CRDT/backend.
3. Implement a deliberately simple independent vector/reference interpreter for the repaired semantics. Validate the full fixture and bounded-history corpus. If it fails, record the smallest counterexample and refine this repair specification; do not launch a Rust/backend experiment or ask a routine executor to invent missing rules.
4. Compose the independent ordering interpreter with the S04a boundary model: supplied item orders must be generated by the repaired ordering interpreter, while boundary resolution remains independent of production. Cover scoped empty/start/end values, same-token concurrent inserts under every valid delivery order, deleted-neighbor collapse reporting, later comparison, and distinct UTF-8 byte/list-index units. Review compatibility with supported alpha token bytes before unlocking S03; no file-store recovery prerequisite applies to this design gate.

**Acceptance:**

- The repair includes implementable pseudocode, a transformation map within current components, and an independent reference interpreter passing S01.
- Technical review checks nested/concurrent runs, reverse insertion, post-merge edits, undo, and reuse of existing storage. A superficial comparator tweak or an incomplete specification does not unlock S03.
- The independent interpreter plus BoundaryModel passes all W2 named fixtures and preserves the specified stable identities and token meaning. Missing ordering rules or a supported-token incompatibility keep S03 closed; S04a/S04b live work has its own earlier gate.

**Verification:**

Run the reference interpreter on all S01 fixtures and bounded enumerations; retain exact traces and implementation-independent expected outputs. Verify the W-series named cases assigned above at this task's implementation layer and record their evidence; later dependent cases remain pending.

## S03 — Apply the ordering repair to existing Buffer and IdList paths

**Status:** see TASKS.json (initially not started)  
**Depends on:** S02, P06b, S04b\
**Coverage:** F8  

**Files / scope:**

- `libs/collabtext/src/crdt/Buffer.cpp`
- `libs/collabtext/src/crdt/IdList.cpp`
- `libs/collabtext/src/crdt/SequenceOrder.cpp`
- `libs/collabtext/tests/tst_sequence_semantics.cpp`
- `docs/specs/vnext-boundary-semantics.md`
- `libs/collabtext/tests/fixtures/external-position/`
- `tests/consumer-smoke/wave-contract/`

**Steps:**

1. Apply the S02 transformation map to current Buffer/IdList integration helpers. Use their existing general/slow paths as the initial correctness baseline; preserve logical identities and run compression. Do not create a new production engine alongside them. Production decisions must match the independent reference interpreter.
2. Adjust existing shared-locator order assumptions and split relocations only where the reviewed repair requires it. Keep unaffected storage/tree/undo behavior; update codecs/schema fixtures for explicitly changed fields and reject obsolete formats.
3. Disable old fast paths when their invariants no longer apply; do not optimize this task. Remove the F8 expected-fail marker only after all exact-content fixtures pass.
4. Rerun W03's installed live-position strict set and the supported-token golden fixtures after the ordering repair. Preserve the meaning/readability of any approved published token version; if operation fields change, update their versioned codecs. Retain original token bytes and add reduced post-merge/undo cases rather than reminting tokens to hide a discrepancy.

**Acceptance:**

- Production Buffer and IdList match the reference model on every S01 history and delivery permutation.
- All existing valid local editing, duplicate, undo, and convergence tests pass or have explicitly justified semantic expectation updates.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_sequence_semantics|tst_convergence|tst_idlist_convergence|tst_evaluation_regressions'` plus seeded differential traces under ASan.

## S04a — Specify scoped boundary identities and an independent position model

**Status:** see TASKS.json (initially not started)\
**Depends on:** S01, P06a, W01\
**Coverage:** F7, R2, R4, R13\
**Design/decision gate:** A complete boundary specification, implementation map, and independent executable model must pass before S04b. An API value declaration or comparator test alone does not satisfy this gate.\

**Files / scope:**

- `docs/specs/vnext-boundary-semantics.md`
- `docs/specs/vnext-public-api.md`
- `docs/specs/vnext-sequence-semantics.md`
- `docs/specs/vnext-alpha-capabilities.md`
- `libs/collabtext/tests/reference/BoundaryModel.h`
- `libs/collabtext/tests/tst_boundary_model.cpp`
- `libs/collabtext/tests/fixtures/external-position/`
- `docs/specs/vnext-external-position-contract.md`

- `libs/collabtext/CMakeLists.txt`
- `tools/validate-consumer-fixtures.py`
- `libs/collabtext/tests/reference/BoundaryApiContract.h`
- `docs/specs/vnext-consumer-probes.md`

- `tools/verify_changed.py`
- `tools/test_verify_changed.py`

**Steps:**

1. Trace current anchor_at/resolve_anchor and IdList lookup through origin intervals, fragment splitting, visibility/UndoMap changes, sentinel handling, and ignored bias. Record the exact unsafe helpers and a narrow implementation map for S04b. Use the P06a operation/item allocation domains and per-stream context; never mint a token from a transient fragment pointer, locator, local actor slot, or visible integer offset alone.
2. Specify opaque text boundary and separate list-element identity tokens, scoped by document/stream/primitive/epoch. Define sufficient immutable item/neighborhood/causal context for both affinities under the S01 concurrent histories. Left/right neighbors alone are not accepted without model evidence. Give creation/resolution pseudocode, sentinel-free empty/start/end representation, and checked limits for context and encoded size.
3. Specify typed outcomes with actual UTF-8 byte offsets or list indices, plus an explicit collapse/deletion condition. Distinguish invalid input, foreign context/epoch, unsupported token version, awaiting causal state, and unavailable referent/history. State the strict attachment interpretation separately from a caret boundary; list lookup misses never become valid end positions.
4. Define boundary-token versioning and a bounded canonical encoding independent of the operation codec. Specify truncation/trailing-data rejection, exact enum/context validation, and no mutation or implicit pin on decode/resolve. Define token comparison as a checked operation in a specified document state; do not promise a history-independent ordering of separately minted tokens.
5. Build an independent ordered-item/visibility reference model for mint/resolve/compare. Fixtures supply exact item order, causal edit groups and visibility states, including both current and intended repaired concurrent-paste orders; the model does not reproduce the complete S02 integration algorithm. Enumerate bounded two/three-actor boundary histories over those supplied states with insertion, deletion, local undo/redo, empty/end boundaries, duplicates and missing causal state. Compare the same minted token across equivalent replica states without calling production anchor/codec helpers.
6. Review that the representation can express all S01 boundary histories and the intended repaired ordering, not just current interleaving behavior. S02 must consume this boundary contract and preserve its item identities and token meaning; it retains responsibility for the complete ordering algorithm/model. If a history needs more boundary context, retain the reduced counterexample and revise this gate before S04b.
7. Update the API guide with compilable target signatures and exact result/ownership/offset rules for S04b. Record which obligations are live/model evidence and which remain S04 recovery certification, S05 pins, or G-series GC. No storage milestone or full ordering implementation is required for this specification/model gate.

**Acceptance:**

- A reviewed specification includes implementable mint/resolve/compare pseudocode, sufficient stable context, typed outcomes, canonical token codec rules, and a transformation map inside existing engine helpers.
- The independent model passes exact WP1-WP4/WP7 expectations and concurrent same-token fixtures with documented bounds, seeds, and reduced traces. Mutations that ignore affinity, erase scope, or map missing IDs to end are detected.
- The model separates strict content attachment loss from valid caret collapse, and its chosen token meaning is an explicit input to S02 rather than an accidental property of current locator ordering.

**Verification:**

Run ./tools/verify-roadmap.sh debug tst_boundary_model and Release/ASan equivalents; run bounded histories twice with the same seed, retain counts/traces and the three mutation checks, and review the implementation map before unlocking S04b.

## S04b — Implement live scoped positions and their public token codec

**Status:** see TASKS.json (initially not started)\
**Depends on:** S04a, A02a, P06b\
**Coverage:** F7, R2, R13\

**Files / scope:**

- `libs/collabtext/include/collabtext/Anchor.h`
- `libs/collabtext/src/crdt/Anchor.h`
- `libs/collabtext/src/crdt/Buffer.cpp`
- `libs/collabtext/src/crdt/IdList.cpp`
- `libs/collabtext/src/codec/AnchorCodec.cpp`
- `libs/collabtext/tests/tst_anchor.cpp`
- `libs/collabtext/tests/tst_anchor_codec.cpp`
- `libs/collabtext/tests/tst_idlist_anchor.cpp`
- `docs/specs/vnext-boundary-semantics.md`
- `docs/specs/vnext-public-api.md`
- `docs/specs/vnext-alpha-capabilities.md`
- `docs/specs/vnext-external-position-contract.md`

- `libs/collabtext/include/collabtext/ActorId.h`
- `libs/collabtext/include/collabtext/CrdtEngine.h`
- `libs/collabtext/include/collabtext/IdListEngine.h`
- `libs/collabtext/src/CrdtEngine.cpp`
- `libs/collabtext/src/IdListEngine.cpp`
- `libs/collabtext/tests/reference/BoundaryApiContract.h`
- `libs/collabtext/CMakeLists.txt`

- `libs/collabtext/src/identity/ReplicaState.cpp`

- `libs/collabtext/src/crdt/Buffer.h`
- `libs/collabtext/src/crdt/IdList.h`
- `libs/collabtext/src/crdt/PositionPayload.h`
- `libs/collabtext/src/crdt/PositionTraversal.h`
- `libs/collabtext/src/crdt/PositionTraversal.cpp`
- `libs/collabtext/tests/tst_positions.cpp`
- `libs/collabtext/tests/position-replay/main.cpp`
- `libs/collabtext/tests/public-headers/only_Anchor_h.cpp`
- `libs/collabtext/include/collabtext/ALPHA_CAPABILITIES.md`
- `docs/USING_VNEXT.md`

- `tests/consumer-smoke/main.cpp`
- `libs/collabtext/tests/tst_operation_values.cpp` (qualify the private payload namespace after adding public position types)

**Steps:**

1. Implement the reviewed S04a boundary representation and helpers within the existing Buffer/IdList engines. Start with a straightforward checked traversal; preserve item identities through split/deletion/undo and avoid adding a second production sequence engine or an optimization project.
2. Expose checked mint/resolve/compare and library-owned encode/decode through the A02a public facades. Bind document/stream/primitive/epoch on construction and reject foreign scope before looking up an item. Replace unchecked public paths that can produce an end sentinel, assert, or fabricate a plausible offset on a miss.
3. Implement exact affinity at insertion boundaries, scoped empty/start/end boundaries, UTF-8 scalar-boundary validation, distinct list-element lookup, and explicit collapsed versus unavailable outcomes. Use S04a rules for missing causal data and for undo restoration. Queries and rejection must leave text, version, undo state and notifications unchanged.
4. Implement the bounded independently versioned token codec using checked utilities. Test canonical byte stability, malformed/unknown enum or version/context, truncation at every field boundary, and all 256 trailing one-byte suffixes; decoding creates no pin. Publish the token-format manifest and supported-reader rule in the API guide, independent of C++ object layout.
5. Implement a child-process token test: process A creates context and edits, encodes actual P06b operations and a token; process B constructs a fresh engine from the same seed/context, replays operations, decodes the same bytes, applies remote edits and resolves. This establishes codec/replay evidence, not full file-store crash recovery. Also resolve one minted token in two independently replayed replicas.
6. Differentially test production positions against BoundaryModel over bounded histories using the actual logical item order/visibility as input; separately check fixed hand-written ordering/position fixtures. Do not treat current interleaving as intended repaired content, or use an anchor-only comparison to close F8. Update the alpha manifest only for passed live/token cases. Preserve F7 retained-position/recovery obligations until S04/S05/G-series evidence exists; do not mark ordering, durable pins or persistent undo complete.

**Acceptance:**

- The original boundary in ab becomes offsets 1/2 for Left/Right after insertion of X; all scoped empty/start/end, Unicode, deletion/undo, wrong-object, and missing-list cases match the independent model.
- Foreign tokens and malformed inputs return typed failures without assert/abort, fake offsets, mutation or notifications; token bytes round-trip in a fresh-process replay and on a second replica.
- The public installed surface can expose actual implemented positions; S03/L08 are not prerequisites. Full repaired-order/recovery certification remains S04 and durable retention remains S05.

**Verification:**

Run ./tools/verify-changed.sh --target "^tst_(anchor|anchor_codec|positions|idlist_anchor|boundary_model|convergence|idlist_convergence)$" plus Release and a bounded ASan differential campaign. Run the fresh-process replay worker and record exact token bytes/fixture hashes; W03 separately certifies the installed adapter.

## S04 — Certify boundary anchors across repaired ordering and durable recovery

**Status:** see TASKS.json (initially not started)  
**Depends on:** S04b, S03, L08, W03\
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
- `docs/specs/vnext-boundary-semantics.md`
- `libs/collabtext/tests/tst_session_recovery.cpp`
- `docs/reports/vnext-boundary-certification.md`

**Steps:**

1. Treat S04a/S04b as the implemented live/token baseline rather than implementing a second anchor system. Review exact boundary interpretation against S02/S03 repaired ordering and every supported alpha token fixture; preserve stable causal identities through splitting, local/remote edits, deletion and undo/redo.
2. Extend token round-trip from S04b's fresh-process operation replay to the actual L08 file/session lifecycle: close/reopen, own/remote log replay, sender-only and receiver-only restart, duplicate delivery, delayed causal input and failed durable writes. Reuse the same originally minted token bytes in all processes.
3. Test typed distinction between malformed/foreign/unsupported tokens, temporarily missing causal state, collapsed boundaries and unavailable history. Text results carry scalar-aligned UTF-8 byte offsets; list results carry indices. State remains unchanged on rejection and missing list IDs never fabricate an end anchor.
4. Rerun the installed strict live-position probes plus repaired-order WP6 using actual durable replay. Publish supported token-version readers and canonical golden fixtures; no pre-vNext anchor conversion is required. S05 still owns durable pin lifecycles and G-series tasks own checkpoint/GC translation.
5. Record the certification report with exact named cases, modes, fixture/token hashes, supported reader versions and remaining retention limits. Remove only F7 anchor markers whose full acceptance now passes; do not close retained-position/GC obligations based solely on live tests.

**Acceptance:**

- All S04b exact affinity/scope/Unicode/deletion cases pass after S03, and the same original supported bytes resolve identically in recovered equivalent replica states.
- Actual close/reopen/restart/fault cases pass with explicit incomplete/unavailable results where causal recovery is incomplete; codec-only replay is not substituted for durability evidence.
- external_token_roundtrip_reopen, boundary_affinity_ab_insert_X, scoped_empty_start_end_tokens, shared_token_concurrent_boundary_inserts, deleted_neighbors_explicit_collapse, foreign_or_unsupported_token_is_explicit and missing_list_id_is_not_an_end_anchor have exact executable evidence; pin/checkpoint/GC cases remain visibly assigned to later tasks.

**Verification:**

Run ./tools/verify-roadmap.sh debug "tst_anchor_codec|tst_anchor|tst_idlist_anchor|tst_sequence_semantics|tst_session_recovery|tst_evaluation_regressions" plus Release and bounded ASan restart/differential cases; rerun installed strict live-position and repaired-order probes. Record named evidence and supported-token hashes in the certification report.

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
5. Keep registration/lookup/release and consumer frontier projection available through headless public APIs with consumer-supplied persistence hooks; DocumentSession is an optional reference owner. External canonical-log users must not be forced to adopt the file transport, engine-owned block tree or per-session history as their cross-block undo manager. Add WP8 pin/retirement/recovery cases and expose installed assertions before final A06 certification.

**Acceptance:**

- A durable comment on deleted text survives permitted undo and restart; stale ephemeral cursors do not pin history indefinitely.
- Querying a version older than the supported retention floor reports an explicit error.
- Tests cover externally stored reply tokens, durable pin recovery before GC, partial consumer-log/pin commits, release/replay, and retiring the author while its reply remains live. Pin obligations and translation requirements are executable inputs to G01; historical projections resolve against their own epoch/state, and live pins do not promise unlimited historical text.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_anchor_retention|tst_comment|tst_chat_message|tst_edits_since|tst_persistent_undo'`. Verify the W-series named cases assigned above at this task's implementation layer and record their evidence; later dependent cases remain pending.

## S06 — Boundedly enumerate sequence/undo histories against the model

**Status:** see TASKS.json (initially not started)  
**Depends on:** S05, S03, V04\
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
