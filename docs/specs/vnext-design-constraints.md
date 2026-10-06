# vNext design constraints

**Status:** binding on `roadmap/reliability-v2`. Accepted by the user; no further
product-policy question is open.
**Task:** D01. **Baseline:** `529b80a` (V08, closing M0).
**Depends on:** V08's [API inventory](../roadmap/2026-10-reliability/api-inventory.md)
and [DECISIONS.md](../roadmap/2026-10-reliability/DECISIONS.md).

This document says what the project is committed to building and, just as
importantly, what it is committed *not* doing. It is the answer to "what may a
later task change, and what may it not change?" Every constraint is numbered so
later tasks can cite it (`C7`) instead of re-arguing it.

It is not an algorithm specification. Where correctness depends on an algorithm —
ordering repair (S02), reclamation (G01) — the constraint here is only that a
written specification and an independent model must exist first. This document
deliberately cannot design those algorithms, and neither can a design gate that
has not been reached.

---

## 1. The four resolved user policies, carried forward

Each policy below is the user's own recorded answer. The "in practice" column is
what a task must do about it; the "forbids" column is what would contradict it.

### C1 — Fresh start: free API and formats, no legacy migration

*User:* "there is no existing data or stores or history to preserve. this is not
in production. we can start totally fresh, however we want."

- **In practice:** new source, ABI, wire, and store formats may be designed
  freely. Version numbers are per artifact. Obsolete and unknown versions are
  rejected explicitly rather than guessed at.
- **Forbids:** legacy-store decoders, importers, pre-vNext data archives, anchor
  translation from old stores, compatibility shims, mixed-version editing,
  porting deadlines, and "support both old and new" APIs.
- Fresh-start permission covers the pre-vNext experimental data. Once a vNext
  token format is published for external canonical logs, its supported readers
  and live-pin translations are retention obligations under
  [W1/W3](vnext-external-position-contract.md), independent of C++ layout.
- Existing source and evaluation artifacts remain *regression evidence*, not
  compatibility obligations. `tests/fixtures/behavior/` and the V02 findings are
  evidence; the defect they document does not bind the new behavior.

### C2 — Devices stay enrolled until explicitly retired

*User:* "forever offline devices need to be explicitly retired. we can introduce
a flag and interface asking after a few months."

- **In practice:** a missing or idle peer contributes missing coverage and keeps
  its retained-undo obligations. Only an explicit, durable retirement transition
  changes membership. A configurable advisory reminder (proposed default 90 days)
  with keep/snooze/dismiss may surface a stale device for review. Time passage,
  reminder display, and dismissal never mutate membership.
- **Forbids:** automatic eviction by timeout, "absence implies removal", and any
  GC rule that treats a silent peer as retired.
- A retired device must preserve its local branch and enter
  export/reconciliation/rejoin before new-epoch editing. The project does not
  claim automatic lossless merging of arbitrary old-epoch edits.

### C3 — Undo *and* redo survive reopen

*User:* "yes, undo/redo survive."

- **In practice:** actor-local undo and redo restore across close, reopen, and
  crash, within configured step and byte limits. Grouping/coalescing, cursor
  metadata, redo truncation, and durable retirement of trimmed history are
  preserved. Undo reverses the actor's own edits and preserves peer changes.
- **Forbids:** a session-only undo stack, a redo stack that is rebuilt empty, and
  treating "undo works until you close the app" as acceptable.
- Shared checkpoints do not replace private actor history.

### C4 — Improve the existing native C++20 engine

*User:* "No, i want to improve the engine we have, not rewrite it. Let's improve
what we have to the best possible engine it can be."

- **In practice:** the `Buffer` and `IdList` engines, their `SumTree`/run
  storage, identity and undo machinery, file-transport separation, and Qt
  integration are retained and repaired in place. Difficulties are resolved by
  deeper local design work.
- **Forbids:** vendoring or adopting Yrs or another CRDT, a Rust-core rewrite, a
  backend feasibility trial, a substitution abstraction layer, or a parallel
  replacement engine.

---

## 2. What is being retained

These components are the foundation the repair work builds on. "Retained" means
kept and improved, not frozen: the roadmap expects them to change internally.

