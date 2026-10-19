# Sequence ordering: the incremental repair algorithm

**Task:** S02, then S03. **Date:** 2026-10-06. §0 is the S03 outcome and the
specification production implements. §§1-7 are the S02 rule and interpreter, kept
as history: they changed no production code, and §0 supersedes their algorithm. S01
([vnext-sequence-semantics.md](vnext-sequence-semantics.md)) supplied the
required behavior and the 17 fixtures; this document supplies the algorithm, the
transformation map, and the compatibility argument that keeps S03 from being
closed by a superficial comparator tweak.

The independent interpreter is `libs/collabtext/tests/reference/SequenceModel.h`,
driven by `libs/collabtext/tests/tst_sequence_model.cpp`. Neither links
production ordering, `Locator`, identity allocation, `Buffer`, `IdList`,
`Fragment`, anchor or codec code.

## 0. S03 outcome: the production algorithm is FugueMax

**Read this section first.** S02 specified a rule (R1/R2 below, §2) and an
interpreter for it. S03 tried to apply it to the engine and found, with the
engine's own tests, that a flat fractional position cannot express it (§5a). A
spike that patched around the flat position — reusing locators, then ordering
ties replica-major — fixed forward typing and then broke placement when a peer
split a typed run while its author kept typing. The problem is not new: it is
the interleaving problem, and it has a published solution. S03 adopts it.

> Weidner and Kleppmann, *The Art of the Fugue: Minimizing Interleaving in
> Collaborative Text Editing*, arXiv 2305.00583 — Algorithm 1 (Fugue) plus the
> two FugueMax modifications of §5.3. FugueMax is *maximally non-interleaving*
> (Definition 4) and Theorem 10 shows the list order is then uniquely determined,
> so there is no design freedom left to argue about; only identity order is ours.

**S02's R1/R2 is superseded, not extended.** The two agree on every S01 fixture
and on about 98% of random histories, and they differ where it matters: R2 orders
siblings by first-item identity only, so it is not *backward* non-interleaving.
`tst_fuguemax_model::s02ModelIsSupersededByFugueMax` pins the smallest history on
which they differ (six operations): a user types a pair immediately before an
element of their own, and S02 strands that element behind an unrelated peer's
whole subtree. FugueMax keeps the three together. §§1-7 below are retained for
provenance and for the fixture rationale; where they conflict with this section,
this section governs.

### 0.1 What an element records

Every inserted run carries a **link** (`src/crdt/Placement.h`), describing its
FIRST element; later elements are right children of the one before and are
derived, never stored:

| Field | Meaning |
| --- | --- |
| `parent` | The element it hangs from; empty means the document start (a virtual root). |
| `side` | `Right` or `Left` of that parent. |
| `right_origin` | The element immediately after the insertion point at mint time, **tombstones included**; empty means the end. Equal to `parent` for a left child. |

The document is the **in-order traversal** of the tree these links form: each
node's left children (with their subtrees), the node, then its right children. The
tree depends only on the *set* of operations, so every delivery order produces the
same tree and the same list — convergence follows from the construction.

### 0.2 Minting (`SequenceOrder::mint_link`)

