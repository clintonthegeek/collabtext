#include <QTest>
#include "BenchmarkSupport.h"
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
#include <vector>
#include "TestActor.h"

using CollabText::Test::actor;

using namespace CollabText::Crdt;
using CollabText::Bench::Buffer;
using CollabText::Test::NetworkRecorder;
using CollabText::Test::Trace;
using CollabText::Test::TraceModel;
namespace Bench = CollabText::Bench;
using namespace CollabText::Bench;

// ============================================================================
// Helpers
// ============================================================================
//
// V06: timing is steady, every figure carries its unit and its work count, and
// a metric that was not measured is absent. See BenchmarkSupport.h.

/// Report a measured region. The caller names what `work_items` counts, so a
/// reader never has to infer the denominator, and the engine's structural size
/// is reported separately from process RSS.
static void report(const std::string& name, uint64_t wall_ns, uint64_t work_items,
                   const char* work_label, uint64_t source_edits = 0,
                   uint64_t remote_applications = 0, const Buffer* engine = nullptr) {
    Measurement measurement;
    measurement.name = name;
    measurement.wall_ns = wall_ns;
    measurement.work_items = work_items;
    measurement.work_label = work_label;
    measurement.source_edits = source_edits;
    measurement.remote_applications = remote_applications;
    measurement.rss_kb = process_rss_kb();
    if (engine) measurement.engine = engine_size(*engine);
    Bench::print(measurement);
}

