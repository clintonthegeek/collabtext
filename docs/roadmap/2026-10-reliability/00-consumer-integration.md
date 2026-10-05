# Early consumer integration: package, positions, probes and alphas


**Amendment:** 2026-10-04, authorized by the user after reviewing Wave ask2.
**Authority:** [TASKS.json](TASKS.json); dependencies and status live there.
**Scope:** improve the existing native C++ text/IdList engine, package it early,
and make real consumer feedback part of implementation. No external consumer
repository is edited, no message is sent, and no tag/package is published by this
planning amendment. Completed/in-progress task statuses and evidence are retained.

Read [EXECUTION.md](EXECUTION.md), [DECISIONS.md](DECISIONS.md), the binding
[position requirements](../../specs/vnext-external-position-contract.md), and the
individual tasks in this file, [04](04-semantics.md), [06](06-public-api.md), and
[08](08-release.md). Phase numbers group concerns; registry order is the preferred
serial schedule. There is no prerequisite to finish every earlier phase.

## Source material to inspect

- `/home/clinton/dev/wave/docs/handoff/2026-10-04-collabtext-ask2-sequencing-and-probes.md`
- `/home/clinton/dev/wave/docs/handoff/2026-10-04-collabtext-devs-our-asks.md`
- `/home/clinton/dev/wave/docs/reference-notes/reports/2026-10-04-crdt-alternatives-automerge-yrs-loro.md`
- `/home/clinton/dev/wave/docs/specs/0001-wave-0.1-protocol.md`, Document/Position obligations.
- `/home/clinton/dev/Markoff/docs/specs/2026-05-07-d5-collab-activation-design.md`, layer/transport ownership.
- `/home/clinton/dev/Markoff/docs/handoff/2026-05-08-collabtext-joint-design-positions.md`, opaque operation round-trip and public headers.

These paths are read-only research inputs. Copy the relevant test scenario and
provenance into this repository in W01; ordinary builds/tests must not require
these checkouts. Competitor numbers/aborts are reported observations until their
exact harnesses and versions have been reproduced. Do not infer an industry-wide
impossibility or an engine-adoption requirement from the report.

## Preferred serial implementation route

| Order | Task(s) | Concrete handoff / stop condition |
|---|---|---|
| 1 | P06a remaining steps 3–6 | Finish the running identity adoption, preserve the landed run-interval/allocation gate, measure actual layout; do not restart the task or tick once per scalar. |
| 2 | A01 | Independent component builds; Core cannot discover Qt/compression/hash packages. |
| 3 | A02a → A06a | Real implemented public facades, installed/exported package, relocated external consumer. No P07/S04/L08 gate. |
| 4 | W01 → W02 | Versioned eight-probe fixtures, honest installed baseline matrix, locally runnable branch CI job. |
| 5 | E07a | Reviewable package-alpha candidate with exact SHA, capability/format manifest, commands and proposed tag; publication is pending separate authorization. |
| 6 | S01 → S04a | Desired sequence/boundary fixtures, then precise position contract, pseudocode and independent boundary model. Stop at any unresolved identity/boundary counterexample. |
| 7 | P06b → S04b | Versioned real operations for replica/fresh-process tests, then public live positions and bounded token codec. No S03/L08 dependency. |
| 8 | W03 → E07b | Strict installed WP1–WP5/WP7 plus plaintext fixture; reviewable position-alpha candidate with supported token readers and remaining limits. |
| 9 | S02 → S03 | Complete ordering interpreter/repair and production implementation; preserve the earlier position contract and supported token bytes. |
| 10 | P07 → P08 → L02–L08 | Receipt/commit and bounded decode pipeline, then actual durable operation/history/save/recovery lifecycle. |
| 11 | S04 → S05 → S06 → S07 | Certify anchors under repaired order and real recovery, implement durable pins/history, enumerate combined histories, restore safe fast paths. |
| 12 | G01–G08; A02–A08; X01–X10; E03–E06 | Validated reclamation, complete public/Qt package certification, measured optimization and final release dossier; obey each registry edge. |

