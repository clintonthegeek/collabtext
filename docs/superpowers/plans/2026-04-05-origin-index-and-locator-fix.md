# Origin Index + Locator::between Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enable O(log n) for all common remote edits (including deletions) and all local edits without relocations, by adding an origin-based lookup index and fixing Locator::between to always return strictly-between values.

**Architecture:** Two independent features. (A) A per-replica sorted map (`unordered_map<uint16_t, map<uint32_t, Locator>>`) that maps origin timestamps to fragment locators, enabling O(log n) deletion run lookups. (B) Fix `biased_mid()` to never return `hi` and handle the gap==1 case in `between()` by descending to the next digit level. Together these expand the fast paths in `apply_remote_edit` (now covers deletions + insertions) and `apply_local_edit` (now covers insertions too).

**Tech Stack:** C++20, Qt6 Test, CMake

**Spec:** `docs/superpowers/specs/2026-04-05-origin-index-and-locator-fix-design.md`

**Build/test commands:**
```bash
cmake --build build-dev -j$(nproc)
ctest --test-dir build-dev --output-on-failure -E "tst_realistic|tst_benchmark"
```

---

## File Map

| File | Action | Responsibility |
|------|--------|---------------|
| `libs/collabtext/src/crdt/Locator.cpp` | Modify | Fix biased_mid + between caller |
| `libs/collabtext/src/crdt/Buffer.h` | Modify | Add m_origin_index member, rebuild_origin_index decl |
| `libs/collabtext/src/crdt/Buffer.cpp` | Modify | Origin index maintenance + expanded fast paths |
| `libs/collabtext/tests/tst_locator.cpp` | Modify | Add between() edge case tests |

---

### Task 1: Fix Locator::between

Fix `biased_mid` and the gap==1 caller in `between()`.

**Files:**
- Modify: `libs/collabtext/src/crdt/Locator.cpp:14-19` (biased_mid)
- Modify: `libs/collabtext/src/crdt/Locator.cpp:106-114` (between no-more-digits case)
- Modify: `libs/collabtext/tests/tst_locator.cpp`

- [ ] **Step 1: Fix biased_mid**

In `libs/collabtext/src/crdt/Locator.cpp`, replace lines 14-19:

```cpp
static uint64_t biased_mid(uint64_t lo, uint64_t hi) {
    assert(lo < hi);
    uint64_t gap = hi - lo;
    if (gap == 1) return lo;  // No integer strictly between; caller must descend
    uint64_t step = gap >> 48;
    if (step == 0) step = 1;
    return lo + step;
    // Proof of correctness when gap >= 2:
    // If gap < 2^48: step = 1, lo + 1 < lo + 2 <= hi. Result < hi. ✓
    // If gap >= 2^48: step = gap >> 48 < gap, lo + step < hi. ✓
}
```

- [ ] **Step 2: Fix the no-more-digits case in between()**

Replace lines 106-114 (the block starting with `// lo has no more digits`):

```cpp
        // lo has no more digits. (result..., ld) == lo at this depth.
        // Need a value strictly > ld and strictly < hd.
        if (ld + 1 < hd) {
            // Room between ld and hd
            uint64_t mid = biased_mid(ld, hd);
            result.push_back(mid);
            return Locator(result);
        }
        // ld + 1 == hd: no integer strictly between them at this level.
        // Descend below ld. Since lo = (..., ld) with no more digits,
        // anything (..., ld, X) where X > DMIN is strictly > lo.
        // And (..., ld, ...) < (..., hd, ...) = hi since ld < hd.
        result.push_back(ld);
        result.push_back(biased_mid(DMIN, DMAX));
        return Locator(result);
```

- [ ] **Step 3: Add edge case tests**

In `libs/collabtext/tests/tst_locator.cpp`, add test slots:

```cpp
void TestLocator::between_adjacent_digits()
{
    // Create two locators with adjacent first digits
    // Force gap == 1 at the first digit level
    Locator lo(std::vector<uint64_t>{100});
    Locator hi(std::vector<uint64_t>{101});
    Locator mid = Locator::between(lo, hi);

    QVERIFY(lo < mid);
    QVERIFY(mid < hi);
    // mid should have descended: (100, X) where X > 0
    QVERIFY(mid.depth() == 2);
}

void TestLocator::between_adjacent_deep()
{
    // Adjacent at a deeper level
    Locator lo(std::vector<uint64_t>{50, 200});
    Locator hi(std::vector<uint64_t>{50, 201});
    Locator mid = Locator::between(lo, hi);

    QVERIFY(lo < mid);
    QVERIFY(mid < hi);
}

void TestLocator::between_never_equals_bounds()
{
    // Stress test: 500 sequential between() calls, verify ordering
    Locator lo = Locator::min();
    Locator hi = Locator::max();

    std::vector<Locator> chain;
    chain.push_back(lo);

    for (int i = 0; i < 500; ++i) {
        Locator mid = Locator::between(chain.back(), hi);
        QVERIFY(chain.back() < mid);
        QVERIFY(mid < hi);
        chain.push_back(mid);
    }

    // Also test with shrinking ranges
    lo = Locator::min();
    hi = Locator::max();
    for (int i = 0; i < 200; ++i) {
        Locator mid = Locator::between(lo, hi);
        QVERIFY(lo < mid);
        QVERIFY(mid < hi);
        // Alternate: sometimes narrow from below, sometimes from above
        if (i % 2 == 0) lo = mid;
        else hi = mid;
    }
}
```

The `depth()` method may need to be added if it doesn't exist — check the
Locator.h header. If not, check digit count via the comparison behavior
(a 2-digit locator (100, X) is > (100) since trailing digits default to
DMIN=0 and X > 0).

- [ ] **Step 4: Build and test**

```bash
cmake --build build-dev --target tst_locator -j$(nproc)
./build-dev/libs/collabtext/tst_locator -v2
```

Also run the full fast suite to verify no regressions:

```bash
ctest --test-dir build-dev --output-on-failure -E "tst_realistic|tst_benchmark"
```

- [ ] **Step 5: Commit**

```bash
git add libs/collabtext/src/crdt/Locator.cpp libs/collabtext/tests/tst_locator.cpp
git commit -m "fix: Locator::between always returns strictly between lo and hi

biased_mid returns lo (not hi) when gap==1. between() descends
to the next digit level when adjacent digits have no room."
```

---

### Task 2: Add origin index to Buffer

Add the `m_origin_index` data structure and maintain it alongside the
fragment tree.

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.h`
- Modify: `libs/collabtext/src/crdt/Buffer.cpp`

- [ ] **Step 1: Add origin index member and rebuild method to Buffer.h**

In `libs/collabtext/src/crdt/Buffer.h`, add to the private section (near
the other member variables):

```cpp
    /// Per-replica sorted map: origin_value → locator.
    /// Enables O(log m) lookup of fragments by origin timestamp.
    std::unordered_map<uint16_t, std::map<uint32_t, Locator>> m_origin_index;

    /// Rebuild the origin index from the current fragment tree.
    void rebuild_origin_index();
```

- [ ] **Step 2: Implement rebuild_origin_index and wire into set_fragments**

In `libs/collabtext/src/crdt/Buffer.cpp`, add the implementation:

```cpp
void Buffer::rebuild_origin_index() {
    m_origin_index.clear();
    m_fragment_tree.for_each([this](const Fragment& f) {
        m_origin_index[f.origin.replica_id][f.origin.value] = f.locator;
    });
}
```

Call it from `set_fragments`, after building the tree (around the end of
the method):

```cpp
void Buffer::set_fragments(std::vector<Fragment>&& frags) {
    for (auto& f : frags) {
        f.visible = f.compute_visible(m_undo_map);
        assert(f.byte_length == f.text.size());
    }
    rebuild_insertion_index(frags);
    FragmentTree tree;
    for (auto& f : frags) tree.push_item(std::move(f));
    m_fragment_tree = std::move(tree);
    rebuild_origin_index();  // NEW
}
```

Also add it to the `apply_local_edit` fast path (after `m_fragment_tree =
std::move(new_tree)`) and the `apply_remote_edit` full path exit. Any path
that replaces `m_fragment_tree` must rebuild the origin index.

- [ ] **Step 3: Maintain origin index in apply_remote_edit_fast**

In the existing `apply_remote_edit_fast`, after each `insert_item` call,
update the index:

```cpp
    for (auto &ins : op.inserted_fragments) {
        Fragment frag(ins.origin, ins.locator,
                      static_cast<uint32_t>(ins.content.size()), ins.length,
                      ins.content);
        frag.visible = true;
        FragmentOrderDim target{ins.locator, ins.origin};
        m_fragment_tree.insert_item<FragmentOrderDim>(target, std::move(frag));
        // Update origin index
        m_origin_index[ins.origin.replica_id][ins.origin.value] = ins.locator;
    }
