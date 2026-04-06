# Fragment Boundary Insertion Bug

**Date:** 2026-04-06
**Status:** Confirmed, reproducing test written, fix pending
**Severity:** High — causes view desynchronization during concurrent editing
**Reproducing test:** `tst_buffer::sequential_insert_char_by_char_text`

---

## 1. Symptom

When two panes are open and both users are typing, the receiving pane's
QTextDocument gradually diverges from its CRDT Buffer. The text shows
characters in the wrong order — typically two adjacent characters
swapped. Once divergence occurs, every subsequent local edit compounds
it, and the pane becomes unrecoverably garbled.

Example from the test app:

```
Buffer:  "A quick jbfrown fox"    (b before f)
Qt doc:  "A quick jfbrown fox"    (f before b — correct)
```

The user typed 'j' then 'f' at adjacent positions. The Qt document has
them in the correct order. The Buffer has them swapped.

---

## 2. Reproducing Test

```cpp
void sequential_insert_char_by_char_text() {
    Buffer alice(1);
    Buffer bob(2);

    // Alice types "A quick brown fox" one character at a time
    std::string text = "A quick brown fox";
    for (size_t i = 0; i < text.size(); ++i) {
        auto op = alice.apply_local_edit(
            {{static_cast<uint32_t>(i), static_cast<uint32_t>(i)}},
            {std::string(1, text[i])});
        bob.apply_ops({op});
    }
    QCOMPARE(bob.text(), std::string("A quick brown fox"));

    // Bob inserts 'j' at byte 8 (between "A quick " and "brown")
    bob.apply_local_edit({{8, 8}}, {"j"});
    QCOMPARE(bob.text(), std::string("A quick jbrown fox"));

    // Bob inserts 'f' at byte 9 (between 'j' and 'b')
    bob.apply_local_edit({{9, 9}}, {"f"});
    QCOMPARE(bob.text(), std::string("A quick jfbrown fox"));
    // ACTUAL: "A quick jbfrown fox" — FAIL
}
```

**Key condition:** Alice types character-by-character. If Alice types the
entire string in one `apply_local_edit` call (one fragment), the bug does
NOT reproduce. The bug requires many single-character fragments from a
remote replica.

---

## 3. Root Cause

The bug is in `Buffer::apply_local_edit()` at Buffer.cpp line 788, in
the "Phase 0: Prefix copy" section.

### 3.1 The Prefix Copy

When `apply_local_edit({{9, 9}}, {"f"})` runs (the second insert), it
needs to:

1. Copy all fragments before byte 9 into a new tree (the "prefix")
2. Insert the new 'f' fragment
3. Copy all remaining fragments (the "suffix")

The prefix copy uses `cursor.slice({first_start})`:

```cpp
// ---- Phase 0: Prefix copy (up to first range) ----
uint32_t first_start = ranges[order[0]].first;  // = 9
new_tree.push_tree(cursor.slice({first_start}));
```

### 3.2 The `slice` Semantics

`SumTree::Cursor::slice(target)` at SumTree.h line 113 extracts items
from the current position up to `target`. The inclusion rule at line 134
is:

```cpp
if (item_end <= target) {
    result.push_item(Item(lf.items[level.index]));
    m_position = item_end;
    level.index++;
} else {
    return result;
}
```

An item is included if its cumulative end position is **less than or
equal to** the target. This is a `<=` comparison.

### 3.3 The Fragment Layout

After Alice types "A quick brown fox" character-by-character, the
fragment tree on Bob's side has 17 single-character fragments (one per
char). After Bob inserts 'j' at byte 8, the layout is:

```
Fragment  | Text | Visible bytes | Cumulative end
----------|------|---------------|----------------
alice 'A' | "A"  | 1             | 1
alice ' ' | " "  | 1             | 2
alice 'q' | "q"  | 1             | 3
alice 'u' | "u"  | 1             | 4
alice 'i' | "i"  | 1             | 5
alice 'c' | "c"  | 1             | 6
alice 'k' | "k"  | 1             | 7
alice ' ' | " "  | 1             | 8
bob   'j' | "j"  | 1             | 9  ← item_end = 9
alice 'b' | "b"  | 1             | 10
alice 'r' | "r"  | 1             | 11
...
```