This is a single-executor route, not a request for parallel agents. If P06a is
temporarily blocked, A01/W01/S01 are independently ready; finish the current
task cleanly before changing shared types. Pure packaging does not need identity
adoption, although the preferred route avoids churn by finishing that already
running task first. A02a describes only actual implemented types and capabilities;
it must not invent a successful future API in order to meet an early date.

## Gate ownership and meaning

| Gate | Must pass | May remain visibly unfinished |
|---|---|---|
| Installed baseline, W02/E07a | Public headers, primitive edits/operation handles, relocated install, explicit eight-case results, exact known-failure allowlist | Live anchors/token codec, repaired ordering, recovery, persistent undo, pins/GC |
| Live positions, S04b/W03/E07b | Scoped identity, affinity, typed resolution/collapse, malformed/foreign rejection, token codec and fresh-process operation replay, public installed adapter | Whole-passage ordering repair, actual crash/reopen lifecycle, durable pins, checkpoint translation and reclamation |
| Recovered positions, S04 | Same supported token bytes under S03 repaired ordering and L08 real recovery | Pin protection/epoch translation until S05/G-series gates |
| Final acceptance, E05 | All promised W1–W6/WP1–WP8 features, persistent undo AND redo, certified GC and installed consumers | Only explicitly recorded user-approved deferrals; preparation is not publication |

S04a's reference model evaluates positions against supplied ordered item/visibility
states. It does not secretly contain the full future ordering algorithm. S02 later
generates those states with its independent ordering interpreter and composes the
two models. S04b comparisons can therefore verify positions without claiming F8
is fixed. Fixed hand-authored boundary expectations still catch a mutually wrong
production/model interpretation.

Do not invent a universal error-on-character-deletion rule. A caret boundary can
remain valid; an attachment that lost its intended content must expose that loss.
W01/S04a specify explicit collapse/deletion information so the consumer can choose
strict attachment behavior without corrupting Markdown editing selections.
Wrong context, bad encoding and off-end input always receive typed rejection.
Containing block deletion is checked by the consumer; token scope is enforced by
the engine. Neither check can be replaced by an assertion or a convenient offset.

## Eight-probe acceptance map

| Probe | Scenario and early expected result | Feature owner / later strict evidence |
|---|---|---|
| WP1 | `ab`, same boundary at 1, insert X: Left=1, Right=2; every scalar boundary, empty/start/end, concurrent delivery | S04a model, S04b production, W03 installed; S04/S06 after order/recovery |
| WP2 | Delete one/both referents; explicit resolved/collapsed/awaiting/unavailable outcomes. Consumer strict attachment surfaces detachment; caret policy remains available. | S04a/S04b/W03; S04 recovery, S05 retention and undo policy |
| WP3 | Delete the containing consumer block; return a typed gone/stale block result before engine lookup | W02 adapter baseline, W03 installed strict gate |
| WP4 | Mint at 0/len/empty; reject len+1, non-scalar byte offsets, and wrong primitive units | S04a/S04b/W03; S04 after recovery |
| WP5 | Canonical encode/decode, all trailing-byte suffixes/truncations, foreign/version failures, fresh process and second-replica replay | S04b/W03; S04 real store lifecycle; S05/G06/G08 original bytes through permitted GC/translation |
| WP6 | Split concurrent paragraphs, reverse/nested typing, subsequent edits; exact intended whole-passage content | S01/S02/S03 and S06. W02/W03 report pending or known_failure until repaired; equality alone is insufficient. |
| WP7 | Resolve against wrong document/stream/block/primitive/epoch; typed failure, no mutation/assert/abort | S04a/S04b/W03 and S04; process-isolated workers record abnormal exits as errors |
| WP8 | GC disabled retains data; stored bytes alone are not a pin; explicit pin/register/release/recover/retire/translation | W02 no-GC baseline only; S05/G01/G06/G08 strict durable retention; E05 final installed run |

W01 defines fixtures; W02 runs every case with explicit capability states; W03
requires the live set; E05 requires the complete promised set. Never turn
unsupported, empty test selection, a skipped worker, or a known failure into
passing feature evidence. Every matrix row records revision, seed, fixture hash,
mode, named assertions, outcome and signal/exit status where relevant.

## Installed verification script contract

A06a implements `tools/verify-consumer-package.sh` with these exact options:

