# Installed consumer baseline — W02

The installed-only adapter runs 17 named cases across all fourteen W01 fixture
IDs and WP1–WP8. Debug/Release observe the same 15 passes, one exact ordering
failure and one unsupported durable-pin capability. The worker uses only public
headers from relocated prefix B and CollabText::Core; fixture files are data,
not source includes. Qt/zstd/OpenSSL discovery is disabled for Core consumers.

| Probe | Assertions executed | Outcome / limits |
|---|---|---|
| WP1 | ab→aXb left/right 1/2; every non-BMP scalar boundary; same original tokens on independent replicas, reversed causal arrival and duplicate delivery | pass |
| WP2 | deleting an unrelated neighbour preserves a caret; selected referent deletion explicitly collapses caret or detaches strict attachment; undo/redo reuse original bytes | pass |
| WP3 | consumer block deletion returns Gone for both valid and malformed bytes, before decode or item lookup; original bytes retained | pass |
| WP4 | scoped empty/end affinity, exact Unicode/off-end failures, text bytes versus list indices and same element token on a peer | pass |
| WP5 | canonical byte equality, every proper prefix, all 256 trailing values, explicit version/enum errors, input ownership and query purity; malformed worker returns no offset | pass; fresh-process replay added by W03 |
| WP6 | independent concurrent passage operations converge, but abc/XYZ becomes aXbYcZ and destroys both contiguous passages | exact known_failure; S02/S03 |
| WP7 | each foreign scope/type/epoch refuses before missing causal state; missing list lookup and end-anchor mutations refuse with unchanged state | pass |
| WP8 | retained list entry/tombstone and undo restoration of original text/list tokens with no reclamation; exported durable pin support absent | no-GC smoke pass; durable_pins unsupported; S05/G-series |

## Exact baseline exceptions

The checked `wave-contract/cases.json` table binds each exception to case ID,
capability, format and exact observed value; the result retains its hash.
`concurrent_paste_passages_contiguous` alone may report known_failure with
`{"text":"aXbYcZ","contiguous":false,"converged":true}` under CTBO/1.
`durable_pin_registration` alone may report unsupported with the named owning
S05/G01/G06/G08 capability. The installed manifest must also state that pins are
unimplemented. Any new offset, text, capability miss, signal, exit failure,
unknown case or missing fixture fails. Neither exception is a feature pass.

Reviewed expected observations are hand-written independently of worker output.
Workers cannot classify their own outcome. The Python runner compares exact JSON,
logs stdout/stderr, exit and signal, seed, producer revision, fixture and case
hashes, formats and installed manifest version. Top-level package results embed
that matrix and compiler/configuration information.

## Verification

- Initial `--gate baseline` failed with exit 4 (owning W02 unimplemented).
- Debug/Release relocated Core gates pass with the same 17 cases; CTest 2/2.
- Optional Qt installed gate passes, including all installed Qt header units.
  Its first run exposed a real export dependency defect: QtAdapter references
  FileTransport when enabled in the producer, but requesting only qt omitted that
  imported target. The package config now loads that dependency when needed;
  Core requests retain no optional discovery.
- A deliberately bad expectation fails with exit 1/error. An injected SIGABRT
  child fails with exit 1/error and recorded signal 6. Both retain JSON/logs under
  `build-consumer-verify/w02-negative/`.
- Passing a Debug producer as Release is refused with exit 3 before building;
  requested mode cannot relabel incompatible producer evidence.
- Workflow YAML/local matrix and shell syntax validate. Prepared branch CI invokes
  these same commands for Debug/Release Core and Qt when >=6.8 is discoverable,
  and uploads JSON/logs even on failure. Remote execution remains pending.

Evidence paths/commands are pinned in TASKS.json and the task log. No external
consumer edits, publication, benchmark, full recovery, persistent undo or durable
pin/reclamation certification. W03 separately enforces strict live-position cases.