| Component | Where | Why it is retained |
|---|---|---|
| `Buffer` + fragment/run storage | `src/crdt/Buffer.*` | The text engine. Its behaviour is specified and its tests are extensive; V08 froze nine fixtures against it. |
| `IdList` | `src/crdt/IdList.*` | The structural primitive; the only new CRDT primitive this project will ever add. |
| `SumTree` | `src/crdt/SumTree.h` | The indexed storage both engines share, with monoid summaries. |
| `SequenceOrder` + `Link` | `src/crdt/SequenceOrder.*`, `Placement.h` | FugueMax placement: an inserted run records its parent, side and right origin, and the list is the tree's in-order traversal. Replaced the fractional `Locator` in S03 (docs/specs/vnext-sequence-algorithm.md §0). |
| `UndoMap` + selective undo | `src/crdt/UndoMap.*` | Per-actor restoration state; C3 depends on it. |
| `Anchor` | `src/crdt/Anchor.*` | The basis for cursors, comments, and chat anchors; needs affinity and context fixes, not replacement. |
| `Clock` / `Global` | `src/crdt/Clock.h` | Lamport clocks and version vectors; need persistent identities and 64-bit counters. |
| File-transport separation (`OpStream` / `StreamSync`) | `include/collabtext/OpStream.h`, `src/crdt/StreamSync.*` | The four-method transport boundary is the right shape; only its receipts and error surface need work. |
| Qt integration | `src/ui/*`, `include/collabtext/CollabDocument.h` | Deferred rather than replaced (A03). |
| Test harnesses | `tests/TestSeed.h`, `TraceHarness.h`, `StoreFaultHarness.h` | Contributor-facing surface in their own right (U8 in the inventory). |

---

## 3. Intended public behavior

From the V08 inventory. This is what "working" means for a user of the library.

- **B1** A text editor over a shared document with local undo and redo (U1),
  where redo survives reopen (C3).
- **B2** A structural list keyed by stable ids (U2).
- **B3** Edits moving between machines through a shared folder (U3), where the
  durability claim matches what the code actually does — F9 must be fixed by
  L01/L02, not described around.
- **B4** One engine, many transports; a transport is a small object (U4).
- **B5** Presence and cursors that never allocate operation ids (U5).
- **B6** Anchors that survive concurrent edits, including comment and chat
  anchors (U6).
- **B7** Plain text and IdList in one Qt application (U7), delivered through one
  installable package and one documented consumer.
- **B8** A failure in development is replayable and reducible (U8).

Every mutation reports an explicit outcome: applied, duplicate, deferred, or
rejected — with rejection reasons that distinguish wrong-epoch, unresolvable
anchor, malformed, and unsupported-format input (P01 defines the type).

---

## 4. Format freedom, and the limits on it

- **C5** Each artifact family carries its own version: envelope, operation codec,
  checkpoint, membership, and anchors. A change to one does not silently
  invalidate another.
- **C6** A reader must reject an obsolete or unknown version explicitly. It must
  never guess, and it must never skip unreadable input in a way that advances
  coverage as though the data had been read.
- **C7** Because there is nothing to preserve (C1), an incompatible format
  change is permitted and is recorded as a documented hard cut. It is not
  accompanied by a pre-vNext compatibility layer. This clean-start rule does not
  cancel the supported persisted-token contract after publication (W1/W3).

---

## 5. How the engine may be repaired

These are the constraints that make "improve the existing engine" an engineering
discipline rather than an intention.

- **C8 — Narrow transformations.** A repair changes the smallest identifiable
  set of invariants inside existing components. A task that wants to rewrite a
  component must first show that a narrow repair is impossible, with a recorded
  counterexample (see C10).
- **C9 — Failing test first.** Every algorithmic or interface change begins with
  a failing regression that fails for the intended reason. A compiler error is
  not a failing test.
- **C10 — Specification and independent model before an algorithm.** Ordering
  repair (S02), reclamation (G01), and any comparable algorithm change require a
  written specification with explicit invariants plus an independent model or
  enumerator that does not call the production helper it validates. Where that
  artifact cannot be produced, the dependent task records the gap and stops; it
  does not improvise semantics.
