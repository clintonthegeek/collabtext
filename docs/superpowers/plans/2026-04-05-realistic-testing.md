# Realistic Network Simulation & Testing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a reusable NetworkSim harness with edit strategies (random + realistic typing), then use it for 5 correctness tests and 3 benchmarks covering multi-client scenarios with latency, disconnects, and sustained editing.

**Architecture:** `NetworkSim` wraps N `Buffer` instances with per-replica message queues, simulated clock, and disconnect/reconnect state. `EditStrategy` is a polymorphic interface with `RandomStrategy` (adversarial) and `RealisticStrategy` (cursor-based typing). All simulation is deterministic — no threads, no real timers, reproducible via seed.

**Tech Stack:** C++20, Qt6 Test, existing CRDT Buffer/Operations types

**Spec:** `docs/superpowers/specs/2026-04-05-realistic-testing-design.md`

---

## File Map

| File | Action | Responsibility |
|------|--------|---------------|
| `libs/collabtext/src/crdt/EditStrategy.h` | Create | EditStrategy interface, RandomStrategy, RealisticStrategy (header-only) |
| `libs/collabtext/src/crdt/NetworkSim.h` | Create | NetworkSim class, NetworkConfig, ScheduledOp |
| `libs/collabtext/src/crdt/NetworkSim.cpp` | Create | NetworkSim implementation |
| `libs/collabtext/tests/tst_realistic.cpp` | Create | 5 correctness tests |
| `libs/collabtext/tests/tst_benchmark.cpp` | Modify | 3 new benchmarks |
| `libs/collabtext/CMakeLists.txt` | Modify | Add NetworkSim.cpp to library, add tst_realistic |

---

### Task 1: EditStrategy Interface + RandomStrategy

**Files:**
- Create: `libs/collabtext/src/crdt/EditStrategy.h`

- [ ] **Step 1: Create EditStrategy.h with interface and RandomStrategy**

```cpp
#pragma once

#include "crdt/Buffer.h"
#include <random>
#include <string>
#include <vector>

namespace CollabText::Crdt {

struct EditAction {
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    std::vector<std::string> new_text;
};

class EditStrategy {
public:
    virtual ~EditStrategy() = default;
    virtual EditAction next_edit(const Buffer& buf, std::mt19937& rng) = 0;
};

/// Random position, random size, random UTF-8 text. Maximizes edge case
/// coverage. Used for correctness tests.
class RandomStrategy : public EditStrategy {
public:
    EditAction next_edit(const Buffer& buf, std::mt19937& rng) override {
        std::string text = buf.text();
        uint32_t len = static_cast<uint32_t>(text.size());
        uint32_t start = random_byte_offset(rng, text);
        uint32_t end = start;
        if (start < len) {
            std::string suffix = text.substr(start);
            end = start + random_byte_offset(rng, suffix);
        }
        std::string replacement;
        if (rng() % 3 != 0) {
            replacement = random_text(rng, 5);
        }
        return {{{{start, end}}}, {replacement}};
    }

private:
    static std::string random_text(std::mt19937& rng, int maxChars) {
        int count = 1 + (rng() % maxChars);
        std::string result;
        for (int i = 0; i < count; ++i) {
            int kind = rng() % 100;
            if (kind < 60) {
                result += static_cast<char>('a' + (rng() % 26));
            } else if (kind < 75) {
                result += '\xc3';
                result += static_cast<char>(0xa0 + (rng() % 32));
            } else if (kind < 90) {
                result += '\xe4';
                result += static_cast<char>(0xb8 + (rng() % 4));
                result += static_cast<char>(0x80 + (rng() % 64));
            } else {
                result += '\xf0';
                result += '\x9f';
                result += static_cast<char>(0x98 + (rng() % 8));
                result += static_cast<char>(0x80 + (rng() % 64));
            }
        }
        return result;
    }

    static uint32_t random_byte_offset(std::mt19937& rng, const std::string& text) {
        if (text.empty()) return 0;
        std::vector<uint32_t> boundaries = {0};
        for (size_t b = 0; b < text.size(); ) {
            unsigned char c = static_cast<unsigned char>(text[b]);
            if (c < 0x80) b += 1;
            else if ((c & 0xE0) == 0xC0) b += 2;
            else if ((c & 0xF0) == 0xE0) b += 3;
            else b += 4;
            boundaries.push_back(static_cast<uint32_t>(b));
        }
        return boundaries[rng() % boundaries.size()];
    }
};

/// Models a user typing in a text editor. Maintains a cursor position.
/// Action distribution: 50% sequential type, 15% backspace, 10% select+delete,
/// 10% cursor jump, 10% paste, 5% select+replace.
class RealisticStrategy : public EditStrategy {
public:
    EditAction next_edit(const Buffer& buf, std::mt19937& rng) override {
        uint32_t len = buf.visible_length();
        if (m_cursor > len) m_cursor = len;

        int action = rng() % 100;

        if (action < 50) {
            // Sequential type: insert 1-5 chars at cursor
            int count = 1 + static_cast<int>(rng() % 5);
            std::string text = random_ascii(rng, count);
            uint32_t pos = m_cursor;
            m_cursor = pos + static_cast<uint32_t>(text.size());
            return {{{{pos, pos}}}, {text}};
        } else if (action < 65) {
            // Backspace: delete 1-3 chars behind cursor
            uint32_t del = 1 + (rng() % 3);
            if (del > m_cursor) del = m_cursor;
            if (del == 0) return next_edit(buf, rng);  // nothing to delete, retry
            uint32_t start = m_cursor - del;
            m_cursor = start;
            return {{{{start, start + del}}}, {""}};
        } else if (action < 75) {
            // Select+delete: delete 5-20 chars near cursor
            if (len == 0) return next_edit(buf, rng);
            uint32_t del = 5 + (rng() % 16);
            uint32_t start = m_cursor;
            if (start + del > len) {
                if (len >= del) start = len - del;
                else { start = 0; del = len; }
            }
            m_cursor = start;
            return {{{{start, start + del}}}, {""}};
        } else if (action < 85) {
            // Cursor jump: move to a random valid position
            m_cursor = len > 0 ? rng() % (len + 1) : 0;
            // After jump, do a small type
            int count = 1 + static_cast<int>(rng() % 3);
            std::string text = random_ascii(rng, count);
            uint32_t pos = m_cursor;
            m_cursor = pos + static_cast<uint32_t>(text.size());
            return {{{{pos, pos}}}, {text}};
        } else if (action < 95) {
            // Paste: insert 10-50 chars at cursor
            int count = 10 + static_cast<int>(rng() % 41);
            std::string text = random_ascii(rng, count);
            uint32_t pos = m_cursor;
            m_cursor = pos + static_cast<uint32_t>(text.size());
            return {{{{pos, pos}}}, {text}};
        } else {
            // Select+replace: delete 5-15 chars, insert 5-15 chars
            if (len < 5) return next_edit(buf, rng);
            uint32_t del = 5 + (rng() % 11);
            uint32_t start = m_cursor;
            if (start + del > len) {
                if (len >= del) start = len - del;
                else { start = 0; del = len; }
            }
            int ins = 5 + static_cast<int>(rng() % 11);
            std::string text = random_ascii(rng, ins);
            m_cursor = start + static_cast<uint32_t>(text.size());
            return {{{{start, start + del}}}, {text}};
        }
    }

private:
    uint32_t m_cursor = 0;

    static std::string random_ascii(std::mt19937& rng, int count) {
        std::string result(count, ' ');
        for (int i = 0; i < count; ++i)
            result[i] = static_cast<char>('a' + (rng() % 26));
        return result;
    }
};

} // namespace CollabText::Crdt
```