```sh
tools/verify-consumer-package.sh --mode debug --gate package --components core
tools/verify-consumer-package.sh --mode release --gate baseline --components core
tools/verify-consumer-package.sh --mode debug --gate live --components core
tools/verify-consumer-package.sh --mode asan --gate live --components core
```

- `--mode`: `debug`, `release`, or `asan`; sanitizers follow the existing runner's supported compiler/flags contract.
- `--gate`: `package` (A06a), `baseline` (W02), `live` (W03), or `final` (A06/E05).
- `--components`: `core`, `file-transport`, or `qt`; each includes Core and only the dependencies required by the selected components. The required headless position gate uses `core`.
- `--output-dir`: optional task-owned path for retained logs/results. Otherwise create a unique ignored worktree verification directory and print its location. Never replace a developer's existing install/build tree silently.
- `--help`: show options, implemented gates and their owning task IDs. Unknown/missing values and unimplemented gate/component combinations fail with a diagnostic.

Exit zero means the selected gate's assertions passed, never merely that the
worker ran. Any configure/build/test failure, missing scenario, zero tests,
unexpected signal, or strict known_failure/unsupported result returns nonzero.
The baseline uses only its exact reviewed allowlist; new failures are fatal.
Emit a versioned `results.json` with candidate revision, mode/options, compiler,
capability/format manifests, fixture hashes, test/case IDs, outcomes, expected
versus observed values, logs and child exit/signal data. Reserve exit handling
for diagnostics, not successful skips. Scripts must be runnable without remote
CI, sibling checkouts or temporary source harnesses.

These commands are the implementation target; the script does not yet exist.
Each creating task registers actual test workers and implements only its stage;
later tasks extend the same script without changing its option names or silently
lowering a requested gate. Physical GC stays disabled until G06 regardless of
the selected gate. Before G06/G08, `final` cannot certify their pending promises.

## Format and publication policy

The first package alpha can precede usable positions. The later position alpha
must have a reviewed supported-token manifest before consumer persistence is
advertised. Approved published supported token bytes become a reader/translation
obligation even when C++ API/ABI changes are allowed. Keep supported golden bytes;
do not remint stored references as a substitute for preserving meaning. There is
no obligation to convert pre-vNext experimental stores/tokens.

E07a/E07b prepare local artifacts and exact proposed tag/SHA/release notes. Once
the user explicitly authorizes publication, the executor verifies the clean
candidate and checks, confirms the named remote destination, publishes only the
approved immutable alpha tag/branch artifacts, and verifies a clean remote fetch
at the recorded SHA. It records actual remote CI/accessibility and hands the
user pin/install/probe instructions. Do not force-update an existing tag, merge
master, send a consumer message, or add package hosting destinations implicitly.
If no publication instruction exists, preparation can complete but availability
remains pending; report the concrete candidate without claiming a delivered tag.

Core remains plaintext/IdList and transport-agnostic. Markoff/Wave own their
block maps, Markdown rendering or inline formatting, authorization/event logs,
cross-block history and transport choices. DocumentSession, file transport and
Qt adapters are optional compositions, not compulsory gateways to primitives.
Forever-offline devices remain enrolled until explicitly retired; durable reply
pins are independent of author device lifetime. Persistent undo/redo and recovery
remain required later features rather than being dropped to accelerate integration.

## W01 — Capture the eight consumer probes as versioned fixtures

**Status:** see TASKS.json (initially not started)\
**Depends on:** V04, V08\
**Coverage:** F7, F8, R4, R11\

**Files / scope:**

- `docs/specs/vnext-consumer-probes.md`
- `libs/collabtext/tests/fixtures/external-position/`
- `tests/consumer-smoke/wave-contract/probes/`
- `docs/reports/vnext-consumer-probe-provenance.md`
- `docs/specs/vnext-external-position-contract.md`
- `tools/validate-consumer-fixtures.py`

**Steps:**

