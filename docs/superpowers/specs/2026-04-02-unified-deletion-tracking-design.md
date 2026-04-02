# Unified Deletion Tracking — Design Spec

**Date:** 2026-04-02
**Scope:** Replace `delete_count` with `deletions: Vec<Lamport>` on Fragment.
Unify insertion-undo and deletion-undo into a single UndoMap parity mechanism.
Simplify UndoOperation wire format.

---

## 1. Problem

The engine tracks undo state via two independent mechanisms:

- **Insertion undo:** UndoMap with `(edit_id, undo_id)` SumTree entries.
  Parity-based: even count = visible, odd = undone.
- **Deletion undo:** `delete_count` integer on Fragment. Undo decrements,
  redo increments. `undelete_keys` and `is_redo` on UndoOperation.

This split complicates the undo/redo code paths (Buffer::undo, redo,
apply_remote_undo all have separate insertion-undo and deletion-undo
branches) and prevents version-aware deletion queries (`was_undone` for
deletions is impossible with a counter).

## 2. Solution

Replace `uint32_t delete_count` with `std::vector<Lamport> deletions`.
Each deletion operation pushes its timestamp into the vector. Visibility
uses a single check:

```cpp
bool compute_visible(const UndoMap& undo_map) const {
    // Insertion must not be undone
    if (undo_map.is_undone(origin)) return false;
    // Every deletion must be undone (or no deletions exist)
    for (auto& del : deletions) {
        if (!undo_map.is_undone(del)) return false;
    }
    return true;
}
```

Undo of a deletion = mark the deletion's timestamp as undone in the
UndoMap. Same parity mechanism as insertion undo. No special cases.

## 3. Fragment Changes

### Before

```cpp
struct Fragment {
    Lamport origin;
    Locator locator;
    std::string content;
    uint32_t length;
    uint32_t delete_count;   // <-- removed
    bool visible;
};
```

### After

```cpp
struct Fragment {
    Lamport origin;
    Locator locator;
    std::string content;
    uint32_t length;
    std::vector<Lamport> deletions;  // <-- replaces delete_count
    bool visible;
};
```

**Splitting:** When a fragment is split, both halves inherit the full
`deletions` vector (copied). This matches the current behavior where
both halves inherit `delete_count`.

**Memory:** The vector is typically empty (most fragments are never
deleted) or contains 1 entry. Concurrent multi-replica deletion of the
same character would produce 2+ entries but this is rare. The 24-byte
overhead of an empty `std::vector` vs 4-byte `uint32_t` is negligible
relative to the `std::string content` already stored per fragment.

**Summary:** `FragmentSummary` is unchanged. It uses the `visible` flag
for byte accounting, which is set by `compute_visible()`.

## 4. Deletion Recording

### Local edits (apply_local_edit)

`mark_deleted` currently increments `delete_count`. Change to push
a deletion timestamp:

```cpp
// Pre-tick a deletion timestamp at the start of the edit
Lamport deletion_ts = m_clock.tick();

auto mark_deleted = [&](Fragment& f) {
    f.deletions.push_back(deletion_ts);
    f.visible = false;
    for (uint32_t c = 0; c < f.length; ++c) {
        Lamport ts = f.timestamp_at(c);
        op.deleted_timestamps.push_back(ts);
    }
};
```

The pre-tick ensures `deletion_ts` is available before any range
processing. The operation timestamp is set after all ranges are
processed:

```cpp
if (!op.inserted_fragments.empty()) {
    op.timestamp = Lamport(m_replica_id, m_clock.value - 1);
} else {
    op.timestamp = deletion_ts;
}
```

For insert-only edits, `deletion_ts` is "wasted" (one extra clock tick)
but does not affect correctness.

### Remote edits (apply_remote_edit)

When deleting a character, push `op.timestamp` to `fragment.deletions`:

```cpp
// Instead of: frags[fi].delete_count++;
frags[fi].deletions.push_back(op.timestamp);
```

### Normalization (normalize_fragments)

When atomizing multi-character fragments at shared locators, each
single-character fragment inherits the parent's `deletions` vector.

## 5. UndoEntry Changes

### Before

```cpp
struct UndoEntry {
    std::vector<UndoMapKey> inserted_keys;
    std::vector<UndoMapKey> deleted_keys;
};
```

### After

```cpp
struct UndoEntry {
    std::vector<UndoMapKey> inserted_keys;  // per-character insertion timestamps
    Lamport deletion_id;                     // edit timestamp (for undoing deletions)
    bool had_deletions = false;
};
```

The `deleted_keys` vector (which stored per-character timestamps of
deleted characters) is replaced by a single `deletion_id` (the edit's
timestamp). All deletions from one edit share the same deletion
timestamp, so only one UndoMap entry is needed to undo them all.

## 6. Undo / Redo

### undo()

```cpp
std::optional<Operation> Buffer::undo() {
    // ... stack management ...
    UndoOperation op;
    op.version = m_version;
    op.timestamp = m_clock.tick();

    // Undo inserted characters (same as current)
    for (auto& key : entry.inserted_keys) {
        Lamport edit_id(key.replica_id, key.lamport_value);
        uint32_t current = m_undo_map.undo_count(edit_id);
        m_undo_map.insert(UndoMapEntry{{edit_id, op.timestamp}, current + 1});
        op.counts.push_back({edit_id, current + 1});
    }

    // Undo deletions (NEW — via UndoMap, not delete_count)
    if (entry.had_deletions) {
        uint32_t current = m_undo_map.undo_count(entry.deletion_id);
        m_undo_map.insert(
            UndoMapEntry{{entry.deletion_id, op.timestamp}, current + 1});
        op.counts.push_back({entry.deletion_id, current + 1});
    }

    // Recompute visibility
    auto frags = get_fragments();
    set_fragments(std::move(frags));

    m_version.observe(op.timestamp);
    return op;
}
```

