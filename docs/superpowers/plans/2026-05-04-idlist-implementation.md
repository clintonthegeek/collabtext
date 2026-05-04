# IdList Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement `CollabText::Crdt::IdList` — a CRDT-shaped ordered list of opaque `uint64` elements, sharing collabtext's existing op-causality, anchor, undo, and GC machinery — alongside `Buffer`.

**Architecture:** New primitive lives in `libs/collabtext/src/crdt/IdList.{h,cpp}`. Reuses `SumTree`, `Locator`, `Anchor`, `Clock`, `UndoMap`, and the deferred-op pattern verbatim. `IdListOperation` is a separate variant from `Operation`; wire format additively bumps `schema_version` 2 → 3. Algorithmically a slimmer cousin of `Buffer` (no UTF-8 splitting, no multi-char fragments, atomic elements).

**Tech Stack:** C++20, Qt6 Test (test runner only — engine itself is Qt-free), CMake 3.19+, the existing `SumTree<T,B>` template.

**Binding spec:** `docs/specs/2026-05-04-d-evolution-response.md`. The "What we won't do" section is load-bearing — do not expand the API beyond what's listed there.

**Reference for the algorithmic skeleton:** `libs/collabtext/src/crdt/Buffer.{h,cpp}`. When in doubt about an implementation detail, the rule is: look at `Buffer`'s analogue first. Differences are simplifications.

---

## File Structure

### New files

| Path | Responsibility |
|---|---|
| `libs/collabtext/src/crdt/IdList.h` | Public API: `IdListEntry`, `IdListSummary`, dimension types, `IdList` class |
| `libs/collabtext/src/crdt/IdList.cpp` | Implementation: local ops, remote apply, anchors, undo, GC |
| `libs/collabtext/src/crdt/IdListOperations.h` | `IdListInsert`, `IdListRemove`, `IdListUndoOp`, `IdListOperation` variant, `IdListOperationQueue` |
| `libs/collabtext/tests/tst_idlist.cpp` | Local API: insert, remove, ids() invariants |
| `libs/collabtext/tests/tst_idlist_anchor.cpp` | Anchor stability under concurrent edits |
| `libs/collabtext/tests/tst_idlist_undo.cpp` | Undo, redo, coalescing, collaborative undo |
| `libs/collabtext/tests/tst_idlist_gc.cpp` | `collect_garbage()` + `compact(watermark)` |
| `libs/collabtext/tests/tst_idlist_serialization.cpp` | Wire format round-trip + schema version |
| `libs/collabtext/tests/tst_idlist_convergence.cpp` | Multi-replica convergence scenarios |
| `libs/collabtext/tests/tst_idlist_fuzz.cpp` | Randomized N-replica fuzz with invariant checks |
| `docs/CRDT_IDLIST_SPEC.md` | Spec mirror of `CRDT_ENGINE_SPEC.md` for `IdList` |

### Modified files

| Path | Reason |
|---|---|
| `libs/collabtext/CMakeLists.txt` | Add `IdList.cpp` and `tst_idlist*` targets |
| `libs/collabtext/src/crdt/Serialization.h` | Encoder/decoder declarations for `IdListOperation` |
| `libs/collabtext/src/crdt/Serialization.cpp` | Encoder/decoder for new op variant |
| `libs/collabtext/src/crdt/SidecarManifest.{h,cpp}` | Bump `schema_version` 2 → 3 |
| `docs/ARCHITECTURE.md` | Note that the engine now provides two primitives |
| `README.md` | Update "What it is" to mention `IdList` (post-ship, in β10) |

### Untouched

`Buffer.{h,cpp}` — no changes. `CollabDocument`, `SyncManager`, `FileSync`, `StreamSync` — no changes (StreamSync is already multi-stream and payloads are opaque). `app/` — frozen during β.

---

## Phase β1 — Skeleton (compile, no behavior)

Goal: get a stub `IdList` class compiling and a test that exercises construction. No real functionality yet — this lets later phases focus purely on algorithm.

### Task 1.1: Define the entry struct and summary

**Files:**
- Create: `libs/collabtext/src/crdt/IdList.h`

- [ ] **Step 1: Write `IdList.h` with the entry struct, summary, and class skeleton**

```cpp
#pragma once

#include "crdt/Anchor.h"
#include "crdt/Clock.h"
#include "crdt/Locator.h"
#include "crdt/SumTree.h"
#include "crdt/UndoMap.h"
#include "crdt/IdListOperations.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace CollabText::Crdt {

// Same branching factor as the fragment tree.
static constexpr std::size_t IDLIST_TREE_B = 6;

struct IdListSummary {
    uint32_t visible_count = 0;
    uint32_t deleted_count = 0;
    Locator max_locator;
    Lamport max_origin = Lamport::min();
    Global max_version;
    Global min_insertion_version;
    Global max_insertion_version;

    static IdListSummary zero() { return {}; }

    void add_summary(const IdListSummary& other) {
        visible_count += other.visible_count;
        deleted_count += other.deleted_count;
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

/// Seek by visible-element index.
struct VisibleIndex {
    uint32_t value = 0;
    static VisibleIndex zero() { return {0}; }
    void add_summary(const IdListSummary& s) { value += s.visible_count; }
    auto operator<=>(const VisibleIndex&) const = default;
    bool operator==(const VisibleIndex&) const = default;
};

/// One ID entry in the list. Each entry is atomic — no splitting.
struct IdListEntry {
    using Summary = IdListSummary;

    Lamport origin;        ///< Lamport timestamp of the inserting op
    Locator locator;       ///< Fractional position
    uint64_t id = 0;       ///< Opaque application-owned id
    std::vector<Lamport> deletions;  ///< Lamport timestamps of removing ops
    bool visible = true;

    IdListEntry() = default;
    IdListEntry(Lamport orig, Locator loc, uint64_t i)
        : origin(orig), locator(std::move(loc)), id(i) {}

    bool was_visible() const { return deletions.empty(); }

    bool compute_visible(const UndoMap& undo_map) const {
        if (undo_map.is_undone(origin)) return false;
        for (auto& del : deletions)
            if (!undo_map.is_undone(del)) return false;
        return true;
    }

    bool is_visible(const UndoMap& undo_map) const { return compute_visible(undo_map); }

    bool was_visible_at(const Global& version, const UndoMap& undo_map) const {
        if (!version.observed(origin)) return false;
        if (undo_map.was_undone(origin, version)) return false;
        for (auto& del : deletions) {
            if (version.observed(del) && !undo_map.was_undone(del, version))
                return false;
        }
        return true;
    }

    IdListSummary summary() const {
        IdListSummary s;
        if (visible) s.visible_count = 1; else s.deleted_count = 1;
        s.max_locator = locator;
        s.max_origin = origin;
        s.max_version.observe(origin);
        s.min_insertion_version.observe(origin);
        s.max_insertion_version.observe(origin);
        return s;
    }
};

using IdListTree = SumTree<IdListEntry, IDLIST_TREE_B>;

class IdList {
public:
    explicit IdList(uint16_t replica_id);

    // ---- Local operations (filled in later phases) ----
    IdListOperation insert_after(const Anchor& after, uint64_t id);
    IdListOperation remove_at(const Anchor& target);

    // ---- Remote ----
    void apply_ops(const std::vector<IdListOperation>& ops);

    // ---- Undo / redo ----
    std::optional<IdListOperation> undo();
    std::optional<IdListOperation> redo();
    size_t undo_depth() const { return m_undo_cursor; }
    bool coalesce_last_undo();
    size_t max_undo_depth() const { return m_max_undo_depth; }
    void set_max_undo_depth(size_t depth);

    // ---- Queries ----
    std::vector<uint64_t> ids() const;
    uint32_t size() const;  // visible element count

    Anchor anchor_of(uint64_t id, Bias bias = Bias::Left) const;
    Anchor anchor_at_index(uint32_t index, Bias bias = Bias::Left) const;
    uint32_t resolve_anchor(const Anchor& a) const;
    int compare_anchors(const Anchor& a, const Anchor& b) const;

    const Global& version() const { return m_version; }
    uint16_t replica_id() const { return m_replica_id; }

    // ---- GC ----
    size_t collect_garbage();
    size_t compact(const Global& watermark);

    // ---- Diagnostics / testing ----
    std::vector<IdListEntry> entries() const;
    size_t tombstone_count() const;
    size_t entry_count() const;

private:
    uint16_t m_replica_id;
    Lamport m_clock;
    Global m_version;
    UndoMap m_undo_map;
    IdListTree m_entry_tree;

    // Filled in later phases:
    struct UndoEntry {
        std::vector<UndoMapKey> inserted_keys;
        std::vector<Lamport> deletion_ids;
    };
    std::vector<UndoEntry> m_undo_stack;
    size_t m_undo_cursor = 0;
    size_t m_max_undo_depth = 1000;
};

} // namespace CollabText::Crdt
```

