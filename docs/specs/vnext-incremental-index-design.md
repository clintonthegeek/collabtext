# Incremental fragment tree and index design

Status: X04a design/model gate passed on 2026-10-20. This document is the
implementation contract for X07, X04 and X05. It changes no public API,
operation bytes, token bytes, ordering rule, archive reader, undo policy, or GC
eligibility. Physical collection remains governed by its existing certificates
and consumer policy.

## 1. Current cost and constraints

`Buffer` currently owns `SumTree<Fragment>` as canonical retained order, but
structural edits call `get_fragments()`, mutate a whole `std::vector<Fragment>`,
and call `set_fragments()` to rebuild the tree. `SequenceOrder::integrate()` also
receives a vector and rebuilds a per-call actor/counter lookup index. Remote
deletion scans every run for each target range. Remote undo and local
undo/redo recompute visibility over the whole tree. `SumTree` is copy-on-write
at its heap nodes, but its public mutation surface has no transaction object,
stable node identity, parent directory, or item locator. Its node/slot addresses
cannot be retained across mutation.

The October 19 profile records 505,000 scalar position items materialized over
five 100-KB boundary mint/resolve pairs, and 50,020 list items flattened over
five aged 5,000-entry list insertions. X07a removed the first measured position
projection. X04a therefore specifies the remaining tree/index path before any
mutation implementation begins. The performance target is not permission to
duplicate an index that costs as much to rebuild as the work it avoids.

The canonical text state remains a document-ordered sequence of retained
`Fragment` runs, including hidden runs and zero-width markers. Native
FugueMax links determine order. Identity is the actor plus a checked contiguous
counter interval. A run split is representational: its suffix gets the derived
identity and `link_at(k)` of its first item. No index participates in choosing
placement or changes the order of existing items.

## 2. Owned state and handles

The fragment sequence and all derived structures form one value, `FragmentState`:

```text
FragmentState {
    FragmentTree tree;                 // canonical retained order
    NodeDirectory nodes;               // current-version node topology
    RunDirectory runs;                 // RunId -> leaf NodeId and slot
    OriginIndex origins;               // (actor, counter interval) -> RunId
    DeletionIndex deletions;           // deletion identity -> item intervals/RunIds
    next_node_id, next_run_id;          // checked, state-local allocators
}
```

`NodeId` is `(TreeNamespace, local_node_counter)`; `RunId` is a nonzero uint64.
The namespace is allocated once per fresh FragmentState lineage from a checked
process-wide atomic counter and copied with that value. The local node counter
and run counter are checked monotonic uint64 allocators copied with the state.
They are derived runtime handles, not CRDT identities, serialized values, or
authority. Exhaustion is a checked refusal before publication. A split keeps
the original `RunId` on the left interval and
allocates one new `RunId` for the right interval. Coalescing keeps the left ID
and removes the right ID. Restore constructs fresh handles. Copying a Buffer
copies the handle namespace and derived roots along with the tree; the copies
then evolve independently. Equal IDs in divergent Buffer copies are harmless
because a handle is only meaningful inside its owning `FragmentState`.

The tree's item type is a private `FragmentSlot { RunId handle; Fragment value; }`;
its summary delegates to `value.summary()`. The handle is not added to
`Fragment`, `BufferCheckpointState`, operation payloads, or checkpoint codecs.
`SumTree` takes a third node-ID policy parameter, defaulting to `NoNodeIds`;
`FragmentTree` is `SumTree<FragmentSlot, FRAG_TREE_B, StableNodeIds>`. The
`StableNodeIds` policy stores the namespace and next local counter; COW clones
retain a node's ID and new nodes allocate from the candidate policy. UndoMap
keeps the default policy and pays no ID or directory cost. A subtree may be
joined only when its NodeIds are unique in the destination state; before a join,
check the incoming subtree IDs against the candidate directory. If any collide,
use the counted reconstruction fallback, which assigns a fresh namespace and
handles to the complete candidate sequence.

