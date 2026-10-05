# Accepted design and lifecycle decisions

**Updated:** 2026-10-04. All Q1–Q4 are resolved by the user's replies. Improve the existing native C++ engine incrementally. No backend choice remains open. Any new questions belong in ordinary conversation text for this user.

**Applied in:** [vnext-design-constraints.md](../../specs/vnext-design-constraints.md)
(D01), which turns each answer below into numbered constraints (C1–C14), the
release invariants a release must actually have (R1–R10), and the intended
public behavior (B1–B8). This file remains the record of *what the user decided*;
the constraints document is the record of *what that means for a task*. Where the
two could be read differently, the constraints document governs the
implementation and this file governs the intent.

## Q1 — Fresh start: resolved

**User statement:** “there is no existing data or stores or history to preserve. this is not in production. we can start totally fresh, however we want.”

**Selected policy:** freely redesign source/API/ABI, identities, algorithms, wire formats, and stores. No legacy decoder, importer, pre-vNext data archive, anchor translation from old stores, compatibility shims, or mixed-version collaboration is required. Existing source and evaluation artifacts remain useful regression evidence; they do not impose old behavior on the repaired engine.

**Plan changes:** remove E01/E02 entirely and their dependency edges; remove legacy fixtures/codec/shim requirements from V08/P01/P06/S04/A05/A06; replace porting requirements with independently compiling new examples in E03 and a fresh release dossier in E06. Retain task IDs E01/E02 in the registry change record so earlier references remain understandable. There are now 62 active tasks, including new A08.

**Isolation:** retain the requested breaking-change worktree for implementation and review. No production data migration or consumer porting deadline is needed. Master integration and publication remain separate explicit actions.

## Q2 — Explicit device retirement: resolved

**User statement:** “forever offline devices need to be explicitly retired. we can introduce a flag and interface asking after a few months.”

**Selected policy:** an enrolled device may remain offline indefinitely. Missing coverage/retained undo obligations continue to block GC. Only an explicit durable retirement transition changes membership; time, presence expiry, reminder display, and dismissal do not.

**Advisory reminder:** add `stale_for_review` and a device-review interface, with a configurable reminder age, enable/disable, and snooze. Proposed implementation default: 90 days, representing “a few months”; this is a revisable UI default, not a GC timeout. Keep enrolled and Remind later preserve membership. Review retirement displays the affected device/documents and consequences, followed by explicit confirmation.

**Returning devices:** unseen edits may exist on an offline device and cannot be exported by another machine at retirement time. A retired device must preserve its local branch and enter export/reconciliation/rejoin before new-epoch editing. Do not claim automatic lossless merging of arbitrary old-epoch edits after GC.

**Plan changes:** G01/G02 use the selected policy; A08 specifies the flag, reminders, confirmation interface, errors, and fake-clock tests. Reminder age is not a new open product-policy question.

## Q3 — Persistent undo and redo: resolved

**User statement:** “yes, undo/redo survive.”

**Selected policy:** restore both actor-local undo and redo across close/reopen/crash within configured step and byte limits. Preserve grouping/coalescing, cursor metadata, redo truncation, and durable retirement of trimmed history. Undo reverses the actor's edits and preserves peer changes.

**Plan changes:** L06 has one persistence route, with no session-only alternative. L07/G04/G05 and final acceptance test reopening with both stacks. Shared checkpoints do not replace private actor history. Physical GC must honor all still-enrolled actors' retained restoration obligations.

## Q4 — Improve the existing native C++ engine: resolved

**User statement:** “No, i want to improve the engine we have, not rewrite it. Let's improve what we have to the best possible engine it can be.”

**Selected policy:** retain and improve the current C++20 Buffer and IdList engines. Reuse their working SumTree/run/identity/undo machinery, file-transport separation, and Qt integration. Correct invariants and extend components in place with explicit models, regression evidence, and measured optimization. No Yrs vendoring/adoption, Rust-core rewrite, backend trial, or new replacement production engine belongs to this roadmap.

**Meaning of the fresh-start permission:** source/API/ABI and serialized formats may change as needed, and no old data migration is required. This does not authorize discarding the implemented engine or replacing it with another library. Existing working behavior and components are valuable foundations.

**Plan changes:** D01 now specifies accepted constraints instead of waiting for a backend answer. S02 maps ordering defects to precise incremental repairs inside existing integration helpers; S03 applies that map in Buffer/IdList. X10 reports native improvements and may use external engines only as isolated benchmark references. Algorithm or GC difficulties require deeper local design work, not substitution.

