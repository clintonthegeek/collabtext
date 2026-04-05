# Inline Fragment Text Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Store text inline in Fragment, eliminate the O(n*m) origin-interval-lookup in set_fragments(), remove Rope dependency from Buffer.

**Architecture:** Add a `std::string text` field to Fragment so each fragment owns its content. Rewrite `set_fragments()` to a simple O(n) pass (visibility + tree rebuild). Remove `m_visible_text`/`m_deleted_text` Ropes from Buffer. Update all fragment creation, split, and coalesce sites to manage fragment text.

**Tech Stack:** C++20, Qt6 Test framework, CMake

**Spec:** `docs/superpowers/specs/2026-04-05-inline-text-design.md`

**Build/test commands:**
```bash
cmake --build build-dev --target tst_buffer -j$(nproc)
./build-dev/libs/collabtext/tst_buffer -v2
# Full suite:
cmake --build build-dev -j$(nproc)
ctest --test-dir build-dev --output-on-failure
```

---

## File Map

| File | Action | Responsibility |
|------|--------|---------------|
| `libs/collabtext/src/crdt/Fragment.h` | Modify | Add `std::string text` field, 5-arg constructor |
| `libs/collabtext/src/crdt/Buffer.h` | Modify | Remove Rope members, remove `extract_fragment_text`, simplify `set_fragments` and `normalize_fragments` signatures |
| `libs/collabtext/src/crdt/Buffer.cpp` | Modify | Rewrite `set_fragments`, update all call sites, remove rope usage |

---

### Task 1: Add text field to Fragment and update Buffer.h

This task makes the structural changes to the two header files. It adds the
text field, removes Rope dependencies, and updates method signatures. Nothing
compiles yet after this task — the .cpp changes come in subsequent tasks.

**Files:**
- Modify: `libs/collabtext/src/crdt/Fragment.h:113-126`
- Modify: `libs/collabtext/src/crdt/Buffer.h:1-209`

- [ ] **Step 1: Add text field and 5-arg constructor to Fragment**

In `libs/collabtext/src/crdt/Fragment.h`, add `#include <string>` to the
includes, add the `text` field after `visible`, and add a 5-argument
constructor:

```cpp
// After line 6, add:
#include <string>

// The Fragment struct fields become (lines 116-126):
struct Fragment {
    using Summary = FragmentSummary;

    Lamport origin;          ///< Timestamp of the first character
    Locator locator;         ///< Fractional position in the document
    uint32_t byte_length = 0; ///< Byte length of content
    uint32_t length = 0;     ///< Number of characters (may differ from byte_length for multi-byte)
    std::vector<Lamport> deletions;  ///< Lamport timestamps of deletion operations
    bool visible = true;     ///< Cached visibility (set during tree construction)
    std::string text;        ///< Fragment content (owned)

    Fragment() = default;
    Fragment(Lamport orig, Locator loc, uint32_t byte_len, uint32_t char_len)
        : origin(orig), locator(loc), byte_length(byte_len), length(char_len) {}
    Fragment(Lamport orig, Locator loc, uint32_t byte_len, uint32_t char_len,
             std::string txt)
        : origin(orig), locator(loc), byte_length(byte_len), length(char_len),
          text(std::move(txt)) {}

    // ... rest unchanged ...
```

- [ ] **Step 2: Update Buffer.h — remove Rope, simplify signatures**

In `libs/collabtext/src/crdt/Buffer.h`:

**Remove the Rope include** (line 8):
```cpp
// DELETE: #include "crdt/Rope.h"
```

**Remove `extract_fragment_text` declaration** (lines 135-137):
```cpp
// DELETE the entire extract_fragment_text method declaration
```

**Replace the two `set_fragments` declarations** (around lines 111-117)
with a single overload:
```cpp
    /// Rebuild the fragment tree from a vector of fragments.
    /// Fragments must have their `text` field populated.
    void set_fragments(std::vector<Fragment>&& frags);
```

**Remove `new_texts` parameter from `normalize_fragments`** (line 159-160):
```cpp
    /// Atomize multi-character fragments at shared locators.
    void normalize_fragments(std::vector<Fragment>& frags) const;
```

