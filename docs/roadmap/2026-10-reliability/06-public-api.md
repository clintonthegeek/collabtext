# 06. Supported public library and Qt integration

Goal: consumers can install a framework-independent engine, optional file transport, and a complete Qt document binding. All work remains on the isolated branch. Design the new API freely; obsolete wrappers need no compatibility shims. Include explicit device-retirement controls and advisory reminders.

Read [execution rules](EXECUTION.md) and [decisions](DECISIONS.md) before starting. Task dependencies refer to the [master task registry](TASKS.json). New file paths below are intentional; do not assume they already exist.

A02a/A06a establish the installed evolving alpha boundary early; A02/A06 later
complete supported API certification. Follow registry order and the
[consumer lane](00-consumer-integration.md), not phase order.

## A01 — Split CMake targets and remove runtime test dependencies

**Status:** see TASKS.json (initially not started)  
**Depends on:** P01, V08\
**Coverage:** R2, R7, R13  

**Files / scope:**

- `CMakeLists.txt`
- `libs/collabtext/CMakeLists.txt`
- `app/CMakeLists.txt`
- `libs/collabtext/src/crdt/SegmentFormat.cpp`
- `tests/core-smoke/`
- `tools/verify-roadmap.sh`
- `docs/specs/vnext-public-api.md`

**Steps:**

1. Inventory each source file and assign it to Core, FileTransport, QtAdapter, Widgets, or test-only support. Core owns existing Buffer/IdList/tree/undo/identity/value/codec components with C++20-only dependencies; FileTransport owns file sync/segments/compression and durable-store composition. Qt presence/identity UI stays outside Core. Record the target graph before moving source entries, keeping production algorithms unchanged. Trace ReplicaState's DurableFile dependencies explicitly: portable low-level lease/write helpers required by identity ownership must remain in a Qt/compression-free common or Core support unit, so FileTransport depends on Core without Core linking back to FileTransport. Expose the borrowed allocator contract; primitive construction must not automatically open a transport/session.
2. Introduce real component targets exported as CollabText::Core, CollabText::FileTransport, CollabText::QtAdapter and CollabText::Widgets. Make dependencies one-way: optional integrations depend on Core, Core never depends on them. Keep NetworkSim, trace/fault workers and QtTest in test support; give internal tests explicit PRIVATE include paths and link only their required targets.
3. Add COLLABTEXT_BUILD_TESTS, COLLABTEXT_BUILD_QT, COLLABTEXT_BUILD_APPS and COLLABTEXT_BUILD_FILE_TRANSPORT options at the top-level before dependency discovery. Gate app subdirectories, tools and automoc appropriately; QtTest is found only for enabled Qt tests. A core-only configuration must not call find_package for Qt, zstd or a hash provider.
4. Remove QCryptographicHash from portable file-format hashing without changing the currently supported hash algorithm/content identity. Use a documented portable provider such as OpenSSL::Crypto in FileTransport and record its license; compression/hash dependencies belong to FileTransport and must later export correctly for static consumers. Verify known hash vectors and existing segment fixtures.
5. Retarget the dogfood apps, existing Qt wrappers, test executables and verification runner using the new components. Keep producer builds working at every commit and keep existing public source includes private wherever possible; A02a completes facade isolation. Do not silently omit a previously selected test or break the runner's zero-match/sanitizer checks.
6. Create a plain C++ core-smoke executable without QtTest. Configure/build core-only with Qt6, zstd and OpenSSL package discovery explicitly disabled; separately build FileTransport without Qt and the full app/test configuration. This target split needs no recovery, operation-codec or GC milestone and must not wait for L08.

**Acceptance:**

- Core-only configures/builds/runs its plain C++ smoke with Qt6, zstd and OpenSSL discovery disabled. Portable FileTransport builds without Qt; Qt adapter/widgets/apps remain optional.
- Production targets and their exported link interfaces contain no QtTest/NetworkSim/test workers. Core has no mandatory file transport, Qt, event-log or document-session ownership.
- Full builds preserve the current test selection and dogfood linkage; affected segment hash/codec fixtures retain their expected identity and the verification runner still detects instrumentation and zero matches.

