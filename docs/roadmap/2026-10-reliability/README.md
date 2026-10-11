# 2026-10-11 Markoff adoption amendment

The user authorized facilitating Markoff's integration requests. M01–M05 in
`10-markoff-core.md` implement ordered full list identities, scoped terminal
counter-gap application and durable CTLO/0002/CTGP/0001 readers. The normative
extension is `docs/specs/markoff-core-adoption.md`; the response is
`docs/handoff/markoff-core-response.md`. Keep all earlier candidate tags immutable;
prepare alpha.4/manifest v16 and a separate Markoff candidate. Q06 and M06 remain
external production acceptance. Markoff has begun integration; earlier wording
that adoption has not started is historical. No consumer edit, message, push,
publication or master merge is authorized. One executor; model before gaps.

# Reliability roadmap

Execution branch: `feat/fuguemax-placement`. Worktree:
`/home/clinton/dev/collabtext/.worktrees/reliability-v2`.
Read [DIRECTION.md](DIRECTION.md), [DECISIONS.md](DECISIONS.md), then
[EXECUTION.md](EXECUTION.md). [TASKS.json](TASKS.json) is the authoritative
status/dependency registry. Numbered phase files mirror the registry exactly;
completed steps are historical procedure, not remaining work.

As of 2026-10-11, validation, native FugueMax ordering, checked protocol,
recovery, semantic/pin work and early installed-package gates are complete.
G03/G06 have unfinished broader certification work. Remaining reclamation,
packaging, performance and release tasks retain their exact registry status.
The active user-directed lane is [09 — Wave Core](09-wave-core.md): typed
zero-width text markers, retained archive readers, consumer-owned local undo
recovery, deterministic projection composition and a new tested candidate.
External Wave acceptance remains separate from upstream tests.

| Area | Task specification |
|---|---|
| Historical early adoption gates | [00-consumer-integration.md](00-consumer-integration.md) |
| Verification and models | [01-validation.md](01-validation.md) |
| Protocol and identity | [02-protocol.md](02-protocol.md) |
| Recovery | [03-recovery.md](03-recovery.md) |
| Sequence, positions and undo | [04-semantics.md](04-semantics.md) |
| Certified reclamation | [05-reclamation.md](05-reclamation.md) |
| Public API and components | [06-public-api.md](06-public-api.md) |
| Measured performance | [07-performance.md](07-performance.md) |
| Release certification | [08-release.md](08-release.md) |
| Current Wave requirements | [09-wave-core.md](09-wave-core.md) |

Select only tasks with completed dependencies, in the lane selected by DIRECTION.
Use one executor. Record changes, task status and executable evidence together.
Validate registry/Markdown consistency with `python3 validate-plan.py` and scope
with `python3 check-scope.py --strict` from this directory. Merge, publication,
consumer repository edits and external messages require separate authorization.