**Remove the Rope member variables** (lines 176-177):
```cpp
    // DELETE: Rope m_visible_text;
    // DELETE: Rope m_deleted_text;
```

- [ ] **Step 3: Commit header changes**

```bash
git add libs/collabtext/src/crdt/Fragment.h libs/collabtext/src/crdt/Buffer.h
git commit -m "refactor: add Fragment.text field, remove Rope from Buffer.h

Structural header changes for inline text refactor:
- Fragment gains std::string text field + 5-arg constructor
- Buffer.h drops Rope include, Rope members, extract_fragment_text
- set_fragments collapsed to single overload (no new_texts)
- normalize_fragments drops new_texts parameter

Does not compile yet — Buffer.cpp updates follow."
```

---

### Task 2: Rewrite set_fragments and query methods

This task rewrites the core `set_fragments()` to the simple O(n) version,
updates `text()`, `visible_rope_len()`, `deleted_rope_len()`,
`anchor_at()`, `resolve_anchor()`, and removes `extract_fragment_text()`.

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.cpp:29-134` (set_fragments)
- Modify: `libs/collabtext/src/crdt/Buffer.cpp:161-175` (query methods)
- Modify: `libs/collabtext/src/crdt/Buffer.cpp:373-435` (anchors)
- Modify: `libs/collabtext/src/crdt/Buffer.cpp:560-580` (extract_fragment_text)

- [ ] **Step 1: Rewrite set_fragments**

Replace both overloads of `set_fragments` (lines 29-134) with:

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
}
```

Also remove the `origin_key` helper? No — it's still used by
`collect_garbage` and `compact`. Keep it.

- [ ] **Step 2: Rewrite text(), visible_rope_len(), deleted_rope_len()**

Replace lines 161-175:

```cpp
std::string Buffer::text() const {
    std::string result;
    result.reserve(m_fragment_tree.summary().visible_bytes);
    m_fragment_tree.for_each([&](const Fragment& f) {
        if (f.visible) result += f.text;
    });
    return result;
}

uint32_t Buffer::visible_rope_len() const {
    return m_fragment_tree.summary().visible_bytes;
}

uint32_t Buffer::deleted_rope_len() const {
    return m_fragment_tree.summary().deleted_bytes;
}
```

- [ ] **Step 3: Update anchor_at and resolve_anchor**

In `anchor_at` (lines 373-398), remove `vis_rope_pos` tracking and read
text from fragment directly:

```cpp
Anchor Buffer::anchor_at(uint32_t byte_offset, Bias bias) const {
    if (m_fragment_tree.empty()) return Anchor::min();

    uint32_t accumulated = 0;
    Anchor result = Anchor::max();
    bool found = false;

    m_fragment_tree.for_each([&](const Fragment& f) {
        if (found) return;
        if (!f.visible) return;

        if (accumulated + f.byte_length > byte_offset) {
            uint32_t offset_in_frag = byte_offset - accumulated;
            uint32_t char_offset = count_chars(f.text, offset_in_frag);
            result = Anchor(f.origin.replica_id, f.origin.value + char_offset, bias);
            found = true;
            return;
        }
        accumulated += f.byte_length;
    });

    return result;
}
```

In `resolve_anchor` (lines 400-435), same treatment:

```cpp
uint32_t Buffer::resolve_anchor(const Anchor& anchor) const {
    if (anchor.is_min()) return 0;
    if (anchor.is_max()) return visible_length();

    uint32_t accumulated = 0;
    uint32_t result = visible_length();
    bool found = false;

    m_fragment_tree.for_each([&](const Fragment& f) {
        if (found) return;

        if (f.origin.replica_id == anchor.replica_id &&
            anchor.char_value >= f.origin.value &&
            anchor.char_value < f.origin.value + f.length) {

            if (f.visible) {
                uint32_t char_offset = anchor.char_value - f.origin.value;
                uint32_t byte_off = chars_to_bytes(f.text, char_offset);
                result = accumulated + byte_off;
            } else {
                result = accumulated;
            }
            found = true;
            return;
        }

        if (f.visible) {
            accumulated += f.byte_length;
        }
    });

    return result;
}
```

