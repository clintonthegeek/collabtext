# vNext operation identity

**Created:** 2026-10-04, as the design-gate artifact for roadmap task P06a
**Supersedes for identity purposes:** the `Lamport { uint32_t value; uint16_t
replica_id; }` in `src/crdt/Clock.h`, which this document replaces rather than
extends.

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

## 5. Measured cost

Taken on this branch before the change (`x86_64`, GCC 16.2):

| Type | Before | After | Ratio |
|---|---|---|---|
| `Lamport` → identity | 8 | 24 | 3.0x |
| `UndoMapKey` | 8 | 24 | 3.0x |
| `FragmentSummary` | 40 | 56 | 1.4x |
| `Fragment` | 104 | 120 | 1.15x |

`FragmentSummary` is the one that matters: the SumTree aggregates one per edit,
so this is the hot path. 1.4x on the summary is affordable for correctness, and
it is recorded here so that X02–X09 inherit a number rather than discovering the
regression. The threefold identity growth does not multiply into the summary,
because the summary holds one identity, not one per fragment.

If measurement later shows this matters, the lever is **not** to shrink the
identity — that would reintroduce §2.1. It is to stop storing an identity in the
summary at all, by observing that `max_origin` is only consulted for
equal-locator comparisons and could be kept as a per-node extremum rather than a
full copy.

## 6. What does not change

- `Locator` keeps its `uint64_t` digit representation. It is a position, not an
  identity.
- Fragment text stays bytes; scalar counting is P05's `Codec::utf8_length`.
- The version vector stays sparse. Only its key widens, and the sparse
  representation is already a packed array rather than a dense one, so a wider
  key costs bytes per *observed actor* and not per possible actor.
