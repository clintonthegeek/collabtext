# Markoff Core adoption lane

TASKS.json owns status and dependencies. These steps mirror its exact contract.

## M01 — Expose ordered full list insertion identities

**Depends on:** Q05

**Coverage:** R9, R14

**Files:**

- `libs/collabtext/include/collabtext/IdListEngine.h`
- `libs/collabtext/src/IdListEngine.cpp`
- `tests/consumer-smoke/markoff_core.cpp`

**Steps:**

1. Project visible native list entries to owned application_value/Identity values, preserving order and duplicate values. Reject invalid retained state and discarded views instead of silently omitting entries.
2. Add exact-identity element_of and try_anchor_of overloads; retain application-value first-match behavior and unchanged operation codecs. Test concurrency, removal/undo and fresh encoded replay.

**Acceptance:**

- Distinct insertion identities survive visibility changes and replay; ids() and wire bytes remain unchanged.
- Installed consumers need no private tree includes or identity truncation.

**Verification:**

Run tst_markoff_core and relocated consumer_markoff_core in Debug/Release/ASan.

## M02 — Specify and model terminal counter-gap application

**Depends on:** M01

**Coverage:** R9, R14

**Files:**

- `docs/specs/markoff-core-adoption.md`
- `tests/markoff-core/gap-model.cpp`

**Steps:**

1. Specify scoped host-authorized durable terminal records, full reserved intervals, causal prior, exact canonical retry, overlap rejection, bounded retention, deferred retry ownership and restart replay.
2. Run an independent interval/coverage model before production gap integration. Use runtime checks under Release.

**Acceptance:**

- Model rejects conflicting intervals and allocation reuse, and preserves deterministic replay across a reopen boundary.

**Verification:**

Build/run independent gap-model directly with -DNDEBUG and through tst_counter_gap_model.

**Design/decision gate:** The model must pass before M03; a codec or scalar counter raise is insufficient.

## M03 — Apply retained terminal intervals to scoped text and list coverage

**Depends on:** M02

**Coverage:** R9, R14

**Files:**

- `libs/collabtext/include/collabtext/CrdtEngine.h`
- `libs/collabtext/include/collabtext/IdListEngine.h`
- `libs/collabtext/src/CrdtEngine.cpp`
- `libs/collabtext/src/IdListEngine.cpp`
- `libs/collabtext/src/crdt/CounterGapLedger.h`
- `libs/collabtext/src/crdt/Buffer.h`
- `libs/collabtext/src/crdt/Buffer.cpp`
- `libs/collabtext/src/crdt/IdList.h`
- `libs/collabtext/src/crdt/IdList.cpp`
- `libs/collabtext/src/storage/CheckpointCodec.h`
- `libs/collabtext/src/storage/CheckpointCodec.cpp`
- `libs/collabtext/src/storage/CheckpointEngineCodec.cpp`
- `libs/collabtext/src/DocumentSession.cpp`
- `libs/collabtext/tests/tst_checkpoint_codec.cpp`
- `tests/consumer-smoke/markoff_core.cpp`

**Steps:**

1. Add scoped apply_counter_gap value and canonical-byte entry points. Reject wrong scope, malformed records, differing-prior/overlapping gaps, covered intervals and conflicting deferred/quarantined allocations without mutation.
2. Retain terminal intervals with 4096-record/8-MiB limits; reject operation allocation reuse before duplicate handling. Advance causal coverage and allocator observation only on success, retry document waits, and emit no gap document operation/history/publication callback.
3. Preserve gap evidence in checkpoint engine section 4; keep current no-gap sections 2/3. Validate restored gap coverage and retained identities, and carry the field through session checkpoint composition without adding session gap publication.
4. Test both stream kinds with durable ReplicaState reservation, definitively absent host bundle, synced scoped gap before later operation, reopened writer/peers, exact retry, conflicts, missing prior, callback behavior, malformed inputs, bounds and checkpoint round-trip.

**Acceptance:**

- Both peers and reopened engines accept later operations without placeholder edits or reused identities.
- Gap conflicts remain detectable after later coverage and checkpoints; missing prior is not acknowledged or silently retained.