### 3.4 The Off-By-One

When `slice({9})` runs with target = 9:

- It iterates through fragments, accumulating visible bytes.
- When it reaches Bob's 'j' fragment: `item_end = 9`, `target = 9`.
- The test is `item_end <= target`, i.e., `9 <= 9` = **true**.
- So 'j' is **included** in the prefix slice.

The cursor then sits at position 9, pointing at Alice's 'b' fragment.

Then the straddling check at line 791:

```cpp
if (cursor.item() && cursor.position().value < first_start) {
```

`cursor.position().value` is 9, `first_start` is 9. `9 < 9` is
**false**, so no straddling split occurs.

### 3.5 The Consequence for the Insert

The prefix (`new_tree`) now contains everything through 'j'. The
insert phase at line 819 computes the `lo` locator:

```cpp
Locator lo = new_tree.empty() ? Locator::min() : new_tree.last().locator;
```

`new_tree.last()` is Bob's 'j' fragment. So `lo = locator_of_j`.

The `hi` locator comes from the next fragment after the prefix, which
is Alice's 'b'. So `hi = locator_of_b`.

The new 'f' fragment gets `Locator::between(locator_of_j, locator_of_b)`.

**But 'j' was just created by Bob (replica 2) with a locator between
Alice's ' ' (space) and Alice's 'b'.** The locator space is:

```
locator_of_space < locator_of_j < locator_of_b
```

`Locator::between(locator_of_j, locator_of_b)` produces a locator for
'f' between 'j' and 'b'. This positions 'f' AFTER 'j' in the CRDT
ordering, which is correct.

Wait — then why does 'f' end up after 'b' in the visible text?

### 3.6 The Real Problem: Locator Collision

Let me reconsider. When Alice types character-by-character, each
character fragment gets its own locator via `Locator::between`. But
when Alice types 'b' right after 'r' (or rather, types sequentially),
consecutive characters from the same replica may share the same locator
or have very close locators.

Actually, looking more carefully at the engine: when Alice types 'b'
at position 8 (as the 9th character in the original string), the
engine creates a new fragment with a locator between the previous
character's locator and `Locator::max()` (since it's appending at the
end at that point).

When Bob then inserts 'j' at byte 8, the engine needs a locator
between Alice's ' ' (space, position 7) and Alice's 'b' (position 8).
It calls `Locator::between(locator_of_space, locator_of_b)`.

Then when Bob inserts 'f' at byte 9, the engine copies the prefix
including 'j' (because of the `<=` in slice), and needs a locator
between `locator_of_j` and the next fragment.

**The critical issue:** because 'j' was included in the prefix, `lo`
is set to `locator_of_j`. But the next fragment in the cursor is 'b',
which has `locator_of_b`. If `locator_of_j` and `locator_of_b` are
very close (because 'j' was squeezed between space and 'b'), and the
`Locator::between` algorithm can't produce a value between them at the
current depth, it may need to extend the depth. But the key issue is
that the fragment ordering in the final tree is determined by (locator,
origin) ordering during the sort phase.

### 3.7 Actually, the Real Issue Is Simpler

Let me reconsider the whole flow. The `slice({9})` includes 'j' in the
prefix. This means 'j' is in `new_tree`. Then the insert phase puts
'f' after the last item in `new_tree` (which is 'j'). Then the suffix
(starting from 'b') is appended.

The final fragment order in `new_tree` is:
```
[A][ ][q][u][i][c][k][ ][j] [f] [b][r][o][w][n][ ][f][o][x]
                       ↑     ↑   ↑
                    prefix  new  suffix
```

So the visible text is "A quick j**f**brown fox" — which IS correct!

But the bug says we get "jbfrown". So something else must be going on.

Let me re-examine. After `slice({9})`, the cursor is at position 9,
pointing at 'b'. There's no pending fragment. The insert places 'f'
between the end of the prefix and the start of the suffix. The suffix
starts with 'b'.

So the order should be: prefix(j) + insert(f) + suffix(b...) = "jfb..."

But the test shows "jbf...". This means 'b' is coming before 'f' in
the output.

This can only happen if:

