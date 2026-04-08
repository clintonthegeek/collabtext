# Convergence Flake: `adversarial_all_insert_at_zero`

**Date discovered:** 2026-04-07
**Date resolved:** 2026-04-07
**Status:** Fixed — see §10 below
**Severity:** Medium — convergence failure under adversarial concurrent inserts at the same position; rare but real
**Test:** `tst_fuzz::adversarial_all_insert_at_zero` (`libs/collabtext/tests/tst_fuzz.cpp:337`)

---

## 1. Symptom

When five replicas concurrently insert random UTF-8 text at byte offset 0 and
then exchange all operations in shuffled order, two replicas occasionally
end up with the **same set of characters in different orders**. The diff is
typically a swap of two adjacent characters that came from different
replicas.

Captured failing diff (seed `494601222`):

```
Actual   bufs[r].text(): "...亜中" "d" "ú" "d" "n..."
Expected bufs[0].text(): "...亜中" "d" "d" "ú" "n..."
                                    ↑↑↑
                              two adjacent chars
                              swapped
```

The two strings are identical in length and character set. Only the order of
two characters near the middle differs.

---

## 2. The Test

```cpp
void adversarial_all_insert_at_zero() {
    uint64_t seed = std::random_device{}();
    qDebug() << "Seed:" << seed;
    std::mt19937 rng(seed);

    constexpr int N_REPLICAS = 5;
    constexpr int N_OPS = 50;

    std::vector<Buffer> bufs;
    for (int i = 0; i < N_REPLICAS; ++i)
        bufs.emplace_back(static_cast<uint16_t>(i + 1));

    std::vector<Operation> all_ops;

    // Every replica inserts at position 0, 50 times total
    for (int round = 0; round < N_OPS; ++round) {
        int r = rng() % N_REPLICAS;
        std::string text = random_text(rng, 3);  // 1-3 random UTF-8 chars
        auto op = bufs[r].apply_local_edit({{0, 0}}, {text});
        all_ops.push_back(op);
        check_invariants(bufs[r], ...);
    }

    // Deliver all ops to all replicas in random order per replica
    for (int r = 0; r < N_REPLICAS; ++r) {
        auto shuffled = all_ops;
        std::shuffle(shuffled.begin(), shuffled.end(), rng);
        bufs[r].apply_ops(shuffled);
        for (int pass = 0; pass < 10; ++pass) bufs[r].apply_ops({});
        check_invariants(bufs[r], ...);
    }

    // Convergence
    for (int r = 1; r < N_REPLICAS; ++r) {
        QCOMPARE(bufs[r].text(), bufs[0].text());
    }
}
```

The test seed is non-deterministic (`std::random_device`), so each invocation
exercises a different sequence. To reproduce a captured failure, hard-code
the seed.

---

## 3. Failure Rate

Measured by running the test 100 times in isolation on each version:

| Version | Failures | Rate |
|---|---|---|
| Pre-coalesce-undo (master) | 3 / 100 | 3% |
| Post-coalesce-undo | 4 / 100 | 4% |

Statistically indistinguishable; both versions exhibit the same flake at the
same rate. The undo coalescing work added in commits around 2026-04-07 is
**not** the cause — the flake predates it.

The same family of structural invariants (`check_invariants`) passes on every
intermediate state. The failure surfaces only at the final convergence
`QCOMPARE` between replicas. So whatever divergence is happening, it doesn't
trip any of INV-1..INV-9.

---

## 4. Captured Failing Seeds

For deterministic reproduction (replace the `std::random_device` line with
the chosen seed):

| Build version | Seed |
|---|---|
| Pre-coalesce | `161346347` |
| Pre-coalesce | `302550038` |
| Pre-coalesce | `1466167168` |
| Post-coalesce | `363296867` |
| Post-coalesce | `494601222` |
| Post-coalesce | `1297822208` |
| Post-coalesce | `4018929747` |
| Post-coalesce | `4278525325` |

To reproduce:

```cpp
// In adversarial_all_insert_at_zero(), replace:
//     uint64_t seed = std::random_device{}();
// with:
uint64_t seed = 494601222;
```

Then build and run:

```bash
cmake --build build-dev --target tst_fuzz -j$(nproc)
./build-dev/libs/collabtext/tst_fuzz adversarial_all_insert_at_zero
```

---

## 5. Why It Smells Like a Locator-Collision Issue

The 2026-04-06 fragment-boundary insertion bug
(`docs/reports/2026-04-06-fragment-boundary-insertion-bug.md`) had the same
qualitative shape: two adjacent characters in the wrong order, caused by
two different fragments ending up with the *same* locator and being
tie-broken by `(locator, origin)` instead of true insertion intent. That bug
was rooted in `SumTree::Cursor::slice()` leaving the cursor stuck at an
internal node, and is now fixed.

This flake has the same observable shape — adjacent characters swapped, no
structural invariants violated — which strongly suggests it's another path
to a locator collision, just hit by a different scenario.

The triggering scenario here (every insert at position 0 from many replicas
concurrently) is exactly the case where the fractional-position locator
allocator is most stressed: every new fragment wants a locator strictly less
than every existing fragment, in a tight space. The biased-midpoint algorithm
in `Locator::between` may collide when both replicas independently call
`between(min, current_first_locator)` and the gap shrinks faster than the
biased midpoint can subdivide.

That's a working hypothesis — **not yet verified**.

---

## 6. What We Know For Certain

1. The flake is **pre-existing** and unrelated to the undo-coalesce work.
2. The failure is reproducible (~3-4%) with non-deterministic seeds.
3. Captured failing seeds reproduce deterministically.
4. The internal `check_invariants` pass throughout the test — so whatever
   diverges, it diverges between two structurally-valid trees.
5. The diff is always a localized swap of adjacent characters from different
   replicas, never a missing or duplicated character.
6. The trigger scenario (concurrent inserts at the same position from many
   replicas) is precisely the locator allocator's worst case.

## 7. What We Don't Know

- Whether the divergence happens during insertion (locator collision when
  generating the new fragment's locator) or during the merge phase (when
  fragments with the same locator are being sorted by origin).
- Whether `Locator::between` is actually producing colliding locators in
  this scenario, or whether the issue is elsewhere (e.g., the relocation
  path in `apply_local_edit`'s `needs_relocation` branch, or the deferred-
  relocations code path).
- Whether the same issue happens in less adversarial scenarios at lower
  rates (might explain occasional convergence weirdness in other tests).

---

## 8. Recommended Next Steps

When picking this back up:

1. **Pin a failing seed** in the test (one of the captured ones above) so
   the bug reproduces 100% of the time during investigation.

2. **Dump locators after every `apply_local_edit`** in both replicas. Look
   for two fragments on different replicas that end up with the same
   `(locator, origin)` pair when they shouldn't, OR two fragments that have
   different locators but get sorted into different orders on different
   replicas.

3. **Inspect the `needs_relocation` path** in `Buffer::apply_local_edit`
   (Buffer.cpp ~line 837 onwards). When two replicas concurrently insert at
   the same locator group, both would independently take the relocation
   branch. The deferred-relocation logic and the `SplitRelocation` records
   that get broadcast may not be commutative under random delivery order.

4. **Test convergence with K=2 replicas first** to minimize state. Bisect
   N_OPS to find the smallest failing case, then trace by hand.

5. **Check whether the flake also reproduces with ASCII-only `random_text`**
   (set `kind < 100` so only branch 1 fires). If yes, multi-byte UTF-8 is
   not the cause, and the bug is purely about locator/origin ordering. If
   no, then UTF-8 fragment splitting is involved somehow.

---

## 9. Impact

- **Test reliability:** any CI run of `tst_fuzz` has a ~3-4% chance of
  showing a spurious failure on this single test. Other fuzz tests also use
  `std::random_device` seeds, so they may have their own latent flakes; this
  is the only one observed in the recent runs (~200+ invocations).

