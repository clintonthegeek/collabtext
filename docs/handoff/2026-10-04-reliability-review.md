# Reliability workstream: independent review handoff

**Date:** 2026-10-04. **Branch:** `roadmap/reliability-v2`.
**Authoritative worktree:** `/home/clinton/dev/collabtext/.worktrees/reliability-v2`.
**Reviewed agent baseline:** `fac1ecbb428c92acca8ed805d6a45fcc42d32951`.
**Review fixes:** `b3f4f20` validation, `bbe8434` storage/identity.
**Verification:** Debug 62/62, Release 64/64 (both expensive suites included),
ASan/UBSan/LSan 62/62 with leak checking enabled and no suppressions. Strict
evaluation still exposes F4–F8 (exit 5).
Master source remains `046d375`. Main checkout docs are discovery copies.

Read the [progress review](../reports/2026-10-04-reliability-progress-review.md),
[external-position contract](../specs/vnext-external-position-contract.md),
[registry](../roadmap/2026-10-reliability/TASKS.json), and
[execution rules](../roadmap/2026-10-reliability/EXECUTION.md).
The [Wave reply](2026-10-04-reply-to-wave.md) is a saved draft, not a sent message.

V01–V08, D01, P01–P05, and L01 are complete, with review amendments to private
ownership, counter validity, parsing limits, UTF-8 source boundaries, operation
deferral, bounded overflow retention, and durable filesystem primitives.
P06a remains **in progress**: its model/order gate landed but engine identity
migration did not. Complete its new allocation-domain gate (step 2), then the engine-adoption
steps 3–6 and P06b. S01 is independently ready.
There are 63 tasks after splitting P06; the registry is authoritative.

Important distinctions to preserve:

- ActorId/private counters exist; legacy engine still uses narrow Lamport IDs.
- P04/P05 building blocks do not replace legacy decode_operation until P06b.
- L01 filesystem barriers are real; L02–L08 still own end-to-end store/save/reopen.
- F4–F8 remain five explicit known failures. Keep their markers until repaired.
- Physical GC/compaction remain inert until G06 certificates; do not re-enable
  reclamation to satisfy a performance assertion.
- W1–W6 are accepted requirements, not implemented token/pin guarantees. S02/G01
  must supply complete specifications and independent models before dependent code.
- Store rejected source records; engine diagnostic quarantine is bounded and
  drainable, not durable retention or acknowledgement.
- An error after publication is not automatic rollback; reconcile uncertain
  commit and partial append state. All private-state owners must close before
  reopening the same directory. Lease files remain present; never delete one to
  “release” a kernel lease.
- Formatting and Wave canonical-log authorization are consumer responsibilities.
  Library session durability is not a multi-block transaction or unlimited playback.

Run both plan checks when changing requirements:

```sh
python3 docs/roadmap/2026-10-reliability/validate-plan.py --ready
python3 docs/roadmap/2026-10-reliability/check-scope.py --strict
```

Review test commands/results and the recorded implementation patch/commit are in
the [artifact index](../reports/2026-10-04-review-artifacts/README.md). Linux
process-kill tests are not power-loss or cross-platform certification. P06a's
layout table is projected until production adoption and measurement.

Work only here on breaking changes. Do not modify Wave or message its maintainers
without a separate instruction. Ask any genuine questions in ordinary final
text; the user cannot use inline controls. Routine authorized implementation
does not need another permission request. Commit verified modular changes and
record actual evidence before advancing a task's status.
