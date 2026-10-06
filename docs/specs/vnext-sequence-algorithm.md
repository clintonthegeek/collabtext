# Sequence ordering: the incremental repair algorithm

**Task:** S02. **Date:** 2026-10-06. This is the complete repair specification
and its independent reference interpreter. It changes no production integration
code: S03 applies it. S01
([vnext-sequence-semantics.md](vnext-sequence-semantics.md)) supplied the
required behavior and the 17 fixtures; this document supplies the algorithm, the
transformation map, and the compatibility argument that keeps S03 from being
closed by a superficial comparator tweak.

The independent interpreter is `libs/collabtext/tests/reference/SequenceModel.h`,
driven by `libs/collabtext/tests/tst_sequence_model.cpp`. Neither links
production ordering, `Locator`, identity allocation, `Buffer`, `IdList`,
`Fragment`, anchor or codec code.

## 1. The defect, named precisely

`Buffer::normalize_fragments` (`libs/collabtext/src/crdt/Buffer.cpp:498`) finds a
run of fragments sharing one `Locator` that contains more than one actor. It
**atomizes** that run: each fragment is split into single-scalar fragments via
`Fragment::item_at` (`libs/collabtext/src/crdt/Fragment.h:163`), and the pieces
are sorted by `(locator, origin)`.

`Fragment::origin` is the identity of the fragment's *first* item, and a
fragment's items are `origin.counter + offset`. Item counters are per-actor, so
two actors that each paste three characters own item counters `1,2,3`. Ordering
by per-scalar identity is therefore **counter first, then actor bytes**, which
ranks `A/1, B/1, A/2, B/2, A/3, B/3` — the two pastes are interleaved scalar by
scalar into `aXbYcZ`.

Two independent properties are wrong, and they are separate defects:

1. **Atomization destroys group atomicity.** A burst typed or pasted as one unit
   is no longer contiguous in the result.
2. **The tie-break is per scalar where it must be per operation.** Even with the
   fragments left whole, ranking them by each fragment's own first item is only
   accidentally right; what the contract needs is a rule over *operations*.

Measured on this branch, the atomizing rule disagrees with the S01 oracle on four
fixtures whose only difference from the model is ordering — `forward-typing`
(`aXbYcZ`), `nested-post-merge-edit` (`aXbYcZ!`), `reverse-typing` (`cZbYaX`)
and `whole-concurrent-pastes` (`aXbYcZ`). `tst_sequence_model.cpp`'s
`productionAtomizingRuleIsRejected` reproduces exactly those four, so the oracle
is known to detect the defect rather than merely agree with the model.

## 2. The rule

Every logical item records the item it was inserted after: its **parent**,
`START` when at the document start. Those parent links depend only on the
operations, so the tree is identical for every arrival order. The document order
is the **pre-order walk** of that tree.

The children of one parent are ordered by two rules, in this priority:

- **R1 — precedence.** A group inserted immediately *before* a sibling group
  precedes it. Concretely, if group `g`'s right boundary is an item of sibling
  group `h` or a descendant of one, then `g < h`.
- **R2 — branch order.** Otherwise the groups occupy the same position — the
  concurrent case — and are ordered by their **first inserted item**: counter
  ascending, then actor bytes ascending. The whole group moves as one unit.

An operation's items are consecutive siblings and always keep their relative
order within the group.

R1 is what makes reverse typing reverse. In `reverse-typing`, `A/1` is inserted
at `START..END`, `A/3` at `START..A/1`, `A/5` at `START..A/3`. All three are
siblings of `START`; R1 gives `A/5 < A/3 < A/1`, so the text reads `abc` for a
user who typed `c`, `b`, `a`. R2 is the F8 repair: in
`whole-concurrent-pastes` both groups sit at `START..END`, so R1 says nothing and
R2 ranks `A/1` before `B/1` on actor bytes, keeping `abc` and `XYZ` intact.

R2's key is deliberately the **operation's first item**, not each item's own
identity. That is the whole difference from §1: per-scalar keys interleave,
per-operation keys do not.

### Why the tree is arrival-order independent

`parent` is a direct field of the insert operation. No insertion consults the
current document order to choose its parent, so the tree depends only on the
operation set. R1 and R2 are both functions of the tree plus the operation
boundaries. Therefore every delivery order yields the same tree and the same
walk, which is why convergence follows from the rule rather than being asserted
alongside it.

### The pseudocode

