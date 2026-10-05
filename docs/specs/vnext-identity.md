# vNext operation identity

**Created:** 2026-10-04, as the design-gate artifact for roadmap task P06a
**Extended:** 2026-10-05 — §3.1–§3.8 close the allocation-domain gate the review
required.
**Measured:** 2026-10-05 — P06a steps 3 and 4 adopted the types and §3.2's
run-interval allocation; §5 is now measured rather than projected.
**Supersedes for identity purposes:** the `Lamport { uint32_t value; uint16_t
replica_id; }` that used to live in `src/crdt/Clock.h`, which this document
replaced rather than extended. That type no longer exists: `Clock.h` now defines
`Crdt::Identity`, and nothing in the engine or the public headers is spelled
`Lamport` any more.

**Status of each section:** §1–§3.7 are implemented and enforced by the engine.
An engine is constructed from an allocator rather than an actor id, reserves one
interval per local operation, and marks tombstones with the deleting operation's
own identity. §3.8 is what the independent model proves. §4 is a constraint on
P07/L04 rather than code here. §5 is measured.

## 1. What an identity is

An operation's identity is the pair

```
Identity {
    ActorId actor;   // 128-bit, persistent, offline-safe (P02)
    uint64_t counter // checked, monotonic, never reused
}
```

This is the change the roadmap's own scope statement calls for: *"16-bit actor
and 32-bit counter assumptions become persistent actor identities and checked
64-bit counters"* (README, "Scope and compatibility"). P02 built `ActorId` and
`CheckedCounter`; this document is about making the *engine* use them.

The old pair is not wrong in shape, only in width and in where the authority
lives. A caller chose a `uint16_t`, so identity was whatever the process
happened to be constructed with: not persistent, not collision-checked, and
resettable by relaunching with a different number.

## 2. The order, and why it is not negotiable

The engine imposes a total order on identities, `(counter, actor)`, compared
counter-first. It is used in three places, and only the first is subtle:

| Use | Where | Consequence of disagreement |
|---|---|---|
| **Tie-breaking equal locators** | `FragmentSummary::add_summary`, `Buffer::insert_fragment`, `IdList::insert_entry`, `Buffer::normalize_fragments` | **Divergence.** Replicas order the document differently. |
| Undo-map key identity | `UndoMapKey` | Undo resolves the wrong target |
| Causality comparison | `Global::observed_all` | Operations defer forever |

The first row is why this document exists. Two fragments can share a locator, and
that is not an edge case: `Buffer::normalize_fragments` exists specifically to
handle *runs of fragments from different replicas at one shared locator*, and
`apply_remote_edit`'s split-relocation path deliberately assigns
`max(new_locator, sf.locator)`, which can leave two locators equal. Ordering then
falls through to origin, and if two replicas disagree about that order they
produce different documents.

### 2.1 Locally assigned slots are forbidden

The obvious way to keep identities small is to assign each new `ActorId` a dense
local index and store the index in operations. **This breaks convergence**, and
not subtly: replica A might meet actors X and Y in one order and assign X=0, Y=1,
while replica B meets them in the opposite order and assigns Y=0, X=1. Equal
locators from X and Y then tie-break in opposite directions on the two replicas,
and the documents diverge permanently.

An identity that participates in any total order must therefore be a *global*
function of the actor, not a local assignment. `ActorId::compare` — a byte-wise
comparison of the 128-bit value — already satisfies this: it is a deterministic
total order that every replica computes identically, with no coordination.

The cost is accepted deliberately and quantified in §5.

### 2.2 The laws

The order must satisfy, and `tst_identity_ordering_model` proves by
construction and by exhaustive small-case enumeration:

1. **Irreflexive**: `compare(a, a) == 0` and `!(a < a)`.
2. **Antisymmetric**: exactly one of `a < b`, `b < a` holds when `a != b`.
3. **Transitive**: `a < b && b < c` implies `a < c`.
4. **Total**: every pair is comparable — no two identities are "incomparable",
   so there is never an arbitrary choice to make differently on two replicas.