Each fragment-tree node carries a `NodeId`. The state-local `NodeDirectory` is
a persistent map from `NodeId` to `{weak_node, parent_id, child_slot}`. The root
has no parent. A directory entry's `weak_node` is a lookup cache only; the ID is
the handle. The directory does not own nodes and therefore does not trigger
copy-on-write. On a COW clone, the clone keeps the node ID and its directory
entry is updated to the clone. The previous Buffer's directory still names its
own shared node. Node split preserves the old ID on the left and allocates an ID
for the right. Parent/slot entries for moved children are updated in the same
candidate state. Root growth and shrink update the root parent entry. No raw
pointer into a vector, leaf slot, or movable child array is retained as a
handle.

The directory and indexes use immutable path-copied AVL maps keyed by their
total-order keys. Every AVL node stores height and subtree entry count. Insert
and erase path-copy visited nodes, then restore balance. Let
`balance = height(left) - height(right)`: for `balance > 1`, rotate right when
`height(left.left) >= height(left.right)`, otherwise rotate left at the left
child then right at this node. For `balance < -1`, rotate left when
`height(right.right) >= height(right.left)`, otherwise rotate right at the right
child then left at this node. Erase of a two-child node replaces its key/value
with its in-order successor and erases that successor recursively. Copying
`FragmentState` shares immutable map roots. A full `std::map` copy per edit is
forbidden because it retains O(number of runs) mutation cost. `RunDirectory`
maps RunId to the leaf NodeId and slot. A fragment split/slot shift updates only
the affected leaf's handles; a tree-node split updates the slots of the
items/children moved between its bounded-capacity nodes.

The tree owns every node strongly from its root. Directory lookups lock a weak
node only for the duration of a checked read; release that temporary owner
before beginning a mutation so `shared_ptr::use_count` reflects actual tree
sharing. A mutation navigates from the candidate root by NodeId and recorded
child slots. After every COW clone, split, borrow, merge, root change, or child
move, it updates the candidate NodeDirectory before the next indexed lookup.
When a node is removed, erase its directory entry; do not recycle its ID within
the candidate transaction.

`OriginIndex` is an ordered map keyed by `(ActorId, interval_start)`. The value
contains exclusive `interval_end` and `RunId`. Intervals are nonempty, checked
half-open counter ranges. They partition every retained item identity exactly
once, including marker identities; intervals for one actor never overlap.
Lookup uses predecessor search and verifies `counter < interval_end`. A split
replaces one interval with two adjacent intervals. A splice adds the new run's
interval. Collection removes only certified identities. Coalescing replaces
adjacent intervals only when the resulting fragment representation is valid.

`DeletionIndex` is an ordered reverse map keyed by
`(deletion_identity, ActorId, interval_start)` to `{interval_end, RunId}`. A
deletion identity is the complete `(ActorId, counter)` operation identity. It has one
entry for each maximal identity interval in a run's deletion set. Its entries
must agree exactly with `Fragment::deletions`; an overlapping delete adds a
second incidence, and undoing a deletion does not erase that incidence. A run
split partitions every incidence interval, and a merge coalesces adjacent
intervals only when both run membership and deletion membership agree. Undo
queries by deletion identity to get the precise affected runs without scanning
unrelated fragments.

Identity-to-rank pseudocode is fixed:

```text
locate_item(id):
  (start, interval) = origins.predecessor((id.actor, id.counter))
  require start.actor == id.actor && start.counter <= id.counter < interval.end
  slot = runs[interval.RunId]
  leaf = nodes[slot.NodeId].weak_node.lock()
  require leaf is a current leaf && leaf[slot.slot].handle == interval.RunId
  rank = id.counter - start.counter
  for i in 0 .. slot.slot-1:
    rank += leaf.items[i].summary.retained_items
  node = leaf
  while node.parent exists:
    edge = nodes[node.id].parent_id/child_slot
    parent = nodes[edge.parent_id].weak_node.lock()
    for i in 0 .. edge.child_slot-1:
      rank += parent.children[i].summary.retained_items
    node = parent
  return rank
```

A miss or stale generation is not a nearby rank: rebuild and validate the
derived directory, then return the current typed missing-history outcome if the
identity is absent. A range query uses `origins.lower_bound((actor, first))`,
visits intervals until actor changes or interval start reaches the checked
exclusive end, and clips each returned run to the requested bounds. A deletion
query seeks the first key for that complete deletion identity and scans only
that key's ordered incidence entries; dedupe RunIds before visibility updates.
Both range procedures are O(log F + K) in map visits, where K is the number of
intersecting run intervals.

