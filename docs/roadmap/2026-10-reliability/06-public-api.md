# 06. Supported public library and Qt integration

Goal: consumers can install a framework-independent engine, optional file transport, and a complete Qt document binding. All porting work remains on the isolated branch. Preserve familiar names only where doing so does not conceal changed semantics.

Read [execution rules](EXECUTION.md) and [decisions](DECISIONS.md) before starting. Task dependencies refer to the [master task registry](TASKS.json). New file paths below are intentional; do not assume they already exist.

## A01 — Split CMake targets and remove runtime test dependencies

**Status:** see TASKS.json (initially not started)  
**Depends on:** P06, L08  
**Coverage:** R2, R7, R13  

**Files / scope:**

- `CMakeLists.txt`
- `libs/collabtext/CMakeLists.txt`
- `app/CMakeLists.txt`
- `libs/collabtext/src/crdt/SegmentFormat.cpp`
- `tests/core-smoke/`

**Steps:**

1. Introduce CollabText::Core (C++20 only), CollabText::FileTransport (zstd plus portable SHA-256 provider), CollabText::QtAdapter, CollabText::Widgets, and test-only support. Keep NetworkSim/QtTest out of production archives.
2. Use explicit COLLABTEXT_BUILD_TESTS, COLLABTEXT_BUILD_QT, COLLABTEXT_BUILD_APPS, and COLLABTEXT_BUILD_FILE_TRANSPORT options. Core-only configure must not search for Qt or zstd.
3. Record the SHA-256 implementation/dependency and license; default portable file-transport provider can use OpenSSL::Crypto, leaving Core dependency-free. Remove QCryptographicHash from mandatory transport code.

**Acceptance:**

- Core-only configure/build works with Qt package discovery disabled; FileTransport works without Qt; Qt targets are optional.
- Runtime linkage contains no QtTest. Existing applications/tests link only the targets they actually use.

**Verification:**

Configure all-option and core-only builds; use a plain C++ core smoke executable without QtTest. Run the fast suite on the full configuration.

## A02 — Establish an installed public header boundary

**Status:** see TASKS.json (initially not started)  
**Depends on:** A01, P07, S04  
**Coverage:** R2, R13  

**Files / scope:**

- `libs/collabtext/include/collabtext/`
- `libs/collabtext/src/crdt/`
- `tests/consumer-smoke/`
- `docs/specs/vnext-public-api.md`

**Steps:**

1. Move stable public ActorId, OperationId, Anchor, outcome/config types, and engine APIs into self-contained public headers. Keep fragments, locators, raw variants, Global/tree layouts, and writers private.
2. Ensure Buffer and IdList consumer entry points remain accessible through documented facades, including anchors, undo limits, batch apply, and session integration. A facade must not silently remove required capabilities.
3. Remove src from PUBLIC include paths. Public tests compile with only installed headers and libraries, not source-tree includes inherited from the target.

**Acceptance:**

- Every public header compiles independently; no public include references crdt/ or src internals.
- Consumer smoke exercises Buffer/IdList equivalent facades and an external OpStream implementation using stable types only.

**Verification:**

Compile each installed header in a separate translation unit and run the external consumer sample. Inspect compiler include paths for accidental source-tree exposure.

## A03 — Implement a single complete Qt document adapter

**Status:** see TASKS.json (initially not started)  
**Depends on:** A02, L07, S05  
**Coverage:** R3, R8, R9  

**Files / scope:**

- `libs/collabtext/include/collabtext/CollabDocument.h`
- `libs/collabtext/src/CollabDocument.cpp`
- `libs/collabtext/src/CrdtEngine.cpp`
- `libs/collabtext/src/SyncManager.cpp`
- `libs/collabtext/tests/tst_document_adapter.cpp`

**Steps:**

1. Bind all supported QTextDocument mutation paths to DocumentSession, and apply engine deltas back using scoped suppression and edit blocks. Handle typing, paste, IME, programmatic insert/delete, selection replacements, undo/redo, and remote changes.
2. Use one engine-owned undo path; preserve cursor/scroll selections with stable anchors. Remove stale serialization-pending stubs and signal-only pseudo-bindings.
3. Make SyncManager a documented presence/transport composition helper or deprecate it with a precise replacement sample. Route dogfood CollabPane through this supported adapter while preserving widgets.

**Acceptance:**

- Direct document edits and remote engine operations stay synchronized without feedback loops or double operations.
- IME/non-BMP input, multi-cursor edits, undo/redo, and surgical scroll behavior are covered by real adapter tests.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_document_adapter|tst_editor_recovery|tst_multicursor|tst_scroll_stability'`; include QInputMethodEvent and programmatic QTextCursor paths.

## A04 — Implement batch outcomes, subscriptions, and safe notification policy

**Status:** see TASKS.json (initially not started)  
**Depends on:** A03, P05  
**Coverage:** R9  

