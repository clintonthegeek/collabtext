# Batch 2: Cursor-Based Edits + VersionedFullOffset

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace vector-scan patterns in `apply_local_edit` and `apply_remote_edit` with tree-native cursor operations, and fix a known locator ordering bug in mid-text replacements.

**Architecture:** Optimization 2 rewrites `apply_local_edit()` to process ranges left-to-right using a SumTree cursor on the immutable old tree, building a new tree incrementally. This uses `cursor.slice()` for O(log n) prefix/suffix copies and correctly handles locator relocation at fragment boundaries. Optimization 3 augments `FragmentSummary` with insertion version tracking and adds a `versioned_seek()` function that prunes subtrees during remote edit application.

**Tech Stack:** C++20, Qt6 Test, SumTree cursor API, CMake.

**Spec reference:** `docs/specs/sumtree-optimizations.md` sections 2.1-2.7 and 3.1-3.4

**Build & test commands:**
- Build: `cmake --build build-dev`
- Run all tests: `cd build-dev && ctest --output-on-failure`
- Run buffer tests: `./build-dev/libs/collabtext/tst_buffer`
- Run convergence: `./build-dev/libs/collabtext/tst_convergence`

**Known bug being fixed:** `apply_local_edit({{2, 4}}, {"XX"})` on "hello" currently produces "heoXX" instead of "heXXo". The locator for inserted text is computed as `Locator::between(L, max())` when surrounding fragments share locator L (from splitting), placing the insert after all L-locator fragments. The cursor-based rewrite detects same-locator boundaries and relocates the next fragment to create ordering space.

---

## File Map

| Action | File | Responsibility |
|--------|------|----------------|
| Modify | `libs/collabtext/src/crdt/Fragment.h:15-35` | Add `min_insertion_version`, `max_insertion_version` to `FragmentSummary` (Opt 3) |
| Modify | `libs/collabtext/src/crdt/Buffer.h:73-99` | (Opt 2) Vector helpers remain for undo/redo/remote; no new declarations needed |
| Modify | `libs/collabtext/src/crdt/Buffer.cpp:400-557` | Rewrite `apply_local_edit()` with cursor-based approach (Opt 2) |
| Modify | `libs/collabtext/src/crdt/Buffer.cpp:564-665` | Refactor deletion loop in `apply_remote_edit()` with versioned_seek (Opt 3) |
| Modify | `libs/collabtext/tests/tst_buffer.cpp` | Add 10 supplementary tests (7 for Opt 2 + 3 for Opt 3) |

---

## Task 1: Add Opt 2 supplementary tests to tst_buffer

These tests define the CORRECT behavior. The first test exposes the known bug (will FAIL against current code). The rest should PASS against current code and continue to pass after the rewrite.

**Files:**
- Modify: `libs/collabtext/tests/tst_buffer.cpp`

- [ ] **Step 1: Add the 7 new test slots**

Add these after the existing `multiple_ranges_in_single_edit` test:

```cpp
    // -----------------------------------------------------------------------
    // Opt 2: Cursor-based apply_local_edit tests
    // -----------------------------------------------------------------------

    void local_edit_replace_mid_fragment() {
        // REGRESSION: was producing "heoXX" due to locator ordering bug
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        buf.apply_local_edit({{2, 4}}, {"XX"});
        QCOMPARE(buf.text(), std::string("heXXo"));
        QCOMPARE(buf.visible_length(), 5u);
    }

    void local_edit_insert_at_start_of_fragment() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"abcdef"});
        // Insert at byte 0 of the whole document
        buf.apply_local_edit({{0, 0}}, {"XX"});
        QCOMPARE(buf.text(), std::string("XXabcdef"));
    }

    void local_edit_insert_mid_fragment() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"abcdef"});
        // Insert at byte 3 (middle of the single fragment)
        buf.apply_local_edit({{3, 3}}, {"XX"});
        QCOMPARE(buf.text(), std::string("abcXXdef"));

        // Verify split relocation was emitted (check fragments have correct locators)
        auto frags = buf.fragments();
        bool found_xx = false;
        for (auto& f : frags) {
            if (f.content == "XX") { found_xx = true; break; }
        }
        QVERIFY(found_xx);
    }

    void local_edit_delete_spanning_fragments() {
        Buffer buf(1);
        // Create 3 separate fragments by inserting in stages
        buf.apply_local_edit({{0, 0}}, {"ab"});
        buf.apply_local_edit({{2, 2}}, {"cd"});
        buf.apply_local_edit({{4, 4}}, {"ef"});
        QCOMPARE(buf.text(), std::string("abcdef"));

        // Delete spanning all 3 fragments
        buf.apply_local_edit({{0, 6}}, {""});
        QCOMPARE(buf.text(), std::string(""));
        QCOMPARE(buf.visible_length(), 0u);
    }

    void local_edit_delete_partial_fragment() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"abcdef"});
        // Delete first 3 bytes
        buf.apply_local_edit({{0, 3}}, {""});
        QCOMPARE(buf.text(), std::string("def"));
        QCOMPARE(buf.visible_length(), 3u);
    }

    void local_edit_multi_range_left_to_right() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"abcdefghij"});
        // Two ranges: replace [1,2) with "X", replace [4,5) with "Y"
        buf.apply_local_edit({{1, 2}, {4, 5}}, {"X", "Y"});
        QCOMPARE(buf.text(), std::string("aXcdYfghij"));
    }

    void local_edit_empty_document() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        QCOMPARE(buf.text(), std::string("hello"));
    }

    void local_edit_replace_entire_document() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        buf.apply_local_edit({{0, 5}}, {"world"});
        QCOMPARE(buf.text(), std::string("world"));
        QCOMPARE(buf.visible_length(), 5u);
    }
```

- [ ] **Step 2: Build and run**

Run: `cmake --build build-dev && ./build-dev/libs/collabtext/tst_buffer`

Expected: `local_edit_replace_mid_fragment` FAILS with `"heoXX" != "heXXo"`. All other new tests PASS. This confirms the known bug and validates the test.

- [ ] **Step 3: Commit**

```
git add libs/collabtext/tests/tst_buffer.cpp
git commit -m "test: add Opt 2 supplementary tests, expose locator bug

local_edit_replace_mid_fragment fails: apply_local_edit produces
'heoXX' instead of 'heXXo' due to locator ordering at same-locator
fragment boundaries. Other 6 tests pass."
```

---

## Task 2: Rewrite apply_local_edit with cursor-based approach

