# collabtext

A conflict-free plain text engine for offline-first collaborative editing.

collabtext is a CRDT text engine designed for a world where collaborators aren't always online at the same time. Instead of requiring a server or real-time connection, it produces compact operations that can travel over any file transport — Syncthing, Dropbox, a shared drive, a USB stick. Edits merge automatically. Documents converge. No conflicts, no "conflicted copy" files, no manual resolution.

## The problem

You use Syncthing to keep your files in sync across your desktop and laptop. You're out with your laptop, editing a document. That same document is still open on your desktop at home. Maybe you made changes on both machines. Maybe you forgot to save before you left. When Syncthing connects, one of two things happens: one version silently overwrites the other, or you get a `.sync-conflict` file and have to manually figure out what changed where.

This is the reality of every file-sync tool — Syncthing, Dropbox, Nextcloud, shared drives. They sync *files*, not *edits*. If the same file changed in two places, someone loses work or gets a conflict to resolve by hand.

collabtext solves this. Instead of syncing the file, it syncs the *edits*. Every keystroke, every deletion, every undo is captured as an operation with a unique timestamp and causal context. When your machines reconnect, the operations merge automatically and deterministically. Both sets of changes survive. No conflicts. No overwrites. No lost work. Ever.

## Why offline-first matters

Most collaborative editing engines assume a WebSocket. They're built for Google Docs: everyone online, a server in the middle, real-time cursors. That's great when it works, but it excludes a lot of real-world workflows:

- Editing the same document across your own machines that sync via Syncthing — and never worrying about which version "wins"
- Writers working on a shared manuscript across time zones, syncing via Dropbox
- Field researchers editing shared notes with intermittent connectivity
- Teams using NAS-based file sharing instead of cloud services
- Air-gapped environments where real-time collaboration isn't an option
- Any situation where "just open a browser tab" isn't the answer

collabtext treats the network as unreliable and optional. Every replica is fully independent. Edits are captured as operations, synced whenever a transport is available, and merged deterministically. Two people can edit the same paragraph on a plane and merge when they land. Or one person can edit the same document on two machines and never think about sync again.

## What it is

A C++20 library (no framework dependencies) that provides:

- **Conflict-free text merging** — Insert, delete, and replace operations from any number of replicas converge to identical documents, regardless of delivery order, duplication, or delay.

- **Stable position anchors** — Track cursor and selection positions that survive concurrent edits. Left-biased and right-biased modes handle insertion-at-cursor semantics correctly.

- **Collaborative undo/redo** — Per-replica undo stacks with parity-based conflict resolution. Your undo reverses your edit, even if others have edited around it. Undo operations propagate to peers.

- **Garbage collection** — Local tombstone cleanup (safe without coordination) and distributed watermark-based compaction (reclaims memory when all replicas have observed a deletion). Keeps long-lived documents from growing without bound.

- **Causal ordering** — Operations carry version vectors. Out-of-order delivery is handled automatically via a deferred queue. No application-level ordering logic required.

- **Structural list CRDT** — An ordered-list CRDT over opaque `uint64` elements (`IdList`), for applications that need a structural list separate from text content. Designed for composing block-ordered documents where each block is its own `Buffer`. Uses the same causality, anchor, undo, and GC machinery as the text engine.

- **Transport agnostic** — The engine produces and consumes operations. How they travel is your problem. File sync, message queue, REST API, carrier pigeon — the engine doesn't care. Over a low-latency transport like WebSockets, real-time presence features (live cursors, remote selections, concurrent editing feedback) emerge naturally from the existing primitives — no additional protocol required.

## What it isn't