```

- [ ] **Step 4: Build and test**

```bash
cmake --build build-dev -j$(nproc)
ctest --test-dir build-dev --output-on-failure -E "tst_realistic|tst_benchmark"
```

- [ ] **Step 5: Commit**

```bash
git add libs/collabtext/src/crdt/Buffer.h libs/collabtext/src/crdt/Buffer.cpp
git commit -m "feat: add origin index for O(log n) fragment lookup by timestamp

Per-replica sorted map: origin_value → locator. Rebuilt on
set_fragments, maintained incrementally on fast path insertions."
```

---

### Task 3: Expand apply_remote_edit fast path (deletions)

Use the origin index to handle deletion runs in the fast path.

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.cpp`

- [ ] **Step 1: Add origin index lookup helper**

Add a private helper to Buffer.cpp:

```cpp
/// Look up the locator for a fragment covering (replica_id, origin_value).
/// Returns the locator, or std::nullopt if not found.
std::optional<Locator> Buffer::origin_index_lookup(
    uint16_t replica_id, uint32_t origin_value) const
{
    auto rep_it = m_origin_index.find(replica_id);
    if (rep_it == m_origin_index.end()) return std::nullopt;
    auto& rep_map = rep_it->second;
    if (rep_map.empty()) return std::nullopt;

    // Find the entry with largest origin_value <= target
    auto it = rep_map.upper_bound(origin_value);
    if (it == rep_map.begin()) return std::nullopt;
    --it;
    return it->second;
}
```

Add declaration to Buffer.h private section:

```cpp
    std::optional<Locator> origin_index_lookup(
        uint16_t replica_id, uint32_t origin_value) const;
```

- [ ] **Step 2: Expand apply_remote_edit_fast to handle deletions**

Rewrite `apply_remote_edit_fast` to handle deletion runs using the origin
index + edit_item, plus single-char insertions:

```cpp
bool Buffer::apply_remote_edit_fast(const EditOperation &op) {
    // Fast path conditions:
    // - No split relocations
    // - All insertions are single-character
    if (!op.split_relocations.empty()) return false;
    for (auto &ins : op.inserted_fragments) {
        if (ins.length != 1) return false;
    }

    // Apply deletion runs via origin index + edit_item
    for (auto& run : op.deletion_runs) {
        uint32_t remaining = run.count;
        uint32_t next_val = run.start_value;

        while (remaining > 0) {
            auto loc_opt = origin_index_lookup(run.replica_id, next_val);
            if (!loc_opt) break;

            FragmentOrderDim target{*loc_opt, Lamport(run.replica_id, next_val)};

            // Find and mark the fragment as deleted
            bool found = m_fragment_tree.edit_item<FragmentOrderDim>(
                target,
                [&](Fragment& f) {
                    // Verify this fragment actually covers next_val
                    if (f.origin.replica_id != run.replica_id) return;
                    if (next_val < f.origin.value ||
                        next_val >= f.origin.value + f.length) return;

                    uint32_t char_off = next_val - f.origin.value;
                    uint32_t avail = f.length - char_off;
                    uint32_t to_del = std::min(remaining, avail);

                    if (char_off == 0 && to_del == f.length) {
                        // Delete entire fragment
                        f.deletions.push_back(op.deletion_id);
                    } else {
                        // Need to split — fall back to full path
                        // Signal by setting remaining to a sentinel
                        // Actually: we can't split inside edit_item.
                        // Fall back to full path for partial deletions.
                        remaining = UINT32_MAX; // sentinel
                        return;
                    }

                    next_val += to_del;
                    remaining -= to_del;
                });

            if (remaining == UINT32_MAX) return false; // Partial deletion, fall back
            if (!found) break;
        }
    }

    // Apply insertions via insert_item
    for (auto &ins : op.inserted_fragments) {
        Fragment frag(ins.origin, ins.locator,
                      static_cast<uint32_t>(ins.content.size()), ins.length,
                      ins.content);
        frag.visible = true;
        FragmentOrderDim target{ins.locator, ins.origin};
        m_fragment_tree.insert_item<FragmentOrderDim>(target, std::move(frag));
        m_origin_index[ins.origin.replica_id][ins.origin.value] = ins.locator;
    }

    // Update clock and version
    m_clock.observe(op.timestamp);
    m_version.observe(op.timestamp);
    m_clock.observe(op.deletion_id);
    m_version.observe(op.deletion_id);
    for (auto &ins : op.inserted_fragments) {
        Lamport last_ts(ins.origin.replica_id, ins.origin.value + ins.length - 1);
        m_clock.observe(last_ts);
        m_version.observe(last_ts);
    }
    m_version.join(op.version);
    return true;
}
```

