# Decisions and open questions

**Status:** Q1–Q4 awaiting user answers in the conversation. The user cannot use inline question controls; ask these as numbered questions in a final response. This file records proposals, not inferred approval.

## Q1 — Existing consumers and document stores

**Question:** Should the new release provide a migration tool while old/new versions edit separate stores, require live collaboration between old/new versions on the same store, or permit a clean break without old-store migration?

**Recommended:** migration tool plus separate stores. Keep the original API/store usable on master while consumers port to the isolated branch. Source/API adapters help porting, but do not make old/new protocols interchangeable. Migrating creates a new store/epoch and leaves the old sidecar/archive untouched.

**Why the decision matters:** identity widths, ordering semantics, operation schema, ack behavior, and anchor encoding change. Live mixed-version collaboration needs a protocol/semantic bridge, not a handful of overloads. It may not preserve both algorithms' ordering/undo expectations.

**If the recommendation is selected:** D01/P01/P06/P07/E01–E03 follow their current route. Package version is chosen later; architecture workstream “v2” does not dictate version 2.0.

**If live mixed-version collaboration is selected:** D01 must produce additional bridge tasks/spec and update dependencies before P01. The present migration-only plan does not claim to solve that requirement. Old/new writers must continue using separate stores until the bridge passes model/recovery tests.

**If clean break is selected:** retain E01 diagnostics/rescue and golden fixtures where they validate correctness; E02 migration delivery may be explicitly deferred. E05 must label the omission honestly.

**Migration detail requiring explicit acknowledgement:** the safe baseline imports reconstructed legacy visible content and durable annotations into fresh identities, archives legacy history, and starts a new active undo stack. It does not promise to preserve legacy undo/redo groups across changed ordering semantics. Q3 controls undo persistence for subsequent new-release sessions.

## Q2 — Long-offline devices and reclamation

**Question:** Should offline devices remain enrolled until explicitly retired, be automatically retired after a documented timeout, or should distributed GC be deferred while retaining all history?

**Recommended:** explicit retirement. A device may be offline indefinitely and block GC. It remains entitled to contribute old operations/undo while enrolled. Any retirement changes its later merge guarantees and must preserve/export unsynced work before rejoining a new epoch.

**Why the decision matters:** presence expiry is not causal coverage or consent to forget a peer. Safe GC requires defined membership, undo retirement, durable checkpoints, and anchor retention. Treating a silent peer as absent recreates the evaluated defects.

**If explicit retirement is selected:** G01/G02 specify epoch membership and a tested explicit transition. A current member with missing durable coverage blocks certificate construction. No automatic heartbeat eviction.

**If automatic retirement is selected:** a timeout value, ownership/authorization policy, and returning-device rescue/rebase policy must be added to G01; implement them before G02. Time-based removal still cannot delete the only recoverable copy of unsynced edits.

**If indefinite history/deferred GC is selected:** V03 containment remains. Checkpoints can accelerate recovery but covered history is not physically purged. G01–G03 may define future protocols, while G06–G08 are explicitly deferred; no bounded total storage claim.

## Q3 — Undo/redo across close and reopen

**Question:** Should local undo/redo survive reopening within configured step/byte limits, or may it reset at session close?

**Recommended:** survive reopening, bounded by both group count and retained bytes. This includes redo tails, coalescing, cursor metadata, and durable retirement of trimmed history. Undo remains local to the actor; peer changes are not undone.

**Why the decision matters:** local stacks become recoverable state. GC must account for remote retained groups and trimming/retirement; durable transport receipt alone is insufficient. A single coalesced edit can be huge despite a small step limit.

**If persistence is selected:** L06 records local history events and G04/G05 checkpoint private actor-local history without importing another actor's stack from a shared checkpoint.

**If session-only is selected:** L06 documents/tests reset and publishes a durable retirement decision. Remote obligations cannot be discarded merely because this editor closed. GC still follows G01.

## Q4 — Native C++ requirement versus a contingency backend

**Question:** Is retaining a native C++ engine a firm requirement, or may a mature Rust engine through a C interface be considered if the ordering/GC design becomes disproportionately complex?

**Recommended planning route:** retain native C++20. Keep the Qt presentation and file-sync transport regardless. No backend replacement is implemented by the existing task list.

**Why the decision matters:** S02 can select an improved native algorithm without changing language ownership; external alternatives affect build tooling, FFI, undo/persistence semantics, licensing, and the consumer surface. A comparison benchmark is not authorization to substitute a backend.

**If native C++ is firm:** S02 stays native; X10 external comparisons are informational only. An unsolved native design gate is reported explicitly rather than replaced quietly.

**If a contingency is allowed:** S02 may propose a bounded adapter experiment using the same fixtures and traces. Before adopting it, add a concrete replacement design/task plan and evaluate migration/retention behavior. Existing native plan tasks must be revised where architecture actually changes; do not execute two incompatible implementation routes.

## Implementation choices that do not need another user decision

These choices follow the requested evaluation/recommendations and can proceed once relevant dependencies exist:

- Start recovery with complete own/remote replay, then add checkpoints; do not require SQLite.
- Make the core C++20/Qt-free, with optional file transport and Qt components.
- Use persistent 128-bit actor identities, checked uint64 counters, sparse wire vectors, distinct stream/epoch context, and separate Buffer/IdList operation values.
- Use typed decode/apply/store errors and per-stream receipt/commit semantics; remove destructive use of scalar transport fences.
- Make GC conservative until a full protocol exists; return explicit unresolved/wrong-epoch anchors and history-unavailable results.
- Prefer one engine owner and bounded batches before introducing worker threads.
- Keep file transport as the reference implementation and plain text/IdList as the only CRDT primitives.
- Keep profile signing explicitly unsupported/trusted-folder-only until a separately scoped authentication feature exists.
- Claim Linux verification first; document and test other supported platforms rather than inferring portability.
- Do not promise arbitrary cloned-device identity detection: shared document stores do not include private device actor state; copied device state must be explicitly forked. Detect observable duplicate ownership and conflicts without guessing a merge winner.
- Counter exhaustion is a typed failure before mutation, not wraparound or silent ID reuse.

## Decision record template

For each Q:

- User answer and date:
- Exact selected policy:
- Migration/retirement exceptions acknowledged:
- Tasks/specifications amended:
- Required tests added:
- Status: pending / resolved.

D01 is complete only when Q1–Q4 are resolved and any alternate-route tasks are concrete. Technical algorithm and GC protocol decisions are captured separately by S02/G01 and do not masquerade as user product-policy answers.
