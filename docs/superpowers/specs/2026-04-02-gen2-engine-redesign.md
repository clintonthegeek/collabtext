# CollabText Gen 2 Engine Redesign — Specification

**Date:** 2026-04-02
**Status:** Complete. All three phases implemented and verified.
**Prereqs:** All 5 SumTree optimizations complete (Opts 1-5)

---

## 1. Motivation

The Gen 1 engine implements all 5 SumTree optimizations and passes 200+
tests across 16 adversarial fuzz scenarios. It is correct and well-tested.
However, it diverges from the CRDT Engine Spec in three coupled ways:

1. **Text duplication.** Fragment::content stores text inline (owned
   strings). The same text is also stored in the Ropes
   (`m_visible_text`, `m_deleted_text`). Every character exists in memory
   twice. The spec (§4.1, §6.2) envisions fragments as metadata-only
   records referencing positions in the Ropes.

2. **Timestamp-based remote edits.** The current `apply_remote_edit()`
   transmits one Lamport timestamp per deleted character
   (`deleted_timestamps`) and finds characters by identity via
   `versioned_seek_by_timestamp()`. The spec (§10) describes offset-based
   remote edits: the sender transmits byte ranges, and the receiver
   resolves them using `VersionedFullOffset` cursor seeking — O(log n)
   per range instead of O(n) per character.

3. **Split deletion tracking.** Deletions use `delete_count` on
   fragments (incremented per delete, decremented per undo-of-delete).
   Undo of insertions uses the UndoMap. The spec (§7.4) unifies both:
   every deletion is a timestamped entry in the UndoMap, and
   `is_visible()` checks that the insertion is not undone AND all
   deletions are undone (or no deletions exist).

These three changes are deeply coupled:

```
  ┌─ Fragment::content removal ─────────────┐
  │  Fragments become metadata-only.         │
  │  Text lives only in Ropes.               │
  │  Requires RopeBuilder (§6.3) to track    │
  │  text across rope rebuilds.              │
  └──────────────────────────────────────────┘
                    │
                    ▼ enables
  ┌─ Offset-based remote edits ─────────────┐
  │  apply_remote_edit uses byte ranges      │
  │  + VersionedFullOffset cursor seeking.   │
  │  Requires was_visible(frag, version,     │
  │  undo_map) to filter offset resolution.  │
  └──────────────────────────────────────────┘
                    │
                    ▼ requires
  ┌─ Unified deletion tracking ─────────────┐
  │  Fragment.deletions: Vec<Lamport>        │
  │  replaces delete_count.                  │
  │  Undo of deletion = mark deletion as     │
  │  undone in UndoMap (not counter tweak).  │
  │  was_visible needs this for correctness. │
  └──────────────────────────────────────────┘
```

---

## 2. Current State (Gen 1)

### Fragment

```cpp
struct Fragment {
    Lamport origin;          // first character's timestamp
    Locator locator;         // fractional position
    std::string content;     // UTF-8 text (OWNED — duplicated in Ropes)
    uint32_t length;         // character count
    uint32_t delete_count;   // number of active deletes
    bool visible;            // cached visibility
};
```

**34 access sites** read `content` across 6 source files. Categories:
- **Byte length queries** (12 sites): `content.size()` for offset math
- **Text splitting** (8 sites): `content.substr()` during fragment splits
- **UTF-8 navigation** (6 sites): `char_to_byte_offset(content, ...)` etc.
- **Rope rebuild** (2 sites): `push_str(content)` in `set_fragments()`
- **Fragment construction** (4 sites): creating fragments from text
- **Test assertions** (2 sites): reading content for invariant checks

### Remote Edit Application

```cpp
// Current: timestamp-based (O(n) per deletion)
for (auto &ts : op.deleted_timestamps) {
    auto result = versioned_seek_by_timestamp(frags, ts);
    // split + increment delete_count
}
```

Wire format: `EditOperation.deleted_timestamps: Vec<Lamport>` — one
timestamp per deleted character. For a deletion of 1000 characters, this
transmits 1000 Lamport values (8 bytes each = 8 KB).