**Key design decisions:**
- The fast path handles whole-fragment deletions only (char_off == 0 &&
  to_del == f.length). Partial deletions (which require splitting) fall
  back to the full path. This covers the common case (single-character
  fragments from normalized text are always whole-fragment).
- Multi-character insertions still fall back (normalization risk).

- [ ] **Step 3: Build and test**

```bash
cmake --build build-dev -j$(nproc)
ctest --test-dir build-dev --output-on-failure -E "tst_realistic|tst_benchmark"
```

- [ ] **Step 4: Commit**

```bash
git add libs/collabtext/src/crdt/Buffer.h libs/collabtext/src/crdt/Buffer.cpp
git commit -m "perf: expand remote edit fast path to handle deletions

Uses origin index for O(log n) fragment lookup. Handles whole-
fragment deletions + single-char insertions. Falls back for
partial deletions and split relocations."
```

---

### Task 4: Expand apply_local_edit fast path (insertions)

With the Locator::between fix, the local edit fast path can now cover
insertions (not just pure deletions).

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.cpp`

- [ ] **Step 1: Widen the apply_local_edit fast path condition**

In `apply_local_edit`, the fast path currently checks:
```cpp
if (deferred_relocs.empty() && op.inserted_fragments.empty()) {
```

Change to just check for relocations:
```cpp
if (deferred_relocs.empty()) {
```

The Locator::between fix guarantees new locators are strictly between
neighbors, so the cursor-built tree maintains correct ordering even with
insertions.

Also update the origin index in both fast and full paths:

```cpp
    if (deferred_relocs.empty()) {
        new_tree.for_each_mut([this](Fragment& f) {
            f.visible = f.compute_visible(m_undo_map);
        });
        m_fragment_tree = std::move(new_tree);
        rebuild_origin_index();
    } else {
        // ... existing full path ...
    }
```

- [ ] **Step 2: Build and test**

```bash
cmake --build build-dev -j$(nproc)
ctest --test-dir build-dev --output-on-failure -E "tst_realistic|tst_benchmark"
```

Pay special attention to `tst_fuzz` which exercises random editing with
convergence checks — this validates that the Locator fix and wider fast
path don't break correctness.

- [ ] **Step 3: Commit**

```bash
git add libs/collabtext/src/crdt/Buffer.cpp
git commit -m "perf: expand local edit fast path to cover insertions

Locator::between fix guarantees new locators maintain ordering.
apply_local_edit skips extract-sort-rebuild for all non-relocation
edits, not just pure deletions."
```

---

### Task 5: Full regression and benchmark validation

**Files:** None (read-only)

- [ ] **Step 1: Full fast test suite**

```bash
ctest --test-dir build-dev --output-on-failure -E "tst_realistic|tst_benchmark"
```

- [ ] **Step 2: Realistic tests**

```bash
./build-dev/libs/collabtext/tst_realistic -v2
```

- [ ] **Step 3: Fuzz stability (5 runs)**

```bash
for i in $(seq 1 5); do
    ./build-dev/libs/collabtext/tst_fuzz -v2 2>&1 | grep "Totals:"
done
```

- [ ] **Step 4: Benchmarks**

```bash
./build-dev/libs/collabtext/tst_benchmark single_replica_throughput -v2
./build-dev/libs/collabtext/tst_benchmark single_replica_large_doc -v2
./build-dev/libs/collabtext/tst_benchmark realistic_3_client_throughput -v2
./build-dev/libs/collabtext/tst_benchmark gc_under_sustained_editing -v2
./build-dev/libs/collabtext/tst_benchmark tombstone_degradation -v2
```

Pre-refactor baselines (Phase 2):
- 1K: 357 ops/sec
- 100K: 60 ops/sec
- 1M: 66 ops/sec
- 3-client: 23 ops/sec
- GC sustained: 9 ops/sec
- Tombstone 50%: 18 ops/sec

Target improvements:
- 3-client: >50 ops/sec (from 23)
- Single replica should improve from wider local edit fast path