**Earlier Rust assessment:** [RUST_ASSESSMENT.md](RUST_ASSESSMENT.md) is retained as the requested language/interop background. Its earlier adoption recommendation is withdrawn and the proposed trial removed. It is not an implementation plan or an unresolved decision. This user direction also supersedes the evaluation's conditional replacement-backend recommendation.

## Implementation choices already within scope

- Keep Qt/C++ presentation, one file reference transport, plain text and IdList use cases.
- Start with full own/remote replay, then add checkpoints; SQLite is not a prerequisite.
- Use explicit outcomes, bounded queues, single engine ownership, and real checked durable writes.
- Keep unsafe physical reclamation disabled until a complete validated protocol exists.
- Use new versioned formats and reject incompatible input explicitly.
- Keep signing scope honest: trusted-folder prototype until separately scoped authentication.
- Verify Linux first and test other claimed platforms rather than inferring portability.
- Detect observable identity conflicts; ordinary shared-document copies must not copy private writer identity. Arbitrary identical disconnected device clones cannot always be detected before exchange.

## Applied, not just recorded (D01)

D01 has carried all four answers forward into a binding contract:

| Answer | Constraints | What a task must do |
|---|---|---|
| Q1 fresh start | C1, C5, C6, C7 | Design formats freely; version per artifact; reject unknown versions; add no decoder, importer, shim, or dual-version support |
| Q2 explicit retirement | C2, R9, R10 | Keep offline members enrolled; only explicit retirement changes membership; reminders are advisory only |
| Q3 persistent undo and redo | C3, R5, R7 | Both stacks restore across reopen within limits; no reclamation releases a retained obligation |
| Q4 improve the native engine | C4, C8–C14, R1–R4, R6, R8 | Repair in place behind a failing test and, for algorithms, a specification plus an independent model; optimize after correctness; no substitution layer, vendor, or parallel engine |

The acceptance check for that contract is executable:

```sh
python3 docs/roadmap/2026-10-reliability/check-scope.py --strict
```

It scans the task registry for a Rust rewrite, Yrs adoption, a backend
experiment, or legacy-data migration being *required* rather than prohibited, and
exits nonzero if any task's text needs a human decision. The current registry has 72 tasks after the Wave ask2 sequencing amendment. Run the command above
for the current classified-hit count; the review requires 0 needing review.

Documenting these decisions does not complete unimplemented tasks and does not
substitute for the technical design/model gates at S02/G01. Those gates still
require their specifications and executable models; C10 says so explicitly, and a
gate may not redirect work into a replacement engine.


## External consumer amendment — 2026-10-04

Wave's reply-anchor requests fit the selected engine and are integrated into
existing S/L/G/A/E tasks as [W1–W6](../../specs/vnext-external-position-contract.md).
External canonical logs require library-owned versioned token bytes and explicit
durable pins, with recovery-before-GC and independent object/device lifetimes.
Historical playback requires matching archived/projection state. This adds no
legacy-data conversion requirement: future published vNext tokens carry the
supported reader/translation obligation. Plain-text/IdList scope is retained;
Wave owns formatting and its canonical authorization/event-log protocol.
There is no new backend decision or automatic retirement policy.

## Early consumer integration amendment — 2026-10-04

The user authorized writing the accepted Wave ask2 sequencing into the worktree
roadmap. Provide an installed evolving API and continuous consumer probes early;
develop scoped typed live anchors after identity/boundary gates, without waiting
for full ordering or file recovery. [00 — consumer integration](00-consumer-integration.md)
records the single-executor route and exact eight-probe ownership. S04a separates
boundary specification/model from S02 ordering repair; S04 certifies their later
composition with real recovery. S05 and G-series retention/GC gates remain.

Core stays plaintext/IdList and transport-agnostic. Consumer block existence,
formatting, event logs, cross-block undo and transport are consumer-owned;
DocumentSession is an optional reference composition. Typed collapse information
supports caret and strict attachment policies without silent relocation.
No new product policy question is introduced. Preparing E07a/E07b candidates is
authorized roadmap work; pushing tags/packages, changing consumer repositories,
sending messages or merging master is not authorized by this planning instruction.
Approved published supported token bytes carry future reader/translation
obligations; pre-vNext experiments still require no preservation.
