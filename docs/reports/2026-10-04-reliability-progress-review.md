# Independent review of reliability progress and Wave integration

**Date:** 2026-10-04. **Reviewed baseline:** `fac1ecbb428c92acca8ed805d6a45fcc42d32951`,
branch `roadmap/reliability-v2`. **Original evaluated master:** `046d375`.
**Scope:** progress since kickoff `cbe72ea`, focused production review, new failing
regressions and repairs, plan/API consistency, and external-consumer viability.
**Evidence:** [review artifacts](2026-10-04-review-artifacts/README.md).
**Review implementation commits:** `b3f4f20` (validation), `bbe8434` (storage/identity).
**Consumer reply:** [draft statement to Wave](../handoff/2026-10-04-reply-to-wave.md).

## Judgment

The work is making meaningful progress in the direction the owner selected:
repairing the existing C++ engine. The agent established useful reproducibility,
fault injection, identity/validation foundations, and explicit design gates.
It did not substitute another engine. Continue this workstream, with the review
corrections here applied before building further lifecycle work on it.

The branch is **not yet a reliable released CRDT engine or ready for Wave's
stage-4 integration**. Some foundations were incorrectly described as broader
guarantees, executor orientation was stale, and important error/ownership paths
were absent from otherwise green tests. Independent baseline runs passed 62/62
CTest executables in both Debug and Release, but the new review regressions
exposed 16 failing baseline-defect test slots. Those failures are repaired in this review; five
original expected failures remain deliberately visible for later roadmap tasks.

Wave's external-position requirements are viable extensions of the existing
anchor/retention/public API work. They add a real persisted-data contract and
external pin lifecycle that the earlier task descriptions did not fully cover.
They warrant explicit acceptance cases, rather than an assurance that “S04/S05
already cover it.” Formatting remains outside this plain-text release scope.

## What has actually landed

The agent's changes since kickoff span approximately 14,660 inserted lines
across 55 production/build/tool files, in addition to documentation and test
artifacts. This review examined focal production paths and their contracts;
it is not a line-by-line proof of every test/model or of the future algorithms.

| Work | Assessment | Material limits |
| --- | --- | --- |
| V01–V08 | Strong verification foundation: reproducible modes, explicit failures, inert unsafe GC, seeded traces/reduction, corrected simulator, honest benchmarks, crash/fault seams, behavior inventory | Deterministic samples and finite fixtures do not prove general convergence. Benchmark assertions establish correctness of those traces, not broad competitive performance. |
| D01/P01 | Useful written native-engine policy and API/protocol gates | Corrected the guide's offset-free resolution result, namespace, move-only examples, allocation/diagnostic claims, and undefined reentry policy. Most target APIs remain future work. |
| P02 | Persistent ActorId, checked reservation before exposure, private state and writer policy | Needed exclusive ownership of the entire private counter store. ActorId is not yet used by the engine. Current private state is POSIX-oriented and has explicit provisional name bounds. |
| P03 | Separates summary aggregation from actual coverage intersection | `Global::meet()` intentionally remains a nonzero-min summary fold. `coverage_intersection()` is the watermark API. Old probes calling meet do not test the new contract. Unsafe destructive GC remains disabled. |
| P04/P05 | Strict parser building blocks, Unicode checks, typed application outcomes, semantic validation and checked local mutation | Legacy operation codecs remain until P06b. Context/epoch cannot be checked in operations that still carry the legacy identity. Callback batching/reentry remains A04. Review found additional limits/validation failures. |
| P06a | Independent counter-first actor ordering gate is a good prerequisite | **In progress**, not a migrated engine. The 24-byte identity is modeled; widened Fragment/Summary sizes are projected. Real layout/performance evidence is still required after adoption; the added allocation-domain gate must precede it. |
| L01 | Appropriate filesystem seam with observable actual syscalls and process-kill controls | Required fixes before reuse. These primitives are not yet connected to SegmentWriter, StreamSync, sessions, or real save/reopen. L02–L08 and end-to-end F9 remain. |

Status is now consistent in AGENTS, CLAUDE, the worktree README, roadmap README,
and EXECUTION. There are **63 active tasks** after P06 was split. V01–V08, D01,
P01–P05, and L01 are complete; P06a remains unfinished; S01 is independently ready.

## Reproduced defects and repairs