5. **Deterministic across processes**: two independent implementations of the
   comparator, given the same two identities, agree. The test checks this by
   comparing the engine's comparator against a separately written byte-wise one
   rather than against itself.
6. **Counter-first stability**: raising only the counter yields a strictly
   greater identity; raising only the actor may move it either way, but
   deterministically.
7. **No aliasing**: distinct `ActorId`s are never equal under the order, and a
   default-constructed `ActorId` (all zeroes, "invalid" per P02) never equals a
   valid one.

Law 4 is the one that makes the whole scheme work. A partial order would need a
tie-break rule *outside* the identity, and that rule is exactly what two
replicas would implement differently.

## 3. Counter rules

- **Checked.** The 64-bit counter is reserved through `CheckedCounter`
  (`ActorId.h`). At `UINT64_MAX` it returns `counter_exhausted` and changes
  nothing. A wrap is not a recovery: it would let a new operation present itself
  as an old one, which is finding F4 in a different costume.
- **Never reused.** The next reservation is at or above the highest value durably
  recorded for that (actor, stream). A sender restart raises its counter rather
  than restarting it (vnext-protocol.md §7.1).
- **Per (actor, stream).** There is deliberately no counter that spans streams;
  two streams using the same counter value are two independent facts.
- **Zero is not an identity.** `Global::observe` discards value 0 and
  `observed()` is vacuously true for it, so a counter-0 operation can neither be
  recorded nor deduplicated. P05 established that this makes it a
  `invalid_identity` rejection; it is restated here because the wider counter
  makes the sentinel even easier to hit by accident.

### 3.1 The three domains

The comparator model proves ordering laws for supplied identities. It does not
say who hands out which number, and the legacy engine answers that question
three different ways in three different places. Those are separated here.

| Domain | What it names | Allocated from | Counted for |
|---|---|---|---|
| **Operation** | the operation itself: its log record, its dedupe key, its undo id, a deletion event | the durable per-(actor, stream) counter, one value per operation | dedupe, undo map `undo_id`, version vector |
| **Item** | one inserted scalar: a character's position identity, an `IdList` entry's origin | a **run interval** reserved by one operation (§3.2) | fragment ordering, anchors, undo map `edit_id` |
| **Causal** | nothing durable — the local knowledge of what has been seen | the same counter, raised by applying a remote operation (§3.4) | making a later local identity sort after what it has observed |

There is no separate persistent causal store. The durable counter *is* a Lamport
clock whose actor half is persistent, so "causal" and "durable" are one counter
with two ways to move, exactly as in the legacy engine. What is new is that the
three uses are named, disjoint, and no longer alias.

### 3.2 Run-interval reservation

The legacy engine answered "which identity does this character get" by ticking its
clock once per inserted scalar. One P02 `next_operation()` reservation per edit
cannot supply that range, so vNext reserves a **run interval**: one checked
reservation covering an operation and all of its items.

**Implemented.** `Crdt::IdentityAllocator::reserve_run(count)` is the single
allocation path; `Buffer::apply_validated_local_edit` counts the scalars it is
about to insert *before* mutating anything, reserves `n + 1` once, and hands out
item identities from that interval run by run. Undo, redo, `IdList` insert and
`IdList` remove each reserve one value, because they insert no items. An engine
has no constructor that takes only an `ActorId` — it is handed an allocator and
learns its actor from it, so it cannot mint identity at all without one (§3.7).

For a local operation inserting `n` scalars in total, the reservation returns a
base `b` and consumes `n + 1` consecutive values:

```
item identities   b + 0 .. b + n - 1     in document order: run by run, scalar by scalar
operation identity        b + n         (for a deletion-only operation, n = 0: b + 0)
next reservation      b + n + 1
```

Four properties, each of which the legacy engine does *not* have:

1. **Non-aliasing.** The operation identity is strictly greater than every item
   it created and strictly less than the next operation's base. The legacy
   engine sets `op.timestamp` to `m_clock.value - 1` (`Buffer.cpp:900`) — the
   *last* item identity of its own run — so an insert operation and one of its own
   characters carry the same identity today. Splitting the domains removes that
   overlap, and makes "which operation inserted this character" answerable by
   range containment rather than by guesswork.
2. **Disjointness falls out of monotonicity.** Intervals are half-open and handed
   out in increasing order, so no two operations' items can collide without the
   counter being reused, which §3's rules already forbid.
3. **Derived, not re-reserved.** A fragment's item identities are positions in
   its owning operation's interval. Splitting a fragment partitions those
   positions; it never allocates (§3.3).
4. **One durable write.** The whole `n + 1` range is persisted by a single
   checked reservation *before* any of it is used, so a crash cannot leave part
   of an operation's identities issued and part of them reissuable.

`CheckedCounter::reserve_run(count)` is that reservation: it is `reserve()` with a
checked bound, and it either hands out the whole range or changes nothing.

Rejected alternative — a packed subidentity. Giving each operation one counter
and each scalar an index packed beside it (`counter * 2^k + index`, or a
`(counter, index)` pair) is the obvious way to stop consuming a counter per
character. It is rejected because it silently changes the order: under
`(counter, index)` the items of operation `c` all sort before operation `c + 1`,
whereas the legacy counter-per-scalar scheme interleaves them. Equal locators
tie-break on exactly that order, so the change is invisible in the type and
visible in every concurrent-passage fixture. The interval scheme reproduces the
legacy order value for value; that is why it was chosen.

### 3.3 Derived identities: split, deletion, undo/redo

| Event | Identity source | Allocation |
|---|---|---|
| Insert a run of `k` scalars | the operation's interval, positions `0..k-1` of that run | none beyond the operation's reservation |
| Split a fragment at scalar offset `j` | second half's item identities are the owner's positions `j..` | none — derivation, `origin + j`, as today (`Buffer.cpp:413`) |
| Delete | the deleting **operation's** identity, pushed onto each affected fragment's deletion list | none — the operation already consumed one value |
| Undo / redo | undo ids are fresh operation identities; undo-map `edit_id`s are the affected **item** identities | one reservation per undo/redo operation |

The invariant the engine must hold: a derived item identity always satisfies
`base <= identity < base + n` for the operation that owns the run it came from,
and a fragment's derived identities stay inside the interval recorded for that
run. A split that could push an identity outside its interval is a defect, not a
new numbering.

Two consequences worth naming. Deletion no longer needs a separate counter tick:
the legacy `deletion_ts` tick (`Buffer.cpp:586`) exists because the operation's
own timestamp could not serve twice; with §3.2 it can. And the undo map's two
key fields now come from different domains — `edit_id` is an item identity,
`undo_id` is an operation identity — which is why no UndoMap entry can ever be
confused with the operation that produced it.

### 3.4 The causal clock and remote observation

Applying a remote operation raises the local counter past everything that
operation used, so an identity created afterwards sorts after identities the
local replica has already seen. The legacy engine observes the operation's
timestamp, its deletion id, and each run's last item
(`Buffer.cpp:1297-1305`); under §3.2 the operation identity is the highest value
in the interval, so observing it alone covers the run, and the per-run
observations become redundant but harmless.

The raise is **derived state, not durable allocation**. It need not be
persisted: on reopen the engine replays its own log and observes again, and a
lost raise cannot cause reuse because no identity was issued while it was held.
A *reservation* is different in kind and is always durable first — that is the
F4 fix, and §3.6 is about what is left over when the two steps come apart.

### 3.5 Exhaustion at interval boundaries

`UINT64_MAX` is the exhaustion sentinel and is never issued, so the highest
issuable value is `UINT64_MAX - 1`. `reserve_run(count)` requires

