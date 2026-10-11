# Collabtext implementation orientation

Work only in `/home/clinton/dev/collabtext/.worktrees/reliability-v2`, branch
`feat/fuguemax-placement`. Root master contains unrelated work; preserve it.
Read `docs/roadmap/2026-10-reliability/DIRECTION.md`, then `DECISIONS.md` and
`EXECUTION.md`. `TASKS.json` owns task status/dependencies; numbered phase files
mirror its exact steps and acceptance. Completed tasks describe historical work,
not instructions to perform it again.

The 2026-10-11 user authorized deep native-engine changes, API additions and
optimization to finish Wave's Core requirements before Markoff starts. The active
contract is `docs/specs/wave-core-adoption.md`, supplementing the vNext specs.
Current adoption tag `collabtext-adoption-2026-10-11` is immutable provenance;
create a new candidate when fixes change the baseline. Do not move the old tag.

Keep the existing C++20 FugueMax engine, opaque operations, checked allocation,
causal refusal, scope validation and shared compensation semantics. Complete
specification and independent model gates before dependent ordering/reclamation
algorithms. Markers extend the text sequence; they are not a second sequence.
Archive obligations now include CTBO/0002 and marker CTBO/0003 and CTPT/2 in
addition to CTPT/1. The clean-start rule still excludes pre-vNext formats.

Use one executor. Do not edit Wave/Markoff, send external messages, merge master,
or publish without explicit authorization. Local commits and candidate tags are
part of this authorized work. DocumentSession is optional; Wave owns its log.
Physical GC/log pruning remains disabled during initial adoption.

Use affected warm correctness checks while iterating, then Debug, Release,
ASan/UBSan with leaks enabled, and relocated installed-package acceptance once
per arc. Empty CTest selections are failures (`--no-tests=error`). Unknown
changed-path routes fail. Never weaken an oracle or suppress a sanitizer failure.
Set `CCACHE_DIR` inside this worktree. Benchmarks are opt-in, never part of CTest;
measure separately before making performance claims. Keep the installed
`ALPHA_CAPABILITIES.md`, handoff, public contract and task registry truthful in
the same change. Distinguish producer-shaped fixtures from real consumer approval.
