#include <QTest>
#include "crdt/Buffer.h"
#include "crdt/NetworkSim.h"
#include "crdt/EditStrategy.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <numeric>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

using namespace CollabText::Crdt;
using Clock = std::chrono::high_resolution_clock;

// ============================================================================
// Helpers
// ============================================================================

struct BenchResult {
    std::string name;
    int64_t ns_per_op = 0;
    double ops_per_sec = 0;
    uint32_t total_fragments = 0;
    uint32_t visible_bytes = 0;
    int64_t memory_delta_kb = 0;
};

static void report(const BenchResult& r) {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "  %-42s %'10lld ns/op  %'12.0f ops/sec  frags=%u  vis=%u  mem_delta=%lldKB",
        r.name.c_str(),
        static_cast<long long>(r.ns_per_op),
        r.ops_per_sec,
        r.total_fragments,
        r.visible_bytes,
        static_cast<long long>(r.memory_delta_kb));
    qDebug().noquote() << buf;
}

static int64_t rss_kb() {
    std::ifstream f("/proc/self/statm");
    if (!f.is_open()) return 0;
    long pages_total, pages_resident;
    f >> pages_total >> pages_resident;
    long page_size = sysconf(_SC_PAGESIZE);
    return (pages_resident * page_size) / 1024;
}