**The suffix is NOT built by simply appending remaining cursor items.**
Looking at the rest of `apply_local_edit`, after the insert, there's a
final "sort phase" that reorders fragments by (locator, origin). If
'f' has a locator that sorts AFTER 'b' in the final ordering, 'b' will
appear before 'f' regardless of insertion order.

### 3.8 The Sort Phase

At the end of `apply_local_edit`, the fragments are NOT kept in the
order they were pushed to `new_tree`. They go through a sort phase that
orders by (locator, origin). This is essential for CRDT convergence —
all replicas must produce the same visible text regardless of operation
arrival order.

The issue is that 'f' gets a locator that sorts AFTER 'b''s locator,
even though 'f' was meant to appear BEFORE 'b' in the visible text.

Why? Because the prefix copy included 'j', so `lo = locator_of_j`.
And the next fragment is 'b' with `locator_of_b`. The locator for 'f'
is `Locator::between(locator_of_j, locator_of_b)`.

But here's the key: `locator_of_j` was itself produced by
`Locator::between(locator_of_space, locator_of_b)` during the
previous insert. So:

```
locator_of_space < locator_of_j < locator_of_b
```

And now:

```
locator_of_j < locator_of_f < locator_of_b
```

In the sort phase, fragments are ordered by (locator, origin). Since
`locator_of_f` is between `locator_of_j` and `locator_of_b`, the
order should be: j, f, b. That's correct!

Unless `locator_of_j == locator_of_b`, and the between() call can't
produce a value between them...

Actually wait. Let me reconsider the whole thing by looking at the
ACTUAL bug more carefully.

---

## 4. Revised Root Cause Analysis

The issue is more subtle than a simple `slice` off-by-one. Let me
trace through what actually happens when `slice({9})` includes 'j'.

### 4.1 What Happens When slice Includes 'j'

`slice({9})` copies fragments up to and including those whose
cumulative visible byte end is <= 9. The 'j' fragment ends at
cumulative byte 9, so it's included. After the slice:

- `new_tree` contains: [A][ ][q][u][i][c][k][ ][j]
- `cursor` is at position 9, pointing at 'b'
- `pending` is empty

The straddling check (`cursor.position().value < first_start`) is
`9 < 9` = false, so no split.

### 4.2 The Insert

`lo = new_tree.last().locator` = locator of 'j'.
`hi` = locator of 'b' (from cursor.item()).

If `locator_of_j < locator_of_b`, then `hi = locator_of_b` and we
get `locator_of_f = Locator::between(locator_of_j, locator_of_b)`.
The sort order is j < f < b. **Correct.**

But what if `locator_of_j == locator_of_b`?

This happens when 'j' was inserted right before 'b' and the
engine used a relocation to split the locator group. After
relocation, 'j' has a NEW locator (not the original 'b' locator).
But in the ORIGINAL tree (before this edit), 'b' still has its
original locator.

**Critical:** The cursor iterates the ORIGINAL tree (`m_fragment_tree`).
But 'j' was created in a PREVIOUS `apply_local_edit` call, which
rebuilt the tree. So 'j' IS in the current tree. Its locator was
determined during that previous edit.

Let me check: does 'j' get a locator strictly between space and 'b'?
In the previous `apply_local_edit({{8, 8}}, {"j"})`:
- `lo` = locator of space (last item in prefix, which was
  everything through byte 7)
- `hi` = locator of 'b' (next fragment in cursor)

If `locator_of_space < locator_of_b`, then
`locator_of_j = Locator::between(locator_of_space, locator_of_b)`.
This gives `locator_of_space < locator_of_j < locator_of_b`.

Then in the SECOND edit (insert 'f'):
- `lo` = locator_of_j (end of prefix, which includes 'j')
- `hi` = locator_of_b (next in cursor)
- `locator_of_f = Locator::between(locator_of_j, locator_of_b)`

So `locator_of_j < locator_of_f < locator_of_b`. Final order: j, f, b.

**This should be correct.** So why does the test fail?

### 4.3 The Missing Piece: What If slice DOESN'T Include 'j'?

What if the correct behavior is for `slice({9})` to NOT include 'j'?