```
next + count - 1  <=  UINT64_MAX - 1
```

and fails with `counter_exhausted`, changing nothing, when it does not. The
arithmetic is checked, not wrapped: `next + count` itself may overflow `uint64`,
so the bound is tested by subtraction. Boundary behaviour, all of it
executable cases in the model:

| next | count | result |
|---|---|---|
| `UINT64_MAX - 1` | 1 | issues `UINT64_MAX - 1`; counter becomes exhausted |
| `UINT64_MAX - 1` | 2 | `counter_exhausted`, nothing issued |
| `UINT64_MAX` | any | `counter_exhausted`, nothing issued |
| 1 | 0 | rejected — a count of 0 is not an operation, since every operation consumes its own identity |

### 3.6 Abandoned reservations, gaps, and contiguous coverage

Durability-first allocation has a known cost: a crash between the durable
reservation and the operation's arrival in the durable log burns the whole
interval. The counter never goes backwards, so those values are gone, and
nothing will ever fill them.

That is a gap in the stream, and the protocol already has the rule for gaps
(vnext-protocol.md §5): **coverage advances only over a validated, contiguous
prefix.** So an abandoned interval is not a counter problem, it is a coverage
problem, and it must be resolved explicitly:

1. The reservation record is durable and states the interval *before* use, so
   recovery can always enumerate reservations that were never published.
2. On reopen, every reserved-but-unpublished interval is turned into an explicit
   **gap record** appended to the stream — a record that occupies the interval,
   applies as a no-op, and names the interval it accounts for. Peers therefore
   advance their frontier across a *known* absence instead of stalling forever.
3. A maximum observed counter is never treated as proof of contiguity. Receiving
   `(actor, 40)` and `(actor, 43)` is missing 41 and 42 until a record says
   otherwise, whether those records carry operations or gaps.
4. A gap record is terminal. It cannot be filled later, so nothing may depend on
   identities inside an abandoned interval.

Publication and recovery of these records belong to P06b (the codec must carry
the interval length and the gap kind), P07 (the envelope refuses a mismatched
interval), and L02 (framing, partial-append reconciliation, and the reopen path
that emits gap records). This section fixes their obligation; it does not
implement it.

### 3.7 One actor, many documents and streams

Counters are per (actor, stream) and the envelope carries document and stream
(vnext-protocol.md §1.3). Two consequences the engine must respect:

- The **same** counter value in two streams is two independent facts. They are
  different identities because their context differs, and no code may order them,
  deduplicate across them, or treat one as covering the other.
- An engine borrows its allocator. Constructing a `Buffer` with an `ActorId` does
  not create a counter, does not restart one at 1, and is not authority to claim
  durable allocation. The counter lives in `ReplicaState`, one durable owner per
  private state directory (P02), and the engine holds a handle naming its
  (document, stream). Tests that need a throwaway counter must say so explicitly
  as a volatile test counter — never by reusing a production allocator path.

### 3.8 What the independent model proves, and what it does not

[`tst_identity_allocation_model.cpp`](../../libs/collabtext/tests/tst_identity_allocation_model.cpp)
implements §3.2–§3.7 from the text above, without calling the production
allocator it is validating, and the fixtures cover: multi-scalar insert → split →
delete → undo → redo → restart, two documents and two streams under one actor, a
remote clock advance followed by local allocation, a reserved-but-unpublished
interval becoming an explicit gap, and exhaustion at each boundary in §3.5. It
also compares its own run reservation against `CheckedCounter::reserve_run` —
that single comparison is the only place the two implementations meet.

It proves the contract is internally consistent and implementable: identities do
not alias, intervals are disjoint, derivations stay in range, a restart never
reissues, exhaustion is atomic, and gaps are explicit. It does **not** prove that
a migrated engine converges; that is what P06a's steps 3–6 and the convergence
suites are for. It also does not prove durable-allocation behaviour across a real
power loss: the gap rules here are a specification, and the lifecycle evidence
belongs to L02/L08.