// Generate a random UTF-8 string of 1..maxChars characters (reuses tst_fuzz pattern)
static std::string random_text(std::mt19937& rng, int maxChars) {
    int count = 1 + static_cast<int>(rng() % static_cast<unsigned>(maxChars));
    std::string result;
    result.reserve(count * 2);
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

// Find a valid UTF-8 byte boundary in text
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

// Generate ASCII string of exactly N chars (for fast bulk building)
static std::string random_ascii(std::mt19937& rng, int count) {
    std::string result(count, ' ');
    for (int i = 0; i < count; ++i)
        result[i] = static_cast<char>('a' + (rng() % 26));
    return result;
}

// Build a document of approximately target_bytes by inserting random text
// in chunks of chunk_size characters for speed.
static void build_document(Buffer& buf, uint32_t target_bytes, std::mt19937& rng,
                           int chunk_size = 100) {
    while (buf.visible_length() < target_bytes) {
        uint32_t remaining = target_bytes - buf.visible_length();
        int this_chunk = std::min(chunk_size, static_cast<int>(remaining));
        std::string text = random_ascii(rng, this_chunk);
        uint32_t pos = buf.visible_length(); // append at end
        buf.apply_local_edit({{pos, pos}}, {text});
    }
}

// Delete `fraction` of the document one character at a time (creating tombstones).
// Each deletion creates exactly one tombstone fragment.
// Optimization: since build_document uses ASCII only, every byte is a character
// boundary -- we can use visible_length() directly instead of calling text().
static void create_tombstones(Buffer& buf, double fraction, std::mt19937& rng) {
    uint32_t initial = buf.visible_length();
    uint32_t to_delete = static_cast<uint32_t>(initial * fraction);
    for (uint32_t i = 0; i < to_delete; ++i) {
        uint32_t len = buf.visible_length();
        if (len == 0) break;
        // Since the document is ASCII, every byte offset is valid
        uint32_t pos = rng() % len;
        buf.apply_local_edit({{pos, pos + 1}}, {""});
    }
}

// Perform a random edit on a buffer and return the operation.
// Uses ASCII-only positions and replacements for speed -- avoids O(N)
// text() call and character boundary scanning.
static Operation random_edit(Buffer& buf, std::mt19937& rng) {
    uint32_t len = buf.visible_length();
    uint32_t start = len > 0 ? rng() % len : 0;
    uint32_t end = start;
    if (start < len) {
        uint32_t max_del = std::min(static_cast<uint32_t>(5), len - start);
        end = start + (max_del > 0 ? rng() % (max_del + 1) : 0);
    }
    std::string replacement;
    if (rng() % 3 != 0) {
        int count = 1 + static_cast<int>(rng() % 5);
        replacement = random_ascii(rng, count);
    }
    return buf.apply_local_edit({{start, end}}, {replacement});
}

// Perform an edit at a specific byte range
static Operation edit_at(Buffer& buf, uint32_t start, uint32_t end,
                         const std::string& replacement) {
    return buf.apply_local_edit({{start, end}}, {replacement});
}

// ============================================================================
// Benchmark test class
// ============================================================================

class TestBenchmark : public QObject {
    Q_OBJECT

private slots:
    // ========================================================================
    // 1. Single-replica throughput at different document sizes
    // ========================================================================
    void single_replica_throughput() {
        qDebug().noquote() << "\n=== Single-Replica Throughput ===";
        for (uint32_t size : {1000u, 10000u, 100000u}) {
            std::mt19937 rng(42);
            Buffer buf(1);
            build_document(buf, size, rng);

            const int NUM_OPS = 200;
            auto t0 = Clock::now();
            for (int i = 0; i < NUM_OPS; ++i)
                random_edit(buf, rng);
            auto t1 = Clock::now();

            int64_t total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            BenchResult r;
            r.name = std::to_string(size / 1000) + "K doc, random edit";
            r.ns_per_op = total_ns / NUM_OPS;
            r.ops_per_sec = NUM_OPS * 1e9 / total_ns;
            r.total_fragments = static_cast<uint32_t>(buf.fragments().size());
            r.visible_bytes = buf.visible_length();
            report(r);
        }
    }

    // ========================================================================
    // 2. Single-replica large doc (1M chars)
    // ========================================================================
    void single_replica_large_doc() {
        qDebug().noquote() << "\n=== Single-Replica Large Doc (1M) ===";
        std::mt19937 rng(42);
        Buffer buf(1);
        build_document(buf, 1000000, rng, 1000);

        const int NUM_OPS = 100;
        auto t0 = Clock::now();
        for (int i = 0; i < NUM_OPS; ++i)
            random_edit(buf, rng);
        auto t1 = Clock::now();

        int64_t total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        BenchResult r;
        r.name = "1M doc, random edit";
        r.ns_per_op = total_ns / NUM_OPS;
        r.ops_per_sec = NUM_OPS * 1e9 / total_ns;
        r.total_fragments = static_cast<uint32_t>(buf.fragments().size());
        r.visible_bytes = buf.visible_length();
        report(r);
    }

    // ========================================================================
    // 3. Edit patterns: sequential vs random vs hotspot
    // ========================================================================
    void edit_patterns() {
        qDebug().noquote() << "\n=== Edit Patterns (100K doc) ===";
        const int NUM_OPS = 200;
        const uint32_t DOC_SIZE = 100000;

        // --- Sequential (append at end) ---
        {
            std::mt19937 rng(42);
            Buffer buf(1);
            build_document(buf, DOC_SIZE, rng);
            auto t0 = Clock::now();
            for (int i = 0; i < NUM_OPS; ++i) {
                uint32_t pos = buf.visible_length();
                std::string ins = random_ascii(rng, 1 + static_cast<int>(rng() % 5));
                buf.apply_local_edit({{pos, pos}}, {ins});
            }
            auto t1 = Clock::now();
            int64_t total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            BenchResult r;
            r.name = "sequential (append)";
            r.ns_per_op = total_ns / NUM_OPS;
            r.ops_per_sec = NUM_OPS * 1e9 / total_ns;
            r.total_fragments = static_cast<uint32_t>(buf.fragments().size());
            r.visible_bytes = buf.visible_length();
            report(r);
        }

        // --- Random ---
        {
            std::mt19937 rng(42);
            Buffer buf(1);
            build_document(buf, DOC_SIZE, rng);
            auto t0 = Clock::now();
            for (int i = 0; i < NUM_OPS; ++i)
                random_edit(buf, rng);
            auto t1 = Clock::now();
            int64_t total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            BenchResult r;
            r.name = "random position";
            r.ns_per_op = total_ns / NUM_OPS;
            r.ops_per_sec = NUM_OPS * 1e9 / total_ns;
            r.total_fragments = static_cast<uint32_t>(buf.fragments().size());
            r.visible_bytes = buf.visible_length();
            report(r);
        }

        // --- Hotspot (bytes [5000, 5100]) ---
        {
            std::mt19937 rng(42);
            Buffer buf(1);
            build_document(buf, DOC_SIZE, rng);
            auto t0 = Clock::now();
            for (int i = 0; i < NUM_OPS; ++i) {
                uint32_t vis = buf.visible_length();
                uint32_t lo = std::min(5000u, vis);
                uint32_t hi = std::min(5100u, vis);
                uint32_t start = lo + (hi > lo ? rng() % (hi - lo) : 0);
                uint32_t end = start + std::min(static_cast<uint32_t>(1 + rng() % 3), vis - start);
                std::string ins;
                if (rng() % 2 == 0) ins = random_ascii(rng, 1 + static_cast<int>(rng() % 3));
                buf.apply_local_edit({{start, end}}, {ins});
            }
            auto t1 = Clock::now();
            int64_t total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            BenchResult r;
            r.name = "hotspot [5000, 5100]";
            r.ns_per_op = total_ns / NUM_OPS;
            r.ops_per_sec = NUM_OPS * 1e9 / total_ns;
            r.total_fragments = static_cast<uint32_t>(buf.fragments().size());
            r.visible_bytes = buf.visible_length();
            report(r);
        }
    }

    // ========================================================================
    // 4. Edit sizes: compare different insert sizes
    // ========================================================================
    void edit_sizes() {
        qDebug().noquote() << "\n=== Edit Sizes (100K doc) ===";
        const int NUM_OPS = 100;
        const uint32_t DOC_SIZE = 100000;

        for (int insert_size : {1, 10, 100, 1000}) {
            std::mt19937 rng(42);
            Buffer buf(1);
            build_document(buf, DOC_SIZE, rng);

            auto t0 = Clock::now();
            for (int i = 0; i < NUM_OPS; ++i) {
                uint32_t len = buf.visible_length();
                uint32_t pos = len > 0 ? rng() % len : 0;
                std::string ins = random_ascii(rng, insert_size);
                buf.apply_local_edit({{pos, pos}}, {ins});
            }
            auto t1 = Clock::now();

            int64_t total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            BenchResult r;
            r.name = "insert " + std::to_string(insert_size) + " chars";
            r.ns_per_op = total_ns / NUM_OPS;
            r.ops_per_sec = NUM_OPS * 1e9 / total_ns;
            r.total_fragments = static_cast<uint32_t>(buf.fragments().size());
            r.visible_bytes = buf.visible_length();
            report(r);
        }
    }

    // ========================================================================
    // 5. Tombstone degradation (5K doc)
    // ========================================================================
    void tombstone_degradation() {
        qDebug().noquote() << "\n=== Tombstone Degradation (5K doc) ===";
        const int NUM_OPS = 200;

        for (double frac : {0.0, 0.5, 0.9}) {
            std::mt19937 rng(42);
            Buffer buf(1);
            build_document(buf, 5000, rng);

            if (frac > 0.0)
                create_tombstones(buf, frac, rng);

            uint32_t frags_before = static_cast<uint32_t>(buf.fragments().size());

            auto t0 = Clock::now();
            for (int i = 0; i < NUM_OPS; ++i)
                random_edit(buf, rng);
            auto t1 = Clock::now();

            int64_t total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            char label[64];
            std::snprintf(label, sizeof(label), "%.0f%% tombstones", frac * 100);
            BenchResult r;
            r.name = label;
            r.ns_per_op = total_ns / NUM_OPS;
            r.ops_per_sec = NUM_OPS * 1e9 / total_ns;
            r.total_fragments = static_cast<uint32_t>(buf.fragments().size());
            r.visible_bytes = buf.visible_length();
            report(r);
            qDebug().noquote() << "    frags before edits:" << frags_before;
        }
    }

    // ========================================================================
    // 6. Tombstone degradation large (10K doc)
    //    Note: scaled down from 100K to keep char-by-char tombstone
    //    creation feasible. Compare with benchmark 5 to see scaling.
    // ========================================================================
    void tombstone_degradation_large() {
        qDebug().noquote() << "\n=== Tombstone Degradation Scaling ===";
        qDebug().noquote() << "  (Measures how doc size amplifies tombstone cost)";
        const int NUM_OPS = 50;

        // Compare 3K vs 5K vs 8K docs, all at 50% tombstones.
        // Shows whether tombstone cost scales linearly with total fragments.
        for (uint32_t doc_size : {3000u, 5000u, 8000u}) {
            std::mt19937 rng(42);
            Buffer buf(1);
            build_document(buf, doc_size, rng);

            create_tombstones(buf, 0.5, rng);

            uint32_t frags_before = static_cast<uint32_t>(buf.fragments().size());

            auto t0 = Clock::now();
            for (int i = 0; i < NUM_OPS; ++i)
                random_edit(buf, rng);
            auto t1 = Clock::now();

            int64_t total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            char label[64];
            std::snprintf(label, sizeof(label), "%uK, 50%% tombstones (%u frags)",
                          doc_size / 1000, frags_before);
            BenchResult r;
            r.name = label;
            r.ns_per_op = total_ns / NUM_OPS;
            r.ops_per_sec = NUM_OPS * 1e9 / total_ns;
            r.total_fragments = static_cast<uint32_t>(buf.fragments().size());
            r.visible_bytes = buf.visible_length();
            report(r);
        }
    }

    // ========================================================================
    // 7. Multi-replica convergence
    // ========================================================================
    void multi_replica_convergence() {
        qDebug().noquote() << "\n=== Multi-Replica Convergence ===";
        const int EDITS_PER_REPLICA = 50;

        for (int K : {2, 5, 10}) {
            std::mt19937 rng(42);

            // Create K replicas
            std::vector<Buffer> replicas;
            for (int i = 0; i < K; ++i)
                replicas.emplace_back(static_cast<uint16_t>(i + 1));

            // Each replica performs edits independently, collecting operations
            std::vector<std::vector<Operation>> all_ops(K);
            for (int r = 0; r < K; ++r) {
                for (int e = 0; e < EDITS_PER_REPLICA; ++e) {
                    all_ops[r].push_back(random_edit(replicas[r], rng));
                }
            }

            // Cross-apply: each replica applies all OTHER replicas' ops
            auto t0 = Clock::now();
            for (int r = 0; r < K; ++r) {
                for (int s = 0; s < K; ++s) {
                    if (s == r) continue;
                    replicas[r].apply_ops(all_ops[s]);
                }
                // Flush deferred
                for (int pass = 0; pass < 20; ++pass)
                    replicas[r].apply_ops({});
            }
            auto t1 = Clock::now();

            int64_t total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

            // Verify convergence
            std::string expected = replicas[0].text();
            bool converged = true;
            for (int i = 1; i < K; ++i) {
                if (replicas[i].text() != expected) {
                    converged = false;
                    qDebug() << "  DIVERGENCE: replica" << i << "differs from replica 0";
                }
            }

            char label[64];
            std::snprintf(label, sizeof(label), "K=%d replicas, %d edits each", K, EDITS_PER_REPLICA);
            BenchResult r;
            r.name = label;
            r.ns_per_op = total_ns;  // total convergence time
            r.ops_per_sec = (K * EDITS_PER_REPLICA) * 1e9 / total_ns;
            r.total_fragments = static_cast<uint32_t>(replicas[0].fragments().size());
            r.visible_bytes = replicas[0].visible_length();
            report(r);
            qDebug().noquote() << "    converged:" << (converged ? "YES" : "NO");
        }
    }

    // ========================================================================
    // 8. Multi-replica hotspot
    // ========================================================================
    void multi_replica_hotspot() {
        qDebug().noquote() << "\n=== Multi-Replica Hotspot ===";
        const int K = 5;
        const int EDITS_PER_REPLICA = 50;

        std::mt19937 rng(42);

        // Build shared base on replica 1, then sync to all others
        std::vector<Buffer> replicas;
        for (int i = 0; i < K; ++i)
            replicas.emplace_back(static_cast<uint16_t>(i + 1));

        // Replica 0 builds the doc, collecting ops to broadcast
        std::vector<Operation> base_ops;
        {
            Buffer& r0 = replicas[0];
            uint32_t target = 10000;
            while (r0.visible_length() < target) {
                uint32_t remaining = target - r0.visible_length();
                int chunk = std::min(100, static_cast<int>(remaining));
                std::string text = random_ascii(rng, chunk);
                uint32_t pos = r0.visible_length();
                base_ops.push_back(r0.apply_local_edit({{pos, pos}}, {text}));
            }
        }
        // Apply base ops to all other replicas
        for (int i = 1; i < K; ++i) {
            replicas[i].apply_ops(base_ops);
            for (int pass = 0; pass < 10; ++pass)
                replicas[i].apply_ops({});
        }

        // Each replica edits bytes [5000, 5100]
        std::vector<std::vector<Operation>> all_ops(K);
        for (int r = 0; r < K; ++r) {
            for (int e = 0; e < EDITS_PER_REPLICA; ++e) {
                uint32_t vis = replicas[r].visible_length();
                uint32_t lo = std::min(5000u, vis);
                uint32_t hi = std::min(5100u, vis);
                uint32_t start = lo + (hi > lo ? rng() % (hi - lo) : 0);
                uint32_t end = start + std::min(static_cast<uint32_t>(1 + rng() % 3), vis - start);
                std::string ins;
                if (rng() % 2 == 0) ins = random_ascii(rng, 1 + static_cast<int>(rng() % 3));
                all_ops[r].push_back(
                    replicas[r].apply_local_edit({{start, end}}, {ins}));
            }
        }

        // Cross-apply
        auto t0 = Clock::now();
        for (int r = 0; r < K; ++r) {
            for (int s = 0; s < K; ++s) {
                if (s == r) continue;
                replicas[r].apply_ops(all_ops[s]);
            }
            for (int pass = 0; pass < 20; ++pass)
                replicas[r].apply_ops({});
        }
        auto t1 = Clock::now();

        int64_t total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

        std::string expected = replicas[0].text();
        bool converged = true;
        for (int i = 1; i < K; ++i) {
            if (replicas[i].text() != expected) converged = false;
        }

        BenchResult r;
        r.name = "K=5 hotspot [5000,5100]";
        r.ns_per_op = total_ns;
        r.ops_per_sec = (K * EDITS_PER_REPLICA) * 1e9 / total_ns;
        r.total_fragments = static_cast<uint32_t>(replicas[0].fragments().size());
        r.visible_bytes = replicas[0].visible_length();
        report(r);
        qDebug().noquote() << "    converged:" << (converged ? "YES" : "NO");
    }

    // ========================================================================
    // 9. Undo/redo cycle
    // ========================================================================
    void undo_redo_cycle() {
        qDebug().noquote() << "\n=== Undo/Redo Cycle (10K doc) ===";
        std::mt19937 rng(42);
        Buffer buf(1);
        build_document(buf, 10000, rng);

        const int NUM_EDITS = 500;

        // Phase 1: edits
        auto t0 = Clock::now();
        for (int i = 0; i < NUM_EDITS; ++i)
            random_edit(buf, rng);
        auto t1 = Clock::now();
        int64_t edit_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

        // Phase 2: undo all
        auto t2 = Clock::now();
        int undone = 0;
        for (int i = 0; i < NUM_EDITS; ++i) {
            auto op = buf.undo();
            if (op) ++undone;
        }
        auto t3 = Clock::now();
        int64_t undo_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();

        // Phase 3: redo all
        auto t4 = Clock::now();
        int redone = 0;
        for (int i = 0; i < undone; ++i) {
            auto op = buf.redo();
            if (op) ++redone;
        }
        auto t5 = Clock::now();
        int64_t redo_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t5 - t4).count();

        {
            BenchResult r;
            r.name = "edit phase (" + std::to_string(NUM_EDITS) + " edits)";
            r.ns_per_op = edit_ns / NUM_EDITS;
            r.ops_per_sec = NUM_EDITS * 1e9 / edit_ns;
            r.total_fragments = static_cast<uint32_t>(buf.fragments().size());
            r.visible_bytes = buf.visible_length();
            report(r);
        }
        {
            BenchResult r;
            r.name = "undo phase (" + std::to_string(undone) + " undos)";
            r.ns_per_op = undone > 0 ? undo_ns / undone : 0;
            r.ops_per_sec = undone > 0 ? undone * 1e9 / undo_ns : 0;
            r.total_fragments = static_cast<uint32_t>(buf.fragments().size());
            r.visible_bytes = buf.visible_length();
            report(r);
        }
        {
            BenchResult r;
            r.name = "redo phase (" + std::to_string(redone) + " redos)";
            r.ns_per_op = redone > 0 ? redo_ns / redone : 0;
            r.ops_per_sec = redone > 0 ? redone * 1e9 / redo_ns : 0;
            r.total_fragments = static_cast<uint32_t>(buf.fragments().size());
            r.visible_bytes = buf.visible_length();
            report(r);
        }
    }

    // ========================================================================
    // 10. Undo stack depth
    // ========================================================================
    void undo_stack_depth() {
        qDebug().noquote() << "\n=== Undo Stack Depth (10K doc) ===";

        for (int N : {100, 500, 1000}) {
            std::mt19937 rng(42);
            Buffer buf(1);
            build_document(buf, 10000, rng);

            // Perform N edits
            for (int i = 0; i < N; ++i)
                random_edit(buf, rng);

            // Undo all N
            auto t0 = Clock::now();
            int undone = 0;
            for (int i = 0; i < N; ++i) {
                auto op = buf.undo();
                if (op) ++undone;
            }
            auto t1 = Clock::now();
            int64_t total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

            char label[64];
            std::snprintf(label, sizeof(label), "undo N=%d edits", N);
            BenchResult r;
            r.name = label;
            r.ns_per_op = undone > 0 ? total_ns / undone : 0;
            r.ops_per_sec = undone > 0 ? undone * 1e9 / total_ns : 0;
            r.total_fragments = static_cast<uint32_t>(buf.fragments().size());
            r.visible_bytes = buf.visible_length();
            report(r);
        }
    }

    // ========================================================================
    // 11. Fragment proliferation
    // ========================================================================
    void fragment_proliferation() {
        qDebug().noquote() << "\n=== Fragment Proliferation (10K doc + 2K single-char inserts) ===";
        std::mt19937 rng(42);
        Buffer buf(1);
        build_document(buf, 10000, rng);

        int64_t baseline_mem = rss_kb();
        const int TOTAL = 2000;
        const int INTERVAL = 500;

        auto t_start = Clock::now();
        for (int i = 1; i <= TOTAL; ++i) {
            // Single-char random insert (ASCII doc so all byte offsets valid)
            uint32_t len = buf.visible_length();
            uint32_t pos = len > 0 ? rng() % len : 0;
            char c = static_cast<char>('a' + (rng() % 26));
            buf.apply_local_edit({{pos, pos}}, {std::string(1, c)});

            if (i % INTERVAL == 0) {
                auto t_now = Clock::now();
                int64_t elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_now - t_start).count();
                double ops_sec = i * 1e9 / elapsed_ns;
                int64_t mem_now = rss_kb();

                char label[80];
                std::snprintf(label, sizeof(label), "after %d inserts", i);
                BenchResult r;
                r.name = label;
                r.ns_per_op = elapsed_ns / i;
                r.ops_per_sec = ops_sec;
                r.total_fragments = static_cast<uint32_t>(buf.fragments().size());
                r.visible_bytes = buf.visible_length();
                r.memory_delta_kb = mem_now - baseline_mem;
                report(r);
            }
        }
    }

    // ========================================================================
    // 12. Tombstone-undo interaction
    // ========================================================================
    void tombstone_undo_interaction() {
        qDebug().noquote() << "\n=== Tombstone-Undo Interaction ===";
        std::mt19937 rng(42);
        Buffer buf(1);
        build_document(buf, 2000, rng);

        // Delete 80% char-by-char: ~1600 edits, each creating one tombstone.
        uint32_t initial_len = buf.visible_length();
        uint32_t to_delete = static_cast<uint32_t>(initial_len * 0.8);

        auto t_del_start = Clock::now();
        for (uint32_t i = 0; i < to_delete; ++i) {
            uint32_t len = buf.visible_length();
            if (len == 0) break;
            uint32_t pos = rng() % len;
            buf.apply_local_edit({{pos, pos + 1}}, {""});
        }
        auto t_del_end = Clock::now();
        int64_t del_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_del_end - t_del_start).count();

        uint32_t frags_after_del = static_cast<uint32_t>(buf.fragments().size());
        qDebug().noquote() << "  setup: deleted" << to_delete << "chars in"
                           << (del_ns / 1000000) << "ms — vis=" << buf.visible_length()
                           << " frags=" << frags_after_del;

        // Undo the last 500 deletions — measure undo cost with heavy tombstones
        const int UNDO_COUNT = 500;
        auto t0 = Clock::now();
        int undone = 0;
        for (int i = 0; i < UNDO_COUNT; ++i) {
            auto op = buf.undo();
            if (op) ++undone;
        }
        auto t1 = Clock::now();
        int64_t total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

        BenchResult r;
        char label[80];
        std::snprintf(label, sizeof(label), "undo %d deletions (%u tombstone frags)", undone, frags_after_del);
        r.name = label;
        r.ns_per_op = undone > 0 ? total_ns / undone : 0;
        r.ops_per_sec = undone > 0 ? undone * 1e9 / total_ns : 0;
        r.total_fragments = static_cast<uint32_t>(buf.fragments().size());
        r.visible_bytes = buf.visible_length();
        report(r);
    }

    // ========================================================================
    // 13. Memory growth
    // ========================================================================
    void memory_growth() {
        qDebug().noquote() << "\n=== Memory Growth (5000 edits from empty) ===";
        std::mt19937 rng(42);
        Buffer buf(1);

        int64_t baseline_mem = rss_kb();
        const int TOTAL = 5000;
        const int INTERVAL = 500;

        for (int i = 1; i <= TOTAL; ++i) {
            // Mix of insert and delete (ASCII only for speed)
            uint32_t len = buf.visible_length();
            if (len < 10 || rng() % 3 != 0) {
                // Insert
                int count = 1 + static_cast<int>(rng() % 5);
                std::string ins = random_ascii(rng, count);
                uint32_t pos = len > 0 ? rng() % len : 0;
                buf.apply_local_edit({{pos, pos}}, {ins});
            } else {
                // Delete 1 byte (ASCII)
                uint32_t pos = rng() % len;
                buf.apply_local_edit({{pos, pos + 1}}, {""});
            }

            if (i % INTERVAL == 0) {
                int64_t mem_now = rss_kb();
                auto frags = buf.fragments();
                uint32_t tombstone_frags = 0;
                for (auto& f : frags) {
                    if (!f.visible) ++tombstone_frags;
                }

                char line[256];
                std::snprintf(line, sizeof(line),
                    "  edit %5d: vis=%6u  frags=%5u  tombstone_frags=%5u  mem=%lldKB (delta=%lldKB)",
                    i,
                    buf.visible_length(),
                    static_cast<uint32_t>(frags.size()),
                    tombstone_frags,
                    static_cast<long long>(mem_now),
                    static_cast<long long>(mem_now - baseline_mem));
                qDebug().noquote() << line;
            }
        }
    }

    void gc_effectiveness() {
        qDebug() << "\n--- GC Effectiveness ---";

        std::mt19937 rng(42);
        Buffer buf(1);
        buf.set_max_undo_depth(0);  // everything GC-eligible immediately
        build_document(buf, 5000, rng, 100);

        size_t frags_clean = buf.fragment_count();
        qDebug().noquote() << QString("  Clean doc:   %1 fragments").arg(frags_clean);

        create_tombstones(buf, 0.5, rng);
        size_t frags_dirty = buf.fragment_count();
        size_t tombstones = buf.tombstone_count();
        qDebug().noquote() << QString("  After 50%% tombstones: %1 fragments (%2 tombstones)")
            .arg(frags_dirty).arg(tombstones);

        // Measure edit throughput BEFORE GC
        auto t0 = Clock::now();
        int ops_before = 0;
        while (Clock::now() - t0 < std::chrono::milliseconds(500)) {
            random_edit(buf, rng);
            ++ops_before;
        }
        auto elapsed_before = Clock::now() - t0;
        double ns_before = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed_before).count()
                           / static_cast<double>(ops_before);

        // Run GC
        auto gc_start = Clock::now();
        size_t removed = buf.collect_garbage();
        auto gc_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - gc_start).count();

        size_t frags_after = buf.fragment_count();
        qDebug().noquote() << QString("  After GC:    %1 fragments (removed %2, GC took %3 us)")
            .arg(frags_after).arg(removed).arg(gc_elapsed);

        // Measure edit throughput AFTER GC
        auto t1 = Clock::now();
        int ops_after = 0;
        while (Clock::now() - t1 < std::chrono::milliseconds(500)) {
            random_edit(buf, rng);
            ++ops_after;
        }
        auto elapsed_after = Clock::now() - t1;
        double ns_after = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed_after).count()
                          / static_cast<double>(ops_after);

        double speedup = ns_before / ns_after;
        qDebug().noquote() << QString("  Before GC: %1 ns/op (%2 ops/sec)")
            .arg(ns_before, 0, 'f', 0).arg(1e9 / ns_before, 0, 'f', 0);
        qDebug().noquote() << QString("  After GC:  %1 ns/op (%2 ops/sec)")
            .arg(ns_after, 0, 'f', 0).arg(1e9 / ns_after, 0, 'f', 0);
        qDebug().noquote() << QString("  Speedup:   %1x").arg(speedup, 0, 'f', 1);

        QVERIFY2(removed > 0, "GC should have removed tombstones");
        QVERIFY2(frags_after < frags_dirty, "Fragment count should decrease after GC");
    }

    // ========================================================================
    // 14. Realistic 3-client throughput
    // ========================================================================
    void realistic_3_client_throughput() {
        qDebug() << "\n--- Realistic 3-Client Throughput ---";

        NetworkSim net(3, {.min_latency_ms = 100, .max_latency_ms = 100,
                           .duplicate_probability = 0.0}, 42);
        std::mt19937 rng(42);
        RealisticStrategy strategies[3];

        const int TOTAL_OPS = 300;  // 100 per replica (reduced for wall-time budget)
        auto t0 = Clock::now();
        for (int i = 0; i < TOTAL_OPS; ++i) {
            int r = i % 3;
            auto action = strategies[r].next_edit(net.buffer(r), rng);
            net.edit(r, action);
            net.tick(2);  // 2ms between ops (fast typing)
        }
        net.drain();
        auto t1 = Clock::now();

        double wall_ms = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.0;
        qDebug().noquote() << QString("  %1 ops (3 clients x %2), wall time: %3 ms")
            .arg(TOTAL_OPS).arg(TOTAL_OPS / 3).arg(wall_ms, 0, 'f', 1);
        qDebug().noquote() << QString("  Throughput: %1 ops/sec")
            .arg(TOTAL_OPS / (wall_ms / 1000.0), 0, 'f', 0);
        for (int r = 0; r < 3; ++r) {
            qDebug().noquote() << QString("  Replica %1: %2 fragments, %3 visible bytes")
                .arg(r)
                .arg(net.buffer(r).fragment_count())
                .arg(net.buffer(r).visible_length());
        }

        net.assert_convergence("realistic_throughput");
    }

    // ========================================================================
    // 15. Reconnect sync cost
    // ========================================================================
    void reconnect_sync_cost() {
        qDebug() << "\n--- Reconnect Sync Cost ---";

        for (int n_edits : {50, 100, 200}) {
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

            // Reconnect and measure sync time (tick first to deliver bulk pending ops)
            net.reconnect(2);
            net.tick(10000);
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

    // ========================================================================
    // 16. GC under sustained editing
    // ========================================================================
    void gc_under_sustained_editing() {
        qDebug() << "\n--- GC Under Sustained Editing ---";

        for (bool gc_enabled : {false, true}) {
            NetworkSim net(3, {.min_latency_ms = 0, .max_latency_ms = 0}, 42);
            std::mt19937 rng(42);
            RealisticStrategy strategies[3];

            qDebug().noquote() << QString("  GC %1:").arg(gc_enabled ? "ON" : "OFF");

            const int TOTAL_GC_OPS = 600;  // reduced for wall-time budget (O(n^2) fragment cost)
            const int GC_INTERVAL = 100;
            const int REPORT_INTERVAL = 200;

            auto t_start = Clock::now();
            for (int i = 0; i < TOTAL_GC_OPS; ++i) {
                int r = i % 3;
                net.edit(r, strategies[r].next_edit(net.buffer(r), rng));
                net.tick(1);

                // GC every GC_INTERVAL ops
                if (gc_enabled && i > 0 && i % GC_INTERVAL == 0) {
                    for (int gr = 0; gr < 3; ++gr)
                        net.collect_garbage(gr);
                }

                // Report at intervals
                if (i > 0 && i % REPORT_INTERVAL == 0) {
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
                .arg(TOTAL_GC_OPS / (total_ms / 1000.0), 0, 'f', 0);
        }
    }
};

QTEST_MAIN(TestBenchmark)
#include "tst_benchmark.moc"