1. Read both Wave asks and its alternatives report using the paths in 00-consumer-integration.md. Inspect the referenced /tmp/opencode sources selectively; record the exact source file, engine/library version, adapter assumptions, license/provenance, and any missing artifact. Copy only relevant reusable fixture/harness source into this repository; never require /tmp, a Wave checkout, network downloads, or another engine toolchain to run native acceptance.
2. Give the eight probes permanent IDs WP1-WP8. Store deterministic input histories, actor/document/stream IDs, causal edges, minted-token event, affinity, Unicode offset units, expected typed result, and seed in versioned fixtures. Port their input scenarios to a small C++ harness contract; competitor measurements remain attributed historical observations rather than native expected results.
3. Specify WP2 deletion expectations by attachment kind: a valid boundary may resolve with explicit collapse; a strict content attachment must surface detached/unavailable state rather than a bare relocated offset. Define whether undo can restore the referent. Specify WP3 as a consumer-owned block-existence check followed by scoped engine resolution, and WP7 as a library-enforced wrong-context error. Where exact deletion placement depends on S04a, record the mandatory observable outcome and tag the unresolved offset rule design_pending with owner S04a; do not invent universal deletion failure or a fallback integer. S04a must replace every pending boundary expectation with an exact reviewed fixture before S04b unlocks.
4. Specify WP4 scalar-boundary checks including zero, length, length+1, empty text, interior UTF-8 bytes, non-BMP text, and distinct list indices. Specify WP5 exact token round-trip, every one-byte trailing suffix, truncation, invalid enum/version/context, fresh-process replay, and unchanged document state on rejection.
5. Assign staged executable ownership to W02, S04a/S04b, W03, S03/S04, S05, G06/G08, and E05 using the matrix in 00-consumer-integration.md. Define machine-readable pass/known_failure/unsupported/error results; unsupported is capability information, never passing evidence for that feature. No production anchor or ordering change belongs to W01.

**Acceptance:**

- All eight probes have reproducible native fixture inputs and explicit typed expectations; absent competitor source is recorded honestly and does not prevent recreating its documented scenario.
- Fixture review separates carets, strict attachments, deleted containing blocks, wrong-object lookup, no-GC behavior, and durable pin guarantees; it does not infer universal failure from every deletion.
- A fixture parser validates the schema, units, IDs and histories and rejects malformed fixtures. No test depends on a temporary directory or sibling consumer repository.

**Verification:**

Run python3 tools/validate-consumer-fixtures.py --self-test and validate the checked-in probe fixtures twice with the fixed seed; compare normalized histories. Review every WP1-WP8 expectation against W1-W6 and retain fixture hashes/provenance. This task specifies cases, not evidence that the engine passes them.

## W02 — Run installed baseline probes and continuous consumer checks

**Status:** see TASKS.json (initially not started)\
**Depends on:** W01, A06a\
**Coverage:** F7, F8, R4, R11, R13\

**Files / scope:**

- `tests/consumer-smoke/wave-contract/`
- `tools/verify-consumer-package.sh`
- `.github/workflows/consumer-alpha.yml`
- `docs/reports/vnext-consumer-probe-baseline.md`
- `docs/specs/vnext-alpha-capabilities.md`
- `docs/specs/vnext-external-position-contract.md`

**Steps:**

1. Implement a minimal consumer-owned Document adapter entirely in the installed consumer fixture: a block-ID-to-engine map, UTF-8 body access, local/remote operation routing, and capability-aware anchorAt/resolveAnchor forwarding. Keep block lifecycle/formatting/canonical events in this test consumer; do not edit Wave or prescribe its production adapter.
2. Execute every WP1-WP8 scenario against the current installed alpha. Produce a versioned result matrix with exact assertions, expected versus observed outcome, capability, engine revision, seed/fixture hash, and subprocess exit status. Missing anchors/pins may be unsupported and known defects may remain known_failure, but neither classification satisfies later strict gates.
3. Run wrong-object, malformed-token, and deleted-block scenarios in isolated child processes, recording signals and abnormal exits as errors. Cover local text/list operation transfer even before position support exists, so early packaging yields useful behavioral integration evidence rather than only header compilation.
4. Add CI on pushes/pull requests to roadmap/reliability-v2 that invokes the same local package/probe script for Linux Core-only builds and a Qt adapter configuration where available. Run the job without /tmp source harnesses, sibling checkouts, Rust, or network engine downloads; keep final A07 platform claims separate; retain machine-readable results on failure.
5. Separate baseline mode from strict capability mode. Baseline succeeds only for its documented implemented behavior and exact known-failure allowlist; a new abort, assertion mismatch, unexpected unsupported feature, or unknown case fails. Strict mode requires the feature-owning task sets specified in the probe matrix and fails on known_failure/unsupported/error. Do not skip missing capability scenarios or remove existing regression markers merely to make baseline green. Use gate=baseline in the shared script and bind the known-failure allowlist to case ID, capability/format and expected failure outcome, not a wildcard or a forever exemption; update it deliberately as owning tasks pass.