All index roots and the fragment tree are derived, non-durable state. Checkpoints
continue to encode the existing logical fragments, undo entries, causal state,
and metadata only. NodeId, RunId, directory topology, and index pages are never
written. A restore rebuilds and validates every derived map before selecting the
new Buffer state.

## 3. Tree summaries and laws

The summary is an ordered product monoid, composed componentwise:

```text
Summary = {
  visible_utf8_bytes, hidden_utf8_bytes,
  retained_items, visible_items,
  visible_text_scalars, visible_utf16_units,
  retained_runs
}
```

`zero` is the all-zero tuple. `combine(a,b)` is the componentwise sum; it is
associative and has `zero` as left/right identity. It is intentionally not
commutative as a sequence fold API, even though these particular counts have
commutative addition. Every field is uint64 so aggregation does not wrap at the
public uint32 text limit. Public uint32 conversions remain checked refusals.
A marker contributes one retained item, and one visible item when visible; it
contributes zero text bytes, text scalars, and UTF-16 units. A text run
contributes `length` retained items; visibility selects its bytes, scalar count,
and UTF-16 count into the visible or hidden side. Tombstoned text still
contributes retained items and hidden bytes. These rules separate sequence rank
from text coordinates.

Required laws, checked by the independent model and the later production summary
tests:

1. `zero ⊕ s = s = s ⊕ zero`; `(a ⊕ b) ⊕ c = a ⊕ (b ⊕ c)` for every field.
2. A tree's summary equals the fold of its items in retained order for any
   parenthesization, tree height, split, splice, or COW path.
3. Splitting a run at every scalar boundary preserves total summary; the two
   resulting summaries compose to the original. UTF-8 byte widths sum to
   `byte_length`; scalar count equals `length`; UTF-16 count is one per BMP
   scalar and two per non-BMP scalar.
4. A visibility toggle transfers that run's bytes/scalars/UTF-16 units between
   visible and hidden components without changing retained item/run counts.
5. A marker changes retained/visible item counts only, never text coordinates.
6. No subtree summary or identity interval is trusted after a failed candidate
   update. The published root and indexes stay the previous consistent value.

Summary-guided UTF-16 lookup descends by visible UTF-16 units while carrying the
visible byte prefix through the same SumTree cursor. Each fragment caches its
UTF-16 width. Long non-ASCII fragments keep shared scalar-boundary checkpoints
at 128-unit intervals; ASCII fragments and short fragments allocate no
checkpoint storage. Local conversion starts at the nearest preceding checkpoint
and scans no more than one interval. Fragment splits rebuild derived widths and
checkpoints from both resulting strings, and checkpoint restore rebuilds them
from authoritative text. No cache value is durable. The summary does not contain
line/layout data. Origin lookup uses the identity map, not actor maxima in an
aggregate.

## 4. Tree operations and index updates

All operations below execute on a private candidate `FragmentState` whose
immutable roots initially share the published state. They stage tree path copies,
map deltas, updated summaries, undo/coverage values, and emitted operation data.
The only publish point is a no-throw swap of the complete prepared state into
the Buffer. Callbacks run after that swap, once, and may reenter only against the
fully committed state. The old tree/index values remain alive until the swap
finishes, then release through shared ownership.

### Locate, split, splice

`locate(identity)` performs predecessor search in `OriginIndex`, validates the
interval and RunId, follows `RunDirectory` to the leaf and slot, and checks the
fragment's actor/start/length against the index record. A mismatch is an index
invariant failure; it triggers a full derived-state rebuild from the candidate
tree before any further indexed read. If rebuild validation fails, reject the
candidate and preserve the published state.

`split(handle, k)` requires `0 < k < run.length` and a UTF-8 scalar boundary.
Read the fragment by its RunDirectory locator; preserve the prefix's identity,
link, deletion set and RunId. The suffix gets `item_at(k)`, `link_at(k)`, suffix
text/length, the same deletion set, visibility, and a new RunId. Replace one
leaf item by prefix/suffix, splitting the B+ node if required. Update moved
RunDirectory slots, NodeDirectory child slots on node split, the two origin
intervals, and each deletion incidence interval. Recompute summaries bottom-up
on the edited tree path. The full logical retained scalar stream is unchanged.