No evaluation regression was removed to make these fixes pass. Historical tests
were updated only where they contradicted the stronger ownership or checked-close
contract: a simulated restart closes the old private-state owner, fresh counters
start at 1, and atomic-write traces include checked close before publication.

| Finding | Why it mattered | Repair and executable evidence |
| --- | --- | --- |
| Two private-state owners issue from the same counter file | Different document leases did not prevent independently loaded stream counters issuing duplicate operation IDs | Move-only ReplicaState holds a kernel lease on the private directory until close/destruction. `mutable_private_state_has_one_owner_even_for_different_documents`; closed/moved-from owner controls. |
| Duplicate identity directives and injectable stream names | Ambiguous files could silently replace identity/counters or persist directives through a name containing whitespace/newlines | Strict duplicate rejection and bounded ASCII stream names. `duplicate_identity_directives_are_rejected`, `duplicate_stream_directives_are_rejected`, `stream_names_cannot_inject_private_state_directives`. |
| Fresh counter value zero | Validator treats zero as invalid identity, while P02 initially issued it | Start at 1, refuse explicit zero, retain exhaustion sentinel. `fresh_reservations_never_issue_the_zero_identity_sentinel`. |
| Literal JSON text escaped the decoded byte limit | A string without escapes could allocate beyond max_text_bytes; trailing literal chunks also bypassed checks | Check limits before every literal/escape append; keys use their own limit. `literal_and_mixed_strings_obey_decoded_byte_limit`, `object_keys_have_their_own_decoded_byte_limit`. |
| Duplicate-key lookup scanned every earlier key | Large bounded objects incurred quadratic duplicate-check work | Ordered key set preserves object values while making lookup logarithmic. This is a complexity repair, not a measured latency claim. |
| Local edits split UTF-8 scalar encodings | Valid replacement text could still produce invalid document bytes at an interior source offset | Validate every source range endpoint with indexed boundary lookup before mutation. `local_utf8_interior_boundaries_are_refused_without_mutation`. |
| Actor head-of-line queue bypassed semantic validation | Once one operation was deferred, a malformed later operation from that actor entered the queue and could eventually apply without its original validation | Validate before queue/HOL gating. `blocked_replica_does_not_bypass_semantic_validation`. |
| Counter value counted as bytes | Numerically large valid timestamps falsely exhausted the pending budget | Count actual operation storage only. `pending_byte_estimate_does_not_depend_on_counter_value`. |
| Deferred overflow moved into unlimited quarantine | The “bounded” queue still admitted unbounded retained copies | Separate bounded diagnostic quarantine, checked budget changes and drain API; caller keeps rejected source record. `quarantine_does_not_bypass_the_default_pending_operation_bound`, `overflow_retention_is_bounded_and_drainable_for_both_primitives`. |
| IdList lookup miss fabricated end | The consumer could not distinguish missing ID from sentinel placement | Add checked `try_anchor_of`; legacy wrapper remains provisional until S04/A02. `missing_idlist_lookup_has_a_checked_not_found_result`. This does not solve text affinity or stable token encoding. |
| Injected short “success” was trusted | write/replace/append could claim complete publication after writing fewer requested bytes | Check exact counts. `write_all_refuses_a_short_success_from_an_injected_writer`. |
| More than 64 progressing write calls refused | Arbitrary attempt count failed a valid large write split into small chunks | Continue while making progress; retry EINTR and fail zero progress/errors. `progressing_short_writes_are_not_limited_to_sixty_four_calls`. |
| New append name had no directory barrier | File data sync alone could not confirm persistence of the created name | First sync after every open also syncs parent directory, including relative basenames. `first_durable_append_syncs_the_created_name`. |
| Shared fixed temporary and ignored cleanup/close | Competing attempts could overwrite/remove another attempt's temporary; failures lost cleanup context | Unique O_EXCL sibling temporary, checked close, preserve cleanup failure context. `simultaneous_replacements_do_not_share_a_temporary`, `temporary_close_failure_does_not_publish`, `rename_failure_preserves_cleanup_failure_context`. |
| Post-rename failure falsely implied unchanged target | A directory-sync error occurred after the new contents were already visible | Return `commit_uncertain` and require reconciliation. `publication_before_directory_sync_failure_is_reported`. |
| PID/O_EXCL lease could be stolen during empty-record window | A contender treated an incomplete live lease as stale; stale cleanup could unlink another owner's file, and missing directories could recurse | Persistent-inode kernel lease, RAII descriptor ownership, PID diagnostic only; never unlink the lease. `a_locked_lease_with_an_empty_record_cannot_be_stolen`, record-write failure, move/release, and process-death/same-inode recovery controls. |