If 'j' is NOT in the prefix:
- `new_tree` contains: [A][ ][q][u][i][c][k][ ]
- `cursor` at position 8, pointing at 'j'
- `lo` = locator_of_space (last item in prefix)
- `hi` = locator_of_j (next in cursor)
- `locator_of_f = Locator::between(locator_of_space, locator_of_j)`

So `locator_of_space < locator_of_f < locator_of_j`. Final sort
order: space, f, j, b. Visible text: "...k **f**j**b**rown..."

That's ALSO wrong — 'f' would be before 'j', but we want it after 'j'.

### 4.4 The Actual Problem: Prefix Boundary

The issue is that the insert at byte 9 needs the new fragment to go
between 'j' (at byte 8-9) and 'b' (at byte 9-10). The prefix should
include everything BEFORE byte 9. In visible byte terms:

- Bytes 0-7: "A quick " (8 bytes) — definitely prefix
- Byte 8: "j" (1 byte) — this IS before byte 9, should be in prefix
- Byte 9: "b" (1 byte) — this is AT byte 9, should NOT be in prefix

So including 'j' in the prefix is correct! The `<=` in slice is right.

But then we showed the locator computation should also be correct.
So the bug must be somewhere else in the pipeline — perhaps in the
final sort phase, or in the suffix copy.

### 4.5 Let's Actually Debug It

Rather than continuing to theorize, the definitive approach is to
add instrumentation to `apply_local_edit` that dumps the fragment
tree before and after the operation, showing each fragment's
locator, origin, text, and visible flag.

---

## 5. What We Know For Certain

1. **The bug is in the CRDT engine**, not the view layer. The
   reproducing test (`sequential_insert_char_by_char_text`) operates
   purely on `Buffer` objects with no Qt involvement.

2. **The bug requires character-by-character remote text.** When
   Alice types the whole string in one `apply_local_edit` call (one
   fragment), the bug does not reproduce. When Alice types one
   character at a time (many fragments), it does.

3. **The bug manifests on the second of two adjacent inserts.** The
   first insert (byte 8) works correctly. The second insert (byte 9)
   places the new character one position too late in the sort order.

4. **The result is deterministic.** Given the same fragment structure
   (many single-char fragments from Alice, then two sequential inserts
   from Bob), the bug always produces the same wrong output.

5. **The visible text() method agrees with the fragment tree.** The
   divergence is between what `Buffer::text()` returns and what the
   caller expected, not between internal state and text().

---

## 6. Recommended Fix Approach

The fix needs to be in `Buffer::apply_local_edit()`, specifically in
how it determines the insertion point within the fragment tree. The
investigation so far points to either:

**A) The `slice` boundary behavior** — line 788 of Buffer.cpp. The
`<=` comparison in `SumTree::slice` may be including one fragment too
many in the prefix when the target falls exactly at a fragment
boundary. Changing to `<` would exclude the boundary fragment, but
this would change the semantics for ALL slice users and needs careful
analysis.

**B) The locator computation after the prefix** — lines 821-891.
After the prefix is built, the `lo`/`hi` locator computation
determines where the new fragment sorts. If the included-boundary
fragment changes `lo` to a value that places the new fragment on the
wrong side of existing fragments, the fix may be to adjust how `lo`
is determined when the prefix ends exactly at the target byte.

**C) The sort/relocation phase** — the final tree reconstruction may
be reordering fragments in a way that doesn't respect the intended
insertion point.

### Recommended debugging step

Add fragment-level tracing to `apply_local_edit` that dumps, for the
failing test case:

1. The fragment tree before the edit (locator, origin, text, visible
   for each fragment)
2. The prefix contents after `slice`
3. The `lo`, `hi`, and computed locator for the new fragment
4. The final fragment tree after the edit

This will pinpoint exactly which step produces the wrong ordering.

---

## 7. Impact

This bug affects any scenario where:
- Remote text arrives character-by-character (which is always the case
  in the file-sync transport, since each keystroke is a separate op)
- The local user types in the middle of that remote text

This is a core concurrent editing scenario. The bug must be fixed
before the editor can be used reliably for collaboration. However,
single-user editing is unaffected (no remote fragments to interact
with), and editing at the end of the document (appending) is also
unaffected (no fragment boundary straddling).