- [ ] **Step 4: Delete extract_fragment_text**

Remove the entire `extract_fragment_text` method (lines 560-580). All
callers will be updated to use `fragment.text` directly.

- [ ] **Step 5: Commit**

```bash
git add libs/collabtext/src/crdt/Buffer.cpp
git commit -m "refactor: rewrite set_fragments to O(n), remove rope usage from queries

set_fragments now just recomputes visibility + rebuilds tree.
text() walks visible fragments. Anchors read fragment.text.
extract_fragment_text removed.

Does not compile yet — call site updates follow."
```

---

### Task 3: Update split_fragment_at and coalesce_fragments

These helpers create/modify fragments and need to manage the text field.

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.cpp:521-558` (split_fragment_at)
- Modify: `libs/collabtext/src/crdt/Buffer.cpp:319-340` (coalesce_fragments)

- [ ] **Step 1: Rewrite split_fragment_at**

Replace lines 521-558. The method no longer needs extract_fragment_text —
it reads and splits `frags[frag_idx].text` directly:

```cpp
size_t Buffer::split_fragment_at(std::vector<Fragment>& frags,
                                  size_t frag_idx, uint32_t offset_in_frag) const
{
    assert(frag_idx < frags.size());
    assert(offset_in_frag > 0);
    assert(offset_in_frag < frags[frag_idx].byte_length);

    Fragment &orig = frags[frag_idx];
    uint32_t char_count = count_utf8_chars(orig.text, offset_in_frag);

    Fragment second;
    second.origin = Lamport(orig.origin.replica_id, orig.origin.value + char_count);
    second.locator = orig.locator;
    second.text = orig.text.substr(offset_in_frag);
    second.byte_length = static_cast<uint32_t>(second.text.size());
    second.length = orig.length - char_count;
    second.deletions = orig.deletions;
    second.visible = orig.visible;

    orig.text = orig.text.substr(0, offset_in_frag);
    orig.byte_length = offset_in_frag;
    orig.length = char_count;

    Lamport second_origin = second.origin;
    Locator saved_locator = orig.locator;

    insert_fragment(frags, std::move(second));

    for (size_t i = frag_idx + 1; i < frags.size(); ++i) {
        if (frags[i].origin == second_origin &&
            frags[i].locator == saved_locator) {
            return i;
        }
    }

    assert(false && "split_fragment_at: could not find second half after insert");
    return frag_idx + 1;
}
```

- [ ] **Step 2: Update coalesce_fragments to merge text**

In `coalesce_fragments` (lines 319-340), add text concatenation in the
merge branch:

```cpp
void Buffer::coalesce_fragments(std::vector<Fragment>& frags) {
    if (frags.size() < 2) return;
    size_t write = 0;
    for (size_t read = 1; read < frags.size(); ++read) {
        Fragment& prev = frags[write];
        Fragment& curr = frags[read];
        if (prev.visible == curr.visible &&
            prev.locator == curr.locator &&
            prev.origin.replica_id == curr.origin.replica_id &&
            prev.origin.value + prev.length == curr.origin.value &&
            prev.deletions == curr.deletions) {
            // Coalesce: extend prev to cover curr
            prev.text += curr.text;
            prev.byte_length += curr.byte_length;
            prev.length += curr.length;
        } else {
            ++write;
            if (write != read)
                frags[write] = std::move(curr);
        }
    }
    frags.resize(write + 1);
}
```

- [ ] **Step 3: Commit**

```bash
git add libs/collabtext/src/crdt/Buffer.cpp
git commit -m "refactor: split_fragment_at and coalesce_fragments manage fragment.text"
```

---

### Task 4: Rewrite sweep_and_coalesce

The GC path currently extracts text from ropes for the coalesce phase.
With inline text, both passes simplify dramatically.

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.cpp:211-276` (sweep_and_coalesce)

- [ ] **Step 1: Rewrite sweep_and_coalesce**

Replace lines 211-276:

```cpp
template<typename Pred>
size_t Buffer::sweep_and_coalesce(Pred is_gc_eligible) {
    auto frags = get_fragments();
    size_t original_count = frags.size();

    frags.erase(
        std::remove_if(frags.begin(), frags.end(), is_gc_eligible),
        frags.end());

    size_t removed = original_count - frags.size();

    // Coalesce adjacent compatible fragments
    coalesce_fragments(frags);

    if (removed > 0 || frags.size() < original_count) {
        set_fragments(std::move(frags));
    }
    return removed;
}
```

Key simplifications:
- Single pass: GC removal + coalesce + one set_fragments call (was 2)
- No rope text extraction (was the bulk of the old code)
- No new_texts map
- `coalesce_fragments` already handles text concatenation (Task 3)

- [ ] **Step 2: Commit**

```bash
git add libs/collabtext/src/crdt/Buffer.cpp
git commit -m "refactor: simplify sweep_and_coalesce — single pass, no rope extraction"
```

---

### Task 5: Rewrite normalize_fragments

Remove the origin-interval-lookup and new_texts parameter. Read text
directly from fragment.text.

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.cpp:602-709` (normalize_fragments)

- [ ] **Step 1: Rewrite normalize_fragments**

Replace lines 602-709. Signature changes (no new_texts parameter):

```cpp
void Buffer::normalize_fragments(std::vector<Fragment>& frags) const {
    size_t i = 0;
    while (i < frags.size()) {
        size_t run_start = i;
        Locator loc = frags[i].locator;

        size_t run_end = i + 1;
        while (run_end < frags.size() && frags[run_end].locator == loc)
            run_end++;

        bool multi_replica = false;
        uint16_t first_replica = frags[run_start].origin.replica_id;
        for (size_t j = run_start + 1; j < run_end; ++j) {
            if (frags[j].origin.replica_id != first_replica) {
                multi_replica = true;
                break;
            }
        }

        if (multi_replica) {
            std::vector<Fragment> extracted;
            for (size_t j = run_start; j < run_end; ++j) {
                auto &f = frags[j];
                if (f.length == 1) {
                    extracted.push_back(std::move(f));
                } else {
                    // Atomize: split into single-character fragments
                    // Text comes directly from f.text
                    const std::string& ftext = f.text;
                    uint32_t byte_pos = 0;
                    for (uint32_t c = 0; c < f.length; ++c) {
                        uint32_t char_bytes = 1;
                        unsigned char ch = static_cast<unsigned char>(ftext[byte_pos]);
                        if (ch >= 0xF0) char_bytes = 4;
                        else if (ch >= 0xE0) char_bytes = 3;
                        else if (ch >= 0xC0) char_bytes = 2;

                        Lamport single_origin(f.origin.replica_id, f.origin.value + c);

                        Fragment single;
                        single.origin = single_origin;
                        single.locator = f.locator;
                        single.text = ftext.substr(byte_pos, char_bytes);
                        single.byte_length = char_bytes;
                        single.length = 1;
                        single.deletions = f.deletions;
                        extracted.push_back(std::move(single));
                        byte_pos += char_bytes;
                    }
                }
            }

            std::sort(extracted.begin(), extracted.end(),
                [](const Fragment &a, const Fragment &b) {
                    auto cmp = a.locator <=> b.locator;
                    if (cmp != 0) return cmp < 0;
                    return a.origin < b.origin;
                });

            frags.erase(
                frags.begin() + static_cast<ptrdiff_t>(run_start),
                frags.begin() + static_cast<ptrdiff_t>(run_end));
            frags.insert(
                frags.begin() + static_cast<ptrdiff_t>(run_start),
                std::make_move_iterator(extracted.begin()),
                std::make_move_iterator(extracted.end()));

            i = run_start + extracted.size();
        } else {
            i = run_end;
        }
    }
}
```

- [ ] **Step 2: Commit**

```bash
git add libs/collabtext/src/crdt/Buffer.cpp
git commit -m "refactor: normalize_fragments reads fragment.text directly

Eliminates the nested O(n*m) origin-interval-lookup that scanned
old ropes for each multi-character fragment at shared locators."
```

---

### Task 6: Update apply_local_edit

This is the largest call site. Key changes:
- `PendingFrag` struct eliminated — text is in `Fragment.text`
- `extract_cursor_text` lambda eliminated — text is in `cursor.item()->text`
- `split_frag` lambda updated to split text
- New fragments get text set at creation
- `new_texts` map and second set_fragments overload eliminated

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.cpp:715-1140` (apply_local_edit)