**Verification:**

Run core-only and FileTransport-without-Qt configure/build/smoke checks plus the full Debug fast suite and targeted hash/segment fixtures. Inspect target include/link interfaces, test selection and core compile commands; use CMAKE_DISABLE_FIND_PACKAGE_Qt6/zstd/OpenSSL=ON for the Core negative-discovery configuration.

## A02a — Expose the implemented alpha API through self-contained public headers

**Status:** see TASKS.json (initially not started)\
**Depends on:** A01\
**Coverage:** R2, R13\

**Files / scope:**

- `libs/collabtext/include/collabtext/`
- `libs/collabtext/src/crdt/`
- `tests/consumer-smoke/`
- `docs/specs/vnext-public-api.md`
- `docs/specs/vnext-alpha-capabilities.md`
- `docs/specs/vnext-external-position-contract.md`

**Steps:**

1. Inventory the types and callable behavior actually implemented at the task baseline. The normal serial route finishes the already-running P06a before this task, but packaging has no hard identity/codec/recovery milestone prerequisite. Update the API guide with an implementation table: compiled alpha surface, target-only signatures, current identity/codec limits, and future feature-owning task IDs.
2. Expose working Buffer and IdList construction, local text/list edits, reads, operation observation, validated remote apply, and available local undo/redo through public facades over the existing engines. Use opaque implementation and operation handles with out-of-line destruction/move definitions; keep Lamport/Global/Locator/Fragment/SumTree layouts and raw variants in private headers. Follow P01 ownership/result contracts where implemented; document any genuinely unfinished target API instead of inventing successful behavior.
3. Keep implemented public identity/result/configuration types self-contained and framework-independent. For any capability not yet implemented, either omit that entry point from the advertised alpha surface or return the documented typed unsupported result; do not return fake offsets, empty valid tokens, success-only stubs, or silent ActorId-to-slot conversions. Do not create a replacement engine or compatibility wrappers for obsolete source APIs.
4. Remove src from PUBLIC/INTERFACE include directories. Give in-tree engine tests private internal paths explicitly; consumer tests get only the public include tree. Keep Qt-specific headers out of Core and place optional adapter/widget headers in their component surface.
5. Compile every advertised public header as the first and only library header in its own translation unit. Build a small plain C++ consumer that edits text and list content and transfers actual available operation values between two independent engines; record unsupported codecs/transport/anchors honestly. A06a turns this source-boundary check into the installed check.
6. Create a versioned alpha capability manifest with separate facts for identity adoption, operation codec, live positions, token codec, crash recovery, persistent undo/redo, pins, ordering repair, and certified GC. Tie every true capability to an executable assertion; later tasks update both manifest and tests.

**Acceptance:**

- Every advertised public header compiles independently without crdt/ or src includes; public include/link interfaces expose no engine storage layout or QtTest.
- The actual Buffer/IdList alpha consumer can create and edit primitives using public types and receive/apply operations without inspecting fields. Missing features are explicit in the manifest and usage guide.
- A02a completes only the available alpha boundary. P07, S04b, A02 and A06 retain responsibility for their additional implemented capabilities and certification.

**Verification:**

Compile the individual public-header translation units and the plain C++ consumer with internal includes disabled; run affected engine/public serialization suites in Debug. Inspect exported include/link properties and record the capability manifest with named passing cases.

## A02 — Complete and certify the supported public API boundary

**Status:** see TASKS.json (initially not started)  
**Depends on:** A02a, P07, S04\
**Coverage:** R2, R13  

**Files / scope:**

- `libs/collabtext/include/collabtext/`
- `libs/collabtext/src/crdt/`
- `tests/consumer-smoke/`
- `docs/specs/vnext-public-api.md`
- `docs/specs/vnext-external-position-contract.md`