```
# State, per (document, stream, epoch)
parent[item]        # item id -> item id, or START
first_item[group]   # operation id -> its first inserted item identity
right_of[group]     # operation id -> right boundary at mint, or END
items_of[group]     # operation id -> ordered item ids
undone[group]       # operation id -> bool
removed[item]       # item id -> bool

integrate(op):
    if any prerequisite in op.after is not integrated: defer op; return DEFERRED
    match op.kind:
        insert:
            p = op.between.left                     # parent, fixed at mint
            right_of[op.id] = op.between.right
            first_item[op.id] = op.items[0]
            for item in op.items:                  # one consecutive sibling run
                parent[item] = p
                items_of[op.id].append(item)
        remove:
            for item in op.targets: removed[item] = true
        undo:
            undone[op.target] = true
        redo:
            undone[op.target] = false
    drain deferred operations whose prerequisites are now met

before(g, h):                        # R1
    h.right_of != END and in_span(h, g.first_item[h])

order_group(p):                      # children of parent p, canonical
    appenders = [g in groups_of(p)
                 if g.right_of == END or group_of(g.right_of).parent != p]
    sort appenders by first_item                       # R2
    out = []
    emit(g):                          # predecessors first, then g
        for other in sorted(children_of(p)) by first_item:
            if other not emitted and before(other, g): emit(other)
        out.append(g)
    for g in appenders: emit(g)
    return out

walk(p):                             # pre-order
    for g in order_group(p):
        for item in items_of[g]:
            yield item
            yield from walk(item)

visible(item): not removed[item] and not undone[group_of(item)]
```

`order_group` runs in `O(k log k)` over the `k` siblings of one parent, with
`emit` visiting each group once; the whole walk is `O(n log n)` in the item
count, the same shape as the `SumTree` walk already in the engine.

## 3. Transformation map onto current components

This is the part that makes the repair incremental. No new storage engine, no
second sequence representation, no new wire format.

| Current component | Change | Why it is enough |
| --- | --- | --- |
| `Fragment` (`src/crdt/Fragment.h:139`) | Keep `origin`, `locator`, `length`, `deletions`. Add nothing mandatory. | A fragment already *is* a group: one actor's contiguous run with `origin` the first item. `item_at` stays as the derivation the codec and S04b tokens rely on. |
| `Buffer::normalize_fragments` (`:498`) | **Remove the atomization branch.** Replace the run's per-scalar sort with a sort of the whole same-locator fragments by `(locator, origin)`. | This is the defect and the whole repair site. `origin` is already the per-operation key R2 needs. |
| `Locator` minting (`Buffer::locator_between`, `Buffer.cpp:478`) | **RETRACTED — unchanged is wrong for typing.** This row originally claimed `Locator::between` already expresses R1's "immediately before" relation. That is false. Each keystroke takes its own strictly increasing locator, so A's third character sorts above B's first no matter how same-locator ties break. See "What remains open". |
| `FragmentOrderDim` (`:101`) | Unchanged. | Its `operator<=>` is already `(locator, origin)`, and `origin` is already counter-then-actor. The comparator was never the defect; feeding it atomized scalars was. |
| `SumTree` fragment tree | Unchanged. | Ordering keys and insertion points are unchanged; only the contents of a same-locator run change. |
| `IdList::compare_anchors` (`:381`) | Apply the same rule to equal-boundary insertions. | The list facade orders by anchor, and the same-position tie is the same defect in miniature. `list-same-position-tie` and `list-local-undo-and-redo` are its oracles. |
| Undo (`UndoMap`) | Unchanged. | Undo already toggles visibility by origin and never reorders. R1/R2 do not move retained items, so an undone item keeps its place. |
| `Crdt::Identity` order (`include/collabtext/Engine.h:62`) | Unchanged. | Already counter-first then actor bytes — the R2 key. |
| Public position tokens (CTPT v1) | Unchanged bytes. | Tokens name items, not positions in a run. S03 changes no token encoding, so nothing already minted is invalidated. |

The repair is therefore one function. That is the claim the technical review has
to earn, and §5 is where it is checked.

## 4. Interaction with the boundary contract

S04a requires that the **relative order of already integrated retained items is
preserved**, and that a token minted before concurrent insertion resolves
consistently afterwards. The two rules are compatible, and the interpreter
demonstrates it rather than asserting it: `tst_sequence_model.cpp` feeds the
order produced by `SequenceModel` into the independent `BoundaryModel` and checks
all 22 named positions and 14 token comparisons across the 8 token-bearing
fixtures.

