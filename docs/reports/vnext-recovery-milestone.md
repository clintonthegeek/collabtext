# vNext recovery milestone evidence

**Status:** partial evidence recorded; L08 remains in progress.
**Branch:** `feat/fuguemax-placement`.
**Baseline:** `d1a7b28`.

## Separate device trees

`tst_store_lifecycle::physically_separate_replica_roots_replay_after_restart`
uses two independent temporary roots. Device A writes and seals two delivery
segments. Device B receives only the newer segment first; full replay returns a
gap and invokes no visitor. B then receives the older segment with a corrupted
byte; replay reports an error and invokes no visitor. Replacing that file with
the source copy restores complete replay of `first`, `second` in order. A fresh
reader actor on B replays the retained sequence again, modeling process restart
without sharing its private cursor with the original receiver.

`tst_store_lifecycle::buffer_and_idlist_operations_recover_from_copied_replica_tree`
creates real Buffer and IdList operations, encodes them with their respective
wire codecs, copies the producer's replica subtree to a physically separate
receiver root, decodes and applies each operation, then compares visible text
and list IDs. A second writer for the same producer actor is refused while the
first lease is held.

`tst_store_lifecycle::buffer_and_idlist_partial_arrival_is_incomplete_until_older_segments_arrive`
creates two operations and two sealed segments for each engine. It copies only
the newest segment to the receiver first. Both full replays report incomplete
coverage and call no visitor. After the older segments arrive, replay applies
each operation exactly once in order: Buffer recovers `seed!`; IdList recovers
IDs 101 and 202. The separate corruption test exercises the sealed segment
integrity path before any records are delivered to the visitor.

`tst_editor_recovery::manifest_arriving_before_seed_stays_incomplete_then_opens`
places a valid manifest in the sidecar while withholding its declared seed.
Opening reports the missing seed and does not enter collaborative mode. After
the matching seed file arrives, retrying open succeeds with the expected seed
text.

Fixture input (one UTF-8 payload per delivery): `first\nsecond\n`.

- SHA-256 of fixture input: `dbea9325179efe46ea2add94f7b6b745ca983fabb208dc6d34aa064623d7ee23`
- SHA-256 of lifecycle test source at this evidence revision:
  `b366d8f41f6ff67e9f8a7790e1c64f8ac9465a84f39e783b3538c0a434673b47`
- SHA-256 of editor arrival test source:
  `3090cf177a77a05ddc77548de3032236b4d4c6a6109af5255f852cfce042d29e`
- SHA-256 of the editor seed fixture `arriving seed`:
  `1b2529df1dc185756721e439da38092391ea1d36230b521f6960defa6700b977`
- Actor bytes are deterministic test actors 81, 82, and 83; stream is
  `lifecycle:text`, epoch 4.

The store fault corpus additionally covers missing and out-of-order segments,
corrupt and repaired segments, exclusive writer leases, bounded disk-full,
callback/cursor/checkpoint faults, and subprocess termination. Existing
`tst_opstream_replay` covers sender and receiver restart, callback retry, and
uncertain durable retry. `tst_segment_writer` covers open-tail to sealed
segments and failed seal recovery. `tst_session_recovery` covers full replay,
pending intent recovery, causal-log arrival, and actor-local undo around a peer
edit. Garbage collection remains disabled.

## Verification

Compiler: GCC 16.2.1. Selected suites:

- Debug: `./tools/verify-roadmap.sh debug 'tst_store_lifecycle|tst_session_recovery|tst_transport_commit_contract|tst_editor_recovery|tst_opstream_replay|tst_segment_writer'` — 6/6 passed;
  evidence `build-roadmap-debug/verification/20261010T055202Z-2`.
- Release: same six-suite selection — 6/6 passed;
  evidence `build-roadmap-release/verification/20261010T055215Z-2`.
- ASan/UBSan: same six-suite selection with `ASAN_OPTIONS=detect_leaks=0` — 6/6 passed;
  evidence `build-roadmap-asan/verification/20261010T055234Z-2`.
- Bounded store fault corpus: `ASAN_OPTIONS=detect_leaks=0 ./tools/verify-roadmap.sh asan 'tst_store_fault_harness'` — passed;
  evidence `build-roadmap-asan/verification/20261010T055317Z-2`.

The initial sanitizer invocation with leak detection enabled ran all Qt cases
successfully, then LeakSanitizer exited with “does not work under ptrace” in
this environment. The successful sanitizer evidence therefore excludes leak
detection.

## Coverage boundary and defects

The restart cases in `tst_opstream_replay` cover sender restart, receiver
restart, retry after missing/throwing callbacks, and uncertain durable append.
`tst_segment_writer` covers open-tail to sealed-segment transitions, failed
seal recovery, and subprocess interruption. `tst_session_recovery` covers
restart replay, durable local intent, deduplication, peer-preserving undo, and
logical state after missing causal data arrives. `tst_store_fault_harness`
covers exclusive leases and bounded write, sync, rename, cursor, checkpoint,
corruption, repair, disk-full, and process-termination faults. Together with the
separate-tree tests above, the required arrival/restart matrix is executable for
both operation codecs.

No data-loss or exactly-once defect was observed in these cases. This is a
filesystem simulation, not a live Syncthing/cloud-provider test or a physical
power-loss test. LeakSanitizer is unavailable in this runner, so sanitizer
evidence uses ASan/UBSan with leak detection disabled. Segment reclamation and
GC remain disabled.