- [ ] **Step 2: Verify it compiles**

Run: `cmake --build build-dev --target collabtext -j$(nproc) 2>&1 | tail -5`
Expected: Builds (header-only, no new sources needed yet).

- [ ] **Step 3: Commit**

```bash
git add libs/collabtext/src/crdt/EditStrategy.h
git commit -m "feat: EditStrategy interface with Random and Realistic strategies"
```

---

### Task 2: NetworkSim Header

**Files:**
- Create: `libs/collabtext/src/crdt/NetworkSim.h`

- [ ] **Step 1: Create NetworkSim.h**

```cpp
#pragma once

#include "crdt/Buffer.h"
#include "crdt/EditStrategy.h"
#include <functional>
#include <optional>
#include <queue>
#include <random>
#include <string>
#include <vector>

namespace CollabText::Crdt {

struct NetworkConfig {
    uint32_t min_latency_ms = 50;
    uint32_t max_latency_ms = 200;
    double duplicate_probability = 0.05;
    bool allow_reorder = true;
};

struct ScheduledOp {
    uint64_t deliver_at_ms = 0;
    uint16_t from_replica = 0;
    Operation op;

    bool operator>(const ScheduledOp& other) const {
        return deliver_at_ms > other.deliver_at_ms;
    }
};

class NetworkSim {
public:
    NetworkSim(int num_replicas, NetworkConfig config, uint64_t seed);

    /// Apply a local edit on a replica and broadcast to all online peers.
    Operation edit(int replica,
                   const std::vector<std::pair<uint32_t, uint32_t>>& ranges,
                   const std::vector<std::string>& new_text);

    /// Apply an edit generated by a strategy.
    Operation edit(int replica, const EditAction& action);

    /// Undo/redo on a replica and broadcast to all online peers.
    std::optional<Operation> undo(int replica);
    std::optional<Operation> redo(int replica);

    /// Mark a replica as offline.
    void disconnect(int replica);

    /// Bring a replica back online.
    void reconnect(int replica);

    /// Advance the simulated clock by ms milliseconds, delivering due ops.
    void tick(uint64_t ms);

    /// Deliver ALL pending ops, retry deferred.
    void drain();

    /// Check invariants on all replicas (calls QFAIL on failure).
    void check_all_invariants(const char* context) const;

    /// Assert all replicas have converged (same visible text).
    void assert_convergence(const char* context) const;

    /// Run collect_garbage on a specific replica.
    size_t collect_garbage(int replica);

    /// Compute watermark = meet(all replica versions) and compact all.
    size_t compact_all();

    /// Access a replica's buffer.
    const Buffer& buffer(int replica) const;
    Buffer& buffer(int replica);

    /// Number of replicas.
    int num_replicas() const;

    /// Current simulated clock.
    uint64_t clock() const;

private:
    void broadcast(int source, const Operation& op);
    uint64_t random_latency();

    using OpQueue = std::priority_queue<ScheduledOp,
                                        std::vector<ScheduledOp>,
                                        std::greater<ScheduledOp>>;

    std::vector<Buffer> m_replicas;
    std::vector<OpQueue> m_queues;          // per-replica inbox
    std::vector<bool> m_offline;
    std::vector<std::vector<ScheduledOp>> m_pending_offline;  // ops queued while offline
    uint64_t m_clock = 0;
    NetworkConfig m_config;
    std::mt19937 m_rng;
};

} // namespace CollabText::Crdt
```