Coalescing is optional and must not be used to hide an ordering or lookup defect.
Two neighboring runs may coalesce only when neither is a marker, their actors
match, their identity intervals are exactly adjacent without overflow, the right
run's link equals `left.link_at(left.length)`, their deletion vectors and cached
visibility are byte-for-byte equal, and concatenated UTF-8 text is valid with
lengths equal to the summed byte/scalar counts. The left RunId survives; the
right RunId is removed. Update both identity maps and the leaf summary in the
same transaction. Otherwise keep the run boundary.

The FragmentTree is a B+ tree with leaf item capacity 12, internal child
capacity 12, and minimum non-root occupancy 6. All leaves have equal depth. A
13-item/child overflow splits 6 left and 7 right. On underflow, borrow from a
left sibling with occupancy above 6 first, otherwise from a right sibling above
6; if neither can lend, merge into the left sibling when present, otherwise
merge the right sibling into the underfull node. Propagate parent underflow;
collapse a one-child root and replace an empty root with the empty tree. Recompute
all affected summaries bottom-up after final redistribution.

`splice(rank, remove_count, inserted_runs)` removes the requested consecutive
run range and inserts already ordered runs at that retained rank. It is a tree
sequence operation: split at both run boundaries if rank falls inside a run;
split each B+ root at the run-rank boundary by descending the child containing
that rank, partitioning its bounded child array, and returning two roots with
normalized heights; recursively join roots of unequal height by descending the
taller root's boundary spine and inserting the shorter root at the matching
height; when root heights match, put both roots under a new root. After each
join, apply the stated borrow/merge rules at the boundary until every non-root
node has occupancy 6–12. Update directories/indexes for changed leaves and
children, then recompute summaries on the union of affected root paths. The
splice primitive does not choose CRDT order.
FugueMax computes insertion order first, using X04 identity lookup. A candidate
whose splice preconditions do not match the requested identities/rank is refused
as history unavailable and falls back to reconstruction only where the caller
has an authoritative ordered run sequence.

### Visibility and deletion incidence

`add_deletion(identity_interval, deletion_id)` locates each intersecting run
through OriginIndex, splits at interval boundaries, and adds the deletion ID to
each fully covered run. It inserts corresponding DeletionIndex incidences,
recomputes visibility from the existing UndoMap rule, and updates summaries on
the affected leaves/paths. Existing deletions are retained, including overlap.
It does not silently skip a missing identity: the caller's already-validated
operation policy decides whether the absence is an admissible no-op, a
history-unavailable refusal, or a certified-collection outcome.

`recompute_visibility(affected_runs)` obtains runs from the origin/deletion
indexes, reads the current staged UndoMap, and changes only runs whose cached
visibility differs. If undo target coverage cannot be represented as whole
current runs, split at the exact target interval boundaries before recomputing.
For each changed run, update its leaf summary and propagate to root. Do not
revisit unrelated runs. If an index lookup or run-uniformity check fails, rebuild
derived indexes and use the full tree visibility scan as a counted fallback.

## 5. Transaction boundary and refusal policy

The prepared transaction contains the candidate fragment tree, directories and
indexes; candidate UndoMap, version vector and counter-gap state when touched;
candidate local undo/retired-history state when touched; the immutable operation
to return or apply outcome; and notification intent. Deferral/quarantine queues
are not changed by an operation that is rejected or deferred. They are updated
only by the existing outer apply policy after the CRDT transaction verdict.

Preflight must complete all semantic checks before candidate publication:
operation shape and causal dependencies; scope and gap conflicts; counter and
interval arithmetic; UTF-8/scalar boundaries; placement referents and certified
reclamation policy; configured undo/deferred/quarantine budgets; interval
overlap/ownership; summary/public-width bounds; and NodeId/RunId exhaustion.
Local identity allocation remains a durable authority boundary: validate and
compute all predictable refusal conditions first, then perform the existing
all-or-nothing `reserve_run`. A successful reservation is not rolled back if a
later process-level allocation exception occurs. Such a skipped counter remains
safe and is not an emitted operation. The implementation must not claim
allocator rollback.