## 4. Epoch, document, and stream are not in here

`Identity` deliberately carries no epoch, document, or stream. Those belong to
the envelope (vnext-protocol.md §1.3, §3) and are checked on decode and on
transport, not by the ordering above. This is why P05 recorded epoch validation
as a gap: there was no field to compare, and adding one to the identity would put
membership state into the thing that breaks ties.

The consequence to keep in mind: an operation from a retired epoch must be
refused by the envelope or transport layer *before* it reaches ordering. If it
reaches ordering, the engine has no way to know and will order it like any other
operation. P07 and L04 own that refusal.

## 5. Sizes: measured, not projected

Measured on this branch after adoption (`x86_64`, GCC 16.2, `-std=c++20`), same
compiler for both columns so the ratio is meaningful. The baseline column was
measured before the change; the adopted column is
`libs/collabtext/tests/tst_identity_types.cpp`'s subject, compiled against the
migrated engine.

| Type | Before | After | Ratio | Where it matters |
|---|---|---|---|---|
| `Lamport` → `Identity` | 8 | 24 | 3.0x | one per fragment summary |
| `UndoMapKey` | 8 | 24 | 3.0x | undo map entries, per local edit |
| `UndoTreeKey` | 16 | 48 | 3.0x | undo map key |
| `FragmentSummary` | 40 | 56 | 1.4x | **the hot path** — one per SumTree node |
| `Fragment` | 104 | 120 | 1.15x | one per fragment |
| `IdListEntry` | 72 | 88 | 1.22x | one per IdList element |
| `Anchor` | 12 | 32 | 2.7x | cursor anchors, passed by value |
| `Global` | 40 | 64 | 1.6x | one per version summary |
| `FragmentOrderDim` | 32 | 56 | 1.75x | seek dimensions, cursor-local |

`FragmentSummary` is the one to watch: the SumTree aggregates one per node and
re-aggregates on every edit, so 40 → 56 bytes is a 1.4x cost on the hottest
structure in the engine. That is the price of a 128-bit actor and it was accepted
deliberately; the alternative — narrowing the identity — is §2.1, which breaks
convergence. The threefold identity growth does not multiply into the summary,
because the summary holds one identity, not one per fragment.

Two of the rows above are honest surprises rather than predictions:

- **`Global` was not in the original projection at all**, and it grew 1.6x. Its
  inline capacity dropped from 4 pairs to 2 because a pair is 24 bytes instead
  of 8, and the heap pointer costs 8 bytes even when the vector never leaves its
  inline buffer. 40 → 64 is more than the pair arithmetic alone explains. It is
  off the per-edit path (P03 removed the three per-summary Globals), so the cost
  lands per *peer* rather than per edit, and version vectors are held per
  participant. If measurement in X02 says otherwise, the lever is a tagged
  pointer or a `std::variant` of inline/heap storage, not a smaller actor.
- **`FragmentOrderDim` grew to 56** because it now holds
  `std::optional<Identity>`: §"past every origin at this locator" has no
  spelling in a 128-bit key space, so it became an explicit disengaged state
  rather than a magic all-ones identity. It is a cursor-local type, never
  stored per node, so this costs stack rather than aggregate memory.

If measurement later shows the summary growth matters, the lever is **not** to
shrink the identity — that would reintroduce §2.1. It is to stop storing an
identity in the summary at all, by observing that `max_origin` is only consulted
for equal-locator comparisons and could be kept as a per-node extremum rather
than a full copy.

## 6. What does not change

- `Locator` keeps its `uint64_t` digit representation. It is a position, not an
  identity.
- Fragment text stays bytes; scalar counting is P05's `Codec::utf8_length`.
- The version vector stays sparse. Only its key widens, and the sparse
  representation is already a packed array rather than a dense one, so a wider
  key costs bytes per *observed actor* and not per possible actor.