- [ ] **Step 2: Commit**

```bash
git add libs/collabtext/src/crdt/IdList.h
git commit -m "feat(crdt): IdList header skeleton (entry, summary, API surface)"
```

### Task 1.2: Define the operations

**Files:**
- Create: `libs/collabtext/src/crdt/IdListOperations.h`

- [ ] **Step 1: Write `IdListOperations.h`**

```cpp
#pragma once

#include "crdt/Anchor.h"
#include "crdt/Clock.h"
#include "crdt/Locator.h"
#include "crdt/SumTree.h"

#include <cstdint>
#include <variant>
#include <vector>

namespace CollabText::Crdt {

/// Insert `id` between two existing elements at the given pre-computed locator.
/// The receiver places this element using (locator, origin) tiebreak.
struct IdListInsertOp {
    Lamport timestamp;     // == origin of the new element
    Global version;        // causal dependencies
    uint64_t id = 0;
    Locator locator;
};

/// Remove the element with the given origin. The receiver finds it by origin
/// and pushes `timestamp` onto the entry's deletions vector.
struct IdListRemoveOp {
    Lamport timestamp;     // deletion id
    Global version;
    Lamport target_origin; // origin of the entry being removed
};

/// Undo/redo operation; mirrors Buffer's UndoOperation.
struct IdListUndoOpVariant {
    Lamport timestamp;
    Global version;
    std::vector<std::pair<Lamport, uint32_t>> counts;
};

using IdListOperation =
    std::variant<IdListInsertOp, IdListRemoveOp, IdListUndoOpVariant>;

inline Lamport get_idlist_op_timestamp(const IdListOperation& op) {
    return std::visit([](const auto& o) -> Lamport { return o.timestamp; }, op);
}

inline const Global& get_idlist_op_version(const IdListOperation& op) {
    return std::visit([](const auto& o) -> const Global& { return o.version; }, op);
}

// SumTree-backed deferred queue, ordered by Lamport timestamp.
struct IdListOpSummary {
    Lamport max_timestamp = Lamport::min();
    static IdListOpSummary zero() { return {}; }
    void add_summary(const IdListOpSummary& other) {
        if (other.max_timestamp > max_timestamp) max_timestamp = other.max_timestamp;
    }
};

struct IdListOpEntry {
    using Summary = IdListOpSummary;
    Lamport timestamp;
    IdListOperation op;
    IdListOpSummary summary() const { return {timestamp}; }
};

static constexpr std::size_t IDLIST_OPQUEUE_B = 2;
using IdListOperationQueue = SumTree<IdListOpEntry, IDLIST_OPQUEUE_B>;

} // namespace CollabText::Crdt
```

- [ ] **Step 2: Commit**

```bash
git add libs/collabtext/src/crdt/IdListOperations.h
git commit -m "feat(crdt): IdList operation variants + deferred queue type"
```

### Task 1.3: Empty `IdList.cpp` with constructor + stubs

**Files:**
- Create: `libs/collabtext/src/crdt/IdList.cpp`

- [ ] **Step 1: Write a minimal `IdList.cpp` that compiles**

All non-trivial methods abort with `assert(false && "not implemented")` or return `{}` so callers can link. Constructor mirrors `Buffer::Buffer`:

```cpp
#include "crdt/IdList.h"
#include <cassert>

namespace CollabText::Crdt {

IdList::IdList(uint16_t replica_id)
    : m_replica_id(replica_id)
    , m_clock(replica_id, 1)
{}

IdListOperation IdList::insert_after(const Anchor&, uint64_t) {
    assert(false && "IdList::insert_after not yet implemented");
    return IdListInsertOp{};
}

IdListOperation IdList::remove_at(const Anchor&) {
    assert(false && "IdList::remove_at not yet implemented");
    return IdListRemoveOp{};
}

void IdList::apply_ops(const std::vector<IdListOperation>&) {
    assert(false && "IdList::apply_ops not yet implemented");
}

std::optional<IdListOperation> IdList::undo() { return std::nullopt; }
std::optional<IdListOperation> IdList::redo() { return std::nullopt; }
bool IdList::coalesce_last_undo() { return false; }
void IdList::set_max_undo_depth(size_t depth) { m_max_undo_depth = depth; }

std::vector<uint64_t> IdList::ids() const { return {}; }
uint32_t IdList::size() const { return m_entry_tree.summary().visible_count; }

Anchor IdList::anchor_of(uint64_t, Bias) const { return Anchor::min(); }
Anchor IdList::anchor_at_index(uint32_t, Bias) const { return Anchor::min(); }
uint32_t IdList::resolve_anchor(const Anchor&) const { return 0; }
int IdList::compare_anchors(const Anchor&, const Anchor&) const { return 0; }

size_t IdList::collect_garbage() { return 0; }
size_t IdList::compact(const Global&) { return 0; }

std::vector<IdListEntry> IdList::entries() const { return m_entry_tree.items(); }

size_t IdList::tombstone_count() const {
    size_t n = 0;
    m_entry_tree.for_each([&](const IdListEntry& e) { if (!e.visible) ++n; });
    return n;
}

size_t IdList::entry_count() const {
    size_t n = 0;
    m_entry_tree.for_each([&](const IdListEntry&) { ++n; });
    return n;
}

} // namespace CollabText::Crdt
```

- [ ] **Step 2: Wire into `CMakeLists.txt`**

Modify `libs/collabtext/CMakeLists.txt` — add `src/crdt/IdList.cpp` to the `add_library(collabtext STATIC ...)` source list, alphabetically near `src/crdt/Buffer.cpp`.

- [ ] **Step 3: Build to verify it compiles**

Run: `cmake --build build-dev -j`
Expected: clean build, no errors.

- [ ] **Step 4: Commit**

```bash
git add libs/collabtext/src/crdt/IdList.cpp libs/collabtext/CMakeLists.txt
git commit -m "feat(crdt): IdList stub implementation (constructor only)"
```

### Task 1.4: First test — empty IdList

**Files:**
- Create: `libs/collabtext/tests/tst_idlist.cpp`

- [ ] **Step 1: Write the smoke test**