**Files / scope:**

- `libs/collabtext/include/collabtext/CrdtEngine.h`
- `libs/collabtext/include/collabtext/DocumentSession.h`
- `libs/collabtext/src/DocumentSession.cpp`
- `libs/collabtext/tests/tst_callbacks.cpp`

**Steps:**

1. Provide batch remote application outcomes and one visible-change notification per applied batch, with exact deltas. Duplicate/deferred/rejected operations do not claim visible change.
2. Use scoped subscriptions/unsubscribe handles with defined lifetime; reject or queue reentrant mutations according to P01. Do not allow callbacks to outlive their session.
3. Expose single-owner thread rules and bounded pending byte/count queues. Return actionable backpressure rather than abandoning accepted records.

**Acceptance:**

- Callback order/count and invalid/deferred behavior match P01; remote ops never echo as local ops.
- Destroyed/unsubscribed listeners are not called; reentrant mutation has tested deterministic behavior.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_callbacks|tst_document_adapter|tst_transport_commit_contract'`; ASan verifies listener teardown/lifetime cases.

## A05 — Define local_clear and other reset semantics across epochs

**Status:** see TASKS.json (initially not started)  
**Depends on:** A02, G05  
**Coverage:** R14, R2  

**Files / scope:**

- `libs/collabtext/src/crdt/IdList.cpp`
- `libs/collabtext/include/collabtext/IdList.h`
- `libs/collabtext/tests/tst_reset_semantics.cpp`
- `docs/specs/vnext-public-api.md`

**Steps:**

1. Preserve legacy local_clear in migration/compatibility documentation as local-only state discard with version retention. Do not use it for history replay or a collaborative reset.
2. Expose an explicitly named local-view discard only if consumers need it; collaborative clear produces operations, while replacing a store/checkpoint uses an explicit epoch-aware recovery operation.
3. Document which callbacks, identity, clocks, anchors, deferred input, and undo groups survive each action. Reject replay into a cleared engine whose preserved coverage would deduplicate required operations.

**Acceptance:**

- Consumer reset examples are unambiguous; a local clear cannot masquerade as a synced document reset.
- Replay/restoration uses fresh recovery state or valid checkpoint import, never preserved-version local_clear.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_reset_semantics|tst_idlist|tst_session_recovery'`; compile a port of the V08 local_clear consumer sample.

## A06 — Install and export the library package with migration adapters

**Status:** see TASKS.json (initially not started)  
**Depends on:** A04, A05  
**Coverage:** R2, R13  

**Files / scope:**

- `cmake/CollabTextConfig.cmake.in`
- `libs/collabtext/CMakeLists.txt`
- `tests/consumer-smoke/CMakeLists.txt`
- `docs/MIGRATING_TO_VNEXT.md`

**Steps:**

1. Implement install/export/version package metadata, component dependency discovery, stable target names, and separate public include installs. Test a relocated installation prefix.
2. Document old→new headers, types, signatures, callback/ack semantics, error paths, ownership, offsets, and reset APIs; include Buffer and IdList plus custom transport porting examples.
3. Provide limited deprecated adapters only where safe; old scalar GC wrappers cannot create a certificate, and old void push cannot pretend commit success. Mark source and ABI compatibility separately.

**Acceptance:**

- An independent project finds CollabText through find_package and links without source-tree paths.
- Migration guide maps every V08 item and has compilable before/after samples; no same-store mixed-version promise unless D01 explicitly selects and implements it.

**Verification:**

Install to a temporary prefix, hide original include tree, relocate prefix, and configure/build/run external samples for Core, FileTransport, and QtAdapter.

## A07 — Add compiler/platform/package verification in CI

**Status:** see TASKS.json (initially not started)  
**Depends on:** A06, P08, V04  
**Coverage:** R4, R13  

**Files / scope:**

- `.github/workflows/ci.yml`
- `tools/verify-roadmap.sh`
- `docs/specs/vnext-platform-support.md`

**Steps:**

1. Publish support tier: Linux production verification first; Windows/macOS build and functional durability coverage where runners are available. Do not claim verified power-loss guarantees without evidence.
2. Run core-only builds without Qt, full Linux Debug/Release suites, bounded seeded fuzz/model tests, ASan/UBSan on supported compilers, and install-consumer checks.
3. Keep slow campaigns/benchmarks in scheduled or manually triggered jobs; failures retain traces and do not become successful skips. Record any unavailable platform as unsupported/unverified.

**Acceptance:**

- CI catches accidental Qt dependencies, public-header leakage, sanitizer failures, and package regressions.
- Platform-specific durable-file implementations compile/test on their claimed platforms; POSIX APIs no longer leak into unconditional builds.

**Verification:**

Validate workflow syntax locally; run matching Linux jobs locally; require first remote CI results before E06 declares platform support.
