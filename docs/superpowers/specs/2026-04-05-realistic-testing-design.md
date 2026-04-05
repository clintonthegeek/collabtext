# Realistic Network Simulation & Testing — Design Spec

**Date:** 2026-04-05
**Status:** Design ready
**Prereqs:** Gen 2 complete, local + watermark GC implemented

---

## 1. Motivation

The current test suite validates correctness through adversarial random
editing (16 fuzz scenarios, 10-replica chaos, partition-then-merge) and
measures performance through single-dimension benchmarks (throughput by
doc size, tombstone degradation, GC effectiveness).

What's missing:

- **Network realism.** Ops are delivered instantly or dumped in bulk.
  Real networks have 50-200ms latency, jitter, and gradual delivery.
- **Disconnect/reconnect.** The partition test does one clean partition
  between 2 replicas. Real users go offline for minutes and reconnect
  with large deltas across 3+ clients.
- **Sustained sessions.** Current tests run 100-200 steps. Real editing
  sessions produce thousands of ops over minutes.
- **Typing realism.** Edits are at random positions. Real users type
  sequentially, backspace, cursor-jump, and paste — patterns that create
  different fragment/tombstone distributions.
- **GC under load.** GC is tested in isolation, not during sustained
  multi-client editing where tombstones accumulate continuously.

---

## 2. NetworkSim Harness

### 2.1 Architecture

A `NetworkSim` class that owns N `Buffer` instances and manages a
simulated network between them. Pure deterministic simulation — no real
timers or threads. Reproducible via seed.

```
NetworkSim
  ├── replicas: vector<Buffer>
  ├── message_queues: vector<priority_queue<ScheduledOp>>  // per-replica inbox
  ├── offline: vector<bool>                                // per-replica state
  ├── pending_while_offline: vector<vector<ScheduledOp>>   // ops queued during disconnect
  ├── sim_clock: uint64_t                                  // simulated milliseconds
  ├── config: NetworkConfig
  └── rng: mt19937
```

### 2.2 NetworkConfig

```cpp
struct NetworkConfig {
    uint32_t min_latency_ms = 50;
    uint32_t max_latency_ms = 200;
    double duplicate_probability = 0.05;
    bool allow_reorder = true;
};
```

- **Latency:** Each op's delivery time = `sim_clock + uniform(min, max)`.
  When `allow_reorder` is true, each op gets an independent random delay,
  so faster ops can overtake slower ones naturally. When false, ops from
  the same source are delivered in FIFO order (add max of previous
  delivery time).
- **Duplicates:** With probability `duplicate_probability`, an op is
  enqueued twice (at different delivery times).

### 2.3 ScheduledOp

```cpp
struct ScheduledOp {
    uint64_t deliver_at_ms;   // simulated clock time to deliver
    uint16_t from_replica;    // source replica
    Operation op;
};
```

Priority queue ordered by `deliver_at_ms` (min-heap).

### 2.4 Public Interface

```cpp
class NetworkSim {
public:
    NetworkSim(int num_replicas, NetworkConfig config, uint64_t seed);

    /// Apply a local edit on a replica and broadcast to all online peers.
    Operation edit(int replica, const std::vector<std::pair<uint32_t,uint32_t>>& ranges,
                   const std::vector<std::string>& new_text);

    /// Undo/redo on a replica and broadcast to all online peers.
    std::optional<Operation> undo(int replica);
    std::optional<Operation> redo(int replica);

    /// Mark a replica as offline. Ops destined for it accumulate.
    void disconnect(int replica);

    /// Bring a replica back online. Accumulated ops are scheduled
    /// for delivery with normal latency from the current sim_clock.
    void reconnect(int replica);

    /// Advance the simulated clock by `ms` milliseconds.
    /// Delivers all ops whose scheduled time <= new clock.
    void tick(uint64_t ms);

    /// Deliver ALL pending ops, retry deferred, advance clock to infinity.
    void drain();

    /// Check invariants on all replicas.
    void check_all_invariants(const char* context) const;

    /// Assert all replicas have converged (same visible text).
    void assert_convergence(const char* context) const;

    /// Run GC on a specific replica.
    size_t collect_garbage(int replica);

    /// Compute watermark = meet(all replica versions) and compact all.
    size_t compact_all();

    /// Access a replica's buffer (for queries).
    const Buffer& buffer(int replica) const;
    Buffer& buffer(int replica);

    /// Current simulated clock.
    uint64_t clock() const;
};
```