- [ ] **Step 1: Update apply_local_edit**

This is a surgical edit of the existing method. The changes are:

**A. Remove rope position trackers** (delete lines 757-759):
```cpp
// DELETE:
//    uint32_t vis_rope_pos = 0;
//    uint32_t del_rope_pos = 0;
```

**B. Replace PendingFrag** (lines 763-767). Change from struct with separate
text to just a Fragment:
```cpp
    // Replace PendingFrag with just Fragment (text is inline)
    std::optional<Fragment> pending;
```

**C. Update split_frag lambda** (lines 780-800) to split text:
```cpp
    auto split_frag = [](const Fragment& f, uint32_t byte_off)
        -> std::pair<Fragment, Fragment>
    {
        uint32_t char_count = count_utf8_chars(f.text, byte_off);
        Fragment first;
        first.origin = f.origin;
        first.locator = f.locator;
        first.text = f.text.substr(0, byte_off);
        first.byte_length = byte_off;
        first.length = char_count;
        first.deletions = f.deletions;
        first.visible = f.visible;

        Fragment second;
        second.origin = Lamport(f.origin.replica_id, f.origin.value + char_count);
        second.locator = f.locator;
        second.text = f.text.substr(byte_off);
        second.byte_length = f.byte_length - byte_off;
        second.length = f.length - char_count;
        second.deletions = f.deletions;
        second.visible = f.visible;
        return {std::move(first), std::move(second)};
    };
```

**D. Remove extract_cursor_text lambda** (delete lines 803-814). Everywhere
it was called, replace with reading `cursor.item()->text` directly.

**E. Update consume_unchanged lambda** (lines 818-863). Replace all
`PendingFrag` usage with `Fragment`, remove `extract_cursor_text` calls:
```cpp
    auto consume_unchanged = [&](uint32_t vis_bytes) {
        while (pending && vis_bytes > 0) {
            if (pending->visible) {
                uint32_t pv = pending->byte_length;
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
                new_tree.push_item(std::move(*pending));
                pending.reset();
            }
        }
        while (vis_bytes > 0 && cursor.item()) {
            if (!cursor.item()->visible) {
                Fragment frag = *cursor.item();
                new_tree.push_item(std::move(frag));
                cursor.next();
                continue;
            }
            Fragment frag = *cursor.item();
            uint32_t fv = frag.byte_length;
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
```

**F. Update consume_deleted lambda** (lines 866-913). Same pattern:
```cpp
    auto consume_deleted = [&](uint32_t vis_bytes) {
        while (pending && vis_bytes > 0) {
            if (pending->visible) {
                uint32_t pv = pending->byte_length;
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
        while (vis_bytes > 0 && cursor.item()) {
            if (!cursor.item()->visible) {
                Fragment frag = *cursor.item();
                new_tree.push_item(std::move(frag));
                cursor.next();
                continue;
            }
            Fragment frag = *cursor.item();
            uint32_t fv = frag.byte_length;
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
```

**G. Update Phase 0 prefix handling** (lines 916-935). Remove rope pos
tracking, update PendingFrag usage:
```cpp
    if (!order.empty()) {
        uint32_t first_start = ranges[order[0]].first;
        new_tree.push_tree(cursor.slice({first_start}));
        if (cursor.item() && cursor.position().value < first_start) {
            uint32_t split_byte = first_start - cursor.position().value;
            auto [first, second] = split_frag(*cursor.item(), split_byte);
            new_tree.push_item(std::move(first));
            pending = std::move(second);
            cursor.next();
        }
    }
```

**H. Update fragment creation** (around line 1037-1040). Set text:
```cpp
            Fragment frag(frag_origin, new_loc,
                          static_cast<uint32_t>(replacement.size()), char_count,
                          replacement);
            frag.visible = true;
            new_tree.push_item(std::move(frag));
```

