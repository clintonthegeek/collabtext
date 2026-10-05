# Takeover review and consumer source findings — 2026-10-05

Baseline: `2157cfb` on `roadmap/reliability-v2`. External Wave source was read
only; no consumer changes or messages were made. This records verified findings
and implementation decisions, not a claim that the remaining roadmap is done.

## W01 review

The eight-probe corpus exists and its validator self-test reproduced **31
corruption kinds applied 351 times, all rejected**. Its warnings correctly
separate third-party reported measurements from native capability evidence.
Those figures do not describe collabtext and were not independently reproduced
here. Passing a fixture parser is not passing the engine probes.

The earlier agent's sixteen skipped mutation concern belongs to a
self-test implementation that it corrected. It is not evidence that every
completed task is counterfeit, nor a requirement to rerun every mutation and
sanitizer campaign after every edit. Scale checks to changes and honor the
independent model gates where the roadmap actually requires them.

Two details need resolution in S04a, within the accepted contract rather than a
fresh product-policy question:

- S01 already fixes finite caret-collapse examples: deleting the left or both
  neighbors of an insertion gap yields Left=0/Right=1 with explicit collapse.
  W01's pending strict-referent case needs a precise attachment rule and undo
  restoration rule; it does not invalidate those earlier exact examples.
- `wp2-strict-referent-delete` describes `abc`, with `b` deleted later. Its two
  mint directives use neighbors `a` and `c`, spanning a still-visible `b`. This
  can express a surrounding range, but is not a single caret gap at mint time.
  S04a must distinguish a gap from the selected referent and supply exact mint
  directives, loss reporting and restoration snapshots. Do not treat adjacent
  survivors after deletion as proof that the original mint was valid.

The consumer did not name these algorithms. Designing them is our job. Keep a
strict attachment's loss explicit; permit a caret to collapse with a declared
position and loss flags. Retaining logical identities is what allows a later
valid undo to restore the original referent; losing history is a separate typed
unavailability outcome, not evidence for a guessed integer.

## Wave source findings

Read `src/core/include/wave/Position.h`, `Document.h`, `Position.cpp`,
`WholeDocumentDocument.cpp`, and `src/store/src/Canonical.cpp` in Wave.

1. `Position` preserves unknown nonempty bytes and treats empty as whole-block.
   Its `wpos1:` wrapper belongs to Wave. Our codec remains inside that wrapper;
   unsupported or malformed engine bytes must map to `Stale`, never null.
2. `Position.h:81–87` describes Left as moving right on insertion and Right as
   staying left. That is opposite to Wave's requested `ab`/`X` probe (Left=1,
   Right=2). The current whole-document implementation ignores affinity, so this
   is a documented mismatch awaiting an adapter, not an observed CRDT regression.
   Define the adapter mapping by the exact behavior and test both directions.
3. `Canonical.cpp:253–255` converts the position token through
   `QString::fromUtf8` before emitting JSON. Arbitrary binary codec bytes would
   be lossy there. Wave needs a deterministic text-safe representation such as
   base64url inside its versioned wrapper; decoding must reject malformed input
   and preserve unsupported original bytes. Core should not adopt Qt/JSON or
   Wave's envelope to solve this.
4. `Document::resolveAnchor` distinguishes missing block (`Gone`) from unsupported
   position (`Stale`). Block existence remains consumer-owned. Our token scope
   check must independently reject a wrong document/stream/kind/epoch.
5. Wave's resolution variant currently has only an offset or status; it has no
   collapse flags. A strict-reply adapter can map loss of its selected referent
   to `Stale`; if Wave wants usable collapsed caret offsets and diagnostics,
   it must extend its own result type. Dropping our loss marker and returning a
   bare offset would contradict its own no-silent-relocation requirement.
6. Whole-body replacement and projection/history are already explicit consumer
   seams. A token must be resolved against the corresponding replay state and
   stream identity. Live resolution cannot stand in for historical projection,
   and a whole-body reset cannot secretly remint persisted positions.

These are adapter obligations and source findings for the user to relay. No
Wave implementation was tested or modified in this turn.

## Native opacity implementation

The remaining P06a work is feasible without replacing the engine or codecs.
Concrete payload builders move to a private header. Public text/list operations
become distinct immutable shared values, created by the engine or decoder.
Copies retain const storage for deferred queues, callbacks and transport. Private
accessors are read-only and a builder is frozen by value before emission.

This deliberately revises P01's draft move-only handle to a copyable immutable
value. The existing engine uses independent operation retention in several
working components; forcing a clone/move ownership protocol through them is
unnecessary for payload opacity. The public guide records this implementation
choice. Context envelopes and typed codec versions remain P07/P06b work.

A public compile contract now rejects the old variant API and accepts opaque
headers. An initial `.timestamp` check alone missed the variant representation;
the final gate explicitly rejects `std::variant_size` access and fails against
baseline headers for both operation types. Tests also exercise builder freezing,
retained copies, source/decoder lifetime and empty/moved-from rejection.
Implementation `01614af` passed Debug bounded 72/72 plus installed Core
consumer, Release selected 9/9 and unrestricted ASan/UBSan/LSan selected 9/9.
The restricted sanitizer run passed assertions but failed process inspection and
is not counted as acceptance. Exact artifacts are recorded in TASKS.json. No benchmarks are run with tests.


## Boundary gate completed in the takeover

S04a now supplies [the full contract](../specs/vnext-boundary-semantics.md):
scoped retained-order fences plus mint coverage, exact affinity, typed
awaiting/history/scope failures, loss flags, strict selected-referent detachment,
and restoration of the same token when actual visibility returns. A local clear
invalidates position history even for an empty token. The native transformation
map and checked target signatures make S04b concrete; no production marker is
removed by specification/model work.

The independent model caught WP2's a/c range mislabeled as a caret gap. It also
caught WP8 selecting surviving a while claiming deleted b. Both now mint a/b
and explicitly select b for strict attachment. The validator rejects those
original mistakes, plus missing exact collapse/restoration fields. Its expanded
self-test rejects 37 corruption kinds applied 362 times. WP6 prose now agrees
with S01's chosen exact native tie (`abcXYZ`); the consumer did not choose that tie.

Model verification: 21 fixture histories, 50 minted gaps, 48 exact placements,
24 comparisons; 472 supplied text/list orders, 75,328 visibility states, both
affinities and selected deletion/undo/redo, with three reduced mutation cases.
Debug/Release and leak-enabled ASan/UBSan pass. Repeated seed 20261003 traces
are identical. Full sequence integration, actual token codec, store reopen,
persistent undo and pins/GC remain their implementation tasks. No benchmark ran.

Next codec review found a stale public warn-and-skip instruction and optional-only
version errors. P06b must replace them with bounded, typed versioned decoding and
retain failures at its callers. Do not conceal that behavior behind a round-trip
pass. The codec also owns allocation/gap representation under identity §3.6;
P07/L02 own integration/publication, which remains separate.
