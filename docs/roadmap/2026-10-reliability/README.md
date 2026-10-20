# Current lane — 2026-10-19

Start **X01** in [07-performance.md](07-performance.md), using the measured
[d04f151 baseline](../../reports/2026-10-19-performance-evaluation.md) and
[target/verification contract](../../specs/vnext-performance-targets.md).
The revised X tasks supersede the old performance sequencing. The amendment
below records historical adoption authorization; it is not today's selected lane.

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

Execution branch: `feat/fuguemax-placement` in the isolated
`.worktrees/reliability-v2` worktree. Root `master` contains unrelated work and
must be preserved.
Read [DIRECTION.md](DIRECTION.md), [DECISIONS.md](DECISIONS.md), then
[EXECUTION.md](EXECUTION.md). [TASKS.json](TASKS.json) is the authoritative
status/dependency registry. Numbered phase files mirror the registry exactly;
completed steps are historical procedure, not remaining work.

As of 2026-10-11, validation, native FugueMax ordering, checked protocol,
recovery, semantic/pin work and early installed-package gates are complete.
G03/G06 have unfinished broader certification work. Remaining reclamation,
packaging, performance and release tasks retain their exact registry status.
The active user-directed lane is [07 — measured performance](07-performance.md);
X01/X02/X07a/X04a/X07/X07b are complete; X07b retained X07's exact-width
summary after the compact prototype missed cumulative byte-control thresholds.
X04 is next.
[09 — Wave Core](09-wave-core.md) and [10 — Markoff Core](10-markoff-core.md)
record completed upstream work and remaining external acceptance. Do not repeat
completed implementation or treat external acceptance as an optimization prerequisite.

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
| Wave adoption requirements | [09-wave-core.md](09-wave-core.md) |
| Markoff adoption requirements | [10-markoff-core.md](10-markoff-core.md) |

Select only tasks with completed dependencies, in the lane selected by DIRECTION.
Use one executor. Record changes, task status and executable evidence together.
Validate registry/Markdown consistency with `python3 validate-plan.py` and scope
with `python3 check-scope.py --strict` from this directory. Merge, publication,
consumer repository edits and external messages require separate authorization.
