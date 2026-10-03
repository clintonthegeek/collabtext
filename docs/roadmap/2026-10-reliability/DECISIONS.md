# Accepted design and lifecycle decisions

**Updated:** 2026-10-03. All Q1–Q4 are resolved by the user's replies. Improve the existing native C++ engine incrementally. No backend choice remains open. Any new questions belong in ordinary conversation text for this user.

## Q1 — Fresh start: resolved

**User statement:** “there is no existing data or stores or history to preserve. this is not in production. we can start totally fresh, however we want.”

**Selected policy:** freely redesign source/API/ABI, identities, algorithms, wire formats, and stores. No legacy decoder, importer, data archive, anchor translation from old stores, compatibility shims, or mixed-version collaboration is required. Existing source and evaluation artifacts remain useful regression evidence; they do not impose old behavior on the repaired engine.

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

D01 still supplies the complete native-engine/fresh-format/API contract after the V08 inventory. All product choices are settled; documenting them does not complete unimplemented tasks or substitute for the technical design/model gates at S02/G01.