```cpp
#include <QTest>
#include "crdt/IdList.h"

using namespace CollabText::Crdt;

class TestIdList : public QObject {
    Q_OBJECT
private slots:
    void empty_list_has_no_ids() {
        IdList list(1);
        QCOMPARE(list.size(), 0u);
        QCOMPARE(list.ids().size(), size_t{0});
    }

    void replica_id_round_trips() {
        IdList list(42);
        QCOMPARE(list.replica_id(), quint16(42));
    }

    void initial_version_is_empty() {
        IdList list(1);
        QVERIFY(list.version().size() == 0 || list.version()[0] == 0);
    }
};

QTEST_GUILESS_MAIN(TestIdList)
#include "tst_idlist.moc"
```

- [ ] **Step 2: Wire into `CMakeLists.txt`**

Modify `libs/collabtext/CMakeLists.txt` — add `add_crdt_test(tst_idlist)` near the other `add_crdt_test(...)` calls.

- [ ] **Step 3: Build + run**

Run: `cmake --build build-dev -j && ctest --test-dir build-dev -R tst_idlist --output-on-failure`
Expected: PASS, 3 tests.

- [ ] **Step 4: Commit**

```bash
git add libs/collabtext/tests/tst_idlist.cpp libs/collabtext/CMakeLists.txt
git commit -m "test(idlist): smoke test — empty list, replica id round-trip"
```

---

## Phase β2 — Local insert

Goal: `insert_after()` materializes elements; `ids()` returns them in order; locators are placed correctly between neighbours.

### Task 2.1: Insert into empty list

- [ ] **Step 1: Failing test in `tst_idlist.cpp`**

```cpp
void insert_into_empty_list() {
    IdList list(1);
    list.insert_after(Anchor::min(), 0xAA);
    QCOMPARE(list.size(), 1u);
    QCOMPARE(list.ids(), std::vector<uint64_t>{0xAA});
}
```

- [ ] **Step 2: Run, expect failure**

Run: `ctest --test-dir build-dev -R tst_idlist::insert_into_empty_list --output-on-failure`
Expected: FAIL — assertion fires.

- [ ] **Step 3: Implement `insert_after` for the empty case**

In `IdList.cpp`, replace the stub. Use `Buffer::apply_local_edit` as the algorithmic reference:

1. Allocate a `Locator` between the predecessor's locator and the successor's. For empty list, that's `Locator::between(Locator::min(), Locator::max())`.
2. Allocate a `Lamport origin = (m_replica_id, m_clock.value)`; bump `m_clock`.
3. Bump `m_version.observe(origin)`.
4. Create an `IdListEntry`, push into the tree, rebuild summary.
5. Build and return an `IdListInsertOp`.

Implementation sketch — the full method:

```cpp
IdListOperation IdList::insert_after(const Anchor& after, uint64_t id) {
    // Find the predecessor entry (or use Locator::min() if Anchor::min()).
    auto frags = m_entry_tree.items();

    Locator lo = Locator::min();
    Locator hi = Locator::max();
    if (!after.is_min()) {
        // Find by anchor.replica_id + char_value.
        for (size_t i = 0; i < frags.size(); ++i) {
            if (frags[i].origin.replica_id == after.replica_id &&
                frags[i].origin.value == after.char_value) {
                lo = frags[i].locator;
                if (i + 1 < frags.size()) hi = frags[i + 1].locator;
                break;
            }
        }
    } else if (!frags.empty()) {
        hi = frags.front().locator;
    }

    Locator new_loc = Locator::between(lo, hi);
    Lamport origin(m_replica_id, m_clock.value);
    m_clock = Lamport(m_replica_id, m_clock.value + 1);
    Global version_before = m_version;
    m_version.observe(origin);

    IdListEntry entry(origin, new_loc, id);
    entry.visible = true;
    // Insert in (locator, origin) sorted order.
    // ... (mirror Buffer::insert_fragment)

    // Rebuild tree.
    // ... (mirror Buffer::set_fragments)

    IdListInsertOp op;
    op.timestamp = origin;
    op.version = std::move(version_before);
    op.id = id;
    op.locator = std::move(new_loc);
    return op;
}
```

