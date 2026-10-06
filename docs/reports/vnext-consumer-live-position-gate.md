# Installed strict live-position gate — W03

`tools/verify-consumer-package.sh --gate live` certifies the live position
probes through an installed, relocated consumer. It runs in Debug, Release and
the installed Core ASan configuration. Nothing here is satisfied by an
unsupported path, a substituted capability, a reminted token or a source-tree
include.

The gate is two independent consumers, both compiled against prefix B only:

- `consumer_wave_contract` — the Wave-shaped block map from W02, extended with
  the WP5 fresh-process replay case.
- `consumer_markdown_contract` — a Markdown/Markoff-shaped consumer written for
  this task. It shares no adapter, support header or expectation table with the
  Wave-shaped one; reaching the same library a second way is the point.

## The strict set

Strict rows are WP1–WP5 and WP7 plus the WP8 no-GC containment smoke. A strict
row that is anything other than `pass` fails the gate, as does a strict probe
that disappears from the table.

| Probe | Case | Capability | Outcome | Class |
|---|---|---|---|---|
| WP1 | `boundary_affinity_ab_insert_X` | scoped_positions | pass | strict |
| WP1 | `every_scalar_boundary_both_affinities` | scoped_positions | pass | strict |
| WP1 | `same_token_two_replicas_reordered_duplicates` | scoped_positions | pass | strict |
| WP2 | `caret_neighbour_delete` | scoped_positions | pass | strict |
| WP2 | `deleted_referent_explicit_collapse_undo` | scoped_positions | pass | strict |
| WP2 | `strict_selected_referent_delete_undo` | scoped_positions | pass | strict |
| WP3 | `consumer_deleted_containing_block` | consumer_block_map | pass | strict |
| WP4 | `scoped_empty_end_edges` | scoped_positions | pass | strict |
| WP4 | `exact_scalar_end_offend_nonbmp` | scoped_positions | pass | strict |
| WP4 | `distinct_list_indices_and_same_element_replica` | scoped_positions | pass | strict |
| WP5 | `token_canonical_bounds_ownership_and_purity` | position_codec | pass | strict |
| WP5 | `isolated_malformed_token_no_offset` | position_codec | pass | strict |
| WP5 | `fresh_process_original_token_replay` | position_codec | pass | strict |
| WP6 | `concurrent_paste_passages_contiguous` | passage_order | known_failure | pending S02/S03 |
| WP7 | `scope_axes_refuse_before_lookup` | scoped_positions | pass | strict |
| WP7 | `missing_list_cannot_be_end_mutation` | scoped_positions | pass | strict |
| WP8 | `no_gc_retains_deleted_items_for_undo` | gc_disabled | pass | strict |
| WP8 | `durable_pin_registration` | durable_pins | unsupported | pending S05/G01/G06/G08 |

16 strict rows pass; 18 rows are retained. Formats in play are CTPT/1
(experimental, unpublished), CTBO/1 and CTLO/1, recorded in every result row
alongside the fixture hash, seed, capability manifest version and child exits.

**No baseline allowlist entry was removed.** WP6 still reproduces the exact
`aXbYcZ` passage interleave and WP8 pins are still an absent capability, so both
exceptions stay bound to case ID, capability, format and owning task. Neither
counts as a feature pass.

## WP5 — fresh-process replay of originally minted bytes

The probe scenario is encode → persist → reopen in a fresh process → remote edit
→ decode → resolve. The case runs as two separately spawned worker processes:

1. `mint_replay` builds a text engine and a list engine, edits, mints a left
   boundary, a right boundary, a strict attachment and a list element, and writes
   six length-prefixed records through the consumer's own persistence code. The
   engines do not survive the process.
2. `replay` reads those records and builds **three new engines**: two
   physically independent `CrdtEngine` instances (actors 9 and 2, same scope) and
   one `IdListEngine`. It replays the recorded CTBO/CTLO operations, takes a
   remote edit from the peer, then resolves the *original* token bytes in both
   text engines and the list element in the list engine — after overwriting every
   input frame, so a token that still resolves cannot have been borrowing the
   buffer.

Observed: `text=aXb` in both replicas, `[1,2,2]` for left/right/strict in each,
`list_index=0`, two operations replayed. No position is reminted after reopen.

This is codec and token evidence. It is not crash recovery: there is no session,
no durable counter and no recovered document here, and the manifest keeps crash
recovery under L02–L08.

## WP2 — collapse is not an intact attachment

The strict table keeps the two answers apart. Deleting a caret's referent yields
`Collapsed` **with a usable offset** and a loss flag; deleting a strict
attachment's selected referent yields `Stale` with `detached` and no offset.
Undo restores the original bytes in both cases and redo repeats the original
outcome. A collapsed caret is never reported as an intact strict attachment.