`redo()` is identical in structure — `undo_count + 1` toggles parity
back. No `is_redo` flag needed.

### apply_remote_undo()

```cpp
bool Buffer::apply_remote_undo(const UndoOperation& op) {
    // ... causal checks ...
    for (auto& [edit_id, count] : op.counts) {
        m_undo_map.insert(UndoMapEntry{{edit_id, op.timestamp}, count});
    }

    m_clock.observe(op.timestamp);
    m_version.observe(op.timestamp);
    m_version.join(op.version);

    // Recompute visibility
    auto frags = get_fragments();
    set_fragments(std::move(frags));
    return true;
}
```

No more iterating fragments to adjust `delete_count`. No more
`undelete_keys` loop. Visibility is recomputed from the UndoMap by
`set_fragments()` calling `compute_visible()` on each fragment.

## 7. UndoOperation Simplification

### Before

```cpp
struct UndoOperation {
    Lamport timestamp;
    Global version;
    std::vector<std::pair<Lamport, uint32_t>> counts;
    std::vector<UndoMapKey> undelete_keys;  // <-- removed
    bool is_redo = false;                   // <-- removed
};
```

### After

```cpp
struct UndoOperation {
    Lamport timestamp;
    Global version;
    std::vector<std::pair<Lamport, uint32_t>> counts;
};
```

`undelete_keys` and `is_redo` are removed. All undo/redo state flows
through `counts`. The parity model handles directionality automatically.

## 8. What Does NOT Change

- **SumTree, Rope, OperationQueue** — untouched.
- **Anchor system** — uses `visible` flag, not `delete_count` directly.
- **apply_local_edit range processing** — cursor logic, split_frag,
  consume_unchanged, consume_deleted all work on the `visible` flag
  and content, not on `delete_count` directly.
- **EditOperation wire format** — `deleted_timestamps` stays (needed
  for timestamp-based remote delete application). `inserted_fragments`,
  `split_relocations` unchanged.
- **Fragment::content** — retained (future Phase 3 concern).
- **UndoMap API** — `insert()`, `undo_count()`, `is_undone()`,
  `was_undone()` all unchanged. The UndoMap just gets more entries
  (deletion timestamps in addition to character timestamps).

## 9. Migration Sites

18 `delete_count` access sites across 2 files:

| Site | Current | New |
|------|---------|-----|
| Fragment.h: field declaration | `uint32_t delete_count = 0` | `std::vector<Lamport> deletions` |
| Fragment.h: `deleted()` | `delete_count > 0` | `!deletions.empty()` |
| Fragment.h: `compute_visible()` | `if (delete_count > 0) return false` | loop over deletions + UndoMap |
| Fragment.h: `was_visible()` | `delete_count == 0` | `deletions.empty()` |
| Buffer.cpp: `split_fragment_at()` (x2) | `second.delete_count = orig.delete_count` | `second.deletions = orig.deletions` |
| Buffer.cpp: `normalize_fragments()` | `single.delete_count = f.delete_count` | `single.deletions = f.deletions` |
| Buffer.cpp: `mark_deleted` in apply_local_edit | `f.delete_count++` | `f.deletions.push_back(deletion_ts)` |
| Buffer.cpp: `split_frag` lambda (x2) | `first/second.delete_count = f.delete_count` | `first/second.deletions = f.deletions` |
| Buffer.cpp: `apply_remote_edit` (x2) | `frags[fi].delete_count++` | `frags[fi].deletions.push_back(op.timestamp)` |
| Buffer.cpp: `apply_remote_undo` | `delete_count++` / `delete_count--` | removed (UndoMap handles it) |
| Buffer.cpp: `undo()` | `delete_count--` | removed (UndoMap handles it) |
| Buffer.cpp: `redo()` | `delete_count++` | removed (UndoMap handles it) |

## 10. Testing

All existing 200+ tests must pass. New tests:

| Test | Location | Description |
|------|----------|-------------|
| `fragment_with_multiple_deletions` | tst_buffer | Two replicas delete the same character. Undo one deletion — character still invisible. Undo both — visible. |
| `deletion_undo_via_undomap` | tst_buffer | Delete text, undo. Verify text reappears. Redo. Verify text disappears. Same as existing tests but validates new code path. |
| `concurrent_delete_and_undo` | tst_buffer | A deletes char, B undoes A's insertion (hiding it). After merge, char is invisible for two reasons. Redo B's undo — char still invisible (A's delete). Undo A's delete — char visible. |
| `deletion_parity_unit` | tst_undomap | Insert deletion entries, verify undo_count parity works for deletion timestamps. |
| `simplified_undo_operation` | tst_buffer | Verify UndoOperation no longer has undelete_keys or is_redo. Undo/redo round-trip via remote path. |

## 11. Risks

- **Correctness of parity for deletions.** The parity model is proven
  for insertion undo (tested extensively). Extending it to deletions is
  the same mechanism — the risk is in the wiring, not the model.

- **Fragment splitting with deletions vector.** Both halves of a split
  must inherit the same deletions. This is a vector copy, which is
  correct but slightly more expensive than copying a uint32.

- **Pre-ticked deletion timestamp.** Using a pre-ticked Lamport for the
  deletion timestamp means the clock advances one extra tick for
  insert-only edits. This is harmless (Lamport clocks are not required
  to be contiguous) but is a behavioral change from the current code.