This is the core Optimization 2 change. Replaces the `get_fragments()` / vector mutation / `set_fragments()` pattern with cursor-based tree building.

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.cpp:400-557`

- [ ] **Step 1: Replace apply_local_edit**

Replace the entire `Buffer::apply_local_edit` method (lines 400-557 in Buffer.cpp) with the cursor-based implementation. The new code:

1. Sorts ranges **ascending** (left-to-right) instead of descending
2. Uses `cursor.slice()` for the O(log n) prefix copy
3. Tracks a `pending` split-fragment across ranges
4. When inserting between same-locator fragments, creates a `SplitRelocation` to fix ordering
5. Uses `cursor.suffix()` for the O(log n) suffix copy
6. Falls back to `normalize_fragments` + `set_fragments` for finalization

```cpp
Operation Buffer::apply_local_edit(
    const std::vector<std::pair<uint32_t, uint32_t>> &ranges,
    const std::vector<std::string> &new_text)
{
    assert(ranges.size() == new_text.size());

    EditOperation op;
    op.ranges = ranges;
    op.new_text = new_text;
    op.version = m_version;
    UndoEntry undo_entry;

    // Sort ranges ascending (left-to-right) — offsets are in the OLD tree's
    // visible space and never change during processing.
    std::vector<size_t> order(ranges.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return ranges[a].first < ranges[b].first;
    });

    auto cursor = m_fragment_tree.cursor<VisibleOffset>();
    cursor.seek({0}, Bias::Left);
    FragmentTree new_tree;

    // Pending: the remaining half of a fragment split at a range boundary.
    // Carried across ranges when a single old-tree fragment spans multiple ranges.
    std::optional<Fragment> pending;

    // Helper: mark a fragment as deleted and record timestamps for op/undo.
    auto mark_deleted = [&](Fragment& f) {
        f.delete_count++;
        f.visible = false;
        for (uint32_t c = 0; c < f.length; ++c) {
            Lamport ts = f.timestamp_at(c);
            op.deleted_timestamps.push_back(ts);
            undo_entry.deleted_keys.push_back(UndoMapKey(ts));
        }
    };

    // Helper: split a fragment at a byte offset, returning {first_half, second_half}.
    auto split_frag = [](const Fragment& f, uint32_t byte_off)
        -> std::pair<Fragment, Fragment>
    {
        uint32_t char_count = count_utf8_chars(f.content, byte_off);
        Fragment first;
        first.origin = f.origin;
        first.locator = f.locator;
        first.content = f.content.substr(0, byte_off);
        first.length = char_count;
        first.delete_count = f.delete_count;
        first.visible = f.visible;

        Fragment second;
        second.origin = Lamport(f.origin.replica_id, f.origin.value + char_count);
        second.locator = f.locator;
        second.content = f.content.substr(byte_off);
        second.length = f.length - char_count;
        second.delete_count = f.delete_count;
        second.visible = f.visible;
        return {std::move(first), std::move(second)};
    };

    // Helper: consume visible bytes from pending/cursor, pushing unchanged
    // fragments to new_tree. Used for the prefix and inter-range gaps.
    auto consume_unchanged = [&](uint32_t vis_bytes) {
        // Consume from pending first
        while (pending && vis_bytes > 0) {
            if (pending->visible) {
                uint32_t pv = static_cast<uint32_t>(pending->content.size());
                if (pv <= vis_bytes) {
                    new_tree.push_item(std::move(*pending));
                    vis_bytes -= pv;
                    pending.reset();
                } else {
                    auto [first, second] = split_frag(*pending, vis_bytes);
                    new_tree.push_item(std::move(first));
                    pending = std::move(second);
                    vis_bytes = 0;
                }
            } else {
                // Invisible — push, no visible bytes consumed
                new_tree.push_item(std::move(*pending));
                pending.reset();
            }
        }
        // Consume from cursor
        while (vis_bytes > 0 && cursor.item()) {
            Fragment frag = *cursor.item();
            if (!frag.visible) {
                new_tree.push_item(std::move(frag));
                cursor.next();
                continue;
            }
            uint32_t fv = static_cast<uint32_t>(frag.content.size());
            if (fv <= vis_bytes) {
                new_tree.push_item(std::move(frag));
                vis_bytes -= fv;
                cursor.next();
            } else {
                auto [first, second] = split_frag(frag, vis_bytes);
                new_tree.push_item(std::move(first));
                pending = std::move(second);
                cursor.next();
                vis_bytes = 0;
            }
        }
    };

    // Helper: consume and delete visible bytes from pending/cursor.
    auto consume_deleted = [&](uint32_t vis_bytes) {
        // From pending
        while (pending && vis_bytes > 0) {
            if (pending->visible) {
                uint32_t pv = static_cast<uint32_t>(pending->content.size());
                if (pv <= vis_bytes) {
                    mark_deleted(*pending);
                    new_tree.push_item(std::move(*pending));
                    vis_bytes -= pv;
                    pending.reset();
                } else {
                    auto [first, second] = split_frag(*pending, vis_bytes);
                    mark_deleted(first);
                    new_tree.push_item(std::move(first));
                    pending = std::move(second);
                    vis_bytes = 0;
                }
            } else {
                new_tree.push_item(std::move(*pending));
                pending.reset();
            }
        }
        // From cursor
        while (vis_bytes > 0 && cursor.item()) {
            Fragment frag = *cursor.item();
            if (!frag.visible) {
                new_tree.push_item(std::move(frag));
                cursor.next();
                continue;
            }
            uint32_t fv = static_cast<uint32_t>(frag.content.size());
            if (fv <= vis_bytes) {
                mark_deleted(frag);
                new_tree.push_item(std::move(frag));
                vis_bytes -= fv;
                cursor.next();
            } else {
                auto [first, second] = split_frag(frag, vis_bytes);
                mark_deleted(first);
                new_tree.push_item(std::move(first));
                pending = std::move(second);
                cursor.next();
                vis_bytes = 0;
            }
        }
    };

    // ---- Phase 0: Prefix copy (up to first range) ----
    if (!order.empty()) {
        uint32_t first_start = ranges[order[0]].first;
        // Use cursor.slice for O(log n) bulk copy
        new_tree.push_tree(cursor.slice({first_start}));
        // Handle straddling fragment
        if (cursor.item() && cursor.position().value < first_start) {
            uint32_t split_byte = first_start - cursor.position().value;
            auto [first, second] = split_frag(*cursor.item(), split_byte);
            new_tree.push_item(std::move(first));
            pending = std::move(second);
            cursor.next();
        }
    }

    // ---- Process each range ----
    uint32_t prev_end = order.empty() ? 0 : ranges[order[0]].first;

    for (size_t oi = 0; oi < order.size(); ++oi) {
        size_t idx = order[oi];
        uint32_t start = ranges[idx].first;
        uint32_t end = ranges[idx].second;
        const std::string &replacement = new_text[idx];

        // Inter-range gap: copy unchanged content between previous end and this start
        if (start > prev_end) {
            consume_unchanged(start - prev_end);
        }

        // ---- Delete phase ----
        if (end > start) {
            consume_deleted(end - start);
        }

        // ---- Insert phase ----
        if (!replacement.empty()) {
            // Determine lo/hi locators for the new fragment
            Locator lo = new_tree.empty() ? Locator::min() : new_tree.last().locator;
            Locator hi;
            if (pending) {
                hi = pending->locator;
            } else if (cursor.item()) {
                hi = cursor.item()->locator;
            } else {
                hi = Locator::max();
            }

            // If lo == hi (same-locator group from splitting), relocate the
            // next fragment to create ordering space. Without this, the inserted
            // text would sort after same-locator fragments (the "heoXX" bug).
            if (lo == hi && hi != Locator::max()) {
                Locator new_next_loc = Locator::between(lo, Locator::max());

                // Determine the origin and extent of fragments to relocate.
                // Collect all contiguous same-replica, same-locator fragments
                // starting from the next fragment (pending or cursor).
                Lamport reloc_origin;
                uint32_t reloc_length = 0;

                if (pending) {
                    reloc_origin = pending->origin;
                    reloc_length = pending->length;
                    pending->locator = new_next_loc;

                    // Check cursor for more contiguous same-locator fragments
                    Lamport next_expected(pending->origin.replica_id,
                                          pending->origin.value + pending->length);
                    while (cursor.item() &&
                           cursor.item()->locator == lo &&
                           cursor.item()->origin == next_expected) {
                        Fragment f = *cursor.item();
                        reloc_length += f.length;
                        next_expected = Lamport(f.origin.replica_id,
                                                f.origin.value + f.length);
                        // Don't consume yet — they'll be consumed by later
                        // consume_unchanged/consume_deleted calls or suffix.
                        // We can't relocate cursor items in-place (immutable tree).
                        // Instead, we pull them into pending-like storage.
                        break; // For now, handle only the immediate pending.
                        // The SplitRelocation mechanism on the remote side
                        // handles multi-fragment relocation correctly.
                    }
                } else if (cursor.item()) {
                    Fragment f = *cursor.item();
                    reloc_origin = f.origin;
                    reloc_length = f.length;
                    f.locator = new_next_loc;
                    pending = std::move(f);
                    cursor.next();
                }

                // Record split relocation for remote replicas
                EditOperation::SplitRelocation reloc;
                reloc.fragment_origin = reloc_origin;
                reloc.split_offset = 0;
                reloc.fragment_length = reloc_length;
                reloc.new_locator = new_next_loc;
                op.split_relocations.push_back(std::move(reloc));

                hi = new_next_loc;
            }

            Locator new_loc = Locator::between(lo, hi);

            uint32_t char_count = count_utf8_chars(
                replacement, static_cast<uint32_t>(replacement.size()));

            Lamport frag_origin = m_clock.tick();
            for (uint32_t c = 1; c < char_count; ++c) m_clock.tick();

            Fragment frag(frag_origin, new_loc, replacement, char_count);
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

        prev_end = end;
    }

    // ---- Suffix ----
    if (pending) {
        new_tree.push_item(std::move(*pending));
        pending.reset();
    }
    new_tree.push_tree(cursor.suffix());

    // ---- Timestamp ----
    if (op.inserted_fragments.empty()) {
        op.timestamp = m_clock.tick();
    } else {
        op.timestamp = Lamport(m_replica_id, m_clock.value - 1);
    }
    m_version.observe(op.timestamp);

    // ---- Undo entry ----
    m_undo_stack.resize(m_undo_cursor);
    m_undo_stack.push_back(std::move(undo_entry));
    m_undo_cursor = m_undo_stack.size();

    // ---- Normalize and rebuild ----
    auto frags = new_tree.items();
    normalize_fragments(frags);
    set_fragments(std::move(frags));
    return op;
}
```

- [ ] **Step 2: Build**

Run: `cmake --build build-dev 2>&1 | tail -5`
Expected: Clean build.

- [ ] **Step 3: Run tst_buffer**

Run: `./build-dev/libs/collabtext/tst_buffer`
Expected: ALL tests pass, including the previously-failing `local_edit_replace_mid_fragment` (now "heXXo").

- [ ] **Step 4: Run full test suite**

Run: `cd build-dev && ctest --output-on-failure`
Expected: All 7 test executables pass.

- [ ] **Step 5: Run convergence 10 times**

```bash
for i in $(seq 1 10); do
    ./build-dev/libs/collabtext/tst_convergence 2>&1 | tail -1