- [ ] **Step 2: Commit**

```bash
git add libs/collabtext/src/crdt/NetworkSim.h
git commit -m "feat: NetworkSim header — config, ScheduledOp, class declaration"
```

---

### Task 3: NetworkSim Implementation

**Files:**
- Create: `libs/collabtext/src/crdt/NetworkSim.cpp`
- Modify: `libs/collabtext/CMakeLists.txt`

- [ ] **Step 1: Create NetworkSim.cpp**

```cpp
#include "crdt/NetworkSim.h"
#include <QTest>
#include <cassert>

namespace CollabText::Crdt {

NetworkSim::NetworkSim(int num_replicas, NetworkConfig config, uint64_t seed)
    : m_config(config), m_rng(seed)
{
    m_replicas.reserve(num_replicas);
    for (int i = 0; i < num_replicas; ++i)
        m_replicas.emplace_back(static_cast<uint16_t>(i + 1));
    m_queues.resize(num_replicas);
    m_offline.resize(num_replicas, false);
    m_pending_offline.resize(num_replicas);
}

uint64_t NetworkSim::random_latency() {
    if (m_config.min_latency_ms >= m_config.max_latency_ms)
        return m_config.min_latency_ms;
    uint32_t range = m_config.max_latency_ms - m_config.min_latency_ms;
    return m_config.min_latency_ms + (m_rng() % (range + 1));
}

void NetworkSim::broadcast(int source, const Operation& op) {
    for (int r = 0; r < static_cast<int>(m_replicas.size()); ++r) {
        if (r == source) continue;
        ScheduledOp sop;
        sop.deliver_at_ms = m_clock + random_latency();
        sop.from_replica = static_cast<uint16_t>(source + 1);
        sop.op = op;

        if (m_offline[r]) {
            m_pending_offline[r].push_back(sop);
        } else {
            m_queues[r].push(sop);
            // Duplicate with configured probability
            double roll = static_cast<double>(m_rng() % 10000) / 10000.0;
            if (roll < m_config.duplicate_probability) {
                ScheduledOp dup = sop;
                dup.deliver_at_ms = m_clock + random_latency();
                m_queues[r].push(dup);
            }
        }
    }
}

Operation NetworkSim::edit(int replica,
                           const std::vector<std::pair<uint32_t, uint32_t>>& ranges,
                           const std::vector<std::string>& new_text) {
    auto op = m_replicas[replica].apply_local_edit(ranges, new_text);
    broadcast(replica, op);
    return op;
}

Operation NetworkSim::edit(int replica, const EditAction& action) {
    return edit(replica, action.ranges, action.new_text);
}

std::optional<Operation> NetworkSim::undo(int replica) {
    auto op = m_replicas[replica].undo();
    if (op) broadcast(replica, *op);
    return op;
}

std::optional<Operation> NetworkSim::redo(int replica) {
    auto op = m_replicas[replica].redo();
    if (op) broadcast(replica, *op);
    return op;
}

void NetworkSim::disconnect(int replica) {
    m_offline[replica] = true;
}

void NetworkSim::reconnect(int replica) {
    m_offline[replica] = false;
    for (auto& sop : m_pending_offline[replica]) {
        sop.deliver_at_ms = m_clock + random_latency();
        m_queues[replica].push(sop);
    }
    m_pending_offline[replica].clear();
}

void NetworkSim::tick(uint64_t ms) {
    m_clock += ms;
    for (int r = 0; r < static_cast<int>(m_replicas.size()); ++r) {
        if (m_offline[r]) continue;
        std::vector<Operation> batch;
        while (!m_queues[r].empty() &&
               m_queues[r].top().deliver_at_ms <= m_clock) {
            batch.push_back(m_queues[r].top().op);
            m_queues[r].pop();
        }
        if (!batch.empty())
            m_replicas[r].apply_ops(batch);
    }
}

void NetworkSim::drain() {
    // Deliver everything
    m_clock = UINT64_MAX / 2;  // large but not overflow-prone
    tick(0);
    // Retry deferred ops
    for (int pass = 0; pass < 30; ++pass) {
        for (auto& buf : m_replicas)
            buf.apply_ops({});
    }
}

void NetworkSim::check_all_invariants(const char* context) const {
    for (int r = 0; r < static_cast<int>(m_replicas.size()); ++r) {
        const auto& buf = m_replicas[r];
        auto frags = buf.fragments();
        std::string text = buf.text();

        // INV-1: visible_length matches text
        if (buf.visible_length() != static_cast<uint32_t>(text.size()))
            QFAIL(qPrintable(QString("INV-1 at %1 r%2").arg(context).arg(r)));

        // INV-2 + INV-8: fragment byte sums match rope lengths
        uint32_t vis = 0, del = 0;
        for (auto& f : frags) {
            if (f.visible) vis += f.byte_length;
            else del += f.byte_length;
        }
        if (vis != buf.visible_rope_len())
            QFAIL(qPrintable(QString("INV-8 vis at %1 r%2").arg(context).arg(r)));
        if (del != buf.deleted_rope_len())
            QFAIL(qPrintable(QString("INV-8 del at %1 r%2").arg(context).arg(r)));

        // INV-4: fragment ordering
        for (size_t i = 1; i < frags.size(); ++i) {
            auto cmp = frags[i].locator <=> frags[i-1].locator;
            if (cmp < 0)
                QFAIL(qPrintable(QString("INV-4 at %1 r%2").arg(context).arg(r)));
            if (cmp == 0 && frags[i].origin <= frags[i-1].origin)
                QFAIL(qPrintable(QString("INV-4 at %1 r%2").arg(context).arg(r)));
        }

        // INV-5: non-empty fragments
        for (size_t i = 0; i < frags.size(); ++i) {
            if (frags[i].byte_length == 0 || frags[i].length == 0)
                QFAIL(qPrintable(QString("INV-5 at %1 r%2 i%3").arg(context).arg(r).arg(i)));
        }
    }
}

void NetworkSim::assert_convergence(const char* context) const {
    if (m_replicas.size() < 2) return;
    std::string expected = m_replicas[0].text();
    for (size_t r = 1; r < m_replicas.size(); ++r) {
        if (m_replicas[r].text() != expected) {
            qWarning("CONVERGENCE FAILURE at %s: r0 len=%zu, r%zu len=%zu",
                     context,
                     expected.size(), r, m_replicas[r].text().size());
            QFAIL(qPrintable(QString("Convergence failed at %1: r0 != r%2")
                .arg(context).arg(r)));
        }
    }
}

size_t NetworkSim::collect_garbage(int replica) {
    return m_replicas[replica].collect_garbage();
}

size_t NetworkSim::compact_all() {
    if (m_replicas.empty()) return 0;
    Global watermark = m_replicas[0].version();
    for (size_t r = 1; r < m_replicas.size(); ++r)
        watermark.meet(m_replicas[r].version());
    size_t total = 0;
    for (auto& buf : m_replicas)
        total += buf.compact(watermark);
    return total;
}

const Buffer& NetworkSim::buffer(int replica) const {
    return m_replicas[replica];
}

Buffer& NetworkSim::buffer(int replica) {
    return m_replicas[replica];
}

int NetworkSim::num_replicas() const {
    return static_cast<int>(m_replicas.size());
}

uint64_t NetworkSim::clock() const {
    return m_clock;
}

} // namespace CollabText::Crdt
```