Concretely, `shared-token-concurrent-inserts` mints one boundary at `S/1..S/2`
and then has two actors insert `xy` and `XY` there. R1/R2 order the two groups
whole, so the boundary's Left resolves to 1 and Right to 5 in `axyXYb` — the
same tokens, compared in the same replica, give the fixture's declared result.
`unicode-all-boundaries` checks all eight original scalar boundaries with both
affinities, including that Left/Right differ at offset 6 (`α😀Xb`) and coincide
at the ends.

The one thing this does **not** give is a stronger guarantee than the current
one. A token's meaning is unchanged; what changes is that concurrent insertions
no longer interleave, so offsets that used to land between two scalars of another
actor's paste now land at that paste's edges. Tokens minted under the old
ordering were minted against an already-interleaved document and are not
reinterpreted: there is no migration, because there is no published format and
no production store (see `docs/roadmap/2026-10-reliability/DECISIONS.md`).

## 5. Technical review

Checked against the S02 acceptance list. Each row is a property the interpreter
executes, not an argument.

| Required check | Evidence | Result |
| --- | --- | --- |
| Nested concurrent runs | `nested-post-merge-edit`: `B/5` is inserted between `A/1` and `A/2` after observing the merge, giving `a!bcXYZ` with `B/5` before `A/2` under R1 while `B/1..B/3` stay whole after it | pass |
| Reverse insertion | `reverse-typing`: three siblings inserted right-to-left read `abc`, not `cba`; R1 is what produces it | pass |
| Post-merge edits | `undo-redo-and-reinsert`: after undo and redo of a peer's observed branch, `A/8` lands between `A/3` and `B/1` using the restored identities, giving `abc!XYZ` | pass |
| Undo | `local-undo-preserves-peer` hides `A/1..A/3` while `B/1..B/3` survive as `XYZ`; `list-local-undo-and-redo` restores `A/1` on redo | pass |
| Reuse of existing storage | §3's map changes one function body and adds no type, field, file or wire format; `Fragment` is already the group abstraction | pass, subject to §6 |
| Concurrent equal-position groups | `whole-concurrent-pastes`, `forward-typing`, `shared-token-concurrent-inserts`, `concurrent-replacements`: R2 keeps each group whole | pass |
| Bounded enumeration | 12 generated cases (2–3 groups × forward/reverse), each asserting exact text **and** that no actor's scalars form two runs | pass |
| Every delivery order converges | All 29 supplied arrival orders, including the reversed ones, produce identical order, hidden set and text | pass |
| Duplicate and deferred delivery | Duplicates counted not reapplied; `B/6` before its prerequisites defers, then drains to `a!bcXYZ` | pass |
| The oracle can reject | `productionAtomizingRuleIsRejected` reproduces `aXbYcZ`, `aXbYcZ!`, `cZbYaX`, `aXbYcZ` for exactly the four comparable fixtures | pass |

## 5a. What remains open: R1 is not expressible in a flat locator order

**S03 disproved part of this specification's transformation map, and the
production test is what caught it.** `tst_sequence_semantics` drives real
`Buffer` instances, not the reference model, and it separates two cases that
looked like one:

| Case | Shape | After removing the atomization |
| --- | --- | --- |
| Concurrent **pastes** | one operation holding all its items | **fixed** — `abcXYZ`, contiguous |
| Concurrent **typing** | one operation per keystroke | **still interleaved** — `aaabbbccc` |

The cause is representational, and it is measurable without any CRDT reasoning.
Actor A typing `a`,`b`,`c` into an empty document gets locators `[m]`, `[m+1]`,
`[m+2]`. Actor B, typing into its own empty view, gets the same three. So `A/3`
is forced above `B/1`, and `b` can never precede `X`:

```
A/1 == B/1  : 1     both first keystrokes occupy the same position
A/3 <  B/1  : 0     forward-typing's oracle requires this to be true
```

No tie-break rule can fix that, because the constraint is in the locator chain
rather than in the ordering of equal locators. **R1 (group-atomic ordering) is
correct as a specification and is not representable by a flat fractional index.**
It needs position to be tree-structured — or, as the S03 spike investigates,
locator *reuse*: a local insert that continues the author's own contiguous
preceding fragment inheriting that fragment's locator, which keeps the whole
typed run inside one same-locator run where R2 can then keep it contiguous.

The honest status of §3's table is therefore: rows for `Fragment`,
`normalize_fragments`, `FragmentOrderDim` and `Identity` hold for the paste
case; the `Locator` row does not. This document does not claim the typing case.

