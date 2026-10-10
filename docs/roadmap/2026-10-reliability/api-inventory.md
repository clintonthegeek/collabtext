# API inventory and intended surface (V08)

**Baseline inventoried:** revision `046d375` (the evaluated source) plus the
implementation branch `roadmap/reliability-v2` after V01–V07.
**Purpose:** freeze what the library is *for* before the API work in P01, A01,
and A06 changes it, so those tasks have a target rather than a direction.

This is an inventory and an intention, not a compatibility contract. The user
confirmed there is no production data, store, or history: no old store needs to
decode, no external consumer has to be ported, and no public type has to survive.
Nothing here preserves an existing surface for its own sake. What it records is
which surfaces are *load-bearing for a use case someone actually has*, which the
roadmap should keep working, and which are accidents of history that can go.

---

## 1. Use cases the library must serve

These are the requirements. Everything else in this document is subordinate to
them.

| # | Use case | Where it lives today | Status |
|---|---|---|---|
| U1 | A text editor over a shared document, with local undo **and** redo | `CrdtEngine`, `Buffer`, `CollabDocument` | works; redo does not survive reopen (L06) |
| U2 | A structural list (chat/message tree, outline, file tree) keyed by stable ids | `IdList` | works |
| U3 | Edits moving between machines through a shared folder | `StreamSync`, `FileSync`, `SyncManager` | works; durability claims are weaker than documented (F9, L01/L02) |
| U4 | One engine, many transports; a transport is a four-method object | `OpStream` + `StreamSync` | works |
| U5 | Presence and cursors that do not allocate operation ids | `PresenceManager`, `MultiCursorController`, `CollabPlainTextEdit` | works |
| U6 | Anchors that survive concurrent edits, including comments and chat anchors | `Anchor`, `Comment`, `ChatMessage` | works; boundary affinity is wrong (F7, S04–S06) |
| U7 | Plain text and IdList in one Qt application | `CollabPlainTextEdit`, `app/collabedit` | works; no installable consumer yet (A01) |
| U8 | Deterministic, replayable failure investigation for development | `TraceHarness.h`, `StoreFaultHarness.h` | added by V04/V07 |

U8 is new but is now a use case in its own right: the harness files are part of
the intended surface for contributors, and a future change that makes a fixture
unreplayable is a regression.

---

## 2. Public headers and what each one is for

All under `libs/collabtext/include/collabtext/`. A header is *public* because a
consumer includes it by name, not because it is in this directory.