- [ ] **Step 2: Add NetworkSim.cpp to CMakeLists.txt**

In `libs/collabtext/CMakeLists.txt`, add `src/crdt/NetworkSim.cpp` to the `add_library(collabtext STATIC ...)` block, after `src/crdt/Buffer.cpp`:

```cmake
    src/crdt/Buffer.cpp
    src/crdt/NetworkSim.cpp
```

- [ ] **Step 3: Build**

Run: `cmake -S libs/collabtext -B build-dev -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON 2>&1 | tail -3 && cmake --build build-dev -j$(nproc) 2>&1 | tail -5`
Expected: Builds successfully.

- [ ] **Step 4: Run existing tests to verify no regressions**

Run: `ctest --test-dir build-dev --output-on-failure -j4 --exclude-regex tst_benchmark 2>&1 | tail -5`
Expected: All 13 tests pass.

- [ ] **Step 5: Commit**

```bash
git add libs/collabtext/src/crdt/NetworkSim.cpp libs/collabtext/CMakeLists.txt
git commit -m "feat: NetworkSim implementation — latency, jitter, disconnect/reconnect"
```

---

### Task 4: NetworkSim Smoke Tests

**Files:**
- Create: `libs/collabtext/tests/tst_realistic.cpp`
- Modify: `libs/collabtext/CMakeLists.txt`