**Verification:**

Run tst_markoff_core, tst_checkpoint_codec, affected operation/recovery/undo/callback suites, full Debug/Release/leak-enabled ASan and relocated live Core gates.

## M04 — Commit durable CTLO and terminal-gap readers with installed evidence

**Depends on:** M03

**Coverage:** R9, R14

**Files:**

- `libs/collabtext/include/collabtext/Serialization.h`
- `libs/collabtext/include/collabtext/ALPHA_CAPABILITIES.md`
- `tests/consumer-smoke/markoff_core.cpp`
- `tests/consumer-smoke/CMakeLists.txt`
- `libs/collabtext/CMakeLists.txt`
- `tools/verify_changed.py`

**Steps:**

1. Declare indefinite CTLO/0002 insert/remove/undo reader semantics from this adoption cut, plus CTGP/0001 terminal metadata needed for durable host replay. Keep historical experimental stores unsupported.
2. Retain independent literal CTLO insert/remove/undo fixtures, exact canonical round-trip, identity/visibility checks, future-version refusal and malformed prefixes. Register installed public-header Markoff trace and fail-closed verification routing.

**Acceptance:**

- An installed newer reader replays literal archives without rewriting host records or exposing payload builders.

**Verification:**

Run installed consumer_markoff_core in all three relocated package modes and verification tooling tests.

## M05 — Finalize Markoff response and new immutable candidate

**Depends on:** M04

**Coverage:** R9, R14

**Files:**

- `CMakeLists.txt`
- `libs/collabtext/CMakeLists.txt`
- `AGENTS.md`
- `README.md`
- `docs/README.md`
- `docs/USING_VNEXT.md`
- `docs/handoff/consumer-adoption.md`
- `docs/handoff/markoff-core-response.md`
- `docs/specs/markoff-core-adoption.md`
- `docs/specs/vnext-operation-codec.md`
- `docs/specs/vnext-protocol.md`
- `docs/roadmap/2026-10-reliability/DIRECTION.md`
- `docs/roadmap/2026-10-reliability/DECISIONS.md`
- `docs/roadmap/2026-10-reliability/EXECUTION.md`
- `docs/roadmap/2026-10-reliability/README.md`
- `docs/roadmap/2026-10-reliability/TASKS.json`
- `docs/roadmap/2026-10-reliability/10-markoff-core.md`
- `docs/reports/vnext-task-log.md`
- `docs/reports/2026-10-11-markoff-core-candidate.md`
- `tests/consumer-smoke/README.md`

**Steps:**

1. Bump package alpha.4 and capability manifest v16; settle all three asks and host document-key/stream naming obligations in a public response. Align current roadmap/specs without rewriting historical Wave evidence.
2. Run strict plan/scope/verifier checks, full correctness and installed gates. Run Markoff-owned installed probe read-only when available and distinguish its existing coverage from production adoption.
3. Commit source and evidence; create a new annotated collabtext-markoff-core-2026-10-11 tag without moving either earlier tag. Preserve external Q06/M06 gates; do not edit consumers, message, push, publish or merge.

**Acceptance:**

- All upstream asks have implemented public paths and executable evidence; external approval remains separate.

**Verification:**

Run validate-plan.py, check-scope.py --strict, tooling tests, full correctness, three relocated live package gates and available read-only Markoff probe.

## M06 — Record production Markoff acceptance on exact candidate

**Depends on:** M05

**Coverage:** R9, R14

**Files:**

- `docs/handoff/markoff-core-response.md`
- `docs/roadmap/2026-10-reliability/TASKS.json`

**Steps:**

1. Obtain Markoff-owned production BlockId/stream/routing/persistence evidence using full insertion identities and document keys, plus committed terminal gap recovery and retained CTLO archive acceptance on the exact candidate.
2. Record consumer revision, producer SHA, relocated prefix, commands and explicit acceptance. Require final Wave and Markoff same-candidate acceptance and engine checks before explicit user merge authorization.

**Acceptance:**

- No upstream-shaped fixture or unchanged older probe is substituted for production adoption approval.

**Verification:**

Consumer executes its production acceptance and supplies evidence; upstream does not certify it.