An edit at visible offset `o` attaches to the **visible element before `o`** (the
paper's left origin: the `(i-1)`th visible element), and the new run sits
immediately after it, ahead of any tombstones that follow. Let `R` be the element
after it in the full list.

* If the left origin has no right child, the run is its **right child**, carrying
  right origin `R`.
* Otherwise `R` is a descendant of the left origin with no left child, and the run
  is the **left child of `R`** (right origin `R`).

"Has a right child" is decided as "`R` is a strict descendant of the left origin";
the document start is the root and has a right child exactly when the list is
non-empty. A multi-range edit mints every range against the same OLD state, so an
operation's runs never depend on one another.

### 0.3 Integration (`SequenceOrder::integrate`) — the only place a position is decided

* **Right child of `P`.** The right children of `P` are consecutive subtrees
  starting right after `P`. Walk them and stop before the first sibling the new
  element precedes. FugueMax's order: the sibling with the *later* right origin
  (list order, end last) goes first; equal right origins fall back to identity.
* **Left child of `P`.** The left children are the subtrees ending immediately
  before `P`. They are ordered by identity.
* **Identity** is the engine's `Identity` order, counter first then actor bytes.
  For two whole-run siblings with the same right origin that is exactly S02's R2
  key, so every behaviour R2 got right is preserved.

A local edit mints a link and then calls this same function; a remote operation
calls it with the link it was sent. The originator and its peers therefore cannot
disagree about where an operation's items went. Splitting a run to insert inside
it is a presentation artifact: the tail's first element is the run's own
`link_at(k)`, so no tree relation changes.

### 0.4 What changed in the engine

* **Removed:** `Locator` (type, `between`, and its public declaration), the
  fragment/entry `locator` field, `FragmentOrderDim`/`LocatorDim`, the
  `max_locator`/`max_origin` summary fields, the origin index and insertion
  index, `normalize_fragments`, `SplitRelocation`, `locator_between`, and the
  Buffer remote fast path. None of them has a role when position is a relation.
* **Added:** `Placement.h` (`Side`, `Link`), `SequenceOrder.{h,cpp}` (shared by
  `Buffer` and `IdList` through one template), `Fragment::link_at/split_off`.
* **Slow path by design.** Placement is an O(n) whole-vector operation, as the
  roadmap prescribes ("use the existing general paths as the baseline, do not
  optimize"). `Fragment` runs keep the engine's run compression; per-element
  cost is paid only when an element is placed inside a run.
* **Validation.** An inserted run's `parent` and `right_origin` must be elements
  its author's own frontier covers; a left child's parent must be its right
  origin. A placement naming an element the receiver does not hold *after* the
  frontier is satisfied is `malformed` and rejected (it is not a causal wait, and
  parking it would block its actor's queue forever).

### 0.5 Wire format

`CTBO`/`CTLO` advance to **0002**. An inserted run's `"l"` (locator) is replaced by
`"k":{"p":<identity or []>,"s":<0 left|1 right>,"r":<identity or []>}`; the
`"sr"` split-relocation list is gone. List inserts carry `"k"` where they carried
`"loc"`. Version 1 frames are refused with `unsupported_version` (no migration; the
roadmap states none is required). `CTGV`/`CTGP` are unchanged at 0001.

### 0.6 Evidence

| Check | Where | Result |
| --- | --- | --- |
| Literal Algorithm 1 + FugueMax reproduces the paper's worked examples | `tst_fuguemax_model` | Figures 6 and 7, shopping list |
| Convergence, Definition 4 (1) forward non-interleaving, (3) equal-origin tie, on seeded random histories | `tst_fuguemax_model` | pass |
| Every S01 fixture, every arrival order, against the literal model | `tst_fuguemax_model` | 18 fixtures, no fixture changed |
| **Production** Buffer and IdList against the literal model, link by link | `tst_sequence_differential` | 240 seeded traces: text, item order, and every minted link equal |
| Every S01 fixture through the production engines (operation identities reproduced) | `tst_sequence_semantics` | 18 fixtures, every arrival order |
| The oracle can fail | mutation: drop the right-origin rule from `integrate` | the first trace fails |

Concurrent typing and reverse typing, forward and backward, now give `abcabcabc`;
a peer inserting inside a typed run while its author continues typing gives
`abcXYdefgh`; concurrent pastes give `abcXYZ`.

### 0.7 Limits and judgement calls (not oversights)

* **Placement is O(n) per operation.** Optimizing it (an item index, an
  order-maintenance structure) is deliberately deferred; the differential test is
  what makes that safe to do later.
* **Tombstones are never reclaimed**, so the tree only grows. This is unchanged;
  reclamation is S05/G-series.
* **A left origin may be a tombstone for the list primitive.** `IdList` anchors on
  an origin as given (tombstones included), as it always has; `Buffer` anchors on
  the visible predecessor, as the paper does.
* **Sibling identity order is the engine's** (counter first). Theorem 10 leaves the
  order of equal-origin siblings free; changing it would change which of two
  concurrent runs leads, and nothing else.
* **Undo is unchanged.** It toggles visibility by item identity and never moves a
  retained element, so an undone element keeps its place in the tree.

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

## 5a. Historical: R1 is not expressible in a flat locator order

**This section is the record of the failed attempt, not the current engine.**
FugueMax landed in §0. The F8 typing expected-fail was removed. Do not restore
it, and do not implement the locator-reuse spike described below.

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

### 5a-resolution. The locator-reuse spike, and why it was not the answer

Before FugueMax, S03 tried to keep the flat position and patch it: a keystroke
that continues its author's own run reuses that run's locator, and same-locator
ties are ordered by actor then counter. That fixed forward typing — and showed
the real defect. A peer splitting the run relocates the tail it had *observed*;
a concurrent continuation by the author still carries only the reused locator, so
it stays behind and lands in the middle: `abcghXYdef` where the intent is
`abcXYdefgh`. Reverse typing stayed `aaabbbccc`, because reuse would need one
actor's items in descending counter order inside a group, which an ascending tie
rule cannot express. Both failures have one cause: **a position that is a
coordinate cannot say what an element was typed after.** The spike also exposed
that relocation selected by contiguous counters, which a typed run is not (each
operation's own identity takes a counter).

Two further results of that detour are kept: a continuation-by-the-author fixture
(`typing-into-split-run`, `abXcde` in every arrival order), and the finding that
the S02 model orders it correctly but is not backward non-interleaving (§0).

### 5a-prior-art. This is a solved problem

Weidner and Kleppmann (arXiv 2305.00583), Table 1, classify the families. Logoot,
LSEQ and Treedoc — fractional or path identifiers sorted as a flat order, which is
what `Locator` was — interleave on forward typing. RGA and Yjs are proven forward
non-interleaving. Fugue and FugueMax are proven for forward and backward
insertion. What separates the families is not the position representation but
whether an insert records **what it was typed after**. That is §0.

## 6. Limits of the S02 rule (historical; §0.7 holds the current limits)

- **No maximal non-interleaving was claimed — and the rule did not have it.**
  R1/R2 order whole operations and do not recognize paragraph, Markdown or
  sentence intent. §0 shows R2 also fails *backward* non-interleaving in the
  pinned six-operation history; FugueMax does not.
- **"The comparator is not sufficient on its own" held.** §3's claim that
  `Locator::between` already expressed "immediately before" was false (§5a), and
  the conflict was recorded here with its counterexample rather than as a relaxed
  oracle, which is what this bullet asked for.
- **List equality-position ordering** is now the same code as the text path
  (`SequenceOrder`) and is covered by the differential test (§0.6), not only by the
  two list fixtures.
- **GC and pins are untouched.** Placement reorders nothing; retained items keep
  their identities, so an eventual certificate is unaffected. It is also not
  evidence for one — S05/G-series remain open.
- **`docs/specs/vnext-consumer-probes.md`'s WP6 exercises whole-paste
  concurrency**, so S04 must re-run the installed probes before its allowlist
  entry can be removed.

## 7. How to run this

```sh
cmake --build build-dev --target tst_sequence_model tst_fuguemax_model \
    tst_sequence_differential tst_sequence_semantics
for t in tst_sequence_model tst_fuguemax_model tst_sequence_differential tst_sequence_semantics; do
  QT_QPA_PLATFORM=offscreen ./build-dev/libs/collabtext/$t
done
python3 tools/validate_sequence_fixtures.py    # fixture consistency (S01 layer)
```

The S01 fixtures are the oracle and stay exactly as S01 left them. S02 added no
fixture and relaxed no expectation; if a future change cannot satisfy this
interpreter, the specification or the fixtures are what must change, and the
reason has to be written down.