- **C11 — Correctness before performance.** Optimization is measured work
  (X01–X10) that follows a passing correctness suite. An optimization that does
  not help the measured target is reverted or justified in writing.
- **C12 — No substitution layer.** The project will not add a pluggable CRDT
  backend, a feature-flagged alternative engine, or an abstraction whose purpose
  is to make an engine replaceable. One engine ships.
- **C13 — External engines are reference material only.** Another engine may be
  read for principles, and may appear in an isolated benchmark for comparison
  (X10). Its code is not vendored, adopted, linked, or shipped, and a comparison
  result is never a reason to switch.
- **C14 — Containment stays until it is replaced, not weakened.** V03 disabled
  physical reclamation; that stays until G06 provides a certificate. No task may
  re-enable an unsafe opt-in to make a benchmark look better.

---

## 6. Release invariants

These are the properties a release must actually have. They are stated as
checkable properties because "the docs say so" is not one.

- **R1** No lost acknowledged work: an operation a peer has acknowledged receipt
  of is not lost by any later sequence of restarts, partitions, and recoveries.
- **R2** Convergence: replicas that have observed the same operations hold
  identical visible state, for text and for IdList, including under
  reordering, duplication, and partition healing.
- **R3** Causal ordering: an operation is never applied before the operations it
  depends on, and a deferred operation is retried rather than dropped.
- **R4** Writer identity survives restart: a restarted replica does not reuse a
  previously issued identity, and a clone that would collide is detected or
  refused rather than silently accepted.
- **R5** Undo and redo: both restore within configured step and byte limits
  across close/reopen/crash, undo reverses only the actor's own edits, and
  obsolete retention obligations are never released.
- **R6** Anchor fidelity: a resolved anchor names the same content boundary it
  named before concurrent edits; affinity and context are explicit; an
  unresolvable or wrong-epoch anchor is an explicit outcome.
- **R7** Reclamation safety: nothing is reclaimed while any still-enrolled actor
  retains an undo, redo, anchor, or checkpoint obligation for it.
- **R8** Recovery honesty: unsupported or corrupt input is reported and
  quarantined, and it blocks advancing durable coverage rather than being
  skipped.
- **R9** Membership honesty: an offline member is still a member; only explicit
  retirement changes that, and advisory reminders never do.
- **R10** Durability honesty: a "saved" document survives a process kill after
  the call returns, and the documentation makes no claim beyond what the write
  path (real `fsync`, ordered renames, directory syncs) actually guarantees.

---

## 7. What a design gate may and may not do

- A gate (S02, G01) may demand a specification, an independent model, and
  enumerated evidence. It may not redirect the work into a replacement engine,
  a vendor, or a different language (C4, C12, C13).
- If a gate cannot be satisfied with the evidence available, the outcome is a
  recorded gap and a stop, not a chosen-by-the-implementer algorithm (C10).
- A gate's artifact is a design document. Executable models at a gate are
  substantive verification and must actually run (C10).

---

## 8. Conformance check

D01's acceptance requires checking that no task requires a Rust rewrite, Yrs
adoption, a backend experiment, or legacy-data migration. That is mechanical, so
it is mechanical:

```sh
python3 docs/roadmap/2026-10-reliability/check-scope.py --strict
```

The script scans every task's text for the sensitive vocabulary, matches on word
boundaries (so "trusted" is not a Rust rewrite and "migrated fuzz suite" is not
data migration), classifies each hit by its surrounding sentence, and lists the
hits that need a human decision. Its allowed-phrase list is deliberately visible:
an exception nobody can see is an exception nobody can review.

**Review result (2026-10-04):** 63 tasks scanned, 22 hits, all in prohibitive or explicitly
allowed context, 0 needing review. The one allowed phrase is E04's "rewrite
current architecture", which is a documentation rewrite.

Run this check whenever the registry changes, and treat a new unreviewed hit as a
blocker on the change that introduced it.

---

## 9. What this document does not do

It does not specify an algorithm, choose data structures for the protocol work,
set performance targets, or promise a date. It does not reopen any product
question — the four policies in §1 are the user's answers and this document only
records their consequences. Anything this document does not decide is decided by
the task that owns it, within these constraints.