**I. Update relocation handling** (around lines 1000-1012). Remove
extract_cursor_text, use fragment.text:
```cpp
                if (pending) {
                    reloc_origin = pending->origin;
                    reloc_length = pending->length;
                    pending->locator = new_next_loc;
                } else if (cursor.item()) {
                    Fragment f = *cursor.item();
                    reloc_origin = f.origin;
                    reloc_length = f.length;
                    f.locator = new_next_loc;
                    pending = std::move(f);
                    cursor.next();
                }
```

**J. Update suffix** (lines 1058-1063):
```cpp
    if (pending) {
        new_tree.push_item(std::move(*pending));
        pending.reset();
    }
    new_tree.push_tree(cursor.suffix());
```

**K. Update tail** (lines 1114-1119). Remove new_texts map, update
normalize and set_fragments calls:
```cpp
    normalize_fragments(frags);
    set_fragments(std::move(frags));
```

Delete the `new_texts` map construction (lines 1114-1117).

- [ ] **Step 2: Commit**

```bash
git add libs/collabtext/src/crdt/Buffer.cpp
git commit -m "refactor: apply_local_edit uses fragment.text throughout

PendingFrag eliminated (text is inline in Fragment).
extract_cursor_text lambda removed.
split_frag lambda splits text directly.
new_texts map eliminated."
```

---

### Task 7: Update apply_remote_edit and apply_deletion_runs

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.cpp:1146-1289` (apply_deletion_runs + apply_remote_edit)

- [ ] **Step 1: Update apply_deletion_runs**

In `apply_deletion_runs` (lines 1146-1202), replace `extract_fragment_text`
calls with `frags[fi].text`:

```cpp
void Buffer::apply_deletion_runs(
    std::vector<Fragment>& frags,
    const std::vector<EditOperation::DeletionRun>& runs,
    Lamport deletion_id)
{
    for (auto& run : runs) {
        uint32_t remaining = run.count;
        uint32_t next_val = run.start_value;

        while (remaining > 0) {
            bool found = false;
            for (size_t fi = 0; fi < frags.size(); ++fi) {
                auto &f = frags[fi];
                if (f.origin.replica_id != run.replica_id) continue;
                if (next_val < f.origin.value ||
                    next_val >= f.origin.value + f.length) continue;

                found = true;
                uint32_t char_off = next_val - f.origin.value;
                uint32_t avail = f.length - char_off;
                uint32_t to_del = std::min(remaining, avail);

                if (char_off == 0 && to_del == f.length) {
                    f.deletions.push_back(deletion_id);
                } else {
                    if (char_off > 0) {
                        uint32_t byte_off = char_to_byte_offset(f.text, char_off);
                        fi = split_fragment_at(frags, fi, byte_off);
                        to_del = std::min(remaining, frags[fi].length);
                    }

                    if (to_del < frags[fi].length) {
                        uint32_t byte_off = char_to_byte_offset(frags[fi].text, to_del);
                        split_fragment_at(frags, fi, byte_off);
                    }

                    frags[fi].deletions.push_back(deletion_id);
                }

                next_val += to_del;
                remaining -= to_del;
                break;
            }
            if (!found) break;
        }
    }
}
```

- [ ] **Step 2: Update apply_remote_edit**

In `apply_remote_edit` (lines 1204-1289):

**A. Update split relocation text extraction** (around line 1229):
Replace `extract_fragment_text(frags, fi)` with `frags[fi].text`:
```cpp
            uint32_t char_off = target_ts.value - f.origin.value;
            if (char_off > 0) {
                uint32_t byte_off = char_to_byte_offset(f.text, char_off);
                fi = split_fragment_at(frags, fi, byte_off);
            }
```

**B. Update fragment creation for insertions** (lines 1262-1265). Set text:
```cpp
    for (auto &ins : op.inserted_fragments) {
        Fragment frag(ins.origin, ins.locator,
                      static_cast<uint32_t>(ins.content.size()), ins.length,
                      ins.content);
        insert_fragment(frags, std::move(frag));
    }
```

**C. Update tail** (lines 1282-1287). Remove new_texts, update calls:
```cpp
    normalize_fragments(frags);
    set_fragments(std::move(frags));