### Undo Model

Two mechanisms:
- **Insertion undo:** UndoMap with `(edit_id, undo_id)` SumTree entries.
  Parity-based visibility. `was_undone(edit_id, version)` supported.
- **Deletion undo:** `delete_count` integer on fragments. Undo decrements,
  redo increments. No versioned query support.

---

## 3. Target State (Gen 2)

### 3.1 Fragment (metadata-only)

```cpp
struct Fragment {
    Lamport origin;
    Locator locator;
    uint32_t byte_length;              // replaces content.size()
    uint32_t char_length;              // replaces length
    std::vector<Lamport> deletions;    // replaces delete_count
    bool visible;
};
```

No `content` field. Text lives exclusively in the Ropes. Fragment byte
length is stored explicitly. The `deletions` vector records which
operations deleted this fragment (needed for `was_visible`).

### 3.2 Visibility

```cpp
bool is_visible(const Fragment& f, const UndoMap& undos) {
    if (undos.is_undone(f.origin)) return false;
    for (auto& del : f.deletions) {
        if (!undos.is_undone(del)) return false;
    }
    return true;
}

bool was_visible(const Fragment& f, const Global& version,
                 const UndoMap& undos) {
    if (!version.observed(f.origin)) return false;
    if (undos.was_undone(f.origin, version)) return false;
    for (auto& del : f.deletions) {
        if (version.observed(del) && !undos.was_undone(del, version))
            return false;
    }
    return true;
}
```

`is_visible`: insertion is not undone AND every deletion is undone.
`was_visible`: same, but filtered by a specific version.

### 3.3 Remote Edit Application (offset-based)

```cpp
// Target: offset-based (O(log n) per range via VersionedFullOffset)
for (auto& [start, end] : op.ranges) {
    // Seek to start in sender's offset space
    auto prefix = cursor.slice(VersionedFullOffset{start, op.version});

    // Walk fragments in range, applying deletions
    while (cursor.position() < end) {
        Fragment& f = *cursor.item();
        if (was_visible(f, op.version, m_undo_map)) {
            f.deletions.push_back(op.timestamp);
        }
        cursor.next();
    }
}
```

Wire format: `EditOperation.ranges: Vec<(u32, u32)>` — byte offset
pairs, already transmitted. `deleted_timestamps` is removed. For a
deletion of 1000 characters, this transmits one range pair (8 bytes).

### 3.4 Undo (unified)

```cpp
// Undo an edit that inserted text:
//   Record in UndoMap → fragments become invisible
// Undo an edit that deleted text:
//   Record in UndoMap → the deletion becomes "undone" →
//   fragments with that deletion in their deletions list
//   regain visibility

UndoOperation {
    timestamp: Lamport,
    version: Global,
    counts: Vec<(Lamport, u32)>,   // (edit_id, new_undo_count)
    // No more undelete_keys or is_redo
}
```

### 3.5 RopeBuilder

When fragments change (edit, undo, remote edit), the Ropes must be
updated. Currently `set_fragments()` rebuilds both Ropes from
`Fragment::content` — O(n) string copies. Without content, we need the
RopeBuilder (spec §6.3):

```
RopeBuilder walks old ropes + new fragment list in parallel:
  For each fragment in new order:
    if fragment is from old tree:
      extract bytes from old_visible or old_deleted rope
      (based on WAS it visible?)
      push to new_visible or new_deleted rope
      (based on IS it visible now?)
    if fragment is newly inserted:
      push new text to new_visible rope
```

This requires tracking **was_visible** for each fragment during the
rebuild — the old visibility before the mutation. The mutation functions
(`apply_local_edit`, `apply_remote_edit`, `undo`, `redo`) must pass this
information to set_fragments().

---

## 4. Migration Strategy

### Phase 1: Unified deletion tracking — COMPLETE

**Merge:** `53db46d` (Merge feat/unified-deletion-tracking)

