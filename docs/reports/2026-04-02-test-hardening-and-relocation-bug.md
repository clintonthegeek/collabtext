# Test Hardening Report: The Same-Locator Relocation Bug

**Date:** 2026-04-02
**Scope:** Test suite expansion from 172 to 187 tests; discovery and fix of a latent fragment ordering bug in `apply_local_edit`

---

## 1. Motivation

After completing Optimizations 1-3 (Operation Queue, Cursor-Based Local Edit, VersionedFullOffset), the test suite had 172 tests across 9 executables. A coverage audit revealed that while the convergence tests provided strong end-to-end validation, the test suite had critical blind spots:

- **Zero tests with multi-byte UTF-8 characters.** The codebase has UTF-8 splitting logic in 6 places, all exercised only by ASCII.
- **Rope class completely untested.** 120+ lines of implementation with no coverage.
- **Undo/redo tested only in isolation.** No tests for undo after receiving remote edits, concurrent undo from multiple replicas, or redo stack invalidation with interleaved operations.
- **Split relocations never explicitly verified.** The relocation mechanism (central to the Batch 2 locator bug fix) was only exercised implicitly through random convergence.
- **No structural invariant checking.** Tests checked visible text output but never verified internal properties: fragment ordering, byte accounting, UTF-8 well-formedness of fragment content.

The question was asked: do these tests actually stress every dimension? The honest answer was no. 46 new "happy path" tests were added (UTF-8, Rope, undo/remote, anchors) and all passed on first run against the existing implementation. This raised a second, harder question: were the tests probing hard enough, or was the implementation simply correct?

The answer came from the adversarial fuzz suite.

---

## 2. The Adversarial Approach

The `tst_fuzz.cpp` suite was designed around two principles:

**Property-based invariant checking.** Rather than checking specific text output, every test calls `check_invariants()` after operations. This function verifies 7 structural properties that must hold for any valid Buffer state:

1. `visible_length() == text().size()`
2. Sum of visible fragment bytes == `visible_length()`
3. Concatenation of visible fragment content == `text()`
4. Fragment (locator, origin) ordering is strictly non-decreasing
5. No fragment has empty content
6. Fragment `.length` matches actual UTF-8 character count
7. No fragment content starts with a UTF-8 continuation byte (no mid-character splits)

These invariants catch classes of bugs that text-comparison tests miss entirely.

**Adversarial operation patterns.** Rather than random operations alone, the suite includes targeted attack patterns:

- 5 replicas all inserting at position 0 (maximum locator contention)
- 3 replicas repeatedly deleting and replacing a single character (maximum fragment churn)
- 20-deep undo chains interleaved with remote edits from a separate replica
- Multi-range edits with random UTF-8 text on every iteration
- 10-replica chaos with random edits, undo/redo, out-of-order delivery, and duplicate operations
- Network partition simulation (30 independent edits per replica, then full merge)
- Systematic UTF-8 boundary testing (insert and delete at every valid byte boundary in a mixed-width string)

And the test that broke things open:

- **Anchor stability under chaos:** Create anchors at every character position in a 43-character string, then apply 100 random edits. After each edit, verify that all anchors resolve to valid positions and maintain their original relative ordering.

---

## 3. The Discovery

On the first run of the full fuzz suite, all 13 tests passed. On the second run (different random seeds), `anchor_stability_under_chaos` failed:

```
Anchor order inverted at step 1: anchor[33]=32 > anchor[34]=15
```

Characters 33 and 34 in "the quick brown fox jumps over the lazy dog" are 'e' and ' ' (space) in "the lazy". After a single random edit, the anchor on character 34 resolved to byte offset 15 while the anchor on character 33 resolved to byte offset 32. An anchor on a later character resolved to an earlier position in the document.

The failure was intermittent: approximately 1 in 10 runs. It depended on the random seed, which determined the specific edit that triggered the bug.

---

## 4. Isolation