/// Report a region measured by a Stopwatch.
static void report(const std::string& name, const Stopwatch& watch,
                   uint64_t work_items, const char* work_label,
                   uint64_t source_edits = 0, uint64_t remote_applications = 0,
                   const Buffer* engine = nullptr) {
    report(name, watch.elapsed_ns(), work_items, work_label, source_edits,
           remote_applications, engine);
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

// Tombstone count, used by the GC comparison's state check.
static size_t tombstone_count(const Buffer& buf) { return buf.tombstone_count(); }

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
    // 0. The report format itself
    // ========================================================================
    void measurement_reports_carry_units_and_honest_availability() {
        // A benchmark number is only evidence if a reader can tell what it
        // measured. These assertions are about the report format, not about any
        // engine performance, so they are cheap and must never be weakened to
        // accommodate a measurement.
        Measurement full;
        full.name = "sample";
        full.wall_ns = 2'000'000;
        full.work_items = 200;
        full.work_label = "local edits";
        full.source_edits = 200;
        full.remote_applications = 50;
        full.rss_kb = 4096;
        full.engine = EngineSize{128, 32, 1024, 2048};

        const QString text = QString::fromStdString(full.to_text());
        QVERIFY(text.contains("2 ms"));            // wall duration
        QVERIFY(text.contains("200 local edits")); // work count named
        QVERIFY(text.contains("10000 ns/local edits")); // per-unit figure
        QVERIFY(text.contains("process_rss=4096 KB"));
        QVERIFY(text.contains("tombstones=32"));
        QVERIFY(text.contains("remote_applications=50"));

        const QString json = QString::fromStdString(full.to_json());
        for (const char* key : {"\"wall_ns\":2000000", "\"work_items\":200",
                                "\"work_label\":\"local edits\"",
                                "\"ns_per_work_item\":10000",
                                "\"tombstones\":32", "\"visible_bytes\":1024",
                                "\"process_rss_kb\":4096"})
            QVERIFY2(json.contains(key), key);

        // An unmeasured metric is null/absent, never 0.
        Measurement empty;
        empty.name = "unmeasured";
        QVERIFY(!empty.ns_per_work_item().has_value());
        QVERIFY(!empty.work_per_sec().has_value());
        const QString empty_json = QString::fromStdString(empty.to_json());
        QVERIFY(empty_json.contains("\"ns_per_work_item\":null"));
        QVERIFY(empty_json.contains("\"process_rss_kb\":null"));
        QVERIFY(empty_json.contains("\"fragments\"") == false);
        const QString empty_text = QString::fromStdString(empty.to_text());
        QVERIFY(empty_text.contains("process_rss=absent"));
        // Wall duration is zero only because the region measured nothing; it is
        // still reported as a duration rather than omitted.
        QVERIFY(empty_text.contains("wall=0 ms"));
    }

    // ========================================================================
    // 1. Single-replica throughput at different document sizes
    // ========================================================================
    void single_replica_throughput() {
        qDebug().noquote() << "\n=== Single-Replica Throughput ===";
        for (uint32_t size : {1000u, 10000u, 100000u}) {
            std::mt19937 rng(42);
            Buffer buf(CollabText::Test::test_counter(1));
            build_document(buf, size, rng);

            const int NUM_OPS = 200;
            auto t0 = Clock::now();
            for (int i = 0; i < NUM_OPS; ++i)
                random_edit(buf, rng);
            auto t1 = Clock::now();

            int64_t total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                        report(std::to_string(size / 1000) + "K doc, random edit", total_ns, NUM_OPS, "local edits", NUM_OPS, 0, &buf);
        }
    }

    // ========================================================================
    // 2. Single-replica large doc (1M chars)
    // ========================================================================
    void single_replica_large_doc() {
        qDebug().noquote() << "\n=== Single-Replica Large Doc (1M) ===";
        std::mt19937 rng(42);
        Buffer buf(CollabText::Test::test_counter(1));
        build_document(buf, 1000000, rng, 1000);

        const int NUM_OPS = 100;
        auto t0 = Clock::now();
        for (int i = 0; i < NUM_OPS; ++i)
            random_edit(buf, rng);
        auto t1 = Clock::now();

        int64_t total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                report("1M doc, random edit", total_ns, NUM_OPS, "local edits", NUM_OPS, 0, &buf);
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
            Buffer buf(CollabText::Test::test_counter(1));
            build_document(buf, DOC_SIZE, rng);
            auto t0 = Clock::now();
            for (int i = 0; i < NUM_OPS; ++i) {
                uint32_t pos = buf.visible_length();
                std::string ins = random_ascii(rng, 1 + static_cast<int>(rng() % 5));
                buf.apply_local_edit({{pos, pos}}, {ins});
            }
            auto t1 = Clock::now();
            int64_t total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                        report("sequential (append)", total_ns, NUM_OPS, "local edits", NUM_OPS, 0, &buf);
        }

        // --- Random ---
        {
            std::mt19937 rng(42);
            Buffer buf(CollabText::Test::test_counter(1));
            build_document(buf, DOC_SIZE, rng);
            auto t0 = Clock::now();
            for (int i = 0; i < NUM_OPS; ++i)
                random_edit(buf, rng);
            auto t1 = Clock::now();
            int64_t total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                        report("random position", total_ns, NUM_OPS, "local edits", NUM_OPS, 0, &buf);
        }

        // --- Hotspot (bytes [5000, 5100]) ---
        {
            std::mt19937 rng(42);
            Buffer buf(CollabText::Test::test_counter(1));
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
                        report("hotspot [5000, 5100]", total_ns, NUM_OPS, "local edits", NUM_OPS, 0, &buf);
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
            Buffer buf(CollabText::Test::test_counter(1));
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
                        report("insert " + std::to_string(insert_size) + " chars", total_ns, NUM_OPS, "local edits", NUM_OPS, 0, &buf);
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
            Buffer buf(CollabText::Test::test_counter(1));
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
                        report(label, total_ns, NUM_OPS, "local edits", NUM_OPS, 0, &buf);
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
            Buffer buf(CollabText::Test::test_counter(1));
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
                        report(label, total_ns, NUM_OPS, "local edits", NUM_OPS, 0, &buf);
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
                replicas.emplace_back(CollabText::Test::test_counter(static_cast<uint8_t>(i + 1)));

            // Each replica performs edits independently, collecting operations
            std::vector<std::vector<Operation>> all_ops(K);
            for (int r = 0; r < K; ++r) {
                for (int e = 0; e < EDITS_PER_REPLICA; ++e) {
                    all_ops[r].push_back(random_edit(replicas[r], rng));
                }
            }

            // Cross-apply: each replica applies all OTHER replicas' ops
            // Timed region: every remote operation application, including the
            // deferred retry passes that actually finish the merge.
            uint64_t applications = 0;
            Stopwatch watch;
            for (int r = 0; r < K; ++r) {
                for (int s = 0; s < K; ++s) {
                    if (s == r) continue;
                    replicas[r].apply_ops(all_ops[s]);
                    applications += all_ops[s].size();
                }
                // Flush deferred
                for (int pass = 0; pass < 20; ++pass)
                    replicas[r].apply_ops({});
            }
            const uint64_t total_ns = watch.elapsed_ns();

            // Convergence is a correctness property, not a report line: a
            // benchmark that prints "converged: NO" and passes is worse than no
            // benchmark, because it looks like evidence.
            const std::string expected = replicas[0].text();
            for (int i = 1; i < K; ++i) {
                QVERIFY2(replicas[i].text() == expected,
                         qPrintable(QString("convergence benchmark K=%1: replica %2 "
                                            "differs from replica 0 (%3 vs %4 bytes)")
                                        .arg(K).arg(i)
                                        .arg(replicas[i].text().size())
                                        .arg(expected.size())));
            }

            char label[64];
            std::snprintf(label, sizeof(label), "K=%d replicas, %d edits each",
                          K, EDITS_PER_REPLICA);
            report(label, total_ns, applications, "remote applications",
                   static_cast<uint64_t>(K) * EDITS_PER_REPLICA, applications,
                   &replicas[0]);
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
            replicas.emplace_back(CollabText::Test::test_counter(static_cast<uint8_t>(i + 1)));

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

        const std::string expected = replicas[0].text();
        for (int i = 1; i < K; ++i) {
            QVERIFY2(replicas[i].text() == expected,
                     qPrintable(QString("hotspot benchmark: replica %1 differs from "
                                        "replica 0 (%2 vs %3 bytes)")
                                        .arg(i).arg(replicas[i].text().size())
                                        .arg(expected.size())));
        }

        report("K=5 hotspot [5000,5100]", total_ns,
               static_cast<uint64_t>(K) * EDITS_PER_REPLICA * (K - 1),
               "remote applications",
               static_cast<uint64_t>(K) * EDITS_PER_REPLICA,
               static_cast<uint64_t>(K) * EDITS_PER_REPLICA * (K - 1),
               &replicas[0]);
    }

    // ========================================================================
    // 9. Undo/redo cycle
    // ========================================================================
    void undo_redo_cycle() {
        qDebug().noquote() << "\n=== Undo/Redo Cycle (10K doc) ===";
        std::mt19937 rng(42);
        Buffer buf(CollabText::Test::test_counter(1));
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
                        report("edit phase (" + std::to_string(NUM_EDITS) + " edits)", edit_ns,
                   static_cast<uint64_t>(NUM_EDITS), "local edits",
                   static_cast<uint64_t>(NUM_EDITS), 0, &buf);
        }
        {
                        report("undo phase (" + std::to_string(undone) + " undos)", undo_ns,
                   static_cast<uint64_t>(undone), "undo operations",
                   static_cast<uint64_t>(undone), 0, &buf);
        }
        {
                        report("redo phase (" + std::to_string(redone) + " redos)", redo_ns,
                   static_cast<uint64_t>(redone), "redo operations",
                   static_cast<uint64_t>(redone), 0, &buf);
        }
    }

    // ========================================================================
    // 10. Undo stack depth
    // ========================================================================
    void undo_stack_depth() {
        qDebug().noquote() << "\n=== Undo Stack Depth (10K doc) ===";

        for (int N : {100, 500, 1000}) {
            std::mt19937 rng(42);
            Buffer buf(CollabText::Test::test_counter(1));
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
                        report(label, total_ns, static_cast<uint64_t>(undone),
                   "undo operations", static_cast<uint64_t>(undone), 0, &buf);
        }
    }

    // ========================================================================
    // 11. Fragment proliferation
    // ========================================================================
    void fragment_proliferation() {
        qDebug().noquote() << "\n=== Fragment Proliferation (10K doc + 2K single-char inserts) ===";
        std::mt19937 rng(42);
        Buffer buf(CollabText::Test::test_counter(1));
        build_document(buf, 10000, rng);

        const int TOTAL = 2000;
        const int INTERVAL = 500;

        // Cumulative region: fragments are reported structurally. The old
        // "mem_delta" column was process RSS minus a baseline sampled once,
        // which drifts with the allocator and says nothing about the engine.
        Stopwatch watch;
        const auto baseline_rss = process_rss_kb();
        for (int i = 1; i <= TOTAL; ++i) {
            // Single-char random insert (ASCII doc so all byte offsets valid)
            uint32_t len = buf.visible_length();
            uint32_t pos = len > 0 ? rng() % len : 0;
            char c = static_cast<char>('a' + (rng() % 26));
            buf.apply_local_edit({{pos, pos}}, {std::string(1, c)});

            if (i % INTERVAL == 0) {
                Measurement measurement;
                char label[80];
                std::snprintf(label, sizeof(label), "after %d inserts", i);
                measurement.name = label;
                measurement.wall_ns = watch.elapsed_ns();
                measurement.work_items = static_cast<uint64_t>(i);
                measurement.work_label = "local edits";
                measurement.source_edits = static_cast<uint64_t>(i);
                measurement.engine = engine_size(buf);
                // Process RSS, and explicitly as a delta from the sampled
                // baseline when both samples exist.
                measurement.rss_is_delta = true;
                const auto now_rss = process_rss_kb();
                measurement.rss_is_delta = true;
                if (now_rss && baseline_rss)
                    measurement.rss_kb = *now_rss - *baseline_rss;
                Bench::print(measurement);
            }
        }
    }

    // ========================================================================
    // 12. Tombstone-undo interaction
    // ========================================================================
    void tombstone_undo_interaction() {
        qDebug().noquote() << "\n=== Tombstone-Undo Interaction ===";
        std::mt19937 rng(42);
        Buffer buf(CollabText::Test::test_counter(1));
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

        char label[80];
        std::snprintf(label, sizeof(label), "undo %d deletions (%u tombstone frags)",
                      undone, frags_after_del);
        report(label, total_ns, static_cast<uint64_t>(undone), "undo operations",
               static_cast<uint64_t>(undone), 0, &buf);
    }

    // ========================================================================
    // 13. Memory growth
    // ========================================================================
    void memory_growth() {
        qDebug().noquote() << "\n=== Memory Growth (5000 edits from empty) ===";
        std::mt19937 rng(42);
        Buffer buf(CollabText::Test::test_counter(1));

        const int TOTAL = 5000;
        const int INTERVAL = 500;
        const auto baseline_rss = process_rss_kb();
        Stopwatch watch;

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
                const EngineSize size = engine_size(buf);
                // Structural counts, plus process RSS when available. RSS is
                // labelled as process RSS; engine memory is the fragment and
                // tombstone counts above it.
                const auto now_rss = process_rss_kb();
                Measurement measurement;
                measurement.name = "memory growth after " + std::to_string(i) + " edits";
                measurement.wall_ns = watch.elapsed_ns();
                measurement.work_items = static_cast<uint64_t>(i);
                measurement.work_label = "local edits";
                measurement.source_edits = static_cast<uint64_t>(i);
                measurement.engine = size;
                measurement.rss_is_delta = true;
                if (now_rss && baseline_rss)
                    measurement.rss_kb = *now_rss - *baseline_rss;
                Bench::print(measurement);
            }
        }
    }

    // ========================================================================
    // 13b. GC before/after on independently recreated identical state
    // ========================================================================
    void gc_effectiveness() {
        qDebug() << "\n--- GC Effectiveness (recreated identical state) ---";

        // The workload is recorded once — including its document build and its
        // tombstones — then replayed onto a fresh engine for each side of the
        // comparison. Measuring "before" and "after" on the same buffer
        // conflated the GC call with a warm allocator and a different fragment
        // layout. Recording the setup matters: a rebuild that silently dropped
        // the tombstones would compare an empty document with itself.
        Workload workload(CollabText::Test::test_counter(1), "gc-compare");
        std::mt19937 rng(42);
        while (workload.buffer().visible_length() < 5000) {
            const uint32_t end = workload.buffer().visible_length();
            workload.edit(end, end, random_ascii(rng, 100));
        }
        for (uint32_t i = 0; i < 2500; ++i) {
            const uint32_t len = workload.buffer().visible_length();
            if (len == 0) break;
            const uint32_t pos = rng() % len;
            workload.edit(pos, pos + 1, "");
        }
        const size_t tombstones_before = workload.buffer().tombstone_count();
        const size_t fragments_before = workload.buffer().fragment_count();
        const std::string recorded_text = workload.buffer().text();
        QVERIFY(tombstones_before > 0);

        const int OPS = 200;
        // A fixed decision sequence, so both runs execute identical work.
        std::vector<std::pair<uint32_t, uint32_t>> ranges;
        std::vector<std::string> payloads;
        for (int i = 0; i < OPS; ++i) {
            const uint32_t len = workload.buffer().visible_length();
            const uint32_t start = len > 0 ? rng() % len : 0;
            ranges.push_back({start, start});
            payloads.push_back(rng() % 3 != 0 ? random_ascii(rng, 3) : std::string());
        }

        // The rebuild must reproduce the recorded state exactly, or the two
        // sides are not the same experiment. Checked once, outside any lambda
        // that has to return a value.
        workload.rebuild();
        QCOMPARE(workload.buffer().tombstone_count(), tombstones_before);
        QCOMPARE(workload.buffer().fragment_count(), fragments_before);
        QCOMPARE(workload.buffer().text(), recorded_text);

        auto run = [&](bool collect) -> std::tuple<uint64_t, size_t, uint64_t, EngineSize> {
            workload.rebuild();
            Stopwatch watch;
            for (int i = 0; i < OPS; ++i)
                workload.buffer().apply_local_edit({ranges[i]}, {payloads[i]});
            const uint64_t ns = watch.elapsed_ns();
            size_t removed = 0;
            uint64_t gc_ns = 0;
            if (collect) {
                Stopwatch gc_watch;
                removed = workload.buffer().collect_garbage();
                gc_ns = gc_watch.elapsed_ns();
            }
            return std::tuple<uint64_t, size_t, uint64_t, EngineSize>(
                ns, removed, gc_ns, engine_size(workload.buffer()));
        };

        const auto before = run(false);
        const auto after = run(true);
        const uint64_t before_ns = std::get<0>(before);
        const EngineSize before_size = std::get<3>(before);
        const uint64_t after_ns = std::get<0>(after);
        const size_t removed = std::get<1>(after);
        const uint64_t gc_ns = std::get<2>(after);
        const EngineSize after_size = std::get<3>(after);

        report("edits before GC request", before_ns, OPS, "local edits",
               static_cast<uint64_t>(OPS), 0, &workload.buffer());
        report("edits after GC request", after_ns, OPS, "local edits",
               static_cast<uint64_t>(OPS), 0, &workload.buffer());
        Measurement gc_measurement;
        gc_measurement.name = "collect_garbage() request (timed)";
        gc_measurement.wall_ns = gc_ns;
        gc_measurement.work_items = 1;
        gc_measurement.work_label = "collect_garbage() calls";
        gc_measurement.engine = after_size;
        Bench::print(gc_measurement);

        // Containment (V03): physical reclamation is unavailable until a
        // certificate exists, so nothing may be reclaimed and no speedup may be
        // claimed. This asserts the contract rather than the old expectation
        // that reclamation succeeds.
        QCOMPARE(removed, 0u);
        QCOMPARE(before_size.units, after_size.units);
        QCOMPARE(before_size.tombstones, after_size.tombstones);

        qDebug().noquote()
            << QString("  containment: %1 tombstones retained (%2 fragments); "
                       "no reclamation speedup is claimed")
                   .arg(after_size.tombstones).arg(after_size.units);
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
        // Timed region: the local edits and the simulated delivery they cause,
        // including the final drain. Remote applications are counted, not
        // guessed from the local op count.
        const size_t deliveries_before = net.delivery_log().size();
        Stopwatch watch;
        for (int i = 0; i < TOTAL_OPS; ++i) {
            int r = i % 3;
            auto action = strategies[r].next_edit(net.buffer(r), rng);
            net.edit(r, action);
            net.tick(2);  // 2ms between ops (fast typing)
        }
        net.drain();
        const uint64_t wall_ns = watch.elapsed_ns();
        const uint64_t applications =
            static_cast<uint64_t>(net.delivery_log().size() - deliveries_before);

        report("realistic 3-client session", wall_ns, applications,
               "remote applications", static_cast<uint64_t>(TOTAL_OPS),
               applications, &net.buffer(0));
        for (int r = 0; r < 3; ++r) {
            qDebug().noquote() << QString("  Replica %1: %2 fragments, %3 visible bytes")
                .arg(r)
                .arg(net.buffer(r).fragment_count())
                .arg(net.buffer(r).visible_length());
        }

        // Convergence is asserted, not reported.
        const std::string expected = net.buffer(0).text();
        for (int r = 1; r < 3; ++r) {
            QVERIFY2(net.buffer(r).text() == expected,
                     qPrintable(QString("realistic_throughput: replica %1 differs "
                                        "from replica 0").arg(r)));
        }
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

            const uint32_t frags_before = static_cast<uint32_t>(net.buffer(2).fragment_count());
            const size_t deliveries_before = net.delivery_log().size();

            // The timed region is the catch-up itself: reconnect, deliver
            // everything that piled up, and settle the deferred operations.
            // Timing only the drain after a tick measured the leftovers, which
            // is why this benchmark used to report a near-zero "sync cost" for
            // work that had already happened outside the region.
            Stopwatch watch;
            net.reconnect(2);
            net.tick(10000);
            for (int pass = 0; pass < 20; ++pass)
                net.buffer(2).apply_ops({});
            const uint64_t sync_ns = watch.elapsed_ns();
            const uint64_t applied = net.delivery_log().size() - deliveries_before;

            net.drain();
            const uint32_t frags_after = static_cast<uint32_t>(net.buffer(2).fragment_count());
            const std::string expected = net.buffer(0).text();
            QVERIFY2(net.buffer(2).text() == expected,
                     qPrintable(QString("reconnect_sync N=%1: replica 2 did not "
                                        "converge").arg(n_edits)));

            char label[64];
            std::snprintf(label, sizeof(label), "N=%d edits: reconnect catch-up", n_edits);
            report(label, sync_ns, applied, "remote applications",
                   static_cast<uint64_t>(n_edits * 2), applied, &net.buffer(2));

            // The region did real work: deliveries were applied inside it.
            QVERIFY2(applied > 0, "the timed reconnect region applied no operations");
            QVERIFY2(sync_ns > 0, "the timed reconnect region measured zero duration");
            qDebug().noquote() << QString("  frags %1 -> %2").arg(frags_before).arg(frags_after);
        }
    }

    // ========================================================================
    // 15b. Controlled catch-up: the timed region contains the work
    // ========================================================================
    void catch_up_timed_region_contains_the_work() {
        // The acceptance case for the timing repair: a run whose timed region
        // provably contains real work, asserted without a speed threshold.
        NetworkRecorder recorded(3, {.min_latency_ms = 0, .max_latency_ms = 0}, 42,
                                 "catch-up");
        std::mt19937 rng(42);
        RealisticStrategy strategy;

        recorded.edit(0, {{0, 0}}, {std::string(2000, 'x')});
        recorded.tick(1);
        recorded.drain();
        recorded.disconnect(2);
        for (int i = 0; i < 300; ++i) {
            const int r = static_cast<int>(i % 2);
            const auto action = strategy.next_edit(recorded.buffer(r), rng);
            recorded.edit(r, action.ranges, action.new_text);
            recorded.tick(1);
        }

        // Recreate the identical partitioned network, then time the catch-up.
        NetworkSim net(3, recorded.trace().net, recorded.trace().seed);
        Bench::reexecute(recorded.trace(), net);
        QCOMPARE(net.buffer(2).text(), recorded.buffer(2).text());

        const size_t deliveries_before = net.delivery_log().size();
        const size_t fragments_before = net.buffer(2).fragment_count();
        const std::string lagging = net.buffer(2).text();

        Stopwatch watch;
        net.reconnect(2);
        net.tick(10000);
        for (int pass = 0; pass < 20; ++pass)
            net.buffer(2).apply_ops({});
        const uint64_t catch_up_ns = watch.elapsed_ns();
        const uint64_t applied = net.delivery_log().size() - deliveries_before;

        Measurement measurement;
        measurement.name = "controlled catch-up after partition";
        measurement.wall_ns = catch_up_ns;
        measurement.work_items = applied;
        measurement.work_label = "remote applications";
        measurement.source_edits = 300;
        measurement.remote_applications = applied;
        measurement.rss_kb = process_rss_kb();
        measurement.engine = engine_size(net.buffer(2));
        Bench::print(measurement);

        // Nonzero work, nonzero time — no speed threshold, because a
        // threshold on shared CI hardware measures the hardware.
        QVERIFY2(applied > 0, "the catch-up region applied no operations");
        QVERIFY2(catch_up_ns > 0, "the catch-up region measured zero duration");
        QVERIFY2(net.buffer(2).fragment_count() > fragments_before,
                 "the catch-up region did not change the lagging replica");
        QVERIFY2(net.buffer(2).text() != lagging,
                 "the catch-up region left the lagging replica unchanged");

        net.drain();
        const std::string expected = net.buffer(0).text();
        QVERIFY2(net.buffer(2).text() == expected,
                 "replicas did not converge after the catch-up");
    }

    // ========================================================================
    // 16. GC under sustained editing
    // ========================================================================
    void gc_under_sustained_editing() {
        qDebug() << "\n--- GC Under Sustained Editing ---";

        // Record the session once, then run it twice on independently recreated
        // networks: once with GC requests and once without. Running them in one
        // process on the same buffers would let the first run warm the allocator
        // and grow the trees the second run then reuses.
        const int TOTAL_GC_OPS = 600;
        const int GC_INTERVAL = 100;

        Trace session;
        session.model = TraceModel::NetworkSim;
        session.label = "gc-under-sustained-editing";
        session.seed = 42;
        session.replicas = 3;
        session.net = NetworkConfig{.min_latency_ms = 0, .max_latency_ms = 0};
        {
            NetworkRecorder record(3, session.net, session.seed, session.label);
            std::mt19937 rng(42);
            RealisticStrategy strategies[3];
            for (int i = 0; i < TOTAL_GC_OPS; ++i) {
                const int r = i % 3;
                const auto action = strategies[r].next_edit(record.buffer(r), rng);
                record.edit(r, action.ranges, action.new_text);
                record.tick(1);
                if (i > 0 && i % GC_INTERVAL == 0) {
                    for (int gr = 0; gr < 3; ++gr) {
                        record.collect_garbage(gr);
                        Event gc;
                        gc.kind = EventKind::CollectGarbage;
                        gc.replica = gr;
                        record.trace().events.push_back(gc);
                    }
                }
            }
            record.drain();
            session.events = record.trace().events;
        }

        auto run = [&](bool with_gc) {
            NetworkSim net(3, session.net, session.seed);
            Stopwatch watch;
            uint64_t gc_calls = 0;
            uint64_t produced = 0;
            for (const Event& event : session.events) {
                switch (event.kind) {
                case EventKind::Edit:
                    net.edit(event.replica, {{event.start, event.end}}, {event.text});
                    ++produced;
                    break;
                case EventKind::CollectGarbage:
                    net.collect_garbage(event.replica);
                    ++gc_calls;
                    break;
                case EventKind::Tick: net.tick(event.ms); break;
                case EventKind::Drain: net.drain(); break;
                default: break;
                }
            }
            return std::tuple<uint64_t, uint64_t, uint64_t, EngineSize>(
                watch.elapsed_ns(), produced, gc_calls, engine_size(net.buffer(0)));
        };

        const auto without = run(false);
        const auto with = run(true);

        report("sustained editing, no GC requests", std::get<0>(without),
               std::get<1>(without), "local edits", std::get<1>(without), 0);
        report("sustained editing, GC requests every 100 ops", std::get<0>(with),
               std::get<1>(with), "local edits", std::get<1>(with), 0);

        Measurement gc_calls_measurement;
        gc_calls_measurement.name = "collect_garbage() requests issued";
        gc_calls_measurement.wall_ns = 0;
        gc_calls_measurement.work_items = std::get<2>(with);
        gc_calls_measurement.work_label = "collect_garbage() calls";
        gc_calls_measurement.engine = std::get<3>(with);
        Bench::print(gc_calls_measurement);

        // Containment (V03): the two runs must end in identical engine state,
        // because no reclamation is permitted yet. A difference here would mean
        // a GC request changed something it is not allowed to change.
        QCOMPARE(std::get<3>(without).units, std::get<3>(with).units);
        QCOMPARE(std::get<3>(without).tombstones, std::get<3>(with).tombstones);
        QCOMPARE(std::get<3>(without).visible_bytes, std::get<3>(with).visible_bytes);
    }
};

QTEST_MAIN(TestBenchmark)
#include "tst_benchmark.moc"
