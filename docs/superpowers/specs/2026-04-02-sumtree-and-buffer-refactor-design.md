# SumTree & Buffer Refactor Design

## Goal

Replace `std::vector<Fragment>` in Buffer with a proper SumTree (B+ tree with
summary aggregation), add Rope for text storage, and add Insertion Index for
future anchor support. This brings the implementation in line with sections
5-7 and 12.5 of `CRDT_ENGINE_SPEC.md`.

## Architecture

### SumTree<Item, B>

Generic header-only B+ tree template using C++20 concepts.

**Concepts:**
- `SummaryType`: monoid with `zero()` and `add_summary()`
- `Summarizable`: item that produces a `Summary` via `summary()`
- `DimensionOf<D, S>`: extracts a comparable measurement from a Summary

**Node representation:**
- `std::variant<Leaf, Internal>` discriminated by `height` (0 = leaf)
- Leaf: `std::array<Item, 2*B>` items + summaries, `uint16_t count`
- Internal: `std::array<std::shared_ptr<Node>, 2*B>` children + summaries, count
- B is a template parameter, default 6, test with 2

**Structural sharing:**
- Nodes owned via `std::shared_ptr<const Node>` for O(1) subtree cloning
- `slice()` and `suffix()` share unchanged subtrees with the original tree
- COW: mutating operations clone shared nodes along the modified path

**Key operations:**
- `push_item(Item)` — O(log n) rightmost insertion with B-tree splitting
- `push_tree(SumTree)` — O(log n) concatenation via right-spine merging
- `summary()` — O(1) root summary access
- `for_each(fn)` — O(n) in-order iteration
- `items()` — flatten to vector

**Cursor<D>:**
- Parameterized by dimension type D
- Stack-based: `vector<StackEntry{node, index, position}>`
- `seek(target, bias)` — O(log n) tree descent
- `slice(target)` — O(log² n) extraction with subtree sharing
- `suffix()` — O(log² n) extraction of remaining items
- `next()` / `prev()` — O(log n) worst, O(1) amortized
- `item()` — current item pointer (nullptr if at end)
- `position()` — cumulative dimension at current item's start

### Fragment Summary

```
FragmentSummary {
    visible_bytes: uint32_t    — total visible bytes in subtree
    deleted_bytes: uint32_t    — total deleted bytes in subtree
    max_locator: Locator       — maximum fragment Locator in subtree
    max_version: Global        — latest timestamp in subtree
}
```

**Dimensions:**
- `VisibleOffset(uint32_t)` — seeks by visible byte count
- `FullOffset(uint32_t)` — seeks by visible + deleted byte count
- `LocatorDim(Locator)` — seeks by fragment Locator

### Rope

`SumTree<Chunk>` where:
```
Chunk {
    text: std::string      — up to 128 bytes (prod) / 16 bytes (test)
}

ChunkSummary {
    bytes: uint32_t        — byte count
    // Future: lines, tabs for line/column seeking
}
```

Buffer maintains two ropes: `visible_text` and `deleted_text`.

### Insertion Index

`SumTree<InsertionFragment>` where:
```
InsertionFragment {
    timestamp: Lamport          — insertion operation timestamp
    split_offset: uint32_t      — byte offset within insertion
    fragment_id: Locator        — which fragment in the fragment tree
}
```

Keyed by `(timestamp, split_offset)`. Maps anchors to fragment Locators.

## Buffer Refactor

The Buffer replaces:
- `std::vector<Fragment> m_fragments` → `SumTree<Fragment> m_fragments`
- Direct string storage in Fragment → Rope-backed text storage
- Linear scan `resolve_visible_offset()` → cursor `seek<VisibleOffset>()`
- Linear scan insertion/deletion → cursor `slice()` + `push_item()` + `suffix()`
- `normalize_fragments()` removed (SumTree ordering is maintained by Locator)

The edit pattern becomes:
```
new_tree = SumTree()
cursor = old_tree.cursor<VisibleOffset>()
new_tree.push_tree(cursor.slice(edit_start))  // O(log n) prefix
// push modified/new fragments
new_tree.push_tree(cursor.suffix())            // O(log n) suffix
m_fragments = new_tree
```

## Implementation Order

1. SumTree.h (generic, header-only)
2. tst_sumtree.cpp (standalone tests with simple test items)
3. Fragment.h updates (add Summary, dimensions)
4. Rope.h/cpp (Chunk, ChunkSummary, Rope wrapper)
5. InsertionIndex types
6. Buffer.h/cpp refactor (swap in SumTree, keep same public API)
7. Verify all existing tests pass

## Testing

- SumTree standalone: push_item, push_tree, seek, slice, suffix, for_each
- Fragment tree: seek by visible offset, by locator, slice correctness
- Rope: text storage and retrieval
- All existing CRDT tests (tst_buffer, tst_convergence) must continue to pass