The first challenge was reproducing the failure deterministically. The test used `std::random_device{}()` for seeding, so each run explored different operation sequences. A diagnostic version was written that captured the seed on failure and dumped full fragment state:

```
Seed: 347475862
Edit 0: [4, 31) replacement=5 bytes
Edit 1: [19, 21) replacement=3 bytes  
Edit 2: [3, 17) replacement=5 bytes
  After edit 2: anchor[40]=4, anchor[41]=3  ← INVERSION
```

The fragment dump at the point of failure revealed the structural cause:

```
[4] origin=(1,32) len=8 vis=0 del=1 bytes=8
[5] origin=(1,42) len=2 vis=0 del=1 bytes=2   ← ANCHOR41 (char_value=42)
[6] origin=(1,40) len=2 vis=1 del=0 bytes=2   ← ANCHOR40 (char_value=41)
```

Fragment [5] (origin 42) appeared **before** fragment [6] (origin 40) in the tree. Both fragments originated from the same insertion ("the quick brown fox..."), but fragment [6] had been **relocated** to a new, higher locator during a previous edit's insert phase. Fragment [5], which had a higher origin value (42 > 40), was **not** relocated and retained the original locator. Since the original locator sorts before the relocated locator, fragment [5] appeared first in the tree — even though its characters were originally later in the document.

---

## 5. Root Cause Analysis

The bug was in the cursor-based `apply_local_edit` rewrite (Optimization 2). When inserting text between fragments that share the same locator (a common situation after fragment splitting), the code correctly detects the same-locator condition and creates a **split relocation** to move the next fragment to a new, higher locator. This creates ordering space for the insert:

```
Before relocation:  [abc](L)  [def](L)     ← same locator, insert can't go between
After relocation:   [abc](L)  [XX](M)  [def](L')   ← L < M < L', correct ordering
```

The problem: the relocation only moved the **immediate next fragment** (the "pending" fragment from the cursor split). If the original insertion had been previously split into multiple fragments — say `[abc](L)`, `[de](L)`, `[f](L)` by prior edits — only `[de]` was relocated. `[f]` retained locator `L` and sorted **before** the insert and before the relocated `[de]`:

```
Actual:    [abc](L)  [f](L)  [XX](M)  [de](L')   ← [f] before [XX], wrong!
Expected:  [abc](L)  [XX](M)  [de](L')  [f](L')  ← all post-insert fragments relocated
```

The `SplitRelocation` record sent to remote replicas had the same deficiency: its `fragment_length` field only covered the pending fragment, so remotes also failed to relocate subsequent fragments. This caused convergence failures — different replicas produced different text.

---

## 6. The Fix

The fix adds a **deferred relocation pass** between tree building and normalization. After all ranges are processed and the tree is flattened to a vector, the code walks each relocation point and applies it to all contiguous same-replica fragments:

```cpp
for (auto& dr : deferred_relocs) {
    Lamport next_expected = dr.min_origin;
    uint32_t total_chars = 0;
    for (auto& f : frags) {
        if (f.origin.replica_id != dr.min_origin.replica_id) continue;
        if (f.origin.value != next_expected.value) continue;
        if (f.locator == dr.old_loc) {
            f.locator = dr.new_loc;
        }
        total_chars += f.length;
        next_expected = Lamport(f.origin.replica_id,
                                f.origin.value + f.length);
    }
    // Update SplitRelocation to cover the full extent
    for (auto& sr : op.split_relocations) {
        if (sr.fragment_origin == dr.min_origin) {
            sr.fragment_length = total_chars;
            break;
        }
    }
}
```

The contiguous-origin walking logic mirrors exactly what the remote side does in `apply_remote_edit` when processing a `SplitRelocation`. This ensures local and remote replicas apply identical relocations, maintaining convergence.

Key design decisions in the fix:

1. **Deferred, not inline.** The relocation happens after tree building, not during the cursor traversal. This avoids disturbing the cursor position, which would break subsequent range processing in multi-range edits.