**Steps:**

1. Extend the already installed A02a boundary with all now-implemented P07/S04 operation/context/outcome/anchor/transport APIs. Compile the complete P01 target examples and update the supported capability table; keep fragments, locators, raw variants, Global/tree layouts and writers private. Do not redo the packaging split or add obsolete compatibility wrappers.
2. Ensure Buffer and IdList consumer entry points remain accessible through documented facades, including anchors, undo limits, batch apply, and session integration. A facade must not silently remove required capabilities.
3. Keep src absent from PUBLIC/INTERFACE include paths and preserve the A06a export/relocation tests. Compile each complete public header independently and run public tests solely against installed headers/libraries; extend the alpha smoke rather than replacing it with source-tree tests.
4. Expose W1/W2 opaque token encode/decode, checked mint/lookup, and typed text/list resolution in self-contained installed headers. Publish supported token-version handling and exact offset units; keep consumer outer envelopes independent of internal object layout. Add foreign/version/missing-ID installed tests.

**Acceptance:**

- Every public header compiles independently; no public include references crdt/ or src internals.
- Consumer smoke exercises Buffer/IdList equivalent facades and an external OpStream implementation using stable types only.
- Installed clients can persist and resolve tokens without any internal headers or layout assumptions, and distinguish unresolved/foreign/unsupported outcomes from a real end position.

**Verification:**

Compile each installed header in a separate translation unit and run the external consumer sample. Inspect compiler include paths for accidental source-tree exposure. Verify the W-series named cases assigned above at this task's implementation layer and record their evidence; later dependent cases remain pending.

## A03 — Implement a single complete Qt document adapter

**Status:** complete
**Depends on:** A02, L07, S05  
**Coverage:** R3, R8, R9  

**Files / scope:**

- `libs/collabtext/include/collabtext/CollabDocument.h`
- `libs/collabtext/src/CollabDocument.cpp`
- `libs/collabtext/src/CrdtEngine.cpp`
- `libs/collabtext/src/SyncManager.cpp`
- `libs/collabtext/tests/tst_document_adapter.cpp`
- `libs/collabtext/tests/tst_editor_recovery.cpp`
- `libs/collabtext/tests/tst_multicursor.cpp`
- `libs/collabtext/tests/tst_scroll_stability.cpp`
- `libs/collabtext/CMakeLists.txt`
- `app/collabedit/CollabPane.h`
- `app/collabedit/CollabPane.cpp`
- `docs/specs/vnext-public-api.md`

**Steps:**

1. Bind all supported QTextDocument mutation paths to DocumentSession, and apply engine deltas back using scoped suppression and edit blocks. Handle typing, paste, IME, programmatic insert/delete, selection replacements, undo/redo, and remote changes.
2. Use one engine-owned undo path; preserve cursor/scroll selections with stable anchors. Remove stale serialization-pending stubs and signal-only pseudo-bindings.
3. Make SyncManager a documented presence/transport composition helper or deprecate it with a precise replacement sample. Route dogfood CollabPane through this supported adapter while preserving widgets.
4. Keep the QTextDocument adapter and durable DocumentSession composition optional. Markoff/Wave can supply their own Document model, undo grouping, transport and block lifecycle using headless facades; do not move markdown parsing, inline formatting or cross-document coordination into Core.

**Acceptance:**

- Direct document edits and remote engine operations stay synchronized without feedback loops or double operations.
- IME/non-BMP input, multi-cursor edits, undo/redo, and surgical scroll behavior are covered by real adapter tests.

**Verification:**

Run `./tools/verify-roadmap.sh {debug,release,asan} 'tst_document_adapter|tst_editor_recovery|tst_multicursor|tst_scroll_stability'`; all four targets passed in Debug (`20261011T044136Z-2`), Release (`20261011T044153Z-2`), and ASan/UBSan with `ASAN_OPTIONS=detect_leaks=0` (`20261011T044216Z-2`). Adapter tests exercise QInputMethodEvent commits, programmatic QTextCursor edits, selection replacements, non-BMP and multi-cursor input, engine-owned undo/redo, remote deltas without echoes, and stable scroll anchors.

