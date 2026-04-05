# Zed-Style Cursor Walk Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rewrite `apply_local_edit` to use Zed's locator reassignment pattern, eliminating the post-processing sort/normalize/rebuild and the ordering check. Every fragment pushed to the new tree gets a locator guaranteed to be > all previous fragments.

**Architecture:** During the cursor walk, every fragment pushed to `new_tree` gets its locator reassigned via `Locator::between(new_tree.last().locator, fragment.locator)`. This produces monotonically increasing locators, matching Zed's approach. Deferred relocations and post-sort are eliminated. SplitRelocations are no longer generated. Wire format unchanged (field stays, always empty).

**Tech Stack:** C++20, Qt6 Test, CMake

**Spec:** `docs/superpowers/specs/2026-04-05-zed-style-cursor-walk-design.md`

**Build/test commands:**
```bash
cmake --build build-dev -j$(nproc)
ctest --test-dir build-dev --output-on-failure -E "tst_realistic|tst_benchmark"
```

---

## File Map

| File | Action | Responsibility |
|------|--------|---------------|
| `libs/collabtext/src/crdt/Locator.cpp` | Modify | Handle equal arguments in between() |
| `libs/collabtext/src/crdt/Buffer.cpp` | Modify | Rewrite apply_local_edit insert phase + remove post-processing |
| `libs/collabtext/tests/tst_locator.cpp` | Modify | Add test for between() with equal arguments |

---

### Task 1: Fix Locator::between for equal arguments

Zed's pattern calls `Locator::between(max_id, fragment.id)` where max_id
can equal fragment.id (same-locator fragments). Our version asserts `lo < hi`.
We need to handle `lo == hi` by descending to the next digit level, matching
Zed's implementation.

**Files:**
- Modify: `libs/collabtext/src/crdt/Locator.cpp:30`
- Modify: `libs/collabtext/tests/tst_locator.cpp`

- [ ] **Step 1: Relax the assertion and handle equal case**

In `libs/collabtext/src/crdt/Locator.cpp`, line 30, change the assertion:

```cpp
Locator Locator::between(const Locator &lo, const Locator &hi) {
    assert(lo <= hi);  // Was: assert(lo < hi)
```

The existing algorithm already handles the equal case correctly: when
`ld == hd` at every level, it pushes `ld` and continues. At the level
beyond both locators' lengths, lo extends with DMIN (0) and hi with DMAX,
creating room for biased_mid. No other code changes needed.

- [ ] **Step 2: Add test for equal arguments**

In `libs/collabtext/tests/tst_locator.cpp`, add:

```cpp
void TestLocator::between_equal_arguments()
{
    // Same locator: result must be strictly between lo and hi
    // (between descends to a deeper digit level)
    Locator a({500});
    Locator mid = Locator::between(a, a);
    // mid should be (500, X) where X is biased_mid(DMIN, DMAX)
    // Since lo extends with DMIN and hi extends with DMAX at level 1:
    QVERIFY(a < mid || mid < a);  // Must not equal a
    // Actually: between(a, a) produces (500, biased_mid(0, DMAX))
    // which is (500, some_positive_value). This is > a = (500)
    // because a's implicit second digit is 0 (DMIN) and mid's is positive.
    QVERIFY(a < mid);

    // Two-digit equal locator
    Locator b({100, 200});
    Locator mid2 = Locator::between(b, b);
    QVERIFY(b < mid2);
}
```

- [ ] **Step 3: Build and test**

```bash
cmake --build build-dev --target tst_locator -j$(nproc)
./build-dev/libs/collabtext/tst_locator -v2
```

- [ ] **Step 4: Commit**

```bash
git add libs/collabtext/src/crdt/Locator.cpp libs/collabtext/tests/tst_locator.cpp
git commit -m "fix: Locator::between handles equal arguments (lo == hi)

Relaxes assertion from lo < hi to lo <= hi. When equal, the algorithm
descends to the next digit level where lo extends with DMIN and hi
with DMAX, creating room. Matches Zed's Locator::between behavior."
```

---

### Task 2: Rewrite apply_local_edit — Zed-style cursor walk

This is the main task. The cursor walk's insert phase is rewritten to
use Zed's locator reassignment pattern. The deferred relocation mechanism,
post-processing sort/normalize/rebuild, and ordering check are removed.

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.cpp:515-941`

- [ ] **Step 1: Rewrite the insert phase (lines 728-824)**

Replace the entire `// ---- Insert phase ----` block with Zed-style
locator allocation. The key changes:

1. Remove `needs_relocation`, `deferred_relocs`, and `DeferredReloc`
2. Use `Locator::between(new_tree.last().locator, next_fragment.locator)` 
   for new fragments — this is what we already do for the non-relocation case