- [ ] **Step 1: Create tst_realistic.cpp with smoke tests for NetworkSim**

```cpp
#include <QTest>
#include "crdt/NetworkSim.h"
#include "crdt/EditStrategy.h"
#include <random>

using namespace CollabText::Crdt;

class TestRealistic : public QObject {
    Q_OBJECT
private slots:

    void networksim_basic_convergence() {
        NetworkSim net(3, {}, 42);
        net.edit(0, {{0, 0}}, {"hello"});
        net.tick(300);
        net.edit(1, {{0, 0}}, {"world "});
        net.tick(300);
        net.drain();
        net.check_all_invariants("basic");
        net.assert_convergence("basic");
    }

    void networksim_disconnect_reconnect() {
        NetworkSim net(2, {}, 42);
        net.edit(0, {{0, 0}}, {"abc"});
        net.tick(300);
        net.assert_convergence("before_disconnect");

        net.disconnect(1);
        net.edit(0, {{3, 3}}, {"def"});
        net.tick(300);
        // Replica 1 hasn't received "def"
        QCOMPARE(net.buffer(1).text(), std::string("abc"));

        net.reconnect(1);
        net.tick(300);
        net.drain();
        net.assert_convergence("after_reconnect");
    }

    void networksim_strategies_compile() {
        NetworkSim net(2, {.min_latency_ms = 0, .max_latency_ms = 0}, 42);
        std::mt19937 rng(42);

        RandomStrategy random;
        RealisticStrategy realistic;

        // Seed some text first
        net.edit(0, {{0, 0}}, {"hello world this is a test document"});
        net.tick(1);

        // RandomStrategy produces valid edits
        auto a1 = random.next_edit(net.buffer(0), rng);
        net.edit(0, a1);
        net.tick(1);
        net.check_all_invariants("random_edit");

        // RealisticStrategy produces valid edits
        auto a2 = realistic.next_edit(net.buffer(1), rng);
        net.edit(1, a2);
        net.tick(1);
        net.check_all_invariants("realistic_edit");

        net.drain();
        net.assert_convergence("strategies");
    }

    void networksim_compact_all() {
        NetworkSim net(2, {.min_latency_ms = 0, .max_latency_ms = 0}, 42);
        net.edit(0, {{0, 0}}, {"hello"});
        net.tick(1);
        net.edit(0, {{0, 5}}, {""});  // delete
        net.tick(1);
        net.drain();

        net.buffer(0).set_max_undo_depth(0);
        net.buffer(1).set_max_undo_depth(0);
        size_t removed = net.compact_all();
        QVERIFY(removed > 0);
        net.check_all_invariants("after_compact");
        net.assert_convergence("after_compact");
    }
};

QTEST_MAIN(TestRealistic)
#include "tst_realistic.moc"
```

- [ ] **Step 2: Register test in CMakeLists.txt**

Add after the `add_crdt_test(tst_gc)` line:

```cmake
add_crdt_test(tst_realistic)
```

- [ ] **Step 3: Build and run**

Run: `cmake -S libs/collabtext -B build-dev -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON 2>&1 | tail -3 && cmake --build build-dev --target tst_realistic -j$(nproc) 2>&1 | tail -5 && ./build-dev/tst_realistic 2>&1 | grep -E "PASS|FAIL|Totals"`
Expected: All 4 smoke tests pass.

- [ ] **Step 4: Commit**

```bash
git add libs/collabtext/tests/tst_realistic.cpp libs/collabtext/CMakeLists.txt
git commit -m "test: NetworkSim smoke tests — basic convergence, disconnect, strategies"
```

---

### Task 5: Correctness Test 1 — Sustained 3-Client Convergence

**Files:**
- Modify: `libs/collabtext/tests/tst_realistic.cpp`

- [ ] **Step 1: Add sustained_3_client_convergence test**

Add before the closing `};`:

```cpp
    void sustained_3_client_convergence() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        std::mt19937 rng(seed);

        NetworkSim net(3, {}, seed);
        RandomStrategy strategy;

        for (int i = 0; i < 1500; ++i) {  // 500 per replica on average
            int r = rng() % 3;
            int action = rng() % 100;

            if (action < 80) {
                auto a = strategy.next_edit(net.buffer(r), rng);
                net.edit(r, a);
            } else if (action < 90) {
                net.undo(r);
            } else {
                net.redo(r);
            }

            net.tick(10);  // 10ms between ops — continuous delivery

            if (i % 300 == 0)
                net.check_all_invariants(qPrintable(QString("step_%1").arg(i)));
        }

        net.drain();
        net.check_all_invariants("final");
        net.assert_convergence("sustained_3_client");
    }
```

- [ ] **Step 2: Build and run**