- **Production correctness:** the scenario (5+ replicas all inserting at
  position 0 concurrently) is somewhat synthetic but **not** unreasonable.
  If two real users are racing to type the first character of a document, or
  if a "create document" action seeds an empty doc on multiple peers and
  each peer's first edit is at offset 0, this bug could surface in
  production. Worth fixing before declaring the CRDT layer
  production-ready.

- **Severity ceiling:** the divergence is a *swap*, not a *loss*. No data is
  destroyed, no replica gets fewer characters than another. The two
  divergent replicas both contain the union of all inserted characters; they
  just disagree on the order of two of them.

---

## 10. Resolution (2026-04-07)

### Root cause

`Buffer::apply_remote_edit_fast` (the single-character fast path of
`apply_remote_edit`) inserted an incoming single-char fragment into the tree
without checking whether an existing **multi-char** fragment from a different
replica already occupied the same locator. That created an *un-normalized*
state in which a multi-char fragment (e.g. R3's `'dú'`, length 2) coexisted
at one locator with another replica's atomic fragment (e.g. R5's `'d'`,
length 1).

When `text()` walked the tree in `(locator, origin)` order it concatenated
the multi-char fragment whole:

```
(locator L, origin (3,33)) → "dú"
(locator L, origin (5,33)) → "d"
                  result   → "dúd"
```

A replica that had instead received R3's multi-char insert via the slow path
(any 2+ char insert always takes the slow path) would have called
`normalize_fragments`, atomizing R3's fragment into single chars, and would
read:

```
(L, (3,33)) → 'd'
(L, (5,33)) → 'd'
(L, (3,34)) → 'ú'
   result   → "ddú"
```

— hence the captured `dúd` vs `ddú` swap. Section 5's hypothesis ("locator
collision") was correct in spirit; the actual mechanism wasn't `Locator::between`
producing colliding values (it does, deterministically and by design), it
was the fast path failing to atomize when a colliding insert later arrived.

The bug was reachable only via the fast path because the slow path
unconditionally calls `normalize_fragments`. It surfaced specifically when
one replica created a multi-char fragment locally and a *different* replica
later inserted a single-char fragment at the same locator.

### Fix

`libs/collabtext/src/crdt/Buffer.cpp` — added a locator-collision precheck
at the top of `apply_remote_edit_fast`. For each pending insert, the new
code seeks the fragment tree by `FragmentOrderDim{ins.locator, Lamport::min()}`
with `Bias::Right`; if the cursor lands on a fragment with the same locator,
the fast path returns `false` and the operation falls through to the slow
path so `normalize_fragments` can atomize the run.

The precheck runs before any tree mutation, so falling through to the slow
path doesn't double-apply deletions. Cost is `O(K log n)` per fast-path
attempt and only rejects the fast path when an actual collision exists, so
the common no-collision case stays unchanged.

### Verification

- All 8 captured failing seeds (§4) now pass deterministically.
- 500 randomized runs of `adversarial_all_insert_at_zero` post-fix:
  **0 failures** (was ~3-4%).
- Full `ctest` suite (24 tests, including `tst_benchmark` and
  `tst_realistic`): all pass.

### Why the existing invariants didn't catch it

`check_invariants` (INV-1..INV-9) verifies *structural* validity: ordering,
byte-length consistency, fragment non-emptiness, rope sums. The
un-normalized state is structurally valid — the fragments are sorted
correctly by `(locator, origin)`, all byte counts add up, no fragment is
empty. The bug only manifests as a *semantic* divergence between replicas,
caught at the final `QCOMPARE` of `bufs[r].text()`.

A useful additional invariant would be: "no run of fragments at a shared
locator may contain a multi-char fragment together with a fragment from a
different replica." That would have flagged the un-normalized state at every
intermediate step and pointed straight at the offending op. Worth adding if
similar bugs surface again.