## WP3 and WP7 — whose boundary this is

WP3 is the consumer's: a deleted block is `Gone` before any decode or item
lookup, for well-formed bytes and for malformed ones alike, and the original
frame is retained rather than discarded. WP7 is the library's: a foreign
document, stream, epoch or primitive refuses with its own typed error before
missing causal state is consulted, and a query on the foreign engines leaves
their text, version and notifications untouched.

## The independent Markdown consumer

`tests/consumer-smoke/markdown-contract/` composes the installed library the way a
different consumer would: the Markdown bytes, the block map and the transport all
belong to the fixture, and the engine only ever sees characters in one stream.
No Wave event log, no engine-owned block tree, no Qt, no `DocumentSession` or file
storage. Operations cross as opaque frames the consumer queues and routes without
reading a payload field.

| Case | What it establishes |
|---|---|
| `exact_markdown_bytes_round_trip` | A heading, a paragraph with a non-ASCII scalar and a two-line list item parse into three consumer blocks and re-render to the same 51 bytes. |
| `optional_transport_routing` | A peer opened with no local history receives three seed frames and reproduces the source bytes exactly; one local edit routes exactly one frame and converges; a document with no transport attached queues its own frame and leaves the attached wire untouched. |
| `caret_collapse_independent_of_strict_loss` | A non-adjacent edit moves neither reference. Deleting the referent then collapses the caret to offset 5 with `lost_right`, while the strict attachment becomes `Stale`/`detached` with no offset. |
| `consumer_block_deletion_is_gone` | A deleted block answers `Gone` for valid and invalid bytes, retains the original frame, and the consumer re-renders the document from the surviving blocks. |

The runner has no allowlist: every case is a strict pass or the gate fails.

## Verification

```sh
tools/verify-consumer-package.sh --mode debug   --gate live --components core
tools/verify-consumer-package.sh --mode release --gate live --components core
tools/verify-consumer-package.sh --mode asan    --gate live --components core
```

All three pass 3/3 CTest (`consumer_main`, `consumer_wave_contract`,
`consumer_markdown_contract`) with 16/16 strict wave rows and 4/4 Markdown rows,
against capability manifest v6. CTest passing is not the assertion: after CTest
the script re-reads both result files and refuses the run unless the wave result
really is `gate: live`, covers exactly WP1–WP5/WP7, has every strict row at
`pass`, agrees with its own recorded counts, and the Markdown gate passed every
case strictly.

Evidence: `build-consumer-verify/debug-live-20261006T015403Z-347884`,
`build-consumer-verify/release-live-20261006T015432Z-348930`,
`build-consumer-verify/asan-live-20261006T015505Z-349802`. The W02 baseline gate
still passes at its own 2/2 contract
(`build-consumer-verify/debug-baseline-*`); the Markdown fixture is registered
for the live gate only so W02's recorded evidence is unchanged.

Negatives actually executed, not assumed:

- A wrong strict expectation makes `consumer_wave_contract` fail and the gate
  exit 3 (evidence: `/tmp/opencode/w03-live-negative.log`).
- The retained-evidence checker fed a baseline result refuses it with
  `the retained wave result is gate='baseline', not 'live'` and exits nonzero,
  so a baseline run cannot be relabelled as live evidence.
- A deliberately wrong Markdown expectation and an injected `SIGABRT` each fail
  the Markdown runner with exit 1; the signal is recorded as 6.
- A Debug producer offered to a Release gate is refused before any build.

The runner caught a real defect in this task's own worker while it was being
written: the Markdown `exact_bytes` case emitted an unclosed JSON array, which
read as plausible output when eyeballed and was correctly recorded as an error.
That is the reason expectations stay hand-written and out of the worker.

## What this does not certify

- **Ordering (WP6).** Concurrent insertions at one position still interleave
  character by character. S02 specifies, S03 repairs. A later S03 adds its
  repaired-order assertions to the continuing checks; it must not weaken this set.
- **Reclamation.** The WP8 row is a no-GC containment smoke — deleted items and
  tombstones are retained and undo restores original tokens. That is evidence the
  inert default is still inert, not certified reclamation. Durable pins are an
  absent capability owned by S05/G01/G06/G08.
- **Recovery and persistence.** No durable store, no recovered counter, no undo
  across reopen. L02–L08 and L06.
- **Published formats.** CTPT/1 remains experimental and unpublished. Nothing
  here approves it as a durable token contract.
- **Remote CI.** The branch workflow invokes the same script for Debug/Release
  Core and, when Qt ≥ 6.8 is discoverable, Core+Qt. It parses and its shell is
  syntactically valid locally; execution on a remote runner stays pending until
  the branch is pushed.

No benchmark ran, and no claim here reaches Wave production readiness.