Run: `cmake --build build-dev --target tst_realistic -j$(nproc) 2>&1 | tail -3 && ./build-dev/tst_realistic sustained_3_client_convergence -v2 2>&1 | grep -E "PASS|FAIL|Totals|Seed"`
Expected: PASS.

- [ ] **Step 3: Run 5 times with different seeds**

Run: `for i in $(seq 1 5); do ./build-dev/tst_realistic sustained_3_client_convergence 2>&1 | grep -E "FAIL|Totals"; done`
Expected: 5/5 pass.

- [ ] **Step 4: Commit**

```bash
git add libs/collabtext/tests/tst_realistic.cpp
git commit -m "test: sustained 3-client convergence with latency + undo/redo"
```

---

### Task 6: Correctness Tests 2-3 — Disconnect/Reconnect

**Files:**
- Modify: `libs/collabtext/tests/tst_realistic.cpp`

- [ ] **Step 1: Add disconnect_reconnect_cycle test**

```cpp
    void disconnect_reconnect_cycle() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        std::mt19937 rng(seed);

        NetworkSim net(3, {}, seed);
        RandomStrategy strategy;

        // Phase 1: all online, 100 ops
        for (int i = 0; i < 100; ++i) {
            int r = rng() % 3;
            net.edit(r, strategy.next_edit(net.buffer(r), rng));
            net.tick(10);
        }
        net.check_all_invariants("phase1");

        // Phase 2: replica 2 disconnects, 200 ops between 0 and 1
        net.disconnect(2);
        for (int i = 0; i < 200; ++i) {
            int r = rng() % 2;  // only 0 or 1
            net.edit(r, strategy.next_edit(net.buffer(r), rng));
            net.tick(10);
        }
        net.check_all_invariants("phase2");

        // Phase 3: replica 2 reconnects, bulk sync
        net.reconnect(2);
        net.tick(500);  // allow bulk delivery

        // Phase 4: all edit 100 more ops
        for (int i = 0; i < 100; ++i) {
            int r = rng() % 3;
            net.edit(r, strategy.next_edit(net.buffer(r), rng));
            net.tick(10);
        }

        net.drain();
        net.check_all_invariants("final");
        net.assert_convergence("disconnect_reconnect");
    }
```

- [ ] **Step 2: Add cascading_disconnects test**

```cpp
    void cascading_disconnects() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        std::mt19937 rng(seed);

        NetworkSim net(5, {}, seed);
        RandomStrategy strategy;

        // Disconnect replicas one at a time
        for (int phase = 0; phase < 4; ++phase) {
            for (int i = 0; i < 50; ++i) {
                // Only online replicas edit
                std::vector<int> online;
                for (int r = 0; r < 5; ++r) {
                    // Replicas 1-4 disconnect at phases 0-3
                    // Replica 0 stays online
                    if (r == 0 || r > phase + 1) online.push_back(r);
                }
                if (online.empty()) break;
                int r = online[rng() % online.size()];
                net.edit(r, strategy.next_edit(net.buffer(r), rng));
                net.tick(10);
            }
            if (phase + 1 < 5)
                net.disconnect(phase + 1);
        }

        net.check_all_invariants("all_disconnected");

        // Reconnect in reverse order, with 50 ops between each
        for (int phase = 3; phase >= 0; --phase) {
            net.reconnect(phase + 1);
            net.tick(500);
            for (int i = 0; i < 50; ++i) {
                int r = rng() % 5;
                net.edit(r, strategy.next_edit(net.buffer(r), rng));
                net.tick(10);
            }
        }

        net.drain();
        net.check_all_invariants("final");
        net.assert_convergence("cascading_disconnects");
    }
```

- [ ] **Step 3: Build and run**

Run: `cmake --build build-dev --target tst_realistic -j$(nproc) 2>&1 | tail -3 && ./build-dev/tst_realistic 2>&1 | grep -E "PASS|FAIL|Totals"`
Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add libs/collabtext/tests/tst_realistic.cpp
git commit -m "test: disconnect/reconnect cycle and cascading disconnects"
```

---

### Task 7: Correctness Tests 4-5 — Long Partition with GC + Ten Client Sustained

**Files:**
- Modify: `libs/collabtext/tests/tst_realistic.cpp`

- [ ] **Step 1: Add long_partition_with_gc test**

```cpp
    void long_partition_with_gc() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        std::mt19937 rng(seed);

        NetworkSim net(2, {.min_latency_ms = 0, .max_latency_ms = 0}, seed);
        RandomStrategy strategy;

        // Build a shared 5K document
        while (net.buffer(0).visible_length() < 5000) {
            std::string chunk(100, static_cast<char>('a' + (rng() % 26)));
            net.edit(0, {{net.buffer(0).visible_length(),
                          net.buffer(0).visible_length()}}, {chunk});
        }
        net.drain();
        net.assert_convergence("initial_doc");

        // Partition: both edit independently for 500 ops
        net.disconnect(1);
        for (int i = 0; i < 500; ++i) {
            int r = i % 2;  // alternate, but 1 is offline so its ops aren't delivered
            net.edit(r, strategy.next_edit(net.buffer(r), rng));
        }
        net.check_all_invariants("partitioned");

        // Reconnect and sync
        net.reconnect(1);
        net.drain();
        net.check_all_invariants("after_sync");
        net.assert_convergence("after_sync");

        // GC: compute watermark, compact both
        net.buffer(0).set_max_undo_depth(0);
        net.buffer(1).set_max_undo_depth(0);
        size_t removed = net.compact_all();
        qDebug() << "Tombstones removed by compact:" << removed;

        net.check_all_invariants("after_gc");
        net.assert_convergence("after_gc");
    }