**Acceptance:**

- The independent installed consumer reports all eight probes and actually tests implemented primitive edits/operation transfer; no claimed pass is produced by an unsupported path.
- The same script runs locally and in the prepared branch CI workflow, with explicit baseline and strict modes and retained logs/results. New errors fail baseline.
- The baseline report records existing failures and assigns each unfinished probe to its owning task; no Wave production-readiness claim is made.

**Verification:**

Run installed baseline mode in Debug and Release and compare case coverage; deliberately supply a bad expectation and an abnormal worker exit to verify the runner fails. Validate workflow syntax locally and record remote execution as pending until the workflow is actually pushed/run.

## W03 — Certify live position probes through an installed consumer adapter

**Status:** see TASKS.json (initially not started)\
**Depends on:** W02, S04b\
**Coverage:** F7, R2, R4, R13\

**Files / scope:**

- `tests/consumer-smoke/wave-contract/`
- `tools/verify-consumer-package.sh`
- `.github/workflows/consumer-alpha.yml`
- `docs/reports/vnext-consumer-live-position-gate.md`
- `docs/specs/vnext-alpha-capabilities.md`
- `docs/specs/vnext-external-position-contract.md`
- `tests/consumer-smoke/markdown-contract/`

**Steps:**

1. Replace the fixture adapter's unsupported position paths with the actual installed public APIs. Map engine typed results to consumer Resolved/Stale/Gone-style states without discarding error identity or collapse information. Keep deleted block checks in the consumer-owned map; a valid caret collapse must not be presented as an intact strict content attachment.
2. Require strict passes for WP1 affinity, WP2 explicit attachment/collapse handling, WP3 deleted containing block, WP4 exact end/off-end/Unicode boundaries, WP5 token codec and fresh-process operation replay, and WP7 wrong document/stream/block/primitive/epoch. Include text and list APIs and two physically independent engine instances using the same minted token.
3. Keep WP6 concurrent-passage repair visibly pending until S03, and WP8 durable pin/GC retention pending until S05/G06/G08. WP8's no-GC smoke must confirm containment remains active; it cannot count as certified reclamation. S04b replay evidence cannot count as L08 crash recovery.
4. Enforce the live-position strict set in the branch consumer workflow and local script; remove only the matching baseline known-failure entries after the strict cases pass. Keep a separate complete matrix showing all eight probes, capability states, format versions, and later gates. Use gate=live in the shared script for the exact strict set; later S03 adds repaired-order assertions to the continuing checks without weakening this set.
5. Add a Markdown/Markoff-shaped consumer fixture using opaque plain-text operations plus a separate sibling block map: preserve exact Markdown bytes, demonstrate optional transport routing, and show caret collapse independently of strict attachment loss. It must not require a Wave event log, engine-owned block tree, Qt widgets, or DocumentSession/file storage.

**Acceptance:**

- Strict installed WP1-WP5/WP7 pass in Debug/Release with no source includes, aborts, fabricated offsets or unsupported substitutions. WP5 includes fresh-process token replay; WP3 tests consumer deletion and WP7 tests library scope enforcement.
- The report and CI enforce the live capability set while listing unfinished ordering/pin/recovery gates explicitly; no result implies full stage-4 readiness.
- The independent Markdown fixture edits and transfers exact plaintext through consumer-owned composition and demonstrates caret versus strict-reference behavior.

**Verification:**

Run tools/verify-consumer-package.sh in strict live-position mode for Debug/Release and the installed Core ASan configuration; inspect child exits, named probe results and Markdown fixture assertions. Retain the capability/version manifest and report unavailable remote CI execution separately.