| Header | Intended role | Notes for P01/A02 |
|---|---|---|
| `Operations.h` | `Operation` value type plus `encode_operation`/`parse_operation` | Narrow contract: `lamport()` and round-trip are the stable surface (V03's rule, extended by P01). |
| `IdListOperations.h` | `IdListOperation` and its codecs | Same narrow contract. Not shared with `Buffer`'s operation type. |
| `Serialization.h` | Version-vector and stream-snapshot encoders | Format version must become explicit per artifact (P06). |
| `OpStream.h` | The transport boundary: `push`, `set_on_inbound`, `lowest_peer_acked_lamport`, `set_on_ack_update` | Keep the four-method shape. `lowest_peer_acked_lamport` must keep saying it is receipt information, not reclamation authority. |
| `StreamSync.h` | The file-backed `OpStream` implementation | The only transport collabtext ships (DECISIONS.md). |
| `CrdtEngine.h` | Buffer-backed engine with `insert`/`remove`/`undo`/`redo`/`apply_remote` with typed outcomes | Wrapper to be reviewed for obsolescence (A05). |
| `CollabDocument.h` | Qt document wrapper: `insertText`, `removeText`, `undo`, `redo`, `updateReady` | Candidate for consolidation into one adapter (A03, R3). |
| `SyncManager.h` | Qt sync orchestration over a shared folder | Exposes `syncCycle`, `flushLocalUpdates`, `readRemoteUpdates`; naming and responsibility are unclear. |
| `Identity.h`, `IdentityStore.h`, `IdentityProjector.h`, `PresenceManager.h`, `Signing.h` | Identity and presence | Signing is a trusted-folder prototype; the scope must be stated honestly (E04). |
| `CollabDocument.h` + `src/ui/*` | Qt widgets: `CollabPlainTextEdit`, `MultiCursorController`, comment/chat panels, cursor labels | Widget layer is deferred work (A03, A08). |

### Internal headers that leak

`src/crdt/*.h` are reachable today because the library's include path exposes
`src/` publicly, and tests include `crdt/Buffer.h` directly. That is the main
source/API boundary problem: a consumer can depend on internals that no contract
protects. P01/A02 should move the internal path to `PRIVATE` and re-point the
tests at the public headers or at the internal path explicitly.

The most load-bearing internal types are `Buffer`, `IdList`, `Anchor`, `Locator`,
`Global`/`Lamport`, `SumTree`, and `UndoMap`. Their *behaviour* is specified
(`docs/CRDT_ENGINE_SPEC.md`, `docs/CRDT_IDLIST_SPEC.md`); their *headers* are not
part of the surface.

---

## 3. Callbacks and their ownership

Callback ownership is currently implicit, which is finding R9 and P01/A04's
subject.

| Producer | Callback | Ownership today | Intended |
|---|---|---|---|
| `OpStream::set_on_inbound` | one inbound handler | replaces the previous registration | same, documented; no reentrancy guarantee claimed |
| `OpStream::set_on_ack_update` | ack advance | replaces the previous registration | same |
| `Buffer::set_on_change` | `std::function<void()>` | held by value | same, plus documented threading |
| `Buffer::set_on_local_op` | local-op notification | held by value | same |
| `IdList::set_on_change`, `set_on_local_op` | as above | held by value | same |
| Qt signals (`CollabDocument`, `SyncManager`) | Qt signal/slot | Qt connection semantics | unchanged; document which thread may connect |

None of these may be invoked from inside a mutation that is still holding an
internal lock. Today there is no such claim in either direction, so "reentrancy
is undefined" is the honest contract until A04 writes one down.

---

## 4. IdList reset semantics

`IdList::local_clear()` exists (commit `b1f7ed6`) and is *not* a collaboration
operation:

- Drops visible entries, tombstones, the undo stack, and the deferred queue.
- Preserves `replica_id`, clock, version, `max_undo_depth`, and callbacks.
- Preserves `m_version`, so replaying previously seen Lamport values after a
  clear is a silent no-op. Rebuilds must supply fresh operations (a re-parse and
  insert pass), not a replay of the original log.
- Fires neither `on_change` nor `on_local_op`.
- Peers do not see the clear. Replacing a connected session's content is a
  higher-layer concern and is currently undocumented as unsupported.

`Buffer` has no equivalent. `CrdtEngine` exposes no reset. A04/A05 should either
give both primitives the same explicit reset contract or state that only IdList
has one; today the asymmetry is undocumented.

---

## 5. CMake targets

| Target | Kind | Intended |
|---|---|---|
| `collabtext` (alias `CollabText::CollabText`) | static library, links Qt6 Core/Gui/Widgets/Test **publicly** | Must become a Qt-free core with optional Qt and transport components (A01) |
| `reduce-trace` | developer tool (V04) | keep |
| `store_crash_worker` | test dependency, deliberately not a CTest test (V07) | keep |
| `tst_*` (55 registered) | test executables, each name-matching its CTest test | keep the name-matching convention |
| `collabedit`, `collabtext-testapp` | applications | deferred; A01/A06 make them installed consumers |

Public linkage of `Qt6::Test` from the library is a defect: a consumer of the
engine does not need QtTest. `NetworkSim.cpp` including `<QTest>` and calling
`QFAIL` from library code is the concrete reason, and it is test-harness code
living in the shipped library. A01 splits it.

---

## 6. Supported API requirements (what P01/A01/A06 must deliver)

1. **One installable core** with no Qt, no QtTest, and no zstd requirement
   (zstd is currently a hard private link dependency).
2. **Explicit outcomes** on every mutation that can fail or defer: applied,
   duplicate, deferred, rejected, and *wrong epoch* / *unresolvable anchor*.
   Today `apply_ops` returns `void` and the distinction is invisible.
3. **Persistent actor identity** and checked 64-bit counters, replacing the
   16-bit replica/32-bit counter assumptions.
4. **Boundary affinity** on anchors, with `Left`/`Right` meaning what they claim
   (F7/S04).
5. **Per-stream durable coverage**, replacing the single scalar
   `lowest_peer_acked_lamport` as the only receipt signal.
6. **Undo and redo that survive reopen** within configured step and byte limits
   (DECISIONS.md Q3).
7. **Explicit device retirement** with advisory staleness reminders that never
   mutate membership (DECISIONS.md Q2).
8. **A Qt adapter** with one complete document/session binding, replacing the
   `CrdtEngine`/`CollabDocument`/`SyncManager` overlap (R3/A03).
9. **Versioned formats per artifact** — envelope, codec, checkpoint, membership,
   anchors — that reject obsolete and unknown versions explicitly.

## 7. Obsolete surfaces to remove

| Surface | Why it goes |
|---|---|
| `src/` on the public include path | makes internals part of the surface |
| `crt/Buffer.h`, `IdList.h` shims | internal-only after the extraction |
| `lowest_peer_acked_lamport` as a reclamation input | receipt is not authority (V03 already documents this; the *API* still invites the misuse) |
| `collect_garbage()` / `compact(Global)` signatures | return zero and cannot authorise anything until G06 replaces them |
| `Qt6::Test` linked from the library | not a runtime dependency of the engine |
| `NetworkSim` inside the shipped library | test harness; moves to the test tree in A01 |
| `CrdtEngine` vs `CollabDocument` duplication | one adapter (A03/A05) |
| Any legacy-store decoder, importer, shim, or migration | explicitly not wanted (DECISIONS.md Q1) |

## 8. Breaks a reviewer should expect

Source, ABI, and format breaks are all acceptable here. Concretely:

- **Source:** every internal include path; every signature that gains an outcome
  type; `Operation`/`IdListOperation` field access; `set_max_undo_depth` if undo
  limits become paired step *and* byte limits (L06/G01).
- **ABI:** the library is static and rebuilt from source, so ABI breaks are
  build breaks. Nothing is promised.
- **Format:** `schema_version` advances per artifact. Existing files keep
  loading only where the roadmap says so; an incompatible change is a documented
  hard cut, not a silent misread.

## 9. Behavioural fixtures

`libs/collabtext/tests/fixtures/behavior/*.behavior` freeze logical scenarios
with provenance and exact expected outputs. They are versioned event traces plus
declared expectations, so a fixture is executable evidence rather than a document
that drifts. `tst_behavior_fixtures` replays them.

Rules the fixtures follow:

- Every fixture declares `# name`, `# provenance`, `# scope`, and at least one
  `# expect`. The suite fails if any is missing, so a fixture cannot be added
  without saying where it came from.
- `status: desired` means the declared expectation must hold. A failure is a
  regression.
- `status: known-defect` means the declared expectation is what *should* happen
  and today does not. The failing assertion is marked expected, so an unexpected
  pass is itself a failure: the fixing task landed and the marker must go.
- No fixture requires an old store, an old format, or an importer.

Covered: Buffer convergence, concurrent passages (F8), Unicode boundaries,
restart with undo/redo, partition and heal, transport loss, IdList convergence,
IdList restart, comment anchors.

Not covered here, deliberately: **corruption**, because a damaged file is not a
sequence of decisions and cannot be expressed as a replayable trace. Those
fixtures are in `tst_store_fault_harness` (V07), which builds real damaged
directories and reads them back with the production reader. The behaviour suite
asserts that harness's presence rather than pretending to cover corruption.

## 10. Baseline reproduction (046d375)

Each fixture was replayed against the evaluated baseline to separate "the engine
already does this" from "this is what we want". Results are in
`docs/reports/2026-10-03-roadmap-kickoff-artifacts/v08-baseline-fixtures.txt`.

- Eight of nine fixtures meet their declared expectation at the baseline. They
  freeze current correct behaviour, which is what makes them regression tests.
- `buffer_concurrent_passages_stay_contiguous` fails at the baseline exactly as
  the evaluation describes: two producers inserting "hello" and "world" at
  position 0 produce `hweolrllod` — interleaved per character. That is F8, and
  the fixture is its red test for S01–S07.
- Nothing else differs between the baseline and this branch, because V01–V07
  changed verification, network simulation, benchmark honesty, and reclamation
  containment, not Buffer or IdList semantics. That is the expected result, and
  it is why these fixtures are worth freezing before P01 and S01 start editing
  the same code.

No external repository was accessed and no consumer was contacted.
