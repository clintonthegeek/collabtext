# Decisions and remaining backend question

**Updated:** 2026-10-03. Q1–Q3 are resolved by the user's reply. Q4 remains pending an informed backend choice; willingness to install a toolchain is not a decision to replace the engine. Questions belong in ordinary conversation text for this user.

## Q1 — Fresh start: resolved

**User statement:** “there is no existing data or stores or history to preserve. this is not in production. we can start totally fresh, however we want.”

**Selected policy:** freely redesign source/API/ABI, identities, algorithms, wire formats, and stores. No legacy decoder, importer, data archive, anchor translation from old stores, compatibility shims, or mixed-version collaboration is required. Existing source and evaluation artifacts remain useful regression evidence; they do not impose old behavior on the replacement design.

**Plan changes:** remove E01/E02 entirely and their dependency edges; remove legacy fixtures/codec/shim requirements from V08/P01/P06/S04/A05/A06; replace porting requirements with independently compiling new examples in E03 and a fresh release dossier in E06. Retain task IDs E01/E02 in the registry change record so earlier references remain understandable. There are now 62 active tasks, including new A08.

**Isolation:** retain the requested breaking-change worktree for review and experiments. No production data migration or consumer porting deadline is needed. Master integration and publication remain separate explicit actions.

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

## Q4 — Backend route: pending after informed assessment

**Current user position:** interested in Rust benefits/drawbacks, potentially willing to install a toolchain, concerned about language stability. No rewrite or backend adoption has been chosen.

Read [RUST_ASSESSMENT.md](RUST_ASSESSMENT.md). Stable Rust is suitable for production use; the open issue is project fit and maintenance cost, especially persistent collaborative undo and GC.

**Current implementation route:** native C++20 remains provisional. D01 records the actual choice before backend-dependent work. Baseline evidence tasks remain independent of this answer.

**Recommendation:** authorize a small, isolated Yrs/C-ABI feasibility experiment before committing to the extensive native ordering/GC redesign. Test persistent undo/redo and indefinitely offline actors first. Adopt only if the experiment demonstrates the required semantics without maintaining a large private fork. A fresh custom Rust rewrite carries much of the same CRDT design burden as native C++ repair.

If a trial is chosen, add its concrete tasks/dependencies before P01 and revise backend-specific identity, operation, checkpoint, GC, and optimization tasks based on evidence. The task list's custom 128-bit actor IDs and internal operation fields are native design choices; an existing backend may require a different internal representation. Do not wrap the two models as though their histories were interchangeable.

## Implementation choices already within scope

- Keep Qt/C++ presentation, one file reference transport, plain text and IdList use cases.
- Start with full own/remote replay, then add checkpoints; SQLite is not a prerequisite.
- Use explicit outcomes, bounded queues, single engine ownership, and real checked durable writes.
- Keep unsafe physical reclamation disabled until a complete validated protocol exists.
- Use new versioned formats and reject incompatible input explicitly.
- Keep signing scope honest: trusted-folder prototype until separately scoped authentication.
- Verify Linux first and test other claimed platforms rather than inferring portability.
- Detect observable identity conflicts; ordinary shared-document copies must not copy private writer identity. Arbitrary identical disconnected device clones cannot always be detected before exchange.

D01 still supplies the complete fresh-format/API contract after the V08 inventory and Q4 decision. Recording Q1–Q3 here does not complete unimplemented tasks or substitute for the technical design/model gates at S02/G01.