```

- [ ] **Step 2: Add ten_client_sustained test**

```cpp
    void ten_client_sustained() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        std::mt19937 rng(seed);

        NetworkSim net(10, {}, seed);
        RandomStrategy strategy;

        for (int i = 0; i < 2000; ++i) {
            int r = rng() % 10;

            int action = rng() % 100;
            if (action < 80) {
                net.edit(r, strategy.next_edit(net.buffer(r), rng));
            } else if (action < 90) {
                net.undo(r);
            } else {
                net.redo(r);
            }

            net.tick(5);

            // Random disconnects: 10% chance per 100 ops
            if (i % 100 == 0 && i > 0) {
                int target = rng() % 10;
                if (rng() % 10 == 0) {
                    net.disconnect(target);
                }
                // Reconnect a random offline replica
                for (int j = 0; j < 10; ++j) {
                    // Try reconnecting one that might be offline
                    // (reconnect is a no-op if already online)
                    int candidate = rng() % 10;
                    net.reconnect(candidate);
                    break;
                }
            }

            if (i % 500 == 0)
                net.check_all_invariants(qPrintable(QString("step_%1").arg(i)));
        }

        // Reconnect all before draining
        for (int r = 0; r < 10; ++r)
            net.reconnect(r);
        net.drain();
        net.check_all_invariants("final");
        net.assert_convergence("ten_client_sustained");
    }
```

- [ ] **Step 3: Build and run**

Run: `cmake --build build-dev --target tst_realistic -j$(nproc) 2>&1 | tail -3 && ./build-dev/tst_realistic 2>&1 | grep -E "PASS|FAIL|Totals"`
Expected: All 9 tests pass.

- [ ] **Step 4: Run 5 times for stability**

Run: `for i in $(seq 1 5); do ./build-dev/tst_realistic 2>&1 | grep -E "FAIL|Totals"; done`
Expected: 5/5 pass.

- [ ] **Step 5: Commit**

```bash
git add libs/collabtext/tests/tst_realistic.cpp
git commit -m "test: long partition with GC + 10-client sustained convergence"
```

---

### Task 8: Benchmarks — Realistic Throughput + Reconnect Cost + GC Under Load

**Files:**
- Modify: `libs/collabtext/tests/tst_benchmark.cpp`

- [ ] **Step 1: Add includes for NetworkSim and EditStrategy at the top of tst_benchmark.cpp**

After the existing includes, add:

```cpp
#include "crdt/NetworkSim.h"
#include "crdt/EditStrategy.h"
```

- [ ] **Step 2: Add realistic_3_client_throughput benchmark**

Add before the closing `};` of TestBenchmark:

```cpp
    void realistic_3_client_throughput() {
        qDebug() << "\n--- Realistic 3-Client Throughput ---";

        NetworkSim net(3, {.min_latency_ms = 100, .max_latency_ms = 100,
                           .duplicate_probability = 0.0}, 42);
        std::mt19937 rng(42);
        RealisticStrategy strategies[3];

        auto t0 = Clock::now();
        for (int i = 0; i < 1500; ++i) {  // 500 per replica
            int r = i % 3;
            auto action = strategies[r].next_edit(net.buffer(r), rng);
            net.edit(r, action);
            net.tick(2);  // 2ms between ops (fast typing)
        }
        net.drain();
        auto t1 = Clock::now();

        double wall_ms = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.0;
        qDebug().noquote() << QString("  1500 ops (3 clients x 500), wall time: %1 ms")
            .arg(wall_ms, 0, 'f', 1);
        qDebug().noquote() << QString("  Throughput: %1 ops/sec")
            .arg(1500.0 / (wall_ms / 1000.0), 0, 'f', 0);
        for (int r = 0; r < 3; ++r) {
            qDebug().noquote() << QString("  Replica %1: %2 fragments, %3 visible bytes")
                .arg(r)
                .arg(net.buffer(r).fragment_count())
                .arg(net.buffer(r).visible_length());
        }

        net.assert_convergence("realistic_throughput");
    }
