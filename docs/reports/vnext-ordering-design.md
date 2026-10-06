# Ordering repair design record — S02

**Task:** S02, specify the ordering repair. **Date:** 2026-10-06.
**Baseline:** `b5de626`. **Status:** design and independent model complete; no
production code changed.

The specification is
[docs/specs/vnext-sequence-algorithm.md](../specs/vnext-sequence-algorithm.md).
This file records what was decided, what was checked, what was rejected, and what
S03 inherits as an open risk.

## The decision

`Buffer::normalize_fragments` atomizes any same-`Locator` run containing more
than one actor into single scalars and sorts the pieces by per-scalar identity.
Item counters are per-actor, so two three-character pastes own counters `1,2,3`
each and the counter-first ordering interleaves them into `aXbYcZ`.

The repair keeps whole operations contiguous and orders same-position operations
by their **first inserted item**: counter ascending, then actor bytes ascending.
Ordering is the pre-order walk of the insert-after tree, with an explicit
precedence rule for groups inserted immediately before a sibling.

Three properties make this more than a comparator tweak:

- The comparator was never the defect. `FragmentOrderDim::operator<=>` is already
  `(locator, origin)`, and `origin` is already the per-operation key. Feeding it
  atomized scalars was the error. The change is confined to one function body.
- The tree depends only on the operations, so convergence follows from the rule
  instead of being asserted next to it.
- `Fragment` already *is* the group — one actor's contiguous run with `origin` as
  its first item — so no new storage, field, type or wire format is introduced.

## What was executed

`libs/collabtext/tests/reference/SequenceModel.h` is an independent interpreter.
It links neither production ordering nor `Locator`, identity allocation,
`Buffer`, `IdList`, `Fragment`, anchor or codec code, and it shares nothing with
`BoundaryModel` except the hand-off of a total order.

```
tst_sequence_model   8 passed, 0 failed
  everyFixtureOrderAndText          17 fixtures: exact order, hidden set, text
  everyArrivalOrderConverges        29 arrival orders, identical outcomes
  duplicateAndReversedDelivery      duplicates counted; deferred then drained
  namedTokenPositionsAndComparisons 8 token fixtures: 22 positions, 14 comparisons
  boundedEnumeration                12 generated cases: exact text + contiguity
  productionAtomizingRuleIsRejected the oracle rejects the current production rule
```

## The oracle was checked for teeth

A model that agrees with itself proves nothing, and the roadmap records that
three earlier green runs here had the same shape — a check that reported success
after failing. So the last test reconstructs production's atomizing rule and
shows the S01 fixtures reject it.

Restricted to the four fixtures where ordering is the *only* difference (no seed,
inserts only), the atomizing rule produces exactly what the engine produces
today, and the model produces the fixture's oracle:

| Fixture | Atomizing rule (today) | Model / S01 oracle |
| --- | --- | --- |
| `whole-concurrent-pastes` | `aXbYcZ` | `abcXYZ` |
| `forward-typing` | `aXbYcZ` | `abcXYZ` |
| `reverse-typing` | `cZbYaX` | `abcXYZ` |
| `nested-post-merge-edit` | `aXbYcZ!` | `a!bcXYZ` |

Restricting to those four matters. A naive atomizing sketch also "disagrees" with
the nine seeded fixtures — it ignores seeds and deletes, so its output differs
for reasons unrelated to ordering. Counting those would have made the assertion
look stronger while actually proving less, and the test asserts exactly four
instead.

## What the review checked

The S02 acceptance list, item by item. All evidence is in the table in
§5 of the specification; each row is executed, not argued.

- Nested concurrent runs, reverse insertion, post-merge edits and undo each have
  a named fixture that the interpreter reproduces exactly.
- Storage reuse is argued by the transformation map and is the weakest row: it
  is a claim about code S03 has not yet written, so §6 of the specification
  names it as the first thing S03 should try to falsify.
- Duplicate and deferred delivery are executed rather than assumed:
  `nested-post-merge-edit` delivered as `B/6, A/4, B/4` defers `B/6` and then
  drains to `a!bcXYZ`.

## Defects found in the model while writing it

Recorded because they are the reason the model is trustworthy, and because each
was a real bug rather than a test adjustment:

1. **Undo targeted an item id where the fixture names an operation.** The undo
   lookup missed, so `local-undo-preserves-peer`'s second arrival order never
   drained.
2. **Redo cleared the record but not the flag.** The group stayed invisible after
   restore, so `list-local-undo-and-redo` lost `A/1`.
3. **A comparison used a QString and a `std::string`.** A compile error, not a
   behavioural one, and it is written up separately in EXECUTION.md's rule about
   not mistaking a compiler failure for proof of a defect.

A fourth was in the test rather than the model: the S01 `expected.order` field is
the **visible** order, with hidden items named separately. The first run compared
it against the total order and failed on `concurrent-replacements` until the
distinction was made explicit. It is now also asserted that visible plus hidden
equals the total order, so no item can be quietly dropped.

## Rejected alternatives

- **Keep atomizing, order by group.** Preserves contiguity only if every group is
  reconstructed afterwards, which means holding group metadata that `Fragment`
  does not carry through a split. Strictly more state than keeping the fragment.
- **A fraction-index scheme (Yjs/LSEQ-style) with a random tie-break.** Fixes the
  interleave, but a random or arrival-order tie-break makes content
  non-deterministic across replicas, which fails S01's exact-content requirement
  outright. Counter-then-actor is deterministic and already the engine's own
  `Identity` order.
- **Order by timestamp.** Clocks are not a causal order and are not available in
  the operation; the engine has no notion of one.
- **Rewrite the tree as an RGA.** That is a replacement sequence engine, which
  DECISIONS.md excludes and which would discard the working `SumTree`,
  `UndoMap` and codec integration.
- **Weaken the oracle to "replicas agree".** This is precisely what let
  `aXbYcZ` ship as convergence. The fixtures keep exact content and the model is
  held to it.

## What S03 inherits

- The specification above, with R1/R2 and the transformation map.
- An executable oracle: 17 fixtures, 29 arrival orders, 22 token positions, 14
  comparisons, 12 enumerated cases, and a counterexample test that must keep
  reproducing the interleave.
- One open risk, stated plainly: reverse typing depends on
  `Buffer::locator_between` producing a strictly decreasing locator, and S03 will
  be touching the code around it. If that changes, re-check the specification
  rather than assuming it.
- Under-evidenced area: equal-boundary `IdList` ordering has two fixtures against
  six text fixtures.

## Correction after S03 (2026-10-06)

S03 ran the rule against production `Buffer` rather than only against this model,
and the production test disproved part of §"The decision" above.

**Retracted.** The transformation map claimed `Buffer::locator_between` already
produces the "immediately before" relation R1 needs, so locator minting could
stay unchanged. That is false. Each keystroke takes its own strictly increasing
locator, so two actors typing into their own empty views receive the *same*
locator sequence and A's third character is forced above B's first:

```
A/1 == B/1  : 1     both first keystrokes occupy one position
A/3 <  B/1  : 0     forward-typing's oracle requires this to be true
```

No tie-break rule can repair that; the constraint is in the flat locator chain,
not in the ordering of equal locators. R1 remains a correct specification and is
not representable by a flat fractional index.

**What was applied.** Removing the atomization alone fixes concurrent *pastes*:
one operation holds all its items, so keeping fragments whole is sufficient.
`aXbYcZ` becomes `abcXYZ`, verified for two-actor, unequal-length and three-actor
cases including contiguity. That part of the analysis — the comparator was never
the defect, feeding it atomized scalars was — held up under production.

**What was not applied.** Concurrent typing still interleaves. S01's typing
oracle was not weakened; the F8 marker is split so the paste case passes exactly
and the typing case stays expected-fail, tagged *blocked: typing locator
minting*.

The lesson is about this document, not about the code: the independent model
cannot catch this class of defect, because it never uses locators. The rule was
specified, modelled and reviewed, and the claim that mattered most was wrong
because no test exercised the representation it depended on.

## Still pending

Ordering repair is partial. Concurrent pastes are repaired and verified; the
installed WP6 probe exercises whole-paste concurrency but its allowlist entry
cannot be removed until S04 re-runs the installed probes. Concurrent typing
remains blocked on the position representation, with the evidence above attached.
No benchmark ran.