**Scope:** Replaced `delete_count` with `deletions: Vec<Lamport>`.
Updated `compute_visible`, `is_visible`, UndoOperation wire format.
Removed `undelete_keys` and `is_redo` from UndoOperation.

**Result:** Single parity-based visibility model. One mechanism
(UndoMap) handles both insertion undo and deletion undo.

**Report:** `docs/superpowers/specs/2026-04-02-unified-deletion-tracking-design.md`

### Phase 2: Offset-based remote edit application — COMPLETE

**Merge:** `9434776` (Merge feat/offset-remote-edits)

**Scope:** Rewrote `apply_remote_edit()` to use run-length encoded
deletion runs instead of per-character timestamp lookup. Removed
`deleted_timestamps` from EditOperation. Added `was_visible` check.

**Result:** Wire format transmits byte ranges instead of per-character
timestamps. Deletion of 1000 characters: 8 bytes (one range pair)
instead of 8 KB (1000 Lamport values).

**Report:** `docs/superpowers/specs/2026-04-02-offset-based-remote-edits-design.md`

### Phase 3: Fragment::content removal — COMPLETE

**Commits:** `51b4919`..`2e716f9` (5 implementation commits)

**Scope:** Removed `content` from Fragment. Added `byte_length` field.
Rewrote `set_fragments()` to reconstruct ropes from old ropes via
origin-interval-lookup. Migrated 38 access sites across 4 source
files and 3 test files. Added INV-9 (byte_length consistency).

**Result:** Document text exists in exactly one place (the Ropes).
Fragment is now a metadata-only record.

**Design deviation:** The spec's streaming RopeBuilder (§6.3) was
replaced with an origin-interval-lookup approach because fragments
can be reordered (sorted/normalized) between mutations and
set_fragments(). See report for details.

**Report:** `docs/reports/2026-04-02-gen2-phase3-content-removal.md`

---

## 5. What NOT To Change

- **SumTree implementation** — stable, well-tested, no changes needed.
- **Locator system** — fractional position identifiers are correct.
- **OperationQueue** — deferred replica tracking works.
- **Anchor system** — will need minor updates for content removal but
  the Anchor model itself is sound.
- **Cursor-based apply_local_edit** — the left-to-right cursor
  processing is correct. The RopeBuilder integrates alongside it, it
  doesn't replace it.

---

## 6. Success Criteria — ALL MET

- [x] All existing tests pass after each phase.
- [x] Convergence: 20/20 runs with random seeds after each phase.
- [x] Fuzz: 20/20 runs of all 16 adversarial tests after each phase.
- [x] Memory: After Phase 3, document text exists in exactly one place
  (the Ropes), not two.
- [x] Wire efficiency: After Phase 2, remote deletes transmit byte ranges
  instead of per-character timestamps.
- [x] Model simplicity: After Phase 1, one unified visibility mechanism
  (UndoMap parity) instead of two (UndoMap + delete_count).

---

## 7. Open Questions

1. **Phase 1 ordering.** RESOLVED: Phases executed in spec order
   (1→2→3). Phase 1 first was correct — unified deletions simplified
   Phase 2's `was_visible` implementation.

2. **RopeBuilder complexity.** RESOLVED: The streaming RopeBuilder was
   replaced with an origin-interval-lookup approach. Instead of walking
   old/new ropes in parallel (which breaks when fragments are
   reordered), `set_fragments()` scans the old tree to build a map of
   origin ranges → rope offsets, then looks up each new fragment's text
   by matching its origin into the old tree. This handles splits,
   reordering, and normalization uniformly. See Phase 3 report for
   details.

3. **VersionedFullOffset cursor.** RESOLVED: Kept the standalone walk
   function approach (Option C from the optimization spec). The SumTree
   cursor remains context-free. Phase 2 used run-length encoded
   deletion runs instead of VersionedFullOffset seeking.

4. **Wire format migration.** RESOLVED: Clean break. `deleted_timestamps`
   replaced by `deletion_runs` (run-length encoded). No dual-format
   support — the engine is not yet in production.