The exact tree-rebuild idiom matches `Buffer::set_fragments` / `insert_fragment` — reuse the pattern (it's <30 lines).

- [ ] **Step 4: Run, expect pass**

Run: `ctest --test-dir build-dev -R tst_idlist::insert_into_empty_list --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add libs/collabtext/src/crdt/IdList.cpp libs/collabtext/tests/tst_idlist.cpp
git commit -m "feat(crdt): IdList::insert_after — empty-list case"
```

### Task 2.2: Insert at start of populated list

- [ ] **Step 1: Failing test**

```cpp
void insert_at_start_pushes_existing_back() {
    IdList list(1);
    list.insert_after(Anchor::min(), 0xAA);
    list.insert_after(Anchor::min(), 0xBB);  // Anchor::min() == "before everything"
    QCOMPARE(list.ids(), (std::vector<uint64_t>{0xBB, 0xAA}));
}
```

- [ ] **Step 2: Run, expect failure (or pass — verify ordering)**

Run: `ctest --test-dir build-dev -R tst_idlist::insert_at_start --output-on-failure`

- [ ] **Step 3: Verify `Locator::between(Locator::min(), <existing>)` produces a strictly-smaller locator**

If the `Locator::between` API requires `lo < hi`, this should already work — `Locator::min() < <anything else>`. If failing, the `hi` needs to be set to the first existing entry's locator. The implementation sketch in 2.1 already does this; debug if not.

- [ ] **Step 4: Run, expect pass; commit**

```bash
git commit -am "test(idlist): insert at start orders correctly"
```

### Task 2.3: Insert in middle (anchor at element)

- [ ] **Step 1: Failing test**

```cpp
void insert_after_middle_element() {
    IdList list(1);
    list.insert_after(Anchor::min(), 0xAA);
    auto a_anchor = list.anchor_of(0xAA, Bias::Left);  // resolved later — for now use ids()[0]'s origin
    list.insert_after(Anchor::min(), 0xBB);  // [0xBB, 0xAA]
    list.insert_after(a_anchor, 0xCC);       // [0xBB, 0xAA, 0xCC]  — wait, anchor at 0xAA + insert after = after 0xAA
    QCOMPARE(list.ids(), (std::vector<uint64_t>{0xBB, 0xAA, 0xCC}));
}
```

Note: this test depends on `anchor_of` working. Until anchors land in β4, use a workaround: synthesize the anchor manually from a known origin. Cleaner approach — defer this test to β4 and replace with a version that uses `insert_after(Anchor::min(), ...)` repeatedly to build [0xBB, 0xAA] only.

- [ ] **Step 2: Replace with the deferrable version for now**

```cpp
void multiple_inserts_at_start_reverse_order() {
    IdList list(1);
    list.insert_after(Anchor::min(), 1);
    list.insert_after(Anchor::min(), 2);
    list.insert_after(Anchor::min(), 3);
    QCOMPARE(list.ids(), (std::vector<uint64_t>{3, 2, 1}));
}
```

- [ ] **Step 3: Run, expect pass; commit**

```bash
git commit -am "test(idlist): repeated inserts at Anchor::min() reverse order"
```

### Task 2.4: Tree-rebuild idiom helpers

- [ ] **Step 1: Extract `get_entries()` / `set_entries()` helpers in `IdList.cpp`**

Mirror `Buffer::get_fragments()` / `Buffer::set_fragments()`. Set `visible` from `compute_visible(m_undo_map)` on rebuild. No origin index yet — defer until performance demands it (likely never; IdList element counts are small).

- [ ] **Step 2: Run all idlist tests; commit**

```bash
git commit -am "refactor(crdt): IdList — get/set entries helpers"
```

---

## Phase β3 — Local remove

Goal: `remove_at()` tombstones an entry; visibility recomputes; tombstone stays in the tree (for convergence).

### Task 3.1: Remove the only element

- [ ] **Step 1: Failing test**

```cpp
void remove_only_element() {
    IdList list(1);
    list.insert_after(Anchor::min(), 0xAA);
    // Need an anchor at 0xAA — for now construct manually from origin.
    // The first inserted element has origin = (replica=1, value=1).
    Anchor at_aa(1, 1, Bias::Left);
    list.remove_at(at_aa);
    QCOMPARE(list.size(), 0u);
    QCOMPARE(list.ids().size(), size_t{0});
    QCOMPARE(list.tombstone_count(), size_t{1});
}
```

- [ ] **Step 2: Run, expect failure**

- [ ] **Step 3: Implement `remove_at`**

Mirror `Buffer::apply_local_edit`'s deletion path:
1. Find the entry whose origin matches the anchor.
2. Allocate a deletion Lamport.
3. Push it into `entry.deletions`. Recompute `entry.visible`.
4. Update `m_version`.
5. Build and return `IdListRemoveOp { timestamp, version_before, target_origin }`.
6. Push an `UndoEntry { inserted_keys: {}, deletion_ids: {deletion_lamport} }` (sets up β6).

```cpp
IdListOperation IdList::remove_at(const Anchor& target) {
    auto frags = m_entry_tree.items();
    size_t idx = SIZE_MAX;
    for (size_t i = 0; i < frags.size(); ++i) {
        if (frags[i].origin.replica_id == target.replica_id &&
            frags[i].origin.value == target.char_value) {
            idx = i; break;
        }
    }
    assert(idx != SIZE_MAX && "remove_at: anchor target not found");

    Lamport del_id(m_replica_id, m_clock.value);
    m_clock = Lamport(m_replica_id, m_clock.value + 1);
    Global version_before = m_version;
    m_version.observe(del_id);

    Lamport target_origin = frags[idx].origin;  // capture before frags moves
    frags[idx].deletions.push_back(del_id);
    frags[idx].visible = frags[idx].compute_visible(m_undo_map);
    set_entries(std::move(frags));

    // Push undo entry (used by β6).
    m_undo_stack.push_back({{}, {del_id}});
    m_undo_cursor = m_undo_stack.size();
    trim_undo_stack();

    IdListRemoveOp op;
    op.timestamp = del_id;
    op.version = std::move(version_before);
    op.target_origin = target_origin;
    return op;
}
```

- [ ] **Step 4: Run, expect pass; commit**

```bash
git commit -am "feat(crdt): IdList::remove_at — tombstone the entry"
```

### Task 3.2: Remove preserves order of remaining elements

- [ ] **Step 1: Failing test**

```cpp
void remove_middle_keeps_neighbours() {
    IdList list(1);
    list.insert_after(Anchor::min(), 1);  // origin (1,1)
    list.insert_after(Anchor::min(), 2);  // origin (1,2)
    list.insert_after(Anchor::min(), 3);  // origin (1,3) → ids: [3,2,1]
    list.remove_at(Anchor(1, 2, Bias::Left));  // remove "2"
    QCOMPARE(list.ids(), (std::vector<uint64_t>{3, 1}));
    QCOMPARE(list.tombstone_count(), size_t{1});
}
```

- [ ] **Step 2: Run, expect pass; commit**

```bash
git commit -am "test(idlist): remove preserves order of remaining elements"
```

### Task 3.3: Re-insert with the same id is allowed (different origin)

- [ ] **Step 1: Test that ids are not unique-key constraints**

```cpp
void same_id_can_appear_twice() {
    IdList list(1);
    list.insert_after(Anchor::min(), 0xAA);
    list.insert_after(Anchor::min(), 0xAA);
    QCOMPARE(list.size(), 2u);
    QCOMPARE(list.ids(), (std::vector<uint64_t>{0xAA, 0xAA}));
}
```

- [ ] **Step 2: Run, expect pass; commit**

```bash
git commit -am "test(idlist): duplicate ids allowed (origin is the identifier)"
```

---

## Phase β4 — Anchors

Goal: `anchor_of(id)`, `anchor_at_index(idx)`, `resolve_anchor`, `compare_anchors` work and survive concurrent edits.

### Task 4.1: `anchor_at_index` returns the origin of the visible Nth entry

- [ ] **Step 1: Failing test**

```cpp
void anchor_at_index_picks_visible_entry() {
    IdList list(1);
    list.insert_after(Anchor::min(), 1);  // origin (1,1)
    list.insert_after(Anchor::min(), 2);  // origin (1,2) → ids: [2,1]
    Anchor a0 = list.anchor_at_index(0, Bias::Left);
    Anchor a1 = list.anchor_at_index(1, Bias::Left);
    QCOMPARE(a0.replica_id, quint16(1));
    QCOMPARE(a0.char_value, quint32(2));  // "2" is at index 0
    QCOMPARE(a1.char_value, quint32(1));
}
```

- [ ] **Step 2: Implement**

Walk visible entries, return Anchor at the Nth. Mirror `Buffer::anchor_at` shape but skip the byte/char counting.

- [ ] **Step 3: Run, pass; commit**

```bash
git commit -am "feat(crdt): IdList::anchor_at_index"
```

### Task 4.2: `anchor_of` finds by id

- [ ] **Step 1: Failing test**

```cpp
void anchor_of_finds_first_match() {
    IdList list(1);
    list.insert_after(Anchor::min(), 100);  // (1,1)
    list.insert_after(Anchor::min(), 200);  // (1,2) → [200,100]
    Anchor a = list.anchor_of(100, Bias::Right);
    QCOMPARE(a.char_value, quint32(1));
    QCOMPARE(a.bias, Bias::Right);
}
```

- [ ] **Step 2: Implement** (linear scan; an index can be added later if performance demands it).

- [ ] **Step 3: Run, pass; commit**

```bash
git commit -am "feat(crdt): IdList::anchor_of"
```

### Task 4.3: `resolve_anchor` returns visible index

- [ ] **Step 1: Failing test**

```cpp
void resolve_anchor_returns_visible_index() {
    IdList list(1);
    list.insert_after(Anchor::min(), 1);  // (1,1) at idx 0
    list.insert_after(Anchor::min(), 2);  // (1,2) at idx 0; "1" now at idx 1
    Anchor a = list.anchor_of(1, Bias::Left);
    QCOMPARE(list.resolve_anchor(a), quint32(1));
}

void resolve_min_max_anchors() {
    IdList list(1);
    list.insert_after(Anchor::min(), 1);
    list.insert_after(Anchor::min(), 2);
    QCOMPARE(list.resolve_anchor(Anchor::min()), quint32(0));
    QCOMPARE(list.resolve_anchor(Anchor::max()), list.size());
}
```

- [ ] **Step 2: Implement** (mirror `Buffer::resolve_anchor` minus byte counting).

- [ ] **Step 3: Pass; commit**

```bash
git commit -am "feat(crdt): IdList::resolve_anchor"
```

### Task 4.4: Anchor of a deleted element resolves to the visible position it would have occupied

- [ ] **Step 1: Failing test**

```cpp
void anchor_of_deleted_resolves_to_neighbour() {
    IdList list(1);
    list.insert_after(Anchor::min(), 1);  // (1,1)
    list.insert_after(Anchor::min(), 2);  // (1,2) → [2,1]
    Anchor a_one = list.anchor_of(1, Bias::Left);
    list.remove_at(a_one);
    // After delete, [2]; anchor at deleted "1" with Left bias resolves to 1 (after the visible).
    QCOMPARE(list.resolve_anchor(a_one), quint32(1));
}
```

- [ ] **Step 2: Verify implementation; pass; commit**

```bash
git commit -am "test(idlist): anchor of deleted element resolves to neighbour"
```

### Task 4.5: `compare_anchors`

- [ ] **Step 1: Failing test**

```cpp
void compare_anchors_orders_by_position() {
    IdList list(1);
    list.insert_after(Anchor::min(), 1);
    list.insert_after(Anchor::min(), 2);
    Anchor first = list.anchor_at_index(0, Bias::Left);
    Anchor second = list.anchor_at_index(1, Bias::Left);
    QVERIFY(list.compare_anchors(first, second) < 0);
    QVERIFY(list.compare_anchors(second, first) > 0);
    QCOMPARE(list.compare_anchors(first, first), 0);
}
```

- [ ] **Step 2: Implement** (resolve both, compare positions; tiebreak on bias). Mirror `Buffer::compare_anchors`.

- [ ] **Step 3: Pass; commit**

```bash
git commit -am "feat(crdt): IdList::compare_anchors"
```

### Task 4.6: Move tests for anchors into `tst_idlist_anchor.cpp`

- [ ] **Step 1: Create `libs/collabtext/tests/tst_idlist_anchor.cpp`** with `Q_OBJECT` boilerplate, copy the anchor tests there, wire `add_crdt_test(tst_idlist_anchor)` into CMakeLists.

- [ ] **Step 2: Build, run, commit**

```bash
git commit -am "test(idlist): split anchor tests into tst_idlist_anchor.cpp"
```

---

## Phase β5 — Remote application + causal ordering

Goal: `apply_ops()` accepts ops from another replica, defers if causal deps unmet, and converges with the local replica.

### Task 5.1: Apply a remote insert when deps are satisfied

- [ ] **Step 1: Failing test in `tst_idlist_convergence.cpp`** (new file)

```cpp
#include <QTest>
#include "crdt/IdList.h"

using namespace CollabText::Crdt;

class TestIdListConvergence : public QObject {
    Q_OBJECT
private slots:
    void two_replicas_converge_on_disjoint_inserts() {
        IdList alice(1), bob(2);

        auto op_a = alice.insert_after(Anchor::min(), 0xAA);
        auto op_b = bob.insert_after(Anchor::min(), 0xBB);

        alice.apply_ops({op_b});
        bob.apply_ops({op_a});

        QCOMPARE(alice.ids(), bob.ids());
        QCOMPARE(alice.size(), 2u);
    }
};

QTEST_GUILESS_MAIN(TestIdListConvergence)
#include "tst_idlist_convergence.moc"
```

- [ ] **Step 2: Wire into CMake (`add_crdt_test(tst_idlist_convergence)`); run, expect failure**

- [ ] **Step 3: Implement `apply_ops`** — accept a vector, dispatch each op via `try_apply`, retry deferred queue on success. Mirror `Buffer::apply_ops`. Each variant gets its own apply path:

```cpp
void IdList::apply_ops(const std::vector<IdListOperation>& ops) {
    for (const auto& op : ops) {
        if (m_version.observed(get_idlist_op_timestamp(op))) continue; // dup
        if (try_apply(op)) {
            retry_deferred();
        } else {
            enqueue_deferred({get_idlist_op_timestamp(op), op});
        }
    }
}

bool IdList::try_apply(const IdListOperation& op) {
    const Global& deps = get_idlist_op_version(op);
    if (!m_version.dominates(deps)) return false;
    return std::visit([&](const auto& concrete) {
        return apply_concrete(concrete);
    }, op);
}
```

(Equivalent of `Buffer::try_apply` + `Buffer::retry_deferred` + `Buffer::enqueue_deferred`.)

`apply_concrete(IdListInsertOp)`: place a new entry at the carried locator; bump version; reorder via insertion-sort by `(locator, origin)`.
`apply_concrete(IdListRemoveOp)`: find by `target_origin`, push deletion, recompute visibility.
`apply_concrete(IdListUndoOpVariant)`: defer to β6.

- [ ] **Step 4: Pass; commit**

```bash
git commit -am "feat(crdt): IdList::apply_ops + try_apply for insert/remove"
```

### Task 5.2: Concurrent inserts at the same position tiebreak by origin

- [ ] **Step 1: Failing test**

```cpp
void concurrent_inserts_at_same_position_tiebreak() {
    IdList alice(1), bob(2);
    auto op_a = alice.insert_after(Anchor::min(), 0xAA);
    auto op_b = bob.insert_after(Anchor::min(), 0xBB);

    alice.apply_ops({op_b});
    bob.apply_ops({op_a});

    QCOMPARE(alice.ids(), bob.ids());
    // Lower replica id wins the tiebreak: alice's entry sorts first.
    QCOMPARE(alice.ids(), (std::vector<uint64_t>{0xAA, 0xBB}));
}
```

- [ ] **Step 2: Verify the (locator, origin) sort produces this; pass; commit**

```bash
git commit -am "test(idlist): concurrent insert tiebreak by origin"
```

### Task 5.3: Out-of-order delivery is buffered until deps arrive

- [ ] **Step 1: Failing test**

```cpp
void out_of_order_delivery_buffers_then_applies() {
    IdList alice(1), bob(2);
    auto op_a1 = alice.insert_after(Anchor::min(), 1);
    Anchor at_a1(1, 1, Bias::Right);
    auto op_a2 = alice.insert_after(at_a1, 2);   // depends on op_a1

    bob.apply_ops({op_a2});      // out of order — buffered
    QCOMPARE(bob.size(), 0u);    // not applied yet
    bob.apply_ops({op_a1});      // now both apply
    QCOMPARE(bob.size(), 2u);
    QCOMPARE(bob.ids(), alice.ids());
}
```

- [ ] **Step 2: Verify deferred-queue path; pass; commit**

```bash
git commit -am "test(idlist): out-of-order delivery buffers then applies"
```

### Task 5.4: Insert + remove from two replicas converges

- [ ] **Step 1: Failing test**

```cpp
void insert_then_remove_from_other_replica() {
    IdList alice(1), bob(2);
    auto op_a = alice.insert_after(Anchor::min(), 0xAA);
    bob.apply_ops({op_a});  // bob now sees 0xAA

    Anchor at_aa(1, 1, Bias::Left);
    auto op_b = bob.remove_at(at_aa);
    alice.apply_ops({op_b});

    QCOMPARE(alice.size(), 0u);
    QCOMPARE(bob.size(), 0u);
    QCOMPARE(alice.tombstone_count(), bob.tombstone_count());
}
```

- [ ] **Step 2: Pass; commit**

```bash
git commit -am "test(idlist): cross-replica insert + remove converges"
```

### Task 5.5: Concurrent removes of the same element are idempotent

- [ ] **Step 1: Failing test**

```cpp
void concurrent_removes_of_same_element() {
    IdList alice(1), bob(2);
    auto op_a = alice.insert_after(Anchor::min(), 0xAA);
    bob.apply_ops({op_a});

    Anchor at_aa(1, 1, Bias::Left);
    auto rm_alice = alice.remove_at(at_aa);
    auto rm_bob = bob.remove_at(at_aa);

    alice.apply_ops({rm_bob});
    bob.apply_ops({rm_alice});

    QCOMPARE(alice.size(), 0u);
    QCOMPARE(bob.size(), 0u);
    // Both replicas see two deletions on the same entry; behaviour: both stored, entry stays invisible.
    QCOMPARE(alice.entries().size(), size_t{1});
    QCOMPARE(alice.entries()[0].deletions.size(), size_t{2});
}
```

- [ ] **Step 2: Implementation note**

The remove path appends to `deletions` unconditionally. Multi-deletion is fine; `compute_visible` already iterates the whole vector.

- [ ] **Step 3: Pass; commit**

```bash
git commit -am "test(idlist): concurrent removes accumulate, entry stays invisible"
```

### Task 5.6: Insert-after-deleted-anchor

- [ ] **Step 1: Failing test**

The classic adversarial case: A removes X; B (concurrently) inserts after X. Receiver must place B's insert in the right slot.

```cpp
void insert_after_concurrently_deleted_anchor() {
    IdList alice(1), bob(2);
    auto op_a1 = alice.insert_after(Anchor::min(), 1);
    bob.apply_ops({op_a1});

    // Both observe entry "1" at origin (1,1).
    Anchor at_one(1, 1, Bias::Right);
    // Concurrently:
    auto rm_a = alice.remove_at(at_one);
    auto ins_b = bob.insert_after(at_one, 2);

    alice.apply_ops({ins_b});
    bob.apply_ops({rm_a});

    QCOMPARE(alice.ids(), bob.ids());
    QCOMPARE(alice.ids(), (std::vector<uint64_t>{2}));  // "1" gone, "2" survives
}
```

- [ ] **Step 2: Verify this works** — `IdListInsertOp` carries the locator pre-computed by Bob, so Alice doesn't need the entry "1" to be visible to place "2". The deletion just tombstones "1"; the insert is positionally independent.

- [ ] **Step 3: Pass; commit**

```bash
git commit -am "test(idlist): insert-after-concurrently-deleted converges"
```

### Task 5.7: Three-replica chain delivery

- [ ] **Step 1: Failing test**

```cpp
void three_replicas_arbitrary_chain() {
    IdList alice(1), bob(2), carol(3);
    auto op_a = alice.insert_after(Anchor::min(), 1);
    bob.apply_ops({op_a});
    auto op_b = bob.insert_after(Anchor(1, 1, Bias::Right), 2);
    carol.apply_ops({op_a, op_b});
    auto op_c = carol.insert_after(Anchor(2, 1, Bias::Right), 3);

    alice.apply_ops({op_b, op_c});
    bob.apply_ops({op_c});

    QCOMPARE(alice.ids(), bob.ids());
    QCOMPARE(alice.ids(), carol.ids());
    QCOMPARE(alice.ids(), (std::vector<uint64_t>{1, 2, 3}));
}
```

- [ ] **Step 2: Pass; commit**

```bash
git commit -am "test(idlist): three-replica chain delivery"
```

### Task 5.8: Deduplication on re-delivery

- [ ] **Step 1: Failing test**

```cpp
void duplicate_op_delivery_is_noop() {
    IdList alice(1), bob(2);
    auto op = alice.insert_after(Anchor::min(), 0xAA);
    bob.apply_ops({op});
    bob.apply_ops({op});  // delivered twice
    QCOMPARE(bob.size(), 1u);
}
```

- [ ] **Step 2: The `apply_ops` early-return on `m_version.observed(...)` should already cover this. Pass; commit**

```bash
git commit -am "test(idlist): duplicate op delivery is idempotent"
```

---

## Phase β6 — Undo / redo

Goal: per-IdList undo stack, parity-based collaborative undo (mirror `Buffer`'s `UndoMap` mechanics).

### Task 6.1: Undo a local insert

- [ ] **Step 1: Failing test in `tst_idlist_undo.cpp`** (new file)

```cpp
#include <QTest>
#include "crdt/IdList.h"

using namespace CollabText::Crdt;

class TestIdListUndo : public QObject {
    Q_OBJECT
private slots:
    void undo_local_insert() {
        IdList list(1);
        list.insert_after(Anchor::min(), 0xAA);
        QCOMPARE(list.size(), 1u);
        auto undo_op = list.undo();
        QVERIFY(undo_op.has_value());
        QCOMPARE(list.size(), 0u);
    }
};

QTEST_GUILESS_MAIN(TestIdListUndo)
#include "tst_idlist_undo.moc"
```

- [ ] **Step 2: Wire CMake; run, expect failure**

- [ ] **Step 3: Implement undo**

- `insert_after` now also pushes an `UndoEntry { inserted_keys: {key_for(origin)}, deletion_ids: {} }` (mirroring `Buffer`).
- `undo()` pops, builds an `IdListUndoOpVariant` op that flips parity in `m_undo_map` for the inserted keys + deletion ids, applies it locally, and returns it.
- Mirror `Buffer::undo` exactly — `UndoMap` is shared.

- [ ] **Step 4: Pass; commit**

```bash
git commit -am "feat(crdt): IdList undo for local insert"
```

### Task 6.2: Undo a local remove restores the element

- [ ] **Step 1: Failing test**

```cpp
void undo_local_remove_restores_element() {
    IdList list(1);
    list.insert_after(Anchor::min(), 0xAA);
    list.remove_at(Anchor(1, 1, Bias::Left));
    QCOMPARE(list.size(), 0u);
    list.undo();
    QCOMPARE(list.size(), 1u);
    QCOMPARE(list.ids(), std::vector<uint64_t>{0xAA});
}
```

- [ ] **Step 2: Implement (deletion_ids path in undo); pass; commit**

```bash
git commit -am "feat(crdt): IdList undo for local remove"
```

### Task 6.3: Redo

- [ ] **Step 1: Failing test**

```cpp
void redo_after_undo_restores_state() {
    IdList list(1);
    list.insert_after(Anchor::min(), 0xAA);
    list.undo();
    list.redo();
    QCOMPARE(list.ids(), std::vector<uint64_t>{0xAA});
}
```

- [ ] **Step 2: Implement (mirror `Buffer::redo`); pass; commit**

```bash
git commit -am "feat(crdt): IdList redo"
```

### Task 6.4: Collaborative undo — only my own edits

- [ ] **Step 1: Failing test**

```cpp
void undo_only_unwinds_local_edits() {
    IdList alice(1), bob(2);
    auto op_a = alice.insert_after(Anchor::min(), 0xAA);
    bob.apply_ops({op_a});
    auto op_b = bob.insert_after(Anchor::min(), 0xBB);
    alice.apply_ops({op_b});
    QCOMPARE(alice.size(), 2u);

    auto undo_op = alice.undo();          // unwinds alice's insert
    QVERIFY(undo_op.has_value());
    bob.apply_ops({*undo_op});
    QCOMPARE(alice.ids(), bob.ids());
    QCOMPARE(alice.ids(), std::vector<uint64_t>{0xBB});  // bob's insert remains
}
```

- [ ] **Step 2: Pass; commit**

```bash
git commit -am "test(idlist): collaborative undo unwinds only local edits"
```

### Task 6.5: Coalesce last undo

- [ ] **Step 1: Failing test**

```cpp
void coalesce_last_undo_groups_two_inserts() {
    IdList list(1);
    list.insert_after(Anchor::min(), 1);
    list.insert_after(Anchor::min(), 2);
    QVERIFY(list.coalesce_last_undo());
    list.undo();  // single undo unwinds both
    QCOMPARE(list.size(), 0u);
}
```

- [ ] **Step 2: Implement (mirror `Buffer::coalesce_last_undo`); pass; commit**

```bash
git commit -am "feat(crdt): IdList::coalesce_last_undo"
```

### Task 6.6: Max undo depth trims oldest entries

- [ ] **Step 1: Failing test**

```cpp
void max_undo_depth_trims_oldest() {
    IdList list(1);
    list.set_max_undo_depth(3);
    for (int i = 0; i < 5; ++i) list.insert_after(Anchor::min(), i);
    QCOMPARE(list.undo_depth(), size_t{3});
}
```

- [ ] **Step 2: Pass; commit**

```bash
git commit -am "test(idlist): undo stack respects max depth"
```

---

## Phase β7 — Garbage collection

Goal: `collect_garbage()` removes locally-undo-safe tombstones; `compact(watermark)` removes globally-acknowledged tombstones.

### Task 7.1: `collect_garbage` removes locally-deleted, undo-safe tombstones

- [ ] **Step 1: Failing test in `tst_idlist_gc.cpp`** (new file)

```cpp
#include <QTest>
#include "crdt/IdList.h"

using namespace CollabText::Crdt;

class TestIdListGc : public QObject {
    Q_OBJECT
private slots:
    void collect_garbage_removes_old_tombstones() {
        IdList list(1);
        list.insert_after(Anchor::min(), 0xAA);
        list.remove_at(Anchor(1, 1, Bias::Left));
        list.set_max_undo_depth(0);  // discard all undo
        list.set_max_undo_depth(1000);
        QCOMPARE(list.tombstone_count(), size_t{1});
        size_t collected = list.collect_garbage();
        QCOMPARE(collected, size_t{1});
        QCOMPARE(list.tombstone_count(), size_t{0});
    }
};

QTEST_GUILESS_MAIN(TestIdListGc)
#include "tst_idlist_gc.moc"
```

- [ ] **Step 2: Wire CMake; implement (mirror `Buffer::collect_garbage`); pass; commit**

```bash
git commit -am "feat(crdt): IdList::collect_garbage"
```

### Task 7.2: `collect_garbage` does not remove remote-deleted entries

- [ ] **Step 1: Failing test** — same shape as Buffer's: a remote replica's deletion needs the watermark, not local GC.

```cpp
void collect_garbage_skips_remote_deletions() {
    IdList alice(1), bob(2);
    auto op_a = alice.insert_after(Anchor::min(), 0xAA);
    bob.apply_ops({op_a});
    auto rm_b = bob.remove_at(Anchor(1, 1, Bias::Left));
    alice.apply_ops({rm_b});
    alice.set_max_undo_depth(0);
    alice.set_max_undo_depth(1000);
    QCOMPARE(alice.collect_garbage(), size_t{0});  // remote deletion — needs watermark
}
```

- [ ] **Step 2: Pass; commit**

```bash
git commit -am "test(idlist): collect_garbage preserves remote-deleted entries"
```

### Task 7.3: `compact(watermark)` reclaims acknowledged tombstones

- [ ] **Step 1: Failing test**

```cpp
void compact_with_watermark_reclaims_acknowledged() {
    IdList alice(1), bob(2);
    auto op_a = alice.insert_after(Anchor::min(), 0xAA);
    bob.apply_ops({op_a});
    auto rm_b = bob.remove_at(Anchor(1, 1, Bias::Left));
    alice.apply_ops({rm_b});

    // Watermark = the meet of all replicas' versions (everything everyone has seen).
    Global wm = alice.version();
    wm.meet(bob.version());

    alice.set_max_undo_depth(0); alice.set_max_undo_depth(1000);
    size_t reclaimed = alice.compact(wm);
    QCOMPARE(reclaimed, size_t{1});
}
```

(The exact `Global` API for building a watermark — copy from `tst_gc.cpp`; the helper there is reusable verbatim.)

- [ ] **Step 2: Implement (mirror `Buffer::compact`); pass; commit**

```bash
git commit -am "feat(crdt): IdList::compact watermark-based GC"
```

### Task 7.4: GC interacts correctly with undo (protected entries stay)

- [ ] **Step 1: Failing test**

```cpp
void gc_protects_entries_in_undo_stack() {
    IdList list(1);
    list.insert_after(Anchor::min(), 0xAA);
    list.remove_at(Anchor(1, 1, Bias::Left));  // undo entry retains the deletion id
    QCOMPARE(list.collect_garbage(), size_t{0});  // protected
    list.set_max_undo_depth(0);  // drop the undo entry
    list.set_max_undo_depth(1000);
    QCOMPARE(list.collect_garbage(), size_t{1});  // now reclaimable
}
```

- [ ] **Step 2: Pass; commit**

```bash
git commit -am "test(idlist): GC protects entries referenced by undo stack"
```

---

## Phase β8 — Serialization + wire format

Goal: `IdListOperation` round-trips through JSON with new tags; schema_version bumps additively.

### Task 8.1: Encode/decode `IdListInsertOp`

**Files:**
- Modify: `libs/collabtext/src/crdt/Serialization.h`
- Modify: `libs/collabtext/src/crdt/Serialization.cpp`
- Create: `libs/collabtext/tests/tst_idlist_serialization.cpp`

- [ ] **Step 1: Failing test**

```cpp
#include <QTest>
#include "crdt/IdList.h"
#include "crdt/Serialization.h"

using namespace CollabText::Crdt;

class TestIdListSerialization : public QObject {
    Q_OBJECT
private slots:
    void insert_op_round_trip() {
        IdList list(1);
        auto op = list.insert_after(Anchor::min(), 0xDEADBEEF);
        std::string encoded = encode_idlist_operation(std::get<IdListInsertOp>(op));
        auto decoded = decode_idlist_operation(encoded);
        QVERIFY(decoded.has_value());
        QVERIFY(std::holds_alternative<IdListInsertOp>(*decoded));
        const auto& d = std::get<IdListInsertOp>(*decoded);
        QCOMPARE(d.id, quint64(0xDEADBEEF));
    }
};

QTEST_GUILESS_MAIN(TestIdListSerialization)
#include "tst_idlist_serialization.moc"
```

- [ ] **Step 2: Implement `encode_idlist_operation` / `decode_idlist_operation`**

Tags: `"il-i"` (IdList insert), `"il-r"` (remove), `"il-u"` (undo). Mirror the `encode_edit` / `decode_edit` patterns in `Serialization.cpp`. Decoder uses the same `Parser` struct already defined there.

Key fields per op:
- `il-i`: `ts`, `v`, `id` (uint64 → JSON number), `loc` (locator digits array).
- `il-r`: `ts`, `v`, `to` (target_origin lamport).
- `il-u`: `ts`, `v`, `c` (counts array).

- [ ] **Step 3: Wire CMake; build; pass; commit**

```bash
git commit -am "feat(crdt): JSON serialization for IdList operations"
```

### Task 8.2: Decode error cases

- [ ] **Step 1: Failing tests**

```cpp
void malformed_json_returns_nullopt() {
    QVERIFY(!decode_idlist_operation("not json").has_value());
    QVERIFY(!decode_idlist_operation(R"({"t":"il-i"})").has_value());  // missing fields
    QVERIFY(!decode_idlist_operation(R"({"t":"unknown"})").has_value());
}
```

- [ ] **Step 2: Verify decoder returns `std::nullopt` on malformed input; pass; commit**

```bash
git commit -am "test(idlist): serialization error handling"
```

### Task 8.3: Schema version bump 2 → 3

**Files:**
- Modify: `libs/collabtext/src/crdt/SidecarManifest.{h,cpp}`

- [ ] **Step 1: Find the current `SCHEMA_VERSION` constant** in `SidecarManifest.cpp` (recently bumped 1 → 2 in commit `6c16a52`). Bump to 3.

- [ ] **Step 2: Add a constant or comment marking what schema 3 introduces**: "schema_version 3: IdList operations (`il-i`, `il-r`, `il-u`) may appear in op streams."

- [ ] **Step 3: Run the existing sidecar manifest tests** — `ctest --test-dir build-dev -R tst_sidecar_manifest`. Adjust expected version values if any test asserts on the current version.

- [ ] **Step 4: Commit**

```bash
git commit -am "feat(crdt): bump sidecar schema_version 2 → 3 (IdList ops)"
```

### Task 8.4: Round-trip every op variant through encode/decode

- [ ] **Step 1: Test for remove + undo variants**

```cpp
void remove_op_round_trip() {
    IdList list(1);
    list.insert_after(Anchor::min(), 0xAA);
    auto op = list.remove_at(Anchor(1, 1, Bias::Left));
    auto encoded = encode_idlist_operation(std::get<IdListRemoveOp>(op));
    auto decoded = decode_idlist_operation(encoded);
    QVERIFY(decoded.has_value());
    QVERIFY(std::holds_alternative<IdListRemoveOp>(*decoded));
}

void undo_op_round_trip() {
    IdList list(1);
    list.insert_after(Anchor::min(), 0xAA);
    auto undo = list.undo();
    QVERIFY(undo.has_value());
    auto encoded = encode_idlist_operation(std::get<IdListUndoOpVariant>(*undo));
    auto decoded = decode_idlist_operation(encoded);
    QVERIFY(decoded.has_value());
    QVERIFY(std::holds_alternative<IdListUndoOpVariant>(*decoded));
}
```

- [ ] **Step 2: Pass; commit**

```bash
git commit -am "test(idlist): round-trip every op variant"
```

### Task 8.5: Forward-compat — buffer-only op streams still parse

- [ ] **Step 1: Test that existing `Operation` decode continues to work after the schema bump**

```cpp
void buffer_op_decode_still_works() {
    Buffer buf(1);
    auto op = buf.apply_local_edit({{0,0}}, {"hello"});
    std::string s = encode_operation(op);
    auto decoded = decode_operation(s);
    QVERIFY(decoded.has_value());
}
```

- [ ] **Step 2: Pass; commit**

```bash
git commit -am "test(serialization): Buffer op decode unchanged by schema bump"
```

---

## Phase β9 — Convergence + fuzz harness

Goal: invariant-checking randomized fuzz at parity with `tst_fuzz.cpp`.

### Task 9.1: Invariant checker

- [ ] **Step 1: Write `check_invariants(const IdList&, const char* context)` at top of `tst_idlist_fuzz.cpp`** mirroring the structure of `tst_fuzz.cpp`'s checker:

- INV-1: `size()` == count of visible entries via `entries()`.
- INV-2: visible_count + deleted_count == total entries.
- INV-3: entries sorted by `(locator, origin)` strictly.
- INV-4: no two visible entries share `(replica_id, char_value)` (origin uniqueness).
- INV-5: every visible entry has empty `deletions` OR has a deletion that's been undone.
- INV-6: `version()` observes every entry's origin.

- [ ] **Step 2: Sanity-check the checker on a hand-built IdList; commit**

```bash
git commit -am "test(idlist): fuzz harness invariant checker"
```

### Task 9.2: Two-replica randomized fuzz

- [ ] **Step 1: Driver loop**

```cpp
void two_replica_fuzz_50_ops() {
    std::mt19937 rng(1234);
    IdList alice(1), bob(2);
    std::vector<IdListOperation> alice_outbox, bob_outbox;

    auto choose_anchor = [&](const IdList& list) -> Anchor {
        if (list.size() == 0) return Anchor::min();
        std::uniform_int_distribution<uint32_t> idx(0, list.size() - 1);
        return list.anchor_at_index(idx(rng), Bias::Left);
    };

    for (int step = 0; step < 50; ++step) {
        IdList& side = (step % 2 == 0) ? alice : bob;
        std::vector<IdListOperation>& outbox = (step % 2 == 0) ? alice_outbox : bob_outbox;

        std::uniform_int_distribution<int> action(0, 9);
        int a = action(rng);
        if (a < 6 || side.size() == 0) {
            // insert
            uint64_t id = step + 1;
            outbox.push_back(side.insert_after(choose_anchor(side), id));
        } else {
            // remove
            outbox.push_back(side.remove_at(choose_anchor(side)));
        }
        check_invariants(side, "post-local-op");
    }

    // Flush all ops to both replicas in random order.
    auto all = alice_outbox;
    all.insert(all.end(), bob_outbox.begin(), bob_outbox.end());
    std::shuffle(all.begin(), all.end(), rng);
    alice.apply_ops(all);
    bob.apply_ops(all);
    check_invariants(alice, "post-converge-alice");
    check_invariants(bob, "post-converge-bob");

    QCOMPARE(alice.ids(), bob.ids());
}
```

- [ ] **Step 2: Run, expect pass; commit**

```bash
git commit -am "test(idlist): two-replica randomized fuzz"
```

### Task 9.3: Three-replica fuzz with adversarial delivery

- [ ] **Step 1: Extend to three replicas, randomly drop and reorder messages, every 10 steps deliver a random subset**

Mirror `tst_fuzz.cpp`'s delivery scheduler exactly — the structure is general.

- [ ] **Step 2: Run with 5 different seeds (1234, 5678, 99, 314, 2718)**; commit

```bash
git commit -am "test(idlist): three-replica fuzz with adversarial delivery"
```

### Task 9.4: Fuzz with undo

- [ ] **Step 1: Add an `undo()` action to the action menu (probability ~10%)**

```cpp
// in the action switch:
} else if (a == 9) {
    auto undo_op = side.undo();
    if (undo_op) outbox.push_back(*undo_op);
}
```

- [ ] **Step 2: Run, verify convergence still holds; commit**

```bash
git commit -am "test(idlist): fuzz with undo actions interleaved"
```

---

## Phase β10 — Documentation and public surface

### Task 10.1: Spec doc

**Files:**
- Create: `docs/CRDT_IDLIST_SPEC.md`

- [ ] **Step 1: Write the spec, mirroring `docs/CRDT_ENGINE_SPEC.md`'s structure**: conceptual overview, entry/locator model, operation types, concurrent semantics (insert-after-deleted, concurrent removes, tiebreaks), undo model, GC model, wire format. Diagram the structural similarity to `Buffer`.

- [ ] **Step 2: Commit**

```bash
git commit -am "docs: CRDT_IDLIST_SPEC.md"
```

### Task 10.2: Architecture note

**Files:**
- Modify: `docs/ARCHITECTURE.md`

- [ ] **Step 1: Add a paragraph noting the engine now provides two primitives** — `Buffer` (text) and `IdList` (opaque list) — and that they share the `SumTree`/`Locator`/`Anchor`/`UndoMap` machinery. One paragraph, no rewrite of the existing doc.

- [ ] **Step 2: Commit**

```bash
git commit -am "docs(architecture): note IdList as the second engine primitive"
```

### Task 10.3: README update

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Update the "What it is" / "What it isn't" sections** to mention `IdList` precisely:
  - Add to "What it is": "An ordered-list CRDT over opaque `uint64` elements (`IdList`), for applications that need a structural list separate from text content."
  - Update "What it isn't": keep the disclaimers — no maps, counters, JSON, framework. Make the `IdList` addition feel like a measured second primitive, not a category change.

- [ ] **Step 2: Commit**

```bash
git commit -am "docs(readme): mention IdList as the second primitive"
```

---

## Self-review notes

The following items are deliberate omissions, not gaps:

- **No `moveAfter`** — explicitly deferred per the response spec. Express moves as remove+insert at the application layer.
- **No per-element values** — explicitly out of scope. `IdListEntry::id` is opaque payload, not a tagged value.
- **No public `CollabDocument` integration** — Markoff composes structures themselves on `IdList` + `Buffer`.
- **No origin index** — `Buffer` has one for performance; `IdList` likely doesn't need it given expected list sizes (blocks per document, not chars per document). Add later if profiling demands.
- **No explicit edits_since for IdList** — application can diff `ids()` snapshots cheaply because elements are atomic. If a `changes_since(version)` proves needed, add as a follow-on, not in v1.

If you discover a real gap during execution, append a task to the relevant phase rather than improvising mid-phase.