The first three red test runs retained in the artifacts expose four validation failure
slots, ten identity/storage failure slots, and one overflow-retention failure
slot. A later state-link regression exposed one more baseline defect: existence
lookup followed a dangling or looping private-state link and treated it as a fresh
store, replacing the name and minting a new actor. Inspecting the name without
following it now preserves the entry and returns an error.
`unreadable_existing_state_link_is_never_replaced` checks both cases.

Broad review testing also caught an error introduced by the review itself: the
first UTF-8 boundary patch used SumTree Bias::Right, which selects the preceding
run at an exact endpoint. Valid edits at fragment boundaries were refused.
Bias::Left selects the first run ending strictly after the offset and skips
zero-width tombstones. The corrected patch passes the original integration
assertions and `valid_scalar_boundaries_in_fragmented_text_remain_editable`; the
failed broad run and positive-control red run are retained, not hidden.

Additional passing controls strengthen the repaired behavior; not every
row above represents a separately counted red slot.

## Durability, ownership, and performance implications

The Linux file barrier sequence is write → file sync → checked temporary close →
atomic replacement → containing-directory sync. File data sync alone does not
sync the directory entry. [Linux fsync documentation](https://man7.org/linux/man-pages/man2/fsync.2.html).
If the last barrier fails, the new file can already be visible. Treating that as
rollback risks duplicate effects and incorrect recovery. Append failures may
leave partial bytes; L02 framing and recovery must reconcile them before declaring
a durable record/coverage advance.

Local process ownership now follows an OS lock held by the open file description,
which also blocks independent opens in the same process and releases when all
owning descriptors close. Persistent lock files are not stale ownership.
[Linux flock documentation](https://man7.org/linux/man-pages/man2/flock.2.html).
These locks are advisory; all cooperative local writers must use the protocol,
and shared/network-filesystem semantics need verification. This is a **local
process lease**, not device retirement or proof against copied disconnected
identities. Returning offline devices remain enrolled until explicitly retired.

Private counter persistence now uses the same checked atomic replacement
primitive, and identity fork refuses active writers. Publication failure poisons
an uncertain fork owner until reopen. OS randomness reads are checked directly.
No engine memory/storage architecture was replaced. Provisionally, P02 stream
tokens allow 128 ASCII bytes; lease keys allow 80 document bytes and 32 stream
bytes with collision-free hex path encoding. L05/A02 must document final key
policy; consumers should not infer a general string-ID limitation from this
private-file implementation.

Performance changes here target complexity and bounded memory, with no new
throughput claim. Literal-string bounds are enforced before allocation; duplicate
key lookup moves from quadratic scans to logarithmic lookup; UTF-8 endpoint
validation uses indexed seeks without copying the whole document. Quarantine
defaults to a separate 4096-operation/8-MiB budget. It is bounded diagnostics,
not a durable log. Later budget accounting must include actual allocations and
retention overhead, rather than call estimated payload bytes total RSS.
Current GC containment still permits tombstone growth; live external pins add
their own potentially indefinite metadata obligations. X-series measurements
must distinguish those obligations from eligible reclaimable state.

The review checks Linux behavior only. Windows path/open modes and obvious
durable-file branch issues were corrected, but the whole identity/platform path
is not verified there. `_wopen` has a distinct wide-path interface; compilation
and error guarantees cannot be inferred from a POSIX run.
[Microsoft runtime documentation](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/open-wopen?view=msvc-170).
Subprocess death tests do not certify storage-device power-loss behavior or
Windows/macOS support. A07 remains responsible for claimed platform evidence.

## Wave viability and plan integration

Reviewed the Wave handoff, accepted ADR 0004, protocol 0001, and stage-4 planning.
Wave is at protocol/schema planning with an append-only canonical conversation
log, compensating undo, and a consumer-owned Document seam. Its reuse preference
is conditional, not an unconditional adoption decision. Its whole-block fallback
lets product work continue while engine gates remain incomplete.

The architectural fit is good: block structure separate from one text engine per
block; headless native embedding under Wave's own interface; selective undo as
new operations. IdList's uint64 payload is not a direct arbitrary string block-ID
codec. Keep consumer structure separate or use an explicit collision-checked map.
The unfinished CollabDocument stub is not the intended supported session seam.

[W1–W6](../specs/vnext-external-position-contract.md) now specify library-owned
versioned tokens, actual typed resolution offsets, scoped boundary affinity,
external pin lifecycle, historical playback, formatting scope, and installed
consumer gates. Thirteen existing tasks gained ordered steps, file scope,
acceptance, verification, and requirement IDs. Task dependencies and incomplete
statuses were preserved. The independent sequence and GC models must prove the
necessary rules before production algorithms proceed.

The important new obligations are:

- Published supported vNext token bytes are a future persisted-data contract.
  The clean start for existing experimental stores does not permit silently
  invalidating consumers' future canonical log records.
- External storage and decode are not pin registration. Registration/release
  must be durable, idempotent, keyed by reference lifetime, and recovered before
  GC. Cross-log partial commits require a frontier/orphan protocol; unrelated
  writes are not atomic. Retiring an author cannot release a live reply pin.
- A current live pin protects a boundary, not all historical text. Earlier
  playback resolves against matching isolated history/epoch, rebuilt from a
  retained consumer archive if necessary. Below-floor requests fail explicitly.
- A convergent anchor cannot promise semantic paragraph intent through arbitrary
  total replacement. Deletion collapse and rendering policy must be explicit.
- Formatting is considered and outside the active release scope. Wave owns
  attribute merges, inheritance, spans, and formatting undo; anchors alone do
  not implement those semantics.
- Installed tests are self-contained in collabtext. They use no Wave checkout,
  internal header, or mandatory Qt Widgets/Test dependency. E05 reruns them
  after physical GC is implemented, rather than treating the requirement matrix
  itself as a readiness certificate.

The saved reply explains these commitments without promising a date, sending
an external message, or editing the consumer repository.

## Verification and remaining gates

A sanitizer run overlapping Release compilation passed correctness assertions
and emitted no sanitizer/leak diagnostic, but exceeded two existing absolute
wall-clock limits by small margins (10.244 s versus 10 s; 1.009 s versus 1 s).
That run is retained separately and the full suite is rerun after this review's
Release build completes, with unchanged assertions and no exclusions. These
wall-clock checks are sensitive to host load; A07/X01 should isolate timing jobs
and attach environment evidence rather than interpret a busy sanitizer run as
a production throughput measurement.

Final verification: **Debug 62/62**, **Release 64/64**, and **ASan/UBSan/LSan
62/62**, with no leak suppressions. Release includes both expensive suites;
Debug/sanitizer selections exclude only those two. Plan validation covers all 63
tasks and W1–W6; strict scope check has 22 classified hits, none needing review.
Exact commands and portable results are retained in the artifact index;
the review handoff records the implementation commit. The code under test was
the reviewed baseline plus the recorded review patch, not baseline HEAD alone.
Metadata, source hashes, selected test names, red failures, and final CTest logs
make that distinction explicit.

Five original F4–F8 expected failures remain: restart identity/coverage, recovery
pairing, skipped segments, anchor affinity, and concurrent passage ordering.
F1/F2 are contained by inert GC; F3 uses the correct coverage API. These statuses
do not certify all F9/F10 paths. The strict regression executable is expected
to fail those five cases and must continue doing so until their assigned tasks
actually repair them. No completed future API/retention/task is inferred from
the current test totals.

Review also found an incomplete P06a design obligation: current text runs derive
per-character IDs from origin plus offset, while P02 reserves one operation at a
time. The comparator gate cannot prove non-aliasing of those domains, causal
clock behavior, or coverage across an abandoned reservation. Section 3.1 of the
identity spec and a new P06a step require the exact allocation mapping and
independent fixtures before production adoption. This is unfinished design work,
not a current migrated-engine bug or a reason to replace the engine.

Next: complete that P06a allocation gate and finish engine adoption with real layout measurements, then P06b codecs
and subsequent lifecycle tasks. S01 can independently establish exact sequence
and W2 fixtures. S02/G01 gates, physical GC containment, callback A04 requirements,
and installed-consumer acceptance remain binding. The updated registry, current
handoff, external contract, and progress report should be the next agent's
starting documents.