The preflight/refusal matrix is:

| Operation | Checks before reservation or candidate edits | Checked refusal source |
|---|---|---|
| Local text edit | Range/text cardinality, ordered nonoverlapping byte ranges, visible bounds, UTF-8 validity and scalar boundaries, operation/range/count limits, text/scalar/identity arithmetic, undo and retirement budgets, allocator actor, enough NodeId/RunId space | Existing malformed, invalid-Unicode, limit, or identity errors; allocator durable-write/exhaustion error |
| Marker or relative insertion/removal | Allocator actor, text/marker shape, scalar/byte/undo arithmetic, visible marker/relative target and required link identities, undo/retirement budgets, handle space | Existing malformed, detached/history-unavailable, limit, or identity errors |
| Remote edit | Operation identity and structure before duplicate verdict; counter-gap conflict; causal frontier; reclamation scope; every deletion interval/marker type; every placement parent/right origin; checked interval end and candidate index overlap | Existing rejected/deferred/duplicate/apply outcome; no tree/index publication for rejection or deferral |
| Remote undo | Operation identity/shape before duplicate verdict; causal frontier; no target below certified reclamation; accepted monotonic undo counts; every target identity interval | Existing malformed/deferred/history-unavailable outcome |
| Local undo/redo/group | Requested active/undone group; allocator actor; reserve one operation identity; checked undo-count increment; operation size and retained-history bookkeeping | Existing history-unavailable, identity-reservation, or checked count/size refusal |
| Checkpoint restore | Existing scope/version/allocator/gap checks; every fragment's UTF-8, byte/scalar/marker shape and interval; no duplicate/overlapping identities; valid deletion and undo references; deferred/quarantine budgets; derived-index bijections | Existing checkpoint Result error; original Buffer stays selected |
| Certified collection | Existing certificate, membership frontier, pins/translations, and exact retained-history checks; all removed intervals known in every derived map | Existing refusal leaves the old state; accepted collection commits tree/index/reclamation binding together |

All output-operation vectors, split text, candidate tree nodes, AVL pages,
directory patches, UndoMap pages and local-history copies are candidate-owned
allocations. A failure before the commit point destroys only candidate state.
Any operation result or callback intent allocated before commit is also
candidate-owned. The callback object itself is not copied or invoked while tree
or index roots are changing.

Logical/checked refusal returns before publication, changes no tree/index,
version, UndoMap, local history or callback state, and preserves current typed
refusal codes. Candidate allocations use existing C++ allocation behavior;
the design does not invent a recoverable `bad_alloc` policy. Because all
mutations are staged in copy-on-write/persistent candidate values, stack
unwinding before publish leaves the old Buffer state selected; external durable
identity reservation may have advanced as described above. No helper may mutate
the published tree or indexes before the commit point.

The commit operation is only moves/swaps of already-prepared value roots and
scalars and is required to be non-throwing. It updates the entire fragment-state
root plus any touched undo/coverage/local-history fields together. The observer
callback is the last step. Observer exceptions/reentrancy policy remains the
existing API contract; the engine invokes it only after committed state is
visible. Operation serialization and callback execution are never part of index
mutation.

## 6. Mutator update and fallback table