## A04 — Implement batch outcomes, subscriptions, and safe notification policy

**Status:** in progress
**Depends on:** A03, P05  
**Coverage:** R9  

**Files / scope:**

- `libs/collabtext/include/collabtext/CrdtEngine.h`
- `libs/collabtext/include/collabtext/ActorId.h`
- `libs/collabtext/src/CrdtEngine.cpp`
- `libs/collabtext/src/identity/ReplicaState.cpp`
- `libs/collabtext/include/collabtext/DocumentSession.h`
- `libs/collabtext/src/DocumentSession.cpp`
- `libs/collabtext/CMakeLists.txt`
- `docs/specs/vnext-public-api.md`
- `libs/collabtext/tests/tst_callbacks.cpp`

**Steps:**

1. Provide batch remote application outcomes and one visible-change notification per applied batch, with exact deltas. Duplicate/deferred/rejected operations do not claim visible change.
2. Use scoped subscriptions/unsubscribe handles with defined lifetime; reject or queue reentrant mutations according to P01. Do not allow callbacks to outlive their session.
3. Expose single-owner thread rules and bounded pending byte/count queues. Return actionable backpressure rather than abandoning accepted records.

**Acceptance:**

- Callback order/count and invalid/deferred behavior match P01; remote ops never echo as local ops.
- Destroyed/unsubscribed listeners are not called; reentrant mutation has tested deterministic behavior.

**Verification:**

The focused callback group `tst_callbacks|tst_crdtengine_op_api|tst_document_adapter|tst_transport_commit_contract` passed in Debug (`20261011T044830Z-2`), Release (`20261011T044625Z-2`), and ASan/UBSan with `ASAN_OPTIONS=detect_leaks=0` (`20261011T044722Z-2`). Remaining: session subscription teardown/lifetime, session mutation reentrancy, and bounded pending byte/count backpressure tests.

**Current implementation note:** CrdtEngine remote batches return one outcome per offered operation and notify once only if any operation applies. Callback dispatch holds a stable callback snapshot, and checked same-engine mutation is rejected with `ErrorCode::reentrant_mutation` before changing state. `tst_callbacks` covers mixed applied batches, duplicate/rejected-only batches, callback self-removal, and reentrant local/remote mutation. This closes only the engine callback slice; session listener lifetime and queue/backpressure policy remain open.

## A05 — Define discard_local_view and other reset semantics across epochs

**Status:** see TASKS.json (initially not started)  
**Depends on:** A02, G05  
**Coverage:** R14, R2  

**Files / scope:**

- `libs/collabtext/src/crdt/IdList.cpp`
- `libs/collabtext/include/collabtext/IdList.h`
- `libs/collabtext/tests/tst_reset_semantics.cpp`
- `docs/specs/vnext-public-api.md`

**Steps:**

1. Remove or rename ambiguous discard_local_view behavior. Specify recovery-state reset independently from collaborative clear; no obsolete source signature or version-retaining discard needs to be preserved.
2. Expose an explicitly named local-view discard only if consumers need it; collaborative clear produces operations, while replacing a store/checkpoint uses an explicit epoch-aware recovery operation.
3. Document which callbacks, identity, clocks, anchors, deferred input, and undo groups survive each action. Reject replay into a cleared engine whose preserved coverage would deduplicate required operations.

**Acceptance:**

- Consumer reset examples are unambiguous; a local clear cannot masquerade as a synced document reset.
- Replay/restoration uses fresh recovery state or valid checkpoint import, never preserved-version discard_local_view.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_reset_semantics|tst_idlist|tst_session_recovery'`; compile new reset examples covering local discard, collaborative clear, and recovery.

## A06a — Install and export a relocatable alpha package

**Status:** see TASKS.json (initially not started)\
**Depends on:** A02a\
**Coverage:** R2, R13\

