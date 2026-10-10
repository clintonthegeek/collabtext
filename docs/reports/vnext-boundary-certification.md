# vNext boundary recovery certification

**Task:** S04 — certify boundary anchors across repaired ordering and durable
recovery. **Candidate:** `733097c4d042a4c257939e07f7f85b114ff6b48a` on
`feat/fuguemax-placement`.

## Durable token and repaired-order cases

`TestSessionRecovery::external_token_roundtrip_reopen_durable_file_replay`
mints left/right CTPT tokens at byte 1 in seeded `ab`, preserves those original
bytes, durably appends concurrent whole operations from two producer actors to
FileOpStream, closes both writers, then replays into two fresh scoped engines.
Both engines recover `aXYb`; the same decoded token bytes resolve to offsets 1
and 3, without collapse, in both equivalent recovered states.

`TestSessionRecovery::wp6_split_pastes_survive_durable_replay_with_original_tokens`
mints both affinities in the scoped empty document, durably writes one whole
`abc` paste and one whole `XYZ` paste from different actors, closes both writers,
and recovers the operations into a new engine. Repaired S03 ordering yields
`abcXYZ`; the original empty-boundary token bytes resolve to 0 and 6 after
recovery. This is the WP6 repaired-order check over actual durable file replay.

The exact consumer case names have executable evidence:

| Case | Evidence |
|---|---|
| `external_token_roundtrip_reopen` | Durable close/replay test above; also covered by the retained CTPT reader fixture and installed consumer contract. |
| `boundary_affinity_ab_insert_X` | Installed Wave WP1 strict case, 16/16 strict Wave rows pass. |
| `scoped_empty_start_end_tokens` | Installed Wave WP4 strict cases and `tst_positions`. |
| `shared_token_concurrent_boundary_inserts` | Installed Wave WP1 strict case, plus `tst_positions` equivalent-replica checks. |
| `deleted_neighbors_explicit_collapse` | Installed Wave WP2 strict cases and `tst_positions` deletion/undo/redo tests. |
| `foreign_or_unsupported_token_is_explicit` | Installed Wave WP7 strict cases and `tst_anchor_codec` malformed/version/context checks. |
| `missing_list_id_is_not_an_end_anchor` | Installed Wave WP7 strict case and `tst_positions` list lookup/refusal tests. |

The production codec remains **CTPT/1**, the supported reader version. SHA-256
digests for retained byte fixtures and the repaired-order fixture are:

- `ctpt-v1-supported.json` (16 golden tokens, four token kinds):
  `7a4ad69c18e82a872f4e5e090636350918a7084e704bd4c002b640eafa0c49a5`
- `wp1-affinity-ab-insert-x.json`:
  `2aa964f0dc7da4fed2598c2e0ff85025c5c69e4a7094b049a47fb4fd28ca4ccd`
- `wp6-split-paste.json`:
  `f0bbbc9396bfdb048ab98fd87667b40baa073280332827a7a1149e4d59ee5c7f`

No token is reminted during recovery. Foreign/unsupported bytes remain typed
errors; uncovered causal state remains unavailable. Durable pins, checkpoint
floors, epoch translation and reclamation remain S05/G-series work. The installed
WP8 durable-pin probe remains `unsupported`; no pin or GC claim is made here.

## Verification

GCC 16.2.1; seven position/recovery suites (`tst_anchor_codec`, `tst_anchor`,
`tst_idlist_anchor`, `tst_positions`, `tst_sequence_semantics`,
`tst_session_recovery`, `tst_evaluation_regressions`):

- Debug 7/7: `build-roadmap-debug/verification/20261010T060612Z-2`.
- Release 7/7: `build-roadmap-release/verification/20261010T060623Z-2`.
- ASan/UBSan 7/7 with `ASAN_OPTIONS=detect_leaks=0`:
  `build-roadmap-asan/verification/20261010T060640Z-2`.
- Fresh targeted durable-token suite also passed Debug at
  `build-roadmap-debug/verification/20261010T060932Z-2`.

Installed strict live-position gate at the candidate revision, Core component:

- Debug: Wave 16/16 strict rows and Markdown 4/4 pass;
  `build-consumer-verify/debug-live-20261010T060707Z-2`.
- Release: Wave 16/16 and Markdown 4/4 pass;
  `build-consumer-verify/release-live-20261010T060729Z-2`.
- ASan/UBSan with `ASAN_OPTIONS=detect_leaks=0`: Wave 16/16 and Markdown 4/4
  pass; `build-consumer-verify/asan-live-20261010T060739Z-2`.

Each installed gate also passed its isolated relocated consumer build, all three
consumer tests, and the negative unsupported-component check. LeakSanitizer
cannot run under this environment's ptrace setup; sanitizer results use
AddressSanitizer and UndefinedBehaviorSanitizer with leak detection disabled.

## Limits

No boundary or recovery defect was observed in the listed matrix. The gates do
not certify long-term referent retention after reclamation, since durable pins
and GC are deliberately unimplemented. They also do not claim physical
power-loss or live cloud-sync behavior. WP8 remains explicitly unsupported.