### 2.5 Broadcast Logic

When a replica produces an op (edit/undo/redo):

```
for each other replica R:
    if R is offline:
        enqueue in pending_while_offline[R]
    else:
        delivery_time = sim_clock + uniform(min_latency, max_latency)
        push ScheduledOp{delivery_time, source, op} into message_queues[R]
        if random() < duplicate_probability:
            delivery_time2 = sim_clock + uniform(min_latency, max_latency)
            push ScheduledOp{delivery_time2, source, op} into message_queues[R]
```

### 2.6 Reconnect Logic

When a replica comes back online:

```
for each op in pending_while_offline[R]:
    delivery_time = sim_clock + uniform(min_latency, max_latency)
    push into message_queues[R]
clear pending_while_offline[R]
offline[R] = false
```

This models the "bulk sync on reconnect" pattern: all missed ops arrive
with fresh latency from the reconnection time.

### 2.7 Tick Logic

```
fn tick(ms):
    sim_clock += ms
    for each replica R:
        while message_queues[R].top().deliver_at_ms <= sim_clock:
            op = message_queues[R].pop()
            R.apply_ops({op.op})
```

### 2.8 Drain Logic

```
fn drain():
    // Deliver everything
    sim_clock = UINT64_MAX
    tick(0)  // delivers all scheduled ops
    // Retry deferred ops
    for pass in 0..30:
        for each replica R:
            R.apply_ops({})
```

---

## 3. Edit Strategies

### 3.1 Interface

```cpp
struct EditAction {
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    std::vector<std::string> new_text;
};

class EditStrategy {
public:
    virtual ~EditStrategy() = default;
    virtual EditAction next_edit(const Buffer& buf, std::mt19937& rng) = 0;
};
```

### 3.2 RandomStrategy

The existing random edit pattern from `tst_fuzz.cpp`. Random position,
random size, random UTF-8 text. Maximizes edge case coverage. Used for
correctness tests.

### 3.3 RealisticStrategy

Models a user typing in a text editor. Maintains a cursor position per
instance. Action distribution:

| Action | Weight | Description |
|--------|--------|-------------|
| Sequential type | 50% | Insert 1-5 ASCII chars at cursor, cursor advances |
| Backspace | 15% | Delete 1-3 chars behind cursor |
| Select+delete | 10% | Delete 5-20 chars near cursor |
| Cursor jump | 10% | Move cursor to a random valid position |
| Paste | 10% | Insert 10-50 chars at cursor |
| Select+replace | 5% | Delete 5-15 chars, insert 5-15 chars |

Cursor is clamped to valid UTF-8 boundaries. Generates ASCII text for
benchmark consistency (UTF-8 correctness is covered by RandomStrategy).

---

## 4. Test Scenarios

### 4.1 Correctness Tests (tst_realistic.cpp)

All use `RandomStrategy` and randomized seeds for reproducibility.
All assert convergence + invariants after drain.

#### Test 1: sustained_3_client_convergence

3 replicas, 500 ops per replica, continuous `tick(10)` between ops.
Default latency (50-200ms), 5% duplicates, reorder enabled. Random
undo/redo with 20% probability. Assert convergence after drain.

**Purpose:** Sustained multi-client editing with realistic delivery.

#### Test 2: disconnect_reconnect_cycle

3 replicas. Sequence:
1. All edit for 100 ops with delivery
2. Replica 2 disconnects
3. Replicas 0 and 1 edit for 200 ops
4. Replica 2 reconnects, bulk sync
5. All edit for 100 more ops
6. Drain and assert convergence