**Files / scope:**

- `cmake/CollabTextConfig.cmake.in`
- `CMakeLists.txt`
- `libs/collabtext/CMakeLists.txt`
- `tests/consumer-smoke/CMakeLists.txt`
- `tools/verify-consumer-package.sh`
- `docs/USING_VNEXT.md`
- `docs/specs/vnext-alpha-capabilities.md`
- `docs/specs/vnext-external-position-contract.md`

**Steps:**

1. Implement install(TARGETS)/install(EXPORT), public-header installation, package configuration/version files, and CollabText::Core/FileTransport/QtAdapter/Widgets imported targets for the implemented components. Install only supported public headers; do not copy src or expose source/build-machine paths. Make selected components discover their own dependencies rather than always finding Qt/zstd/OpenSSL.
2. Make the default Core-only find_package path C++20-only. Add negative configuration checks with Qt6, zstd and OpenSSL discovery disabled for Core; verify requesting an unavailable optional component reports a clear CMake failure. A static FileTransport consumer must receive required compression/hash link dependencies through its exported interface.
3. Create a fail-fast tools/verify-consumer-package.sh that configures/builds a producer in a task-specific temporary tree, installs to prefix A, moves the installation to prefix B, and builds/runs a separate consumer using find_package(CollabText CONFIG REQUIRED) and prefix B. Do not delete or move the developer checkout; inspect compile commands so only installed library headers are used. Reject zero consumer tests and retain command/configuration logs. Implement the exact CLI/options and exit/result contract in 00-consumer-integration.md; A06a initially supports gate=package, W02 adds baseline, W03 adds live, and A06/E05 add final. An unimplemented gate must return nonzero rather than silently selecting a weaker mode.
4. Exercise the A02a text/list consumer and public-header compilation from the installation. Add optional FileTransport/Qt component configurations only when those components are enabled. The consumer must configure outside the source tree, with no sibling Wave/Markoff checkout and no inherited producer targets.
5. Document exact configure/build/install/find_package commands, target names, enabled components, C++ standard, offset units, current alpha capabilities, and expected API changes. This is an evolving integration package; do not advertise durable pins, persistent undo, or full recovery before their tasks pass.

**Acceptance:**

- A relocated install builds and runs an independent Core consumer without source-tree include/link paths or Qt/zstd/OpenSSL discovery.
- Optional component discovery and static dependency propagation work for the enabled configurations; every installed public header compiles independently.
- The verification script fails on configuration/build/test failure or zero tests, and produces retained logs plus the exact producer revision and capability manifest.

**Verification:**

Run tools/verify-consumer-package.sh for core-only and enabled optional components in Debug and Release; inspect consumer compile/link commands and the installed tree. Record configurations, revision, compiler, test counts, relocation paths, and manifest; remote CI is not claimed by local evidence.

## A06 — Install and export the fresh public library package

**Status:** see TASKS.json (initially not started)  
**Depends on:** A06a, A04, A05, W03\
**Coverage:** R2, R13  

**Files / scope:**

- `cmake/CollabTextConfig.cmake.in`
- `libs/collabtext/CMakeLists.txt`
- `tests/consumer-smoke/CMakeLists.txt`
- `docs/USING_VNEXT.md`
- `docs/specs/vnext-external-position-contract.md`
- `tests/consumer-smoke/wave-contract/`

**Steps:**

1. Extend and certify the A06a package with all final APIs/components and their complete dependency metadata; preserve target names and relocation checks. Packaging already exists: add the complete consumer scenarios rather than recreating export rules or moving supported headers back into src.
2. Document the new headers, types, callback/ack semantics, error paths, ownership, offsets, and reset APIs; include Buffer, IdList, and custom transport examples. Explain intentional breaks without preserving obsolete signatures.
3. Remove obsolete public wrappers and install only the supported surface. Do not create deprecated adapters, scalar-GC wrappers, legacy codecs, or void-push compatibility paths. State fresh-format identifiers and incompatible-format errors.
4. Add the self-contained W6 installed_headless_wave_contract consumer fixture using a consumer-owned Document adapter and canonical-log envelope. Exercise external_token_roundtrip_reopen, pin registration/recovery, compensating undo, historical_projection_resolves_its_own_tokens, and explicit failures. Build from a relocated install with Qt Widgets/Test disabled and no sibling Wave checkout. Formatting is consumer-owned; demonstrate span endpoints only, with no new formatting CRDT.