```

Delete the `new_texts` map construction (lines 1282-1285).

- [ ] **Step 3: Commit**

```bash
git add libs/collabtext/src/crdt/Buffer.cpp
git commit -m "refactor: apply_remote_edit and apply_deletion_runs use fragment.text

No more extract_fragment_text calls. Inserted fragments carry text.
new_texts map eliminated."
```

---

### Task 8: Build, test, and fix

This task compiles the full project, runs all tests, and fixes any issues.

**Files:**
- Modify: `libs/collabtext/src/crdt/Buffer.cpp` (if fixes needed)

- [ ] **Step 1: Reconfigure and build**

```bash
cmake -S . -B build-dev -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-dev -j$(nproc) 2>&1
```

Fix any compilation errors. Common issues to watch for:
- Missing `#include <string>` if Fragment.h doesn't transitively include it
- Any remaining references to `m_visible_text` or `m_deleted_text`
- Any remaining calls to `extract_fragment_text`
- Any remaining `new_texts` map references
- Any remaining `PendingFrag` struct references
- The `assert(f.byte_length == f.text.size())` in set_fragments may fire if
  any creation site doesn't set text — the assertion tells you which path
  is missing

- [ ] **Step 2: Run the core unit tests**

```bash
./build-dev/libs/collabtext/tst_buffer -v2
./build-dev/libs/collabtext/tst_convergence -v2
./build-dev/libs/collabtext/tst_anchor -v2
```

These exercise local edit, remote edit, undo/redo, anchors, and convergence.

- [ ] **Step 3: Run the fuzz and GC tests**

```bash
./build-dev/libs/collabtext/tst_fuzz -v2
./build-dev/libs/collabtext/tst_gc -v2
```

These are the most demanding correctness tests — random editing with
convergence checks, and GC with invariant validation.

- [ ] **Step 4: Run the realistic tests**

```bash
./build-dev/libs/collabtext/tst_realistic -v2
```

5 correctness scenarios with NetworkSim (3-client sustained, disconnect/
reconnect, cascading disconnects, long partition with GC, 10-client).

- [ ] **Step 5: Run the full test suite**

```bash
ctest --test-dir build-dev --output-on-failure
```

All 14 test targets must pass.

- [ ] **Step 6: Commit any fixes**

```bash
git add -A
git commit -m "fix: resolve compilation and test issues from inline text refactor"
```

---

### Task 9: Run benchmarks and verify improvement

**Files:**
- None (read-only benchmarking)

- [ ] **Step 1: Run the new realistic benchmarks**

```bash
./build-dev/libs/collabtext/tst_benchmark realistic_3_client_throughput -v2
./build-dev/libs/collabtext/tst_benchmark reconnect_sync_cost -v2
./build-dev/libs/collabtext/tst_benchmark gc_under_sustained_editing -v2
```

Record the numbers. Compare against pre-refactor baselines:
- realistic_3_client_throughput: was 17 ops/sec
- reconnect_sync_cost: was sub-ms sync, 1162/1806/2784 frags
- gc_under_sustained_editing: was 6 ops/sec both variants

- [ ] **Step 2: Run single-replica benchmarks**

```bash
./build-dev/libs/collabtext/tst_benchmark single_replica_throughput -v2
./build-dev/libs/collabtext/tst_benchmark single_replica_large_doc -v2
```

Pre-refactor baselines:
- 1K: 203 ops/sec
- 10K: 121 ops/sec
- 100K: 27 ops/sec
- 1M: 22 ops/sec

- [ ] **Step 3: Run stability checks**

```bash
for i in $(seq 1 5); do ./build-dev/libs/collabtext/tst_fuzz -v2 2>&1 | tail -1; done
for i in $(seq 1 5); do ./build-dev/libs/collabtext/tst_realistic -v2 2>&1 | tail -1; done
```

All runs must pass (random seeds, 5 runs each).

- [ ] **Step 4: Commit benchmark results as a doc update if meaningful**

If the improvement is significant, update or create a benchmark report at
`docs/reports/2026-04-05-inline-text-benchmark.md` with before/after
comparison.

```bash
git add docs/reports/
git commit -m "docs: inline text refactor benchmark results"
```