2. **Contiguous-origin walking.** The relocation only affects fragments that form a contiguous sequence of Lamport origins from the same replica. This prevents relocating unrelated fragments that happen to share the same locator (e.g., from concurrent insertions by other replicas at the same position).

3. **SplitRelocation length update.** The `fragment_length` in the operation record is updated to cover all relocated fragments, so remotes receive the full extent.

---

## 7. Verification

The fix was verified through:

- **20 consecutive convergence runs** (each with random seeds across 2, 3, and 5 replicas): all passed.
- **20 consecutive fuzz suite runs** (13 adversarial tests per run, each with random seeds): all passed.
- **Deterministic reproduction** of the original failure (seed 347475862): anchor[40]=4, anchor[41]=5 (was 4 > 3 before fix).
- **Full test suite** (187 tests across 10 executables): 0 failures.

---

## 8. Impact on Codebase Quality

This episode illustrates a principle that the test hardening effort was built around: **happy-path tests verify what you built; adversarial tests verify what you assumed.**

The 46 "happy path" tests added in the first hardening pass (UTF-8, Rope, undo/remote, anchors) all passed immediately. They confirmed that the implementation handled expected inputs correctly. But they couldn't find the relocation bug because it required a specific sequence of operations — a fragment split by one edit, followed by an insert at the split boundary by a later edit — that no manually-written test anticipated.

The adversarial suite found it because it checked **structural invariants** (fragment ordering, anchor monotonicity) rather than specific text outputs, and because it explored the operation space randomly with enough volume (100 edits per run, ~10 runs per minute) to hit the rare trigger condition.

The bug had been latent since the cursor-based rewrite in Optimization 2. It passed all 47 `tst_buffer` tests, all convergence tests (including 40 consecutive runs during validation), and all anchor tests. It was invisible to text-comparison testing because the visible text was usually correct — the bug only manifested in fragment ordering, which affected anchor resolution of deleted characters.

In production use, the bug would have caused:

- **Cursor position jumps** when editing near previously-split regions, as anchors tracking cursor positions resolved to unexpected offsets.
- **Selection inversions** where the selection start resolved to a position after the selection end.
- **Rare convergence failures** in multi-replica scenarios where the mismatched `SplitRelocation` extent caused different replicas to relocate different fragments.

The fix is 25 lines of code. The test that found it is 50 lines. The infrastructure that made it possible — the 7-invariant `check_invariants()` function and the adversarial test patterns — is about 200 lines. This is a favorable ratio: 200 lines of test infrastructure that will catch entire classes of similar bugs in perpetuity, not just this one instance.

The test suite now stands at 187 tests. More importantly, 13 of those tests are adversarial property-checkers that exercise the CRDT engine with random operations, multi-byte text, concurrent replicas, undo/redo chains, network partitions, and duplicate delivery — checking structural invariants after every step. Any future change to the engine that violates fragment ordering, byte accounting, UTF-8 integrity, or convergence will be caught by these tests within seconds.

---

## Appendix: Test Suite Inventory

| Executable | Count | Coverage |
|-----------|------:|---------|
| tst_clock | 13 | Lamport timestamps, version vectors, join/meet |
| tst_locator | 9 | Fractional position identifiers, between() |
| tst_sumtree | 37 | B+ tree: push, cursor, seek, slice, suffix |
| tst_buffer | 47 | Local/remote edit, undo/redo, multi-range, split relocation, convergence |
| tst_convergence | 7 | 2/3/5-replica random stress with undo, duplication, out-of-order delivery |
| tst_anchor | 21 | Position tracking through edits, undo, remote ops, concurrent inserts |
| tst_opqueue | 8 | Deferred operation queue with replica tracking |
| tst_utf8 | 14 | 2/3/4-byte characters through insert, delete, split, undo, anchors |
| tst_rope | 16 | Rope: push, substr, slice, chunking, UTF-8 boundaries |
| tst_fuzz | 15 | Adversarial: invariant checking, 10-replica chaos, partition/merge, UTF-8 convergence |
| **Total** | **187** | |