**Acceptance:**

- An independent project finds CollabText through find_package and links without source-tree paths.
- The new usage guide covers every V08 requirement with compilable samples; no old API/ABI/store compatibility is promised.
- A headless installed consumer passes the W1-W6 in-scope contract without source paths, Qt Widgets/Test, or a Wave checkout; its coverage identifies later certified-GC cases that E05 reruns.

**Verification:**

Install to a temporary prefix, hide original include tree, relocate prefix, and configure/build/run external samples for Core, FileTransport, and QtAdapter. Verify the W-series named cases assigned above at this task's implementation layer and record their evidence; later dependent cases remain pending.

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
2. Run core-only builds without Qt, full Linux Debug/Release suites, bounded seeded fuzz/model tests, ASan/UBSan on supported compilers, and install-consumer checks. Preserve and extend the early consumer-alpha workflow; do not defer its installed checks until this final matrix or replace strict probe cases with successful skips.
3. Keep slow campaigns/benchmarks in scheduled or manually triggered jobs; failures retain traces and do not become successful skips. Record any unavailable platform as unsupported/unverified.

**Acceptance:**

- CI catches accidental Qt dependencies, public-header leakage, sanitizer failures, and package regressions.
- Platform-specific durable-file implementations compile/test on their claimed platforms; POSIX APIs no longer leak into unconditional builds.

**Verification:**

Validate workflow syntax locally; run matching Linux jobs locally; require first remote CI results before E06 declares platform support.

## A08 — Add advisory stale-device flags and explicit retirement UI

**Status:** see TASKS.json (initially not started)  
**Depends on:** G02, A04  
**Coverage:** R3, R10  
**Design/decision gate:** Uses the validated G01/G02 retirement protocol; a reminder timeout is never authorization to retire.  

**Files / scope:**

- `libs/collabtext/include/collabtext/DeviceStatus.h`
- `libs/collabtext/src/ui/DeviceRetirementDialog.cpp`
- `app/collabedit/CollabPane.cpp`
- `libs/collabtext/tests/tst_device_retirement_ui.cpp`
- `docs/USING_VNEXT.md`

**Steps:**

1. Expose advisory last-observed activity and stale_for_review per enrolled device. Provide a configurable reminder age (proposed default 90 days), enable/disable setting, and snooze/dismiss state. Inject a clock for testing; missing timestamps and clock jumps must never change membership or GC eligibility.
2. Show a nonblocking device-review interface after the reminder age with Keep enrolled, Remind later, and Review retirement actions. Open G02's concrete preview before confirmation; merely opening/dismissing the interface never calls retirement commit.
3. Explain that indefinite offline operation is supported while enrolled, retirement changes future merge guarantees, and unreceived edits may exist on the offline device. Commit only an explicit confirmation, surface durable errors, and show returning-device export/reconciliation instructions.

**Acceptance:**

- Clock advance beyond the configured age flags a device but leaves membership, undo pins, and GC eligibility unchanged. Keep, snooze, dismissal, disabled reminders, and restart all preserve enrollment.
- Only explicit confirmation calls the durable retirement API; failure leaves the device enrolled. Successful retirement persists and a returning old-epoch device is routed to rescue/rejoin.

**Verification:**

Run `./tools/verify-roadmap.sh debug 'tst_device_retirement_ui|tst_membership|tst_undo_retirement'` offscreen with fake-clock boundary, snooze/restart, cancellation, and commit-failure cases.