**Purpose:** Offline replica accumulates large delta, then syncs.

#### Test 3: cascading_disconnects

5 replicas. Disconnect them one at a time (replica 1 after 50 ops,
replica 2 after 100 ops, replica 3 after 150 ops, replica 4 after 200
ops). Only replica 0 stays online the whole time. Then reconnect in
reverse order (4, 3, 2, 1), with 50 ops of editing between each
reconnection. Drain and assert convergence.

**Purpose:** Worst case for deferred queue — each reconnecting replica
brings a unique delta that must be reconciled with all previously
reconnected replicas.

#### Test 4: long_partition_with_gc

2 replicas. Both start with a 5K document. Disconnect. Each edits
independently for 500 ops (creating heavy tombstone load). Reconnect
and sync. Compute watermark, both compact(). Assert convergence +
invariants.

**Purpose:** GC safety after a long partition with heavy divergence.

#### Test 5: ten_client_sustained

10 replicas, 2000 total ops (200 per replica on average). Default
latency. Random disconnects (10% chance per 100 ops). Assert convergence
after drain.

**Purpose:** Scale test — convergence under sustained 10-client editing
with intermittent disconnects.

### 4.2 Benchmarks (added to tst_benchmark.cpp)

All use `RealisticStrategy`.

#### Benchmark 1: realistic_3_client_throughput

3 replicas, each performs 500 edits with `RealisticStrategy`. Latency
100ms. Measure:
- Per-replica ops/sec
- Total wall time to convergence (after drain)
- Fragment count at end

**Purpose:** "How fast is collaborative editing with 3 people typing?"

#### Benchmark 2: reconnect_sync_cost

1 replica goes offline. 2 others make N edits each (N = 100, 500, 1000).
Offline replica reconnects. Measure:
- Time to process the bulk sync (apply_ops of accumulated operations)
- Fragment count before/after sync

**Purpose:** Cost of reconnecting after extended offline period.

#### Benchmark 3: gc_under_sustained_editing

3 replicas, 2000 ops total with `RealisticStrategy`. Two variants:
- (A) No GC
- (B) `collect_garbage()` every 200 ops on each replica

Measure: fragment count at intervals (every 200 ops), throughput at
intervals, final fragment count.

**Purpose:** Quantify GC's amortized benefit during sustained editing.

---

## 5. File Map

| File | Action | Responsibility |
|------|--------|---------------|
| `libs/collabtext/src/crdt/NetworkSim.h` | Create | NetworkSim class, NetworkConfig, ScheduledOp |
| `libs/collabtext/src/crdt/NetworkSim.cpp` | Create | NetworkSim implementation |
| `libs/collabtext/src/crdt/EditStrategy.h` | Create | EditStrategy interface, RandomStrategy, RealisticStrategy |
| `libs/collabtext/tests/tst_realistic.cpp` | Create | 5 correctness tests using NetworkSim |
| `libs/collabtext/tests/tst_benchmark.cpp` | Modify | 3 new benchmarks using NetworkSim + RealisticStrategy |
| `libs/collabtext/CMakeLists.txt` | Modify | Add NetworkSim to library sources, add tst_realistic test |

---

## 6. What NOT To Change

- **Buffer** — no changes. NetworkSim wraps Buffer, doesn't modify it.
- **Existing tests** — tst_fuzz.cpp and tst_gc.cpp are left as-is.
  They use inline queue management which is fine for their focused
  adversarial purposes.
- **Existing benchmarks** — the current single-dimension benchmarks
  remain. New benchmarks are additive.

---

## 7. Success Criteria

- All 5 correctness tests pass with multiple random seeds (5 runs each)
- All 3 benchmarks produce meaningful numbers
- NetworkSim is reusable for future tests (clean API, no test-specific
  logic baked in)
- Existing 13 tests continue to pass unchanged
