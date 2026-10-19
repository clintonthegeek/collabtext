# Wave ask2: roadmap amendment and executor handoff

**Date:** 2026-10-04. **Planning baseline:** `4983bc4`.
**Branch:** `roadmap/reliability-v2`.
**Worktree:** `/home/clinton/dev/collabtext/.worktrees/reliability-v2`.

The user authorized putting the accepted Wave ask2 sequencing into this worktree
roadmap. Read the [consumer lane](../roadmap/2026-10-reliability/00-consumer-integration.md)
and [registry](../roadmap/2026-10-reliability/TASKS.json), then execute one eligible
task at a time using [EXECUTION.md](../roadmap/2026-10-reliability/EXECUTION.md).
The superseded input is the earlier full-recovery-first sequencing, not the
native-engine design, independent review or durability/undo/pin/GC requirements.

There are now 72 tasks. Nine were added: W01–W03, A02a, A06a, S04a–S04b,
E07a–E07b. All new tasks remain `not_started`. All existing status, completed
implementation evidence, and P06a in-progress gate evidence are retained.

Preferred route:

1. Finish P06a steps 3–6 using the landed identity-allocation contract. Its
   engine adoption remains unfinished; do not mistake the allocation model for
   production enforcement or restart counters per engine constructor.
2. A01 → A02a → A06a: independent targets, implemented public alpha facades,
   actual install/export and a relocated external consumer.
3. W01 → W02 → E07a: capture the eight probes in this repository, enforce an
   honest installed baseline, and prepare a package alpha with exact pin commands.
4. S01 → S04a, then P06b → S04b: boundary fixtures/specification/independent model,
   real operation codec, scoped typed live positions and public token encoding.
5. W03 → E07b: strict installed live probes and a Markdown-shaped consumer,
   then a position-alpha candidate and supported-token manifest.
6. S02 → S03, P07 → P08 → L02–L08: ordering repair and real lifecycle recovery.
7. S04 → S05 → S06 → S07, then remaining registry tasks: final anchor composition,
   persistent pins/history, combined model checking, certified GC, complete Qt/API
   certification, measured optimization and release acceptance.

A01/W01/S01 can proceed independently if the running identity task has a concrete
blocker. The registry order is the safe serial priority; phase headings are not
a requirement to finish recovery before packaging or anchors. Pure build/header
packaging does not wait for P06a, while live positions do require stable adopted
item identities, a boundary model and P06b replica/replay encoding.

The boundary model consumes supplied exact ordered-item/visibility states and
does not invent the complete future ordering algorithm. Later S02 composes its
ordering interpreter with that model. Live-position tests cannot close F8 or
prove actual crash recovery. Strict address loss/collapse stays observable while
carets can use defined collapse; containing block existence stays consumer-owned,
and wrong-context rejection stays engine-enforced.

E07a/E07b prepare concrete candidate SHAs, proposed immutable tags, checksummed
artifacts, manifests, release notes and reproducible consumer commands. External
publication needs a separate explicit user instruction after that preparation.
Do not report a local candidate as fetchable or remote CI as executed. A full
remotely available Git commit SHA is a valid reproducible consumer pin.

No Wave/Markoff edit, external message, published tag/package or master merge
was performed by this amendment. Core remains native C++20, plaintext/IdList,
transport-agnostic; file sessions and Qt adapters are optional. Persistent undo
and redo, explicit retirement, supported token-reader obligations and disabled
physical GC until G06 remain binding.

Verification for the amendment uses the plan validator (including new transitive
consumer-gate/probe-owner checks), strict scope checker, local-link/diff checks,
and deliberate invalid-graph tests. It does not claim new engine test results
or implementation completion. The older independent review's runtime evidence
remains evidence for its own baseline.