S01's typing oracles are **not** weakened to `ababab` or any other convergent
output. The requirement stands, the F8 typing marker stays expected-fail in
`tst_evaluation_regressions.cpp`, and the capability manifest records the paste
case as repaired and the typing case as blocked.

### 5a-resolution. Forward typing is repaired by locator reuse, not by tree positions

The spike's question was whether the flatness of the locator is what blocks
typing. It is not: the engine already has same-locator runs (a fragment is one
locator plus its items), so the forced interleave came from two choices, not
from the representation:

1. **Minting.** Every keystroke took a fresh `between()` locator. A local insert
   that continues the author's own contiguous preceding fragment now reuses that
   fragment's locator, so a typed run is one same-locator group, the same shape a
   paste has always had. No locator format, codec field or `schema_version`
   change.
2. **Tie order.** Same-locator ties were ordered by `Identity` (counter first),
   which alternates `A/1, B/1, A/2, B/2` within a group. They are now ordered
   **replica-major**: origin actor bytes, then counter (`fragment_precedes`,
   `Buffer.cpp`). This **amends R2** for operations whose first items have
   different counters; the S01 fixtures do not distinguish the two, and
   `tst_sequence_model` is unaffected. Any other site that orders same-position
   items must follow: `IdList` still orders `(locator, origin)` counter-first and
   is deliberately untouched here, so text and list can order the same
   concurrent tie differently until it is brought across.

Reuse also exposed a latent assumption in split relocation: the relocated set
was found by walking `counter == next_expected`, but a typed run is not
counter-contiguous (each operation's own identity takes a counter). It is now
"every fragment at the old locator that follows the split point in group order",
and on the remote path additionally limited to what the originating operation's
version had observed, so a peer's concurrent additions to the group are not
moved by an operation that never saw them. `SplitRelocation.fragment_length` is
still written but no longer read.

**Still open: reverse (prepend) typing.** Reusing a locator for prepends would
need one actor's items in *descending* counter order inside a group, which an
ascending tie rule cannot express, and distinct locators reproduce the
per-scalar interleave (`aaabbbccc`). That case, and only that case, needs a
position-representation decision. It remains an expected failure in
`tst_sequence_semantics` (`reverse` row).

## 6. Limits, and what S03 must not inherit as settled

- **No maximal non-interleaving is claimed.** R1/R2 order whole operations. They
  do not recognize paragraph, Markdown or sentence intent, and they do not merge
  concurrent typing at the same position into one paragraph. S01 was explicit
  that these are finite required examples, not a recognition claim.
- **The comparator is not sufficient on its own.** §3 is the whole claim, and it
  is the first thing S03 should try to falsify. If S03 finds that locator
  minting cannot express "immediately before" for some existing case, the
  conflict belongs here with the exact counterexample — not in a relaxed oracle.
- **Reverse typing relies on `Locator::between` producing a strictly decreasing
  locator.** That is observed behavior of `Buffer::locator_between`
  (`Buffer.cpp:478`) and is covered by `reverse-typing`, but it is an assumption
  about a function the S03 spike touches. §5a supersedes this: the constraint
  that actually blocks typing is not monotonicity but the flatness of the
  representation.
- **List equality-position ordering is specified but only lightly exercised.**
  Two list fixtures cover it. `IdList::compare_anchors` needs the same treatment
  as the text path and has proportionally less evidence.
- **Paste and forward typing are applied; reverse typing is not.**
  `normalize_fragments` no longer atomizes, so concurrent pastes keep their
  passages intact (`abcXYZ`, exact), and locator reuse keeps forward typing
  grouped (§5a-resolution). Concurrent reverse typing still interleaves. `docs/specs/vnext-consumer-probes.md`'s WP6 exercises
  whole-paste concurrency, so S04 must re-run the installed probes before its
  allowlist entry can be removed — this document does not authorise that.
- **GC and pins are untouched.** R1/R2 reorder nothing; retained items keep
  their identities, so an eventual certificate is unaffected. It is also not
  evidence for one — S05/G-series remain open.

## 7. How to run this

```sh
cmake --build build-roadmap-debug --target tst_sequence_model
QT_QPA_PLATFORM=offscreen ./build-roadmap-debug/libs/collabtext/tst_sequence_model
python3 tools/validate_sequence_fixtures.py    # fixture consistency (S01 layer)
```

The S01 fixtures are the oracle and stay exactly as S01 left them. S02 added no
fixture and relaxed no expectation; if a future change cannot satisfy this
interpreter, the specification or the fixtures are what must change, and the
reason has to be written down.