| Entry point | Candidate changes | Derived updates | Refusal/fallback rule |
|---|---|---|---|
| Local text edit | ordered text splice, deletion labels, version, local undo entry | split/splice, origin and deletion intervals, visibility and all summaries | validate offsets, Unicode, budgets and allocator first; no publication on refusal |
| Marker insert/remove/relative insert | zero-width item splice or deletion label, version, local undo | same identity/deletion maps; marker summary rule | marker identity is one retained item; text offsets ignore its width |
| Remote edit | deletion ranges and ordered inserted runs, version/frontier | same split/splice/index updates; all links validated before publish | malformed/missing reclaimed link rejects candidate; causally missing input stays deferred outside candidate |
| Remote undo | UndoMap counts, version, visibility of exact targets | reverse deletion and origin lookup; affected summaries only | absent/certified targets retain existing refusal; no partial UndoMap change |
| Local undo/redo/group | UndoMap counts, local undo cursor/group, version, visibility | same affected-run algorithm as remote undo | reserve before changing staged cursor; failed reserve publishes nothing |
| Restore checkpoint | new logical fragments, UndoMap, gaps/version and existing metadata | discard serialized derived data; build fresh tree, handles, indexes, summaries and validate bijections | existing checkpoint validation; old Buffer remains selected on error |
| Restore local history | local active/retired history metadata | no fragment index change; validate against current logical state | failure leaves existing local history selected |
| Certified collection | remove certified fragments and update reclamation binding | splice/remove all identity/deletion mappings, directory entries and summaries | preserve existing certificate and pin checks; no policy broadening |
| Discard local view/reset | empty retained tree and transient indexes, keep exactly existing durable owner fields | replace all derived roots with empty roots | no stale handles; position history stays unavailable per existing behavior |
| Counter gap application | ledger/version only | no text index change | gaps remain causal metadata, never placeholder fragments |
| Coalesce/limits/retirement acknowledgement | local undo/retired metadata only | no fragment index change | existing checked budget and history rules |
| Deferred retry/quarantine | queue metadata; then zero or more accepted candidate operations | indexes change only for each accepted operation transaction | rejected/deferred operation cannot leak candidate fragments/index records |

There are two explicit correct fallbacks, both counted in diagnostics: (1) a
derived-index invariant mismatch rebuilds every derived index from the staged
tree and revalidates it before continuing; (2) a mutation shape not yet handled
incrementally may use the existing whole-vector oracle to produce an ordered
candidate, then rebuild all derived state atomically. A fallback may not turn a
missing, malformed, or reclaimed identity into an accepted edit. Track counts
and reasons separately (`index_rebuild_after_mismatch`,
`reconstruction_unsupported_shape`, `restore_rebuild`). The supported common
path must not silently fall back. A rebuild that cannot establish exact
one-to-one interval/run coverage rejects the candidate and preserves the prior
state.

## 7. Complexity and memory obligations

For `F` retained fragments, `A` actors, and `K` affected intervals, the target
lookup path is O(log F) for identity predecessor search plus O(height × B) to
resolve a run through the node directory. Split/splice touch O(height × B + K)
tree items and path-copy O(height) tree nodes. Persistent map updates cost
O(log F) per changed interval/handle. Visibility recomputation is O(K + changed
tree paths), not O(F), except the counted correct fallback. A full restore or
index rebuild is O(F + total deletion incidences). Sequence ordering may still
walk affected subtrees; X05 must measure that separately and may not claim
logarithmic edits while a full placement scan remains.

Each run carries a RunId/locator and one origin interval, plus one deletion
incidence entry per maximal run/deletion pair. Each tree node carries a NodeId
and one directory entry; parent directory entries are per node, not per scalar.
The indexes must not allocate one identity object per scalar for a contiguous
run. Report live bytes for all directory/index roots, shared pages, per-run
records, and deletion incidence; distinguish them from undo accounting and
allocator-reserved bytes. Establish the measured X01 baseline before claiming
memory improvement.

## 8. Independent model contract

`tst_incremental_model` is a separate small-state interpreter. It may use a
flat scalar vector and simple maps because it is the oracle, but it includes no
production Buffer, SumTree, SequenceOrder, OriginIndex, DeletionIndex, codec, or
visibility helper. It models identity intervals, run splits, ordered splices,
overlapping deletion incidence, selective visibility, markers, undo counts,
summary folds, and candidate publication. After every accepted event it compares
indexed lookup with a full scalar scan and checks exact interval partitions,
deletion incidence, order, visibility and aggregate values. Refused operations
and deterministic injected preparation failures must leave an identical state
fingerprint and invoke no callback.

The model enumerates all short split/splice/delete/undo traces within its bounded
state space, then executes fixed-seed longer traces. Failure injection occurs
after each modeled prepare stage (tree path, origin map, deletion map, summary,
undo/coverage, operation/callback intent) and before the single publish point.
Run every fixed seed twice and compare trace digest, counterexample count and
state-check count. The Release run uses runtime checks and no `assert` as an
oracle. Existing sequence and boundary models remain separate: this model
checks representation transactions and summaries, not FugueMax order or public
position semantics. Production must later be differentially tested against
both this abstract contract and the existing independent models.