3. For the same-locator case (where `imm_loc <= lo`): instead of relocating
   the next fragment, use `Locator::between(lo, Locator::max())` or find
   a real upper bound. The new fragment gets a locator between lo and hi.
   The next fragment keeps its original locator. Since the new fragment's
   locator is computed via `Locator::between(lo, next.locator)` and
   `between(L, L)` now works (descends to next level), this always produces
   a valid locator > lo and positioned before the suffix.

Replace lines 543-553 (DeferredReloc struct and deferred_relocs vector)
and lines 728-824 (entire insert phase) with:

```cpp
        // ---- Insert phase ----
        if (!replacement.empty()) {
            Locator lo = new_tree.empty() ? Locator::min() : new_tree.last().locator;

            // The next fragment's locator (pending or cursor).
            Locator next_loc = Locator::max();
            if (pending) {
                next_loc = pending->locator;
            } else if (cursor.item()) {
                next_loc = cursor.item()->locator;
            }

            // Zed-style: new fragment gets locator between the max of the
            // already-built tree and the next fragment's locator. When
            // lo == next_loc (same-locator group), between(lo, next_loc)
            // descends to a deeper digit level, creating unique space.
            // No SplitRelocation needed.
            Locator new_loc = Locator::between(lo, next_loc);

            uint32_t char_count = count_utf8_chars(
                replacement, static_cast<uint32_t>(replacement.size()));

            Lamport frag_origin = m_clock.tick();
            for (uint32_t c = 1; c < char_count; ++c) m_clock.tick();

            Fragment frag(frag_origin, new_loc,
                          static_cast<uint32_t>(replacement.size()), char_count,
                          replacement);
            frag.visible = true;
            new_tree.push_item(std::move(frag));

            EditOperation::InsertedFragment ins_rec;
            ins_rec.origin = frag_origin;
            ins_rec.locator = new_loc;
            ins_rec.content = replacement;
            ins_rec.length = char_count;
            op.inserted_fragments.push_back(std::move(ins_rec));

            for (uint32_t c = 0; c < char_count; ++c) {
                undo_entry.inserted_keys.push_back(
                    UndoMapKey(frag_origin.replica_id, frag_origin.value + c));
            }
        }
```

This is SIMPLER than the current code — the entire `needs_relocation`
branch (lines 744-797), the `DeferredReloc` struct (lines 547-551), and
`deferred_relocs` vector (line 552) are all deleted.

- [ ] **Step 2: Remove post-processing sort/normalize/rebuild (lines 851-920)**