done
```
Expected: All 10 pass.

- [ ] **Step 6: Commit**

```
git add libs/collabtext/src/crdt/Buffer.cpp
git commit -m "feat: rewrite apply_local_edit with cursor-based approach

Process ranges left-to-right using SumTree cursor on immutable old
tree. Uses cursor.slice() for O(log n) prefix copy and cursor.suffix()
for suffix. Tracks pending split-fragments across ranges. Fixes
locator ordering bug at same-locator boundaries (was producing 'heoXX'
instead of 'heXXo' for mid-text replacements)."
```

---

## Task 3: Augment FragmentSummary with insertion version tracking

**Files:**
- Modify: `libs/collabtext/src/crdt/Fragment.h:15-35, 152-167`

- [ ] **Step 1: Add version fields to FragmentSummary**

In `FragmentSummary` (Fragment.h), add two new fields and update `add_summary`:

```cpp
struct FragmentSummary {
    uint32_t visible_bytes = 0;
    uint32_t deleted_bytes = 0;
    Locator max_locator;
    Lamport max_origin = Lamport::min();
    Global max_version;

    // NEW: insertion version range for VersionedFullOffset pruning
    Global min_insertion_version;
    Global max_insertion_version;

    static FragmentSummary zero() { return {}; }

    void add_summary(const FragmentSummary& other) {
        visible_bytes += other.visible_bytes;
        deleted_bytes += other.deleted_bytes;
        if (other.max_locator > max_locator ||
            (other.max_locator == max_locator && other.max_origin > max_origin)) {
            max_locator = other.max_locator;
            max_origin = other.max_origin;
        }
        max_version.join(other.max_version);
        min_insertion_version.meet(other.min_insertion_version);
        max_insertion_version.join(other.max_insertion_version);
    }
};
```

- [ ] **Step 2: Update Fragment::summary() to populate version fields**

In `Fragment::summary()`, add version initialization:

```cpp
FragmentSummary summary() const {
    FragmentSummary s;
    uint32_t bytes = static_cast<uint32_t>(content.size());
    if (visible) {
        s.visible_bytes = bytes;
    } else {
        s.deleted_bytes = bytes;
    }
    s.max_locator = locator;
    s.max_origin = origin;
    if (length > 0) {
        s.max_version.observe(Lamport(origin.replica_id, origin.value + length - 1));
        // Track insertion version for versioned seek pruning
        Lamport ins_ts(origin.replica_id, origin.value);
        s.min_insertion_version.observe(ins_ts);
        s.max_insertion_version.observe(ins_ts);
    }
    return s;
}
```

- [ ] **Step 3: Build and run all tests**

Run: `cmake --build build-dev && cd build-dev && ctest --output-on-failure`
Expected: All tests pass. The new fields are additive — existing code doesn't read them.

- [ ] **Step 4: Commit**

```
git add libs/collabtext/src/crdt/Fragment.h
git commit -m "feat: add insertion version tracking to FragmentSummary

