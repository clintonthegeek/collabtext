# M06 recorded — and one correction to the premise

**From:** collabtext maintainers (`/home/clinton/dev/collabtext`)
**To:** Markoff maintainers
**Date:** 2026-10-31
**Re:** `docs/handoff/2026-10-31-from-markoff-m06-acceptance.md`

Your acceptance is recorded. Nothing on your side is needed.

## What was recorded

- `docs/handoff/markoff-core-response.md` now carries your paste-ready wording
  and the evidence table.
- `TASKS.json` M06 is `complete` with consumer revision `78cac549`, producer
  `16cc48f4969abc2940a7fb332b72af018075dab7`, package `0.2.0-alpha.5`, manifest
  v17, both prefixes, your two commands and the results (Debug 101/101,
  relocated standalone consumer 4/4).
- The row's scope is your scope, verbatim: acceptance of that candidate for
  Markoff's production path, not certification of producer gates, not Q06, and
  not merge authorization. Q06 stays `not_started`.

## One correction: X05 is no longer in progress

Your letter quotes the checkpoint tag's own words — "X05 remains in progress
(mixed-tombstone ≈1.18× against a 2× target, routed Release sweep, relocated
package gate)". That was true when `collabtext-perf-checkpoint-2026-10-21` was
cut. It stopped being true on 2026-10-30:

- X05 acceptance is met. Replacement and deletion splices now cross retained-run
  boundaries; five pinned Release pairs give an aged mixed 5-KB/50%-tombstone
  per-edit mean ratio of 0.263 (3.8×; worst pair 0.283) against a 2× target,
  with the 0%-tombstone control at 0.621 and no-history public rows at
  0.975–1.011 mean.
- Routed Debug, Release and leak-enabled ASan/UBSan each passed 103/103; the
  relocated installed-package gates passed in Debug, Release and ASan; the
  strict live gate passed in Release.
- A newer checkpoint is published: **`collabtext-perf-checkpoint-2026-10-30`**
  (`fd3c09e`), same package label `0.2.0-alpha.5` and manifest v17, installed
  API identical to your pin. Under our published rule (newest annotated tag on
  `master`) it is the supported pin.

Your M06 acceptance is of `16cc48f` and stands as recorded; it is not silently
extended to the newer tag. If you move the pin, the installed API is unchanged,
so it needs a rebuild and your own re-acceptance record, not new adoption work
on our side. That choice and its timing are yours.

## What we did not change

No consumer repository was edited for this record, and no merge or cutover is
implied. The `markoff-core-response.md` probe note about deriving the fixture
stream through `blockTextStreamName` remains history — your provider seams now
carry that coverage, so treat it as satisfied by your §"Coverage" list rather
than as an open ask.
