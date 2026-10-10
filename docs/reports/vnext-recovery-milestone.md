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

Fixture input (one UTF-8 payload per delivery): `first\nsecond\n`.

- SHA-256 of fixture input: `dbea9325179efe46ea2add94f7b6b745ca983fabb208dc6d34aa064623d7ee23`
- SHA-256 of lifecycle test source at this evidence revision:
  `f40c6cfe18f946153e592594b8209ee529042b17c502b44063e6deace44f1320`
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

- Debug: `./tools/verify-roadmap.sh debug 'tst_store_lifecycle|tst_session_recovery|tst_transport_commit_contract'` — 3/3 passed;
  evidence `build-roadmap-debug/verification/20261010T054745Z-2`.
- Release: `./tools/verify-roadmap.sh release 'tst_store_lifecycle|tst_session_recovery|tst_transport_commit_contract'` — 3/3 passed;
  evidence `build-roadmap-release/verification/20261010T054755Z-2`.
- ASan/UBSan: `ASAN_OPTIONS=detect_leaks=0 ./tools/verify-roadmap.sh asan 'tst_store_lifecycle|tst_session_recovery|tst_transport_commit_contract'` — 3/3 passed;
  evidence `build-roadmap-asan/verification/20261010T054810Z-2`.
- Bounded store fault corpus: `ASAN_OPTIONS=detect_leaks=0 ./tools/verify-roadmap.sh asan 'tst_store_fault_harness'` — passed;
  evidence `build-roadmap-asan/verification/20261010T054823Z-2`.

The initial sanitizer invocation with leak detection enabled ran all Qt cases
successfully, then LeakSanitizer exited with “does not work under ptrace” in
this environment. The successful sanitizer evidence therefore excludes leak
detection.

## Remaining matrix work

This report does not claim the complete L08 acceptance matrix. The controlled
missing/corrupt-segment sequence currently uses opaque payloads; the Buffer and
IdList cases cover intact copied-store recovery but do not yet run through each
partial-arrival and repair state. The editor manifest-before-seed arrival path
also lacks an integration regression that creates the two files in separate
arrival steps. Keep L08 open until those cases are executable and the complete
matrix is reviewed.

These are coverage gaps rather than observed data loss. Corrupt or incomplete
storage is reported as an error in the tested `replay_all` path. Segment
reclamation is still disabled.