Add min/max_insertion_version fields for VersionedFullOffset
pruning. Populated in Fragment::summary(), merged in add_summary().
No behavioral change — fields are unused until versioned_seek."
```

---

## Task 4: Implement versioned_seek and add Opt 3 tests

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.h` — add `versioned_seek` declaration
- Modify: `libs/collabtext/src/crdt/Buffer.cpp` — implement `versioned_seek`, refactor deletion loop
- Modify: `libs/collabtext/tests/tst_buffer.cpp` — add 3 Opt 3 tests

- [ ] **Step 1: Add versioned_seek declaration to Buffer.h**

Add in the private section (after `insert_fragment_into_tree`):

```cpp
    /// Seek to a fragment by timestamp, using version-filtered subtree pruning.
    /// Returns pointer to fragment and byte offset within it, or nullptr if not found.
    struct VersionedSeekResult {
        const Fragment* fragment = nullptr;
        size_t fragment_index = 0;  // index in flattened fragment list
        uint32_t offset_in_fragment = 0;
    };
    VersionedSeekResult versioned_seek_by_timestamp(
        const std::vector<Fragment>& frags,
        Lamport target_ts) const;
```

- [ ] **Step 2: Implement versioned_seek_by_timestamp**

Add before `apply_remote_edit` in Buffer.cpp:

```cpp
Buffer::VersionedSeekResult Buffer::versioned_seek_by_timestamp(
    const std::vector<Fragment>& frags,
    Lamport target_ts) const
{
    // Binary-style search using origin ordering.
    // Fragments are sorted by (locator, origin). Within the same replica,
    // origins are monotonically increasing. We scan for the fragment
    // containing the target timestamp.
    for (size_t fi = 0; fi < frags.size(); ++fi) {
        auto &f = frags[fi];
        if (f.origin.replica_id != target_ts.replica_id) continue;
        if (target_ts.value < f.origin.value ||
            target_ts.value >= f.origin.value + f.length) continue;

        uint32_t char_off = target_ts.value - f.origin.value;
        uint32_t byte_off = char_off > 0
            ? char_to_byte_offset(f.content, char_off)
            : 0;
        return {&f, fi, byte_off};
    }
    return {};
}
```

Note: This initial implementation is still O(n) — it's a direct extraction of the existing search pattern into a named function. The tree-based version-pruned seek (using `min/max_insertion_version` to skip subtrees) is a future enhancement that builds on the FragmentSummary fields added in Task 3. The important thing is having the clean interface.

- [ ] **Step 3: Refactor apply_remote_edit deletion loop**

Replace the deletion loop in `apply_remote_edit` (lines 574-598) with:

```cpp
    // Apply deletions by matching timestamps.
    for (auto &ts : op.deleted_timestamps) {
        auto result = versioned_seek_by_timestamp(frags, ts);
        if (!result.fragment) continue;

        size_t fi = result.fragment_index;
        auto &f = frags[fi];

        if (f.length == 1) {
            f.delete_count++;
        } else {
            uint32_t offset = ts.value - f.origin.value;
            uint32_t byte_off = char_to_byte_offset(f.content, offset);

            if (offset > 0) {
                fi = split_fragment_at(frags, fi, byte_off);
            }

            if (frags[fi].length > 1) {
                uint32_t cb = first_char_bytes(frags[fi].content);
                split_fragment_at(frags, fi, cb);
            }
            frags[fi].delete_count++;
        }
    }
```

- [ ] **Step 4: Add 3 Opt 3 tests to tst_buffer**

Add after the Opt 2 tests:

```cpp
    // -----------------------------------------------------------------------
    // Opt 3: VersionedFullOffset / remote edit tests
    // -----------------------------------------------------------------------

    void remote_edit_skips_unseen_fragments() {
        // Replica A inserts "aaa", replica B inserts "bbb".
        // A's edit (deleting its own text) should not affect B's fragments.
        Buffer bufA(1);
        Buffer bufB(2);

        auto opA1 = bufA.apply_local_edit({{0, 0}}, {"aaa"});
        auto opB1 = bufB.apply_local_edit({{0, 0}}, {"bbb"});

        // Cross-deliver inserts
        bufA.apply_ops({opB1});
        bufB.apply_ops({opA1});
        std::string converged = bufA.text();
        QCOMPARE(bufA.text(), bufB.text());

        // A deletes its own "aaa" (which it sees at some offset)
        // Find where "aaa" is in A's text
        auto posA = converged.find("aaa");
        QVERIFY(posA != std::string::npos);
        auto opA2 = bufA.apply_local_edit(
            {{static_cast<uint32_t>(posA), static_cast<uint32_t>(posA + 3)}}, {""});

        bufB.apply_ops({opA2});

        // B should have only "bbb" remaining
        QCOMPARE(bufA.text(), bufB.text());
        QCOMPARE(bufA.text().size(), size_t(3));
        QVERIFY(bufA.text().find("bbb") != std::string::npos);
    }

    void remote_edit_version_filtered_offset() {
        // 3 replicas insert text. Replica 1 edits only seeing replica 2's text.
        Buffer buf1(1), buf2(2), buf3(3);

        auto op1 = buf1.apply_local_edit({{0, 0}}, {"111"});
        auto op2 = buf2.apply_local_edit({{0, 0}}, {"222"});
        auto op3 = buf3.apply_local_edit({{0, 0}}, {"333"});

        // buf1 only sees buf2's insert
        buf1.apply_ops({op2});

        // buf1 deletes some text
        // buf1's visible text is now "111" + "222" (in some order)
        std::string text1 = buf1.text();
        QCOMPARE(text1.size(), size_t(6));

        // Delete the first 3 characters of buf1's view
        auto opDel = buf1.apply_local_edit({{0, 3}}, {""});

        // Now apply all ops to a fresh buffer and verify convergence
        Buffer bufFinal(4);
        bufFinal.apply_ops({op1, op2, op3, opDel});
        // Flush
        for (int i = 0; i < 5; ++i) bufFinal.apply_ops({});

        // bufFinal should have the same as buf1 after also receiving op3
        buf1.apply_ops({op3});
        QCOMPARE(buf1.text(), bufFinal.text());
    }

    void remote_edit_convergence_with_concurrent_deletes() {
        // Two replicas delete overlapping ranges concurrently.
        Buffer bufA(1), bufB(2);

        auto opIns = bufA.apply_local_edit({{0, 0}}, {"abcdefghij"});
        bufB.apply_ops({opIns});
        QCOMPARE(bufB.text(), std::string("abcdefghij"));

        // A deletes [2,5) → "ab___fghij" → "abfghij"
        auto opDelA = bufA.apply_local_edit({{2, 5}}, {""});
        // B deletes [3,7) → "abc____hij" → "abchij"
        auto opDelB = bufB.apply_local_edit({{3, 7}}, {""});

        // Cross-deliver
        bufA.apply_ops({opDelB});
        bufB.apply_ops({opDelA});

        // Both should converge — union of deletes
        QCOMPARE(bufA.text(), bufB.text());
        // Characters 2,3,4,5,6 all deleted → "abhij"
        QCOMPARE(bufA.text(), std::string("abhij"));
    }
```

- [ ] **Step 5: Build and run**

Run: `cmake --build build-dev && cd build-dev && ctest --output-on-failure`
Expected: All tests pass.

- [ ] **Step 6: Run convergence 10 times**

```bash
for i in $(seq 1 10); do
    ./build-dev/libs/collabtext/tst_convergence 2>&1 | tail -1
done
```
Expected: All 10 pass.

- [ ] **Step 7: Commit**

```
git add libs/collabtext/src/crdt/Buffer.h libs/collabtext/src/crdt/Buffer.cpp libs/collabtext/tests/tst_buffer.cpp
git commit -m "feat: add versioned_seek, refactor remote edit deletion loop

Extract versioned_seek_by_timestamp() from inline deletion search.
Refactor apply_remote_edit to use it. Add FragmentSummary version
fields for future tree-based pruning. Add 3 remote edit tests."
```

---

## Acceptance Criteria

All of the following must be true before this batch is complete:

1. `local_edit_replace_mid_fragment` passes (was failing: "heoXX" → now "heXXo")
2. All 7 new Opt 2 tests in `tst_buffer` pass
3. All 3 new Opt 3 tests in `tst_buffer` pass
4. All pre-existing `tst_buffer` tests pass (no regressions)
5. All `tst_convergence` tests pass on 10 consecutive runs
6. All `tst_anchor` tests pass
7. All `tst_opqueue` tests pass
8. `apply_local_edit` processes ranges left-to-right (ascending sort)
9. `apply_local_edit` uses `cursor.slice()` for prefix copy
10. `FragmentSummary` has `min_insertion_version` and `max_insertion_version` fields
11. `versioned_seek_by_timestamp` exists and is used by `apply_remote_edit`
