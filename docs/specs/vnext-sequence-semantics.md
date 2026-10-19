# Sequence behavior and exact S01 fixture contract

**Task:** S01. **Date:** 2026-10-05. This specifies desired behavior; it does
not claim that the current engine passes F8 or implements scoped position tokens.
The checked-in fixtures are inputs and exact oracles for S02/S04a. S02 still
needs the complete incremental algorithm, transformation map and independent
interpreter before production integration changes.

## Primitive and identity scope

Only UTF-8 plaintext and uint64 IdList values are covered. A fixture is a fresh
document/stream/epoch context, with stable nonzero 128-bit actor bytes. A/B/S
are readable aliases for those bytes, not production short actor IDs. Item and
operation identities are written `actor/counter`, with checked uint64 counters.
All replicas know the fixture's initial seed; it is not a fresh local edit on
each replica. Independent fixtures do not share counters or writer state.

Text inserts name every scalar item and the operation identity that owns its
reserved interval: for k scalars and operation counter c, item counters are
c-k through c-1. The operation counter is not an item identity. Current IdList
inserts intentionally identify their sole element by the insertion operation's
identity, as documented in the adopted P06a implementation. Delete/undo/redo
identities consume their own reservation and never reuse an earlier one.

`after` lists direct causal prerequisites; its transitive closure includes the
actor's previous operation and every inserted neighbor that the editor used.
An initial item is already observed through the seed. A local undo/redo names
only that actor's original edit; peer work is not erased. The repeated original
target identifies the toggled edit, not a reused compensation operation ID.
Every compensation still has a fresh ID. Restart persistence of the local
history stacks belongs to L06, not this fixture-only task.

## Required observable behavior

1. Every valid delivery order converges, and duplicates/deferred dependency retry
   preserve the same logical operation set. Receipt alone is not application or
   durability. `arrival_orders` include reversed dependency arrival deliberately;
   replay must defer unavailable operations and drain them rather than inventing
   a different causal history.
2. The named concurrent-passage examples retain each producer's causal forward
   or reverse-typed passage. For the root branches in these fixtures, rank the
   first inserted item by counter, then actor bytes, ascending. With equal first
   counters and actor A before B, the exact expected content is `abcXYZ`.
   `aXbYcZ`, equality of two wrong replicas, and a character multiset are not
   acceptable oracles. Later causal prepend/append edits in the same unobserved
   competing passage remain in that branch for these examples.
3. An insertion after observing a merged state lands between its specified
   neighbors. `nested-post-merge-edit` is exactly `a!bcXYZ`. Concurrent replacement
   of the same seeded `a` retains both replacement inserts, ordered by the stated
   branch rule, yielding `xYb`; it does not silently select one replacement.
4. Undo hides the owner's edit while preserving peer edits. Redo restores it,
   and a subsequent insertion uses the restored identities rather than treating
   the text as a newly seeded string. The corresponding finite oracles are
   `XYZ` and `abc!XYZ`.
5. IdList equal-boundary insertions use deterministic identity order for the
   named fixture, preserving both uint64 values exactly (including one above
   2^53). A missing application ID yields typed not-found and no mutation; it
   is never a usable end element. List indices are not text byte offsets.

These are finite required examples and invariants, not a claim of maximal
non-interleaving or recognition of paragraph/Markdown intent after arbitrary
replacement. S02 must establish the achievable general rule and validate the
examples against its independent model. A concrete incompatibility is a design
counterexample to resolve there; an executor must not make the oracle weaker to
match current output. This task changes no production integration algorithm.

## Boundary and affinity examples for S04a

Tokens in the fixtures are semantic directives, not an encoding of today's
Anchor layout. They bind document, stream, primitive and epoch, retain their
mint causal state, and identify neighboring stable items or scoped START/END.
That is the input requirement, not a proof that two neighbors alone suffice for
the full representation. S04a now supplies sufficient context, mint/resolve/compare
pseudocode and an independent supplied-order model in
[vnext-boundary-semantics.md](vnext-boundary-semantics.md). S04b supplies actual public bytes.

In `ab`, mint the same boundary at byte 1 before inserting X there: Left resolves
to 1 and Right to 2 in `aXb`. Minting on an empty document, at start, and at end
must return real scoped boundaries with the same insertion affinity; a global
sentinel cannot implement all documents' endpoints. Concurrent `xy` and `XY`
at that minted boundary produce `axyXYb`, with Left=1 and Right=5. Compare the
same originally minted token in each replica, not separate post-merge mints.

The Unicode fixture `α😀b` has valid byte boundaries 0, 2, 6 and 7. Insertion at
6 gives `α😀Xb`: the old boundary's Left=6, Right=7; old end boundaries become
8. Offsets 1/3/4/5 and old length+1 are invalid mint inputs. They must not be
rounded into a nearby valid boundary. Tokens at every original scalar boundary
are enumerated, and UTF-16 conversion remains an adapter responsibility.

Deleting the seeded left neighbor concurrently with insertion yields `Xb`;
deleting both seeded neighbors yields `X`. The fixture's old gap remains a
defined caret boundary: Left=0, Right=1, with explicit collapse information
because original neighbor context was removed. This does not assert that a
strict content attachment still targets an intact passage. Such a consumer can
surface detachment from that explicit information. The S04a contract selects an explicit preceding/following referent for strict
attachments: selected loss detaches, and actual visibility restoration makes the
original token resolve again while its identity is retained. It also requires
relative order preservation of already integrated retained items from S02.
Awaiting mint state is distinct from unavailable history; wrong-object and
malformed inputs must always reject explicitly. GC/pins/translation belong to
S05/G-series tasks and are not implemented or proven here.

## Corpus and execution

The 17 fixtures under `libs/collabtext/tests/fixtures/sequence-semantics/` contain
46 operations and 29 complete arrival permutations. Their schema is
`collabtext-sequence-fixture/1`: context/actor bytes, initial scalar/list items,
operation IDs, causal parents, exact insertion/deletion/undo references, arrival
permutations, mint directives, and exact final item order/content/positions and later token comparisons.
`hidden` accounts explicitly for every inserted item absent from the final view.

The standalone Python validator uses exact integers and strict UTF-8 encoding;
it validates fixture consistency, identities, causal closure, payloads, units,
scope, scalar boundaries and arrival coverage. It does not execute the production
engine or claim to be S02's sequence interpreter. This avoids compiling a Qt
program merely to check static fixture data. Both remain separate verification
layers when S02/S03 adopt these cases.

```sh
python3 tools/validate_sequence_fixtures.py
python3 tools/test_sequence_fixtures.py
tools/verify-changed.sh --target '^tst_sequence_fixtures$'
```

`tst_sequence_fixtures` is registered with CTest and has a matching no-compilation
build target. Its eleven parser tests reject wrong content, missing causal parents,
reused identities, text operation/item aliasing, incomplete arrival permutations,
unobserved neighbors, invalid scalars, non-scalar offsets, incorrect token comparisons and foreign mint scope.
S02/S04a had to add model/exact-content execution evidence; parser success
could not remove F8/F7 expected-failure markers. S03 later removed the F8
ordering marker after the engines matched FugueMax. Do not put it back.
F7 retention remains a later gate.
