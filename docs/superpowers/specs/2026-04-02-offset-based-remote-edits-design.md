# Offset-Based Remote Edit Application — Design Spec

**Date:** 2026-04-02
**Scope:** Rewrite apply_remote_edit to use versioned full-text offset
ranges instead of per-character timestamp lookup. Remove
deleted_timestamps from EditOperation.

---

## 1. Problem

`apply_remote_edit()` finds deleted characters by iterating
`deleted_timestamps` — one Lamport per character. Each lookup calls
`versioned_seek_by_timestamp()` which is O(n). For k deletions, total
cost is O(nk). Wire format transmits k timestamps (8 bytes each).

## 2. Solution

The sender converts visible-text ranges to versioned full-text ranges
(including deleted text) and broadcasts those. The receiver resolves
the ranges against its own fragment tree, filtered by the sender's
version. Deletions are applied to fragments within the resolved range
that were visible to the sender (`was_visible_at`).

**Wire format change:**
- Remove: `deleted_timestamps: Vec<Lamport>` (k * 8 bytes)
- Keep: `ranges: Vec<(u32, u32)>` — now in full-text offset space
- Keep: `deletion_id: Lamport` — which deletion timestamp to push

## 3. Visible-to-Full Offset Conversion (sender side)

In `apply_local_edit`, convert the input visible-text ranges to
full-text ranges before storing in EditOperation:

```cpp
static uint32_t visible_to_full(
    const std::vector<Fragment>& frags, uint32_t vis_offset)
{
    uint32_t vis_acc = 0, full_acc = 0;
    for (auto& f : frags) {
        uint32_t bytes = static_cast<uint32_t>(f.content.size());
        if (f.visible && vis_acc + bytes > vis_offset) {
            return full_acc + (vis_offset - vis_acc);
        }
        if (f.visible) vis_acc += bytes;
        full_acc += bytes;
    }
    return full_acc;
}
```

Called once per range boundary (2 calls per range). O(n) per call but
apply_local_edit is already O(n).

## 4. Versioned Delete Range (receiver side)

New method replaces the deleted_timestamps loop:

```cpp
void Buffer::versioned_delete_range(
    std::vector<Fragment>& frags,
    uint32_t range_start, uint32_t range_end,
    const Global& version, Lamport deletion_id)
```

Algorithm:
1. Walk fragments, accumulating full-text offset for version-observed
   fragments only (skip those not in sender's version)
2. Split at range_start if mid-fragment
3. Walk [start, end), deleting fragments that were visible to sender
4. Split at range_end if mid-fragment

`was_visible_at` check:
```cpp
bool was_visible_at(const Fragment& f, const Global& version) const {
    if (!version.observed(f.origin)) return false;
    if (m_undo_map.was_undone(f.origin, version)) return false;
    for (auto& del : f.deletions) {
        if (version.observed(del) && !m_undo_map.was_undone(del, version))
            return false;
    }
    return true;
}
```

## 5. What Changes

| Component | Before | After |
|-----------|--------|-------|
| EditOperation.ranges | Visible-text offsets | Versioned full-text offsets |
| EditOperation.deleted_timestamps | Per-char Lamport vec | Removed |
| apply_local_edit | Stores visible ranges + builds deleted_timestamps | Converts to full ranges, no deleted_timestamps |
| apply_remote_edit | Loops deleted_timestamps, finds by identity | Loops ranges, resolves by versioned offset |
| mark_deleted lambda | Pushes to deleted_timestamps | No longer builds deleted_timestamps |
| versioned_seek_by_timestamp | Used for deletion lookup | Removed (dead code) |

## 6. What Does NOT Change

- Insertions: still applied by locator (inserted_fragments)
- Split relocations: still applied by timestamp
- UndoMap, Rope, Anchors: unchanged
- Local edit cursor processing: unchanged (still works in visible space)
- UndoEntry: unchanged
- Undo/redo: unchanged
