# S05 external anchor retention milestone

**Candidate:** `b4501391e7c117a0dc3c34bf95ff9f0d559f5015`
**Status:** S05 complete; physical garbage collection remains disabled.

## Delivered

- Added the headless `AnchorPinRegistry` public API. A consumer appends canonical
  pin events through its persistence callback; the registry advances only after
  durable success. Recovery applies contiguous events idempotently, exposes the
  projected frontier and exact live pins, and rejects a gap or conflicting
  event. Pins are keyed by immutable consumer reference IDs, independent of an
  author engine/device. Release is terminal for that ID; a restored reference
  uses a new ID.
- Checked CTPT bytes against the requested document, stream, primitive and epoch
  before pin registration. Comment and chat codecs preserve scoped token bytes
  and require an expected scope when reading them. They retain unresolved bytes
  for later operation replay; their old actor/counter anchors remain explicitly
  legacy and unscoped.
- Changed `DocumentSession::edits_since` to return a typed result. Queries below
  the retained floor report `history_unavailable`; future frontiers report
  `awaiting_state`. The current session does not advance its floor or discard
  history. CollabEdit now handles these outcomes through its existing error
  signal.
- Added installed Wave WP8 assertions for registration, lookup, release,
  canonical replay and recovery-frontier gating. Added unit coverage for append
  failure/uncertain commit, sequence gaps, scope errors, event retry concurrency,
  release replay, terminal tombstones, and original comment-token resolution
  after deletion, undo, author-engine destruction and fresh-replica replay.

## Verification

All commands used GCC 16.2.1. LeakSanitizer is unavailable in this runner, so
ASan/UBSan commands disabled leak detection.

- Required S05 Debug set:
  `./tools/verify-roadmap.sh debug 'tst_anchor_retention|tst_comment|tst_chat_message|tst_edits_since|tst_persistent_undo|tst_session_recovery|tst_anchor_codec'` — 7/7, evidence `build-roadmap-debug/verification/20261010T072851Z-2`.
- Required S05 Release set — 7/7, evidence `build-roadmap-release/verification/20261010T072913Z-2`.
- Required S05 ASan/UBSan set with `ASAN_OPTIONS=detect_leaks=0` — 7/7, evidence `build-roadmap-asan/verification/20261010T072943Z-2`.
- Change-routed Debug suite — 84/84, including public-header checks and
  `tst_editor_recovery`; installed Core package gate passed. Evidence
  `build-roadmap-debug/verification/20261010T073135Z-changed-511t5876` and
  `build-consumer-verify/debug-package-20261010T073323Z-2175`.
- Installed live gate at this candidate passed in Debug (Wave 16/16 strict,
  Markdown 4/4), Release (16/16, 4/4) and ASan/UBSan (16/16, 4/4):
  `build-consumer-verify/debug-live-20261010T073355Z-2`,
  `build-consumer-verify/release-live-20261010T073407Z-2`, and
  `build-consumer-verify/asan-live-20261010T073418Z-2`.
- `tools/check-public-headers.sh`, `validate-plan.py`, and `git diff --check`
  passed.

## Limits carried forward

The registry is a consumer-log projection, not a built-in durable database. The
consumer must replay it and prove its projection frontier before allowing a
collector to run. The registry does not perform reclamation, checkpoint
translation or membership retirement. G01/G06 must include live pins,
incomplete projection frontiers and translations in certificate eligibility.
Live token/reference metadata grows with retained references and canonical pin
events; G08 owns measurement. Current pins preserve current resolution, not
unlimited historical playback. Historical projections still require their own
matching operations/checkpoint and return `history_unavailable` below their
floor.