Replace lines 851-920 (the entire "Apply deferred relocations, sort,
normalize, rebuild" section) with:

```cpp
    // ---- Commit the cursor-built tree directly ----
    // Zed-style locator assignment guarantees monotonic ordering:
    // - Prefix: sliced from old tree (ordered)
    // - Middle: fragments pushed with locators via between(max_id, original)
    // - Suffix: appended from old tree (ordered, all locators > max_id)
    new_tree.for_each_mut([this](Fragment& f) {
        f.visible = f.compute_visible(m_undo_map);
    });
    m_fragment_tree = std::move(new_tree);
    rebuild_origin_index();
```

This removes:
- The `used_fast_path` flag
- The O(n) ordering check
- The O(n) extract-to-vector
- The O(n) deferred relocation application
- The O(n log n) sort
- The O(n) normalize_fragments call
- The O(n) set_fragments rebuild

What remains: O(n) visibility pass + O(n) origin index rebuild. These are
lightweight tree walks with tiny per-item cost.

- [ ] **Step 3: Build and run fast tests**

```bash
cmake --build build-dev -j$(nproc)
ctest --test-dir build-dev --output-on-failure -E "tst_realistic|tst_benchmark"
```

- [ ] **Step 4: Run fuzz stability (the critical test)**

```bash
for i in $(seq 1 10); do
    ./build-dev/libs/collabtext/tst_fuzz 2>&1 | grep "Totals:"
done
```

All 10 runs must pass 16/16. The fuzz test exercises random editing with
convergence checks and INV-4 ordering invariants. If the Zed-style cursor
walk produces correctly ordered trees, there will be zero INV-4 violations.

**If fuzz tests fail:** The most likely cause is the `consume_unchanged`
and `consume_deleted` helpers pushing fragments with original locators that
violate monotonicity relative to the new tree's max_id. The fix: those
helpers must also reassign locators for fragments they push. See Step 5.

- [ ] **Step 5: If needed — reassign locators in consume helpers**

If Step 4 reveals ordering violations, the consume_unchanged and
consume_deleted helpers need to reassign locators for fragments they push
to new_tree, matching Zed's pattern:

In `consume_unchanged` and `consume_deleted`, before each
`new_tree.push_item(std::move(frag))`, add:

```cpp
    // Reassign locator to maintain monotonic ordering (Zed-style)
    Locator max_loc = new_tree.empty() ? Locator::min() : new_tree.last().locator;
    if (!(frag.locator > max_loc) &&
        !(frag.locator == max_loc && frag.origin > /* prev origin */)) {
        frag.locator = Locator::between(max_loc, frag.locator);
    }
```

Actually, a simpler approach: ALWAYS reassign. Every fragment pushed to
new_tree gets `locator = Locator::between(max_loc, original_loc)` when
`max_loc >= original_loc`. When `max_loc < original_loc`, the original
locator is fine (it's already monotonically increasing).

```cpp
    // Before pushing any fragment to new_tree:
    if (!new_tree.empty()) {
        const auto& last = new_tree.last();
        if (frag.locator < last.locator ||
            (frag.locator == last.locator && frag.origin <= last.origin)) {
            frag.locator = Locator::between(last.locator, frag.locator);
            // Note: between(L, L') where L >= L' — handled by relaxed between()
        }
    }
```

Wait — `between(last.locator, frag.locator)` requires last.locator <=
frag.locator. If last.locator > frag.locator, that violates between's
precondition. Instead:

```cpp
    if (!new_tree.empty()) {
        const auto& last = new_tree.last();
        if (!(frag.locator > last.locator ||
              (frag.locator == last.locator && frag.origin > last.origin))) {
            // Fragment would violate ordering. Assign a new locator
            // that's > the current max.
            frag.locator = Locator::between(last.locator, Locator::max());
        }
    }
```

This ensures every pushed fragment has a locator > the last fragment in
new_tree (or, for same-locator, a strictly greater origin — which is
guaranteed for same-replica fragments since origins are monotonic).

Apply this guard to all push_item calls in:
- `consume_unchanged` (lines 604, 609, 615, 623, 630, 635)
- `consume_deleted` (lines 651, 657, 662, 670, 678, 684)
- The prefix copy straddling fragment (line 702)
- The suffix pending push (line 831)

To avoid code duplication, extract a helper:

```cpp
    // Helper: push fragment to new_tree, reassigning locator if needed
    // to maintain monotonic ordering.
    auto push_to_new_tree = [&](Fragment frag) {
        if (!new_tree.empty()) {
            const auto& last = new_tree.last();
            if (frag.locator < last.locator ||
                (frag.locator == last.locator && frag.origin <= last.origin)) {
                frag.locator = Locator::between(last.locator, Locator::max());
            }
        }
        new_tree.push_item(std::move(frag));
    };
```

Then replace all `new_tree.push_item(std::move(frag))` calls in
consume_unchanged, consume_deleted, and the prefix split with
`push_to_new_tree(std::move(frag))`.

**Do NOT apply this to cursor.slice() or cursor.suffix()** — those use
push_tree which appends entire subtrees. The sliced prefix is guaranteed
ordered (it's from the old tree), and the suffix is guaranteed > everything
in the new tree (suffix fragments have locators >= the original tree's
locators at positions past the edit, which are >= any reassigned locators).

After this change, rebuild and rerun fuzz 10 times.

- [ ] **Step 6: Commit**

```bash
git add libs/collabtext/src/crdt/Buffer.cpp
git commit -m "perf: Zed-style cursor walk in apply_local_edit

Reassign locators during cursor walk to guarantee monotonic ordering.
Eliminates deferred relocations, post-processing sort, normalization,
and the ordering-check fallback.

SplitRelocations no longer generated for local edits. Wire format
field kept for backward compatibility with old operations."
```

---

### Task 3: Full regression and benchmark validation

**Files:** None (read-only)

- [ ] **Step 1: Full fast test suite**

```bash
ctest --test-dir build-dev --output-on-failure -E "tst_realistic|tst_benchmark"
```

13/13 must pass.

- [ ] **Step 2: Realistic tests**

```bash
./build-dev/libs/collabtext/tst_realistic -v2
```

11/11 must pass.

- [ ] **Step 3: Fuzz stability (10 runs)**

```bash
for i in $(seq 1 10); do
    ./build-dev/libs/collabtext/tst_fuzz 2>&1 | grep "Totals:"
done
```

All 10 runs: 16/16.

- [ ] **Step 4: Benchmarks**

```bash
./build-dev/libs/collabtext/tst_benchmark single_replica_throughput -v2
./build-dev/libs/collabtext/tst_benchmark single_replica_large_doc -v2
./build-dev/libs/collabtext/tst_benchmark realistic_3_client_throughput -v2
```

Pre-refactor baselines:
- 1K: 361 ops/sec
- 10K: 226 ops/sec
- 100K: 61 ops/sec
- 1M: 59 ops/sec
- 3-client: 53 ops/sec

Expected improvements:
- Single-replica: significant (local edits skip sort/rebuild)
- 3-client: modest (remote edits already fast-pathed)