```

- [ ] **Step 3: Add reconnect_sync_cost benchmark**

```cpp
    void reconnect_sync_cost() {
        qDebug() << "\n--- Reconnect Sync Cost ---";

        for (int n_edits : {100, 500, 1000}) {
            NetworkSim net(3, {.min_latency_ms = 0, .max_latency_ms = 0}, 42);
            std::mt19937 rng(42);
            RealisticStrategy strategy;

            // Seed some initial text
            net.edit(0, {{0, 0}}, {std::string(1000, 'x')});
            net.tick(1);
            net.drain();

            // Disconnect replica 2
            net.disconnect(2);

            // Replicas 0 and 1 make N edits each
            for (int i = 0; i < n_edits * 2; ++i) {
                int r = i % 2;
                net.edit(r, strategy.next_edit(net.buffer(r), rng));
                net.tick(1);
            }

            uint32_t frags_before = static_cast<uint32_t>(net.buffer(2).fragment_count());

            // Reconnect and measure sync time
            net.reconnect(2);
            auto t0 = Clock::now();
            net.drain();
            auto t1 = Clock::now();

            double sync_ms = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.0;
            uint32_t frags_after = static_cast<uint32_t>(net.buffer(2).fragment_count());

            qDebug().noquote() << QString("  N=%1 edits: sync=%2 ms, frags %3 -> %4")
                .arg(n_edits).arg(sync_ms, 0, 'f', 1)
                .arg(frags_before).arg(frags_after);

            net.assert_convergence("reconnect_sync");
        }
    }
```

- [ ] **Step 4: Add gc_under_sustained_editing benchmark**

```cpp
    void gc_under_sustained_editing() {
        qDebug() << "\n--- GC Under Sustained Editing ---";

        for (bool gc_enabled : {false, true}) {
            NetworkSim net(3, {.min_latency_ms = 0, .max_latency_ms = 0}, 42);
            std::mt19937 rng(42);
            RealisticStrategy strategies[3];

            qDebug().noquote() << QString("  GC %1:").arg(gc_enabled ? "ON" : "OFF");

            auto t_start = Clock::now();
            for (int i = 0; i < 2000; ++i) {
                int r = i % 3;
                net.edit(r, strategies[r].next_edit(net.buffer(r), rng));
                net.tick(1);

                // GC every 200 ops
                if (gc_enabled && i > 0 && i % 200 == 0) {
                    for (int gr = 0; gr < 3; ++gr)
                        net.collect_garbage(gr);
                }

                // Report at intervals
                if (i > 0 && i % 500 == 0) {
                    auto t_now = Clock::now();
                    double elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                        t_now - t_start).count() / 1000.0;
                    qDebug().noquote() << QString("    @%1 ops: %2 frags (r0), %3 ms elapsed")
                        .arg(i)
                        .arg(net.buffer(0).fragment_count())
                        .arg(elapsed, 0, 'f', 1);
                }
            }
            auto t_end = Clock::now();

            net.drain();
            double total_ms = std::chrono::duration_cast<std::chrono::microseconds>(
                t_end - t_start).count() / 1000.0;
            qDebug().noquote() << QString("    Final: %1 frags (r0), %2 ms total, %3 ops/sec")
                .arg(net.buffer(0).fragment_count())
                .arg(total_ms, 0, 'f', 1)
                .arg(2000.0 / (total_ms / 1000.0), 0, 'f', 0);
        }
    }
```

- [ ] **Step 5: Build and run benchmarks**

Run: `cmake --build build-dev --target tst_benchmark -j$(nproc) 2>&1 | tail -3 && timeout 600 ./build-dev/tst_benchmark realistic_3_client_throughput reconnect_sync_cost gc_under_sustained_editing -v2 2>&1 | grep -v "^libEGL\|^pci id\|^$\|^Config:\|^INFO\|LWP\|Thread"`
Expected: All 3 benchmarks produce numbers and pass.

- [ ] **Step 6: Commit**

```bash
git add libs/collabtext/tests/tst_benchmark.cpp
git commit -m "bench: realistic 3-client throughput, reconnect cost, GC under sustained editing"
```

---

### Task 9: Final Regression Suite

- [ ] **Step 1: Build and run the complete test suite**

Run:
```bash
cmake --build build-dev -j$(nproc) && ctest --test-dir build-dev --output-on-failure -j4 --exclude-regex tst_benchmark
```
Expected: All 14 tests pass (13 existing + tst_realistic).

- [ ] **Step 2: Run tst_realistic 5 times**

Run: `for i in $(seq 1 5); do echo "Run $i:"; ./build-dev/tst_realistic 2>&1 | grep -E "FAIL|Totals"; done`
Expected: 5/5 pass.

- [ ] **Step 3: Run tst_fuzz 5 times**

Run: `for i in $(seq 1 5); do ./build-dev/tst_fuzz 2>&1 | grep -E "FAIL|Totals"; done`
Expected: 5/5 pass.

- [ ] **Step 4: Commit any fixups**

If all tests pass, no commit needed. If fixups were required:
```bash
git add -u
git commit -m "fix: test adjustments from final regression run"
```