- Not a rich text engine (plain text only — no bold, no links, no attributes)
- Not a general-purpose CRDT framework — `IdList` is the only non-text primitive and it exists precisely because it's the same algorithm as `Buffer` with a different element type. No maps, counters, registers, or JSON CRDTs.
- Not a real-time presence system out of the box (though the anchor system provides the primitives — over a low-latency transport, live cursors work naturally)
- Not the fastest CRDT engine (see [Performance](#performance))

## Building

Requires C++20 and CMake 3.19+. The core library has no external dependencies.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Tests use Qt6 Test (optional — the engine itself is Qt-free):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

## Quick start

```cpp
#include "crdt/Buffer.h"
using namespace CollabText::Crdt;

// Each replica gets a unique ID (uint16_t)
Buffer alice(1), bob(2);

// Alice types "hello world"
auto op1 = alice.apply_local_edit({{0, 0}}, {"hello world"});

// Send op1 to Bob (via file, network, anything)
bob.apply_ops({op1});
// bob.text() == "hello world"

// Both edit concurrently
auto opA = alice.apply_local_edit({{5, 5}}, {","});   // "hello, world"
auto opB = bob.apply_local_edit({{5, 5}}, {"!"});      // "hello! world"

// Exchange operations (in any order, with any delay)
alice.apply_ops({opB});
bob.apply_ops({opA});

// Both converge to the same text
assert(alice.text() == bob.text());
```

## API overview

```cpp
class Buffer {
public:
    explicit Buffer(uint16_t replica_id);

    // Editing
    Operation apply_local_edit(ranges, new_text);  // returns op to broadcast
    void apply_ops(const std::vector<Operation>&); // apply remote ops

    // Undo/redo
    std::optional<Operation> undo();  // returns op to broadcast
    std::optional<Operation> redo();

    // Document state
    std::string text() const;
    uint32_t visible_length() const;

    // Position tracking
    Anchor anchor_at(uint32_t byte_offset, Bias bias) const;
    std::optional<uint32_t> resolve_anchor(const Anchor&) const;

    // Garbage collection
    size_t collect_garbage();             // local cleanup
    size_t compact(const Global& watermark); // distributed compaction

    // Diagnostics
    size_t fragment_count() const;
    size_t tombstone_count() const;
};
```

## Performance

Benchmarked on x86_64, GCC 15.2, -O2:

| Document size | Throughput | Per-edit latency |
|---------------|-----------|-----------------|
| 1K chars | ~3,100 ops/sec | 0.3 ms |
| 10K chars | ~1,950 ops/sec | 0.5 ms |
| 100K chars | ~450 ops/sec | 2.2 ms |
| 3-client collaborative | ~530 ops/sec | 1.9 ms |

Sub-millisecond for typical documents. Comfortable for human typing speeds up to 100K+ characters.

For context: this is 1-2 orders of magnitude behind real-time-focused engines like Yjs and diamond-types, which are optimized for a different use case (high-frequency WebSocket-based collaboration). For offline file-sync workflows where edits arrive in batches, per-operation throughput matters less than correctness and merge quality — and on those axes, we're solid.

## Testing

The test suite includes:

- **Convergence tests** — Random concurrent edits across 2-10 replicas, verified to converge
- **Fuzz testing** — 16 scenarios with random edit patterns, undo/redo, and ordering variations
- **Realistic editing simulation** — Cursor-tracking model (sequential typing, backspace, selection, paste) across multiple clients with simulated network conditions
- **NetworkSim harness** — Configurable latency, jitter, out-of-order delivery, duplicate operations, disconnect/reconnect, and cascading partition tests
- **Garbage collection** — Tombstone lifecycle, undo-stack protection, distributed compaction

All tests are seeded and reproducible.

```bash
# Fast suite (13 tests, ~7 seconds)
ctest --test-dir build --output-on-failure -E "tst_realistic|tst_benchmark"

# Realistic multi-client tests (11 tests, ~80 seconds)
./build/libs/collabtext/tst_realistic

# Fuzz stability (16 tests per run)
./build/libs/collabtext/tst_fuzz
```

## Architecture

The engine is built on a few core data structures:

- **SumTree** — A B+ tree with monoid summary aggregation. Supports O(log n) seeks by byte offset, fractional position (Locator), or version vector. Branching factor 6 (12 max children per node).

- **Locator** — Fractional position identifiers (variable-length sequences of uint64 digits). `Locator::between(lo, hi)` creates positions between any two existing positions without renumbering. Biased allocation gives ~65K sequential inserts per depth level for both append and prepend patterns.

- **Fragment** — A span of text with a Locator, Lamport origin, deletion list, and visibility flag. The fragment tree is ordered by (Locator, origin) for deterministic merge.

- **UndoMap** — Tracks undo/redo parity per edit across all replicas using Lamport timestamps. A fragment is visible if its insertion is not undone and all its deletions are undone.

## License

GPLv3. See [LICENSE](LICENSE).
