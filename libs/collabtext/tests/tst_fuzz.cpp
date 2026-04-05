#include <QTest>
#include "crdt/Buffer.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <random>
#include <set>

using namespace CollabText::Crdt;

// ============================================================================
// Invariant checkers — these verify structural properties after every operation
// ============================================================================

static void check_invariants(const Buffer& buf, const char* context) {
    auto frags = buf.fragments();

    // INV-1: visible_length == text().size()
    std::string text = buf.text();
    if (buf.visible_length() != static_cast<uint32_t>(text.size())) {
        QFAIL(qPrintable(QString("INV-1 violated at %1: visible_length=%2 but text.size=%3")
            .arg(context).arg(buf.visible_length()).arg(text.size())));
    }

    // INV-2: visible_length == sum of visible fragment byte sizes
    uint32_t vis_sum = 0;
    uint32_t del_sum = 0;
    for (auto& f : frags) {
        if (f.visible)
            vis_sum += f.byte_length;
        else
            del_sum += f.byte_length;
    }
    if (vis_sum != buf.visible_length()) {
        QFAIL(qPrintable(QString("INV-2 violated at %1: fragment vis_sum=%2 but visible_length=%3")
            .arg(context).arg(vis_sum).arg(buf.visible_length())));
    }

    // INV-3: (subsumed by INV-1 + INV-2 + INV-8 — visible text consistency)

    // INV-4: fragment ordering — (locator, origin) strictly non-decreasing
    for (size_t i = 1; i < frags.size(); ++i) {
        auto cmp = frags[i].locator <=> frags[i-1].locator;
        if (cmp < 0) {
            QFAIL(qPrintable(QString("INV-4 violated at %1: frag[%2].locator < frag[%3].locator")
                .arg(context).arg(i).arg(i-1)));
        }
        if (cmp == 0) {
            if (frags[i].origin < frags[i-1].origin) {
                QFAIL(qPrintable(QString("INV-4 violated at %1: same locator but frag[%2].origin < frag[%3].origin")
                    .arg(context).arg(i).arg(i-1)));
            }
            // Same locator, same origin would mean duplicate fragments
            if (frags[i].origin == frags[i-1].origin) {
                QFAIL(qPrintable(QString("INV-4 violated at %1: duplicate fragment at index %2")
                    .arg(context).arg(i)));
            }
        }
    }

    // INV-5: every fragment has non-zero byte_length and length > 0
    for (size_t i = 0; i < frags.size(); ++i) {
        if (frags[i].byte_length == 0 || frags[i].length == 0) {
            QFAIL(qPrintable(QString("INV-5 violated at %1: empty fragment at index %2")
                .arg(context).arg(i)));
        }
    }

    // INV-6: total character count consistency
    {
        uint32_t total_chars = 0;
        for (auto& f : frags) {
            if (f.visible) total_chars += f.length;
        }
        uint32_t actual_chars = 0;
        for (size_t b = 0; b < text.size(); ) {
            unsigned char c = static_cast<unsigned char>(text[b]);
            if (c < 0x80) b += 1;
            else if ((c & 0xE0) == 0xC0) b += 2;
            else if ((c & 0xF0) == 0xE0) b += 3;
            else b += 4;
            ++actual_chars;
        }
        if (total_chars != actual_chars) {
            QFAIL(qPrintable(QString("INV-6 violated at %1: fragment char sum=%2 but text chars=%3")
                .arg(context).arg(total_chars).arg(actual_chars)));
        }
    }

    // INV-7: byte_length >= length (multi-byte chars make bytes > chars)
    for (size_t i = 0; i < frags.size(); ++i) {
        if (frags[i].byte_length < frags[i].length) {
            QFAIL(qPrintable(QString("INV-7 violated at %1: frag[%2] byte_length=%3 < length=%4")
                .arg(context).arg(i).arg(frags[i].byte_length).arg(frags[i].length)));
        }
    }

    // INV-8: rope consistency — rope byte lengths match fragment sums
    if (buf.visible_rope_len() != vis_sum) {
        QFAIL(qPrintable(QString("INV-8 violated at %1: visible_rope_len=%2 but fragment vis_sum=%3")
            .arg(context).arg(buf.visible_rope_len()).arg(vis_sum)));
    }
    if (buf.deleted_rope_len() != del_sum) {
        QFAIL(qPrintable(QString("INV-8 violated at %1: deleted_rope_len=%2 but fragment del_sum=%3")
            .arg(context).arg(buf.deleted_rope_len()).arg(del_sum)));
    }

    // INV-9: byte_length sums match rope lengths
    uint32_t byte_sum_vis = 0, byte_sum_del = 0;
    for (auto& f : frags) {
        if (f.visible) byte_sum_vis += f.byte_length;
        else byte_sum_del += f.byte_length;
    }
    if (byte_sum_vis != buf.visible_rope_len()) {
        QFAIL(qPrintable(QString("INV-9 violated at %1: byte_length vis sum=%2 but rope=%3")
            .arg(context).arg(byte_sum_vis).arg(buf.visible_rope_len())));
    }
    if (byte_sum_del != buf.deleted_rope_len()) {
        QFAIL(qPrintable(QString("INV-9 violated at %1: byte_length del sum=%2 but rope=%3")
            .arg(context).arg(byte_sum_del).arg(buf.deleted_rope_len())));
    }
}

// ============================================================================
// Test class
// ============================================================================

class TestFuzz : public QObject {
    Q_OBJECT

private:
    // Generate a random UTF-8 string of 1-maxChars characters
    static std::string random_text(std::mt19937& rng, int maxChars) {
        int count = 1 + (rng() % maxChars);
        std::string result;
        for (int i = 0; i < count; ++i) {
            int kind = rng() % 100;
            if (kind < 60) {
                // ASCII
                result += static_cast<char>('a' + (rng() % 26));
            } else if (kind < 75) {
                // 2-byte UTF-8 (é range: U+00C0 to U+00FF)
                result += '\xc3';
                result += static_cast<char>(0xa0 + (rng() % 32));
            } else if (kind < 90) {
                // 3-byte UTF-8 (CJK: U+4E00 range)
                result += '\xe4';
                result += static_cast<char>(0xb8 + (rng() % 4));
                result += static_cast<char>(0x80 + (rng() % 64));
            } else {
                // 4-byte UTF-8 (emoji: U+1F600 range)
                result += '\xf0';
                result += '\x9f';
                result += static_cast<char>(0x98 + (rng() % 8));
                result += static_cast<char>(0x80 + (rng() % 64));
            }
        }
        return result;
    }

    // Find a valid UTF-8 byte offset in text (don't split multi-byte chars)
    static uint32_t random_byte_offset(std::mt19937& rng, const std::string& text) {
        if (text.empty()) return 0;
        // Build list of valid byte offsets (character boundaries)
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

    // Generate a random edit on a buffer
    Operation random_edit(Buffer& buf, std::mt19937& rng) {
        std::string text = buf.text();
        uint32_t len = static_cast<uint32_t>(text.size());
        uint32_t start = random_byte_offset(rng, text);
        // end >= start, also on a char boundary
        uint32_t end = start;
        if (start < len) {
            std::string suffix = text.substr(start);
            end = start + random_byte_offset(rng, suffix);
        }
        std::string replacement;
        if (rng() % 3 != 0) { // 2/3 chance of inserting text
            replacement = random_text(rng, 5);
        }
        return buf.apply_local_edit({{start, end}}, {replacement});
    }

private slots:

    // =======================================================================
    // PROPERTY: Invariants hold after every single operation
    // =======================================================================

    void invariants_after_random_edits() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        std::mt19937 rng(seed);

        Buffer buf(1);
        check_invariants(buf, "initial");

        for (int i = 0; i < 200; ++i) {
            random_edit(buf, rng);
            check_invariants(buf, qPrintable(QString("edit_%1").arg(i)));
        }
    }

    void invariants_after_undo_redo_cycles() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        std::mt19937 rng(seed);

        Buffer buf(1);
        // Build up some state
        for (int i = 0; i < 30; ++i) {
            random_edit(buf, rng);
        }
        check_invariants(buf, "after_30_edits");

        // Undo everything
        for (int i = 0; i < 30; ++i) {
            auto op = buf.undo();
            if (!op) break;
            check_invariants(buf, qPrintable(QString("undo_%1").arg(i)));
        }

        // Should be empty or close to it
        // (some remote-like ops might not undo cleanly, but invariants must hold)

        // Redo everything
        for (int i = 0; i < 30; ++i) {
            auto op = buf.redo();
            if (!op) break;
            check_invariants(buf, qPrintable(QString("redo_%1").arg(i)));
        }
    }

    // =======================================================================
    // PROPERTY: Convergence with multi-byte UTF-8
    // =======================================================================

    void convergence_utf8_stress() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        std::mt19937 rng(seed);

        Buffer bufA(1), bufB(2), bufC(3);
        bufA.set_max_undo_depth(30);
        bufB.set_max_undo_depth(30);
        bufC.set_max_undo_depth(30);
        std::vector<std::vector<Operation>> queues(3);

        auto broadcast = [&](const Operation& op, int source) {
            for (int r = 0; r < 3; ++r) {
                if (r == source) continue;
                // Random position insertion (out-of-order delivery)
                size_t pos = queues[r].empty() ? 0 : (rng() % (queues[r].size() + 1));
                queues[r].insert(queues[r].begin() + static_cast<ptrdiff_t>(pos), op);
            }
        };

        Buffer* bufs[] = {&bufA, &bufB, &bufC};

        for (int i = 0; i < 100; ++i) {
            int action = rng() % 100;
            if (action < 50) {
                // Random edit with UTF-8 text
                int r = rng() % 3;
                auto op = random_edit(*bufs[r], rng);
                broadcast(op, r);
            } else if (action < 65) {
                // Undo/redo
                int r = rng() % 3;
                std::optional<Operation> op;
                if (rng() % 2 == 0) op = bufs[r]->undo();
                else op = bufs[r]->redo();
                if (op) broadcast(*op, r);
            } else if (action < 80) {
                // GC on a random replica
                int r = rng() % 3;
                bufs[r]->collect_garbage();
            } else {
                // Deliver some ops
                int r = rng() % 3;
                if (!queues[r].empty()) {
                    int count = 1 + (rng() % std::min<int>(5, static_cast<int>(queues[r].size())));
                    std::vector<Operation> batch;
                    for (int c = 0; c < count && !queues[r].empty(); ++c) {
                        batch.push_back(queues[r].front());
                        queues[r].erase(queues[r].begin());
                    }
                    bufs[r]->apply_ops(batch);
                }
            }

            // Check invariants on all replicas periodically
            if (i % 20 == 0) {
                for (int r = 0; r < 3; ++r) {
                    check_invariants(*bufs[r], qPrintable(
                        QString("replica_%1_step_%2").arg(r).arg(i)));
                }
            }
        }

        // Drain all queues
        for (int r = 0; r < 3; ++r) {
            if (!queues[r].empty()) {
                bufs[r]->apply_ops(queues[r]);
                queues[r].clear();
            }
        }
        for (int pass = 0; pass < 20; ++pass) {
            for (auto* b : bufs) b->apply_ops({});
        }

        // Final invariant check
        for (int r = 0; r < 3; ++r) {
            check_invariants(*bufs[r], qPrintable(QString("final_replica_%1").arg(r)));
        }

        // Convergence
        if (bufA.text() != bufB.text() || bufB.text() != bufC.text()) {
            std::cerr << "CONVERGENCE FAILURE (seed " << seed << ")\n";
            std::cerr << "A: \"" << bufA.text() << "\"\n";
            std::cerr << "B: \"" << bufB.text() << "\"\n";
            std::cerr << "C: \"" << bufC.text() << "\"\n";
        }
        QCOMPARE(bufA.text(), bufB.text());
        QCOMPARE(bufB.text(), bufC.text());
    }

    // =======================================================================
    // ADVERSARIAL: All replicas insert at position 0
    // =======================================================================

    void adversarial_all_insert_at_zero() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        std::mt19937 rng(seed);

        constexpr int N_REPLICAS = 5;
        constexpr int N_OPS = 50;

        std::vector<Buffer> bufs;
        for (int i = 0; i < N_REPLICAS; ++i)
            bufs.emplace_back(static_cast<uint16_t>(i + 1));

        std::vector<Operation> all_ops;

        // Every replica inserts at position 0
        for (int round = 0; round < N_OPS; ++round) {
            int r = rng() % N_REPLICAS;
            std::string text = random_text(rng, 3);
            auto op = bufs[r].apply_local_edit({{0, 0}}, {text});
            all_ops.push_back(op);
            check_invariants(bufs[r], qPrintable(
                QString("insert_at_0_r%1_round%2").arg(r).arg(round)));
        }

        // Deliver all ops to all replicas (random order per replica)
        for (int r = 0; r < N_REPLICAS; ++r) {
            auto shuffled = all_ops;
            std::shuffle(shuffled.begin(), shuffled.end(), rng);
            bufs[r].apply_ops(shuffled);
            for (int pass = 0; pass < 10; ++pass) bufs[r].apply_ops({});
            check_invariants(bufs[r], qPrintable(QString("after_merge_r%1").arg(r)));
        }

        // Convergence
        for (int r = 1; r < N_REPLICAS; ++r) {
            QCOMPARE(bufs[r].text(), bufs[0].text());
        }
    }

    // =======================================================================
    // ADVERSARIAL: All replicas edit the same single character
    // =======================================================================

    void adversarial_fight_over_one_char() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        std::mt19937 rng(seed);

        // Start with "X" on all replicas
        Buffer bufA(1), bufB(2), bufC(3);
        auto init = bufA.apply_local_edit({{0, 0}}, {"X"});
        bufB.apply_ops({init});
        bufC.apply_ops({init});

        std::vector<Operation> all_ops;

        Buffer* bufs[] = {&bufA, &bufB, &bufC};

        // Each replica repeatedly deletes X and inserts something else at 0
        for (int round = 0; round < 30; ++round) {
            for (int r = 0; r < 3; ++r) {
                uint32_t len = bufs[r]->visible_length();
                if (len > 0) {
                    // Delete everything, insert new char
                    auto op = bufs[r]->apply_local_edit({{0, len}},
                        {random_text(rng, 1)});
                    all_ops.push_back(op);
                } else {
                    auto op = bufs[r]->apply_local_edit({{0, 0}},
                        {random_text(rng, 1)});
                    all_ops.push_back(op);
                }
            }
        }

        // Deliver all ops to all replicas
        for (auto* b : bufs) {
            auto shuffled = all_ops;
            std::shuffle(shuffled.begin(), shuffled.end(), rng);
            b->apply_ops(shuffled);
            for (int pass = 0; pass < 20; ++pass) b->apply_ops({});
        }

        for (auto* b : bufs) {
            check_invariants(*b, "fight_over_one_char_final");
        }

        QCOMPARE(bufA.text(), bufB.text());
        QCOMPARE(bufB.text(), bufC.text());
    }

    // =======================================================================
    // ADVERSARIAL: Deep undo/redo chains with interleaved remote ops
    // =======================================================================

    void deep_undo_redo_with_remote() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        std::mt19937 rng(seed);

        Buffer bufA(1), bufB(2);
        std::vector<Operation> a_ops, b_ops;

        // A builds up 20 edits
        for (int i = 0; i < 20; ++i) {
            auto op = random_edit(bufA, rng);
            a_ops.push_back(op);
        }

        // B builds up 20 edits
        for (int i = 0; i < 20; ++i) {
            auto op = random_edit(bufB, rng);
            b_ops.push_back(op);
        }

        // A undoes 10
        std::vector<Operation> a_undos;
        for (int i = 0; i < 10; ++i) {
            auto op = bufA.undo();
            if (op) a_undos.push_back(*op);
        }
        check_invariants(bufA, "after_10_undos");

        // Now A receives all of B's ops
        bufA.apply_ops(b_ops);
        check_invariants(bufA, "after_receiving_b_ops");

        // A redoes 5
        std::vector<Operation> a_redos;
        for (int i = 0; i < 5; ++i) {
            auto op = bufA.redo();
            if (op) a_redos.push_back(*op);
            check_invariants(bufA, qPrintable(QString("redo_%1").arg(i)));
        }

        // B receives all of A's ops + undos + redos
        bufB.apply_ops(a_ops);
        bufB.apply_ops(a_undos);
        bufB.apply_ops(a_redos);
        for (int pass = 0; pass < 20; ++pass) {
            bufA.apply_ops({});
            bufB.apply_ops({});
        }

        check_invariants(bufA, "final_a");
        check_invariants(bufB, "final_b");
        QCOMPARE(bufA.text(), bufB.text());
    }

    // =======================================================================
    // ADVERSARIAL: Rapid multi-range edits on the same region
    // =======================================================================

    void rapid_multi_range_edits() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        std::mt19937 rng(seed);

        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"abcdefghijklmnopqrstuvwxyz"});
        check_invariants(buf, "initial");

        for (int round = 0; round < 50; ++round) {
            std::string text = buf.text();
            uint32_t len = static_cast<uint32_t>(text.size());
            if (len < 2) {
                buf.apply_local_edit({{0, 0}}, {"abcdefghij"});
                continue;
            }

            // Generate 2-3 non-overlapping ranges
            int n_ranges = 2 + (rng() % 2);
            std::vector<uint32_t> points;
            for (int i = 0; i < n_ranges * 2; ++i) {
                points.push_back(random_byte_offset(rng, text));
            }
            std::sort(points.begin(), points.end());
            // Remove duplicates and ensure non-overlapping
            points.erase(std::unique(points.begin(), points.end()), points.end());

            std::vector<std::pair<uint32_t, uint32_t>> ranges;
            std::vector<std::string> new_texts;
            for (size_t i = 0; i + 1 < points.size() && ranges.size() < 3; i += 2) {
                ranges.push_back({points[i], points[i+1]});
                new_texts.push_back(rng() % 2 ? random_text(rng, 3) : "");
            }

            if (!ranges.empty()) {
                buf.apply_local_edit(ranges, new_texts);
                check_invariants(buf, qPrintable(QString("multi_range_%1").arg(round)));
            }
        }
    }

    // =======================================================================
    // ADVERSARIAL: Anchor stability through chaos
    // =======================================================================

    void anchor_stability_under_chaos() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        std::mt19937 rng(seed);

        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"the quick brown fox jumps over the lazy dog"});

        // Create anchors at every character boundary
        std::vector<Anchor> anchors;
        std::string text = buf.text();
        std::vector<uint32_t> boundaries;
        for (size_t b = 0; b <= text.size(); ) {
            boundaries.push_back(static_cast<uint32_t>(b));
            if (b == text.size()) break;
            unsigned char c = static_cast<unsigned char>(text[b]);
            if (c < 0x80) b += 1;
            else if ((c & 0xE0) == 0xC0) b += 2;
            else if ((c & 0xF0) == 0xE0) b += 3;
            else b += 4;
        }
        for (uint32_t off : boundaries) {
            if (off < static_cast<uint32_t>(text.size()))
                anchors.push_back(buf.anchor_at(off, Bias::Left));
        }

        // Hammer the buffer with random edits
        for (int i = 0; i < 100; ++i) {
            random_edit(buf, rng);

            // Resolve ALL anchors — must not crash, must be in range
            for (size_t a = 0; a < anchors.size(); ++a) {
                uint32_t pos = buf.resolve_anchor(anchors[a]);
                if (pos > buf.visible_length()) {
                    QFAIL(qPrintable(QString("Anchor %1 resolved to %2 > visible_length %3 at step %4")
                        .arg(a).arg(pos).arg(buf.visible_length()).arg(i)));
                }
            }

            // Anchors must maintain relative order (if both resolve to different positions)
            for (size_t a = 1; a < anchors.size(); ++a) {
                uint32_t pos_prev = buf.resolve_anchor(anchors[a-1]);
                uint32_t pos_curr = buf.resolve_anchor(anchors[a]);
                // Anchors can collapse to same position (deleted text) but never invert
                if (pos_curr < pos_prev) {
                    // Dump diagnostic state
                    std::cerr << "ANCHOR INVERSION at step " << i
                              << " (seed " << seed << ")\n"
                              << "  anchor[" << (a-1) << "] (replica="
                              << anchors[a-1].replica_id << " cv="
                              << anchors[a-1].char_value << ") = " << pos_prev << "\n"
                              << "  anchor[" << a << "] (replica="
                              << anchors[a].replica_id << " cv="
                              << anchors[a].char_value << ") = " << pos_curr << "\n"
                              << "  text=\"" << buf.text() << "\"\n"
                              << "  visible_length=" << buf.visible_length() << "\n"
                              << "  fragments:\n";
                    auto frags = buf.fragments();
                    for (size_t fi = 0; fi < frags.size(); ++fi) {
                        std::cerr << "    [" << fi << "] loc_depth="
                                  << frags[fi].locator.depth()
                                  << " origin=(" << frags[fi].origin.replica_id
                                  << "," << frags[fi].origin.value
                                  << ") len=" << frags[fi].length
                                  << " vis=" << frags[fi].visible
                                  << " dels=" << frags[fi].deletions.size()
                                  << " \"(in rope)\"\n";
                    }
                    QFAIL(qPrintable(QString("Anchor order inverted at step %1: anchor[%2]=%3 > anchor[%4]=%5 (seed %6)")
                        .arg(i).arg(a-1).arg(pos_prev).arg(a).arg(pos_curr).arg(seed)));
                }
            }
        }
    }

    // =======================================================================
    // ADVERSARIAL: Massive document with pinpoint edits
    // =======================================================================

    void large_document_pinpoint_edits() {
        Buffer buf(1);
        // Build a 10KB document from 100 separate insertions
        for (int i = 0; i < 100; ++i) {
            uint32_t pos = buf.visible_length();
            std::string chunk(100, 'a' + (i % 26));
            buf.apply_local_edit({{pos, pos}}, {chunk});
        }
        QCOMPARE(buf.visible_length(), 10000u);
        check_invariants(buf, "large_doc_built");

        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        std::mt19937 rng(seed);

        auto start = std::chrono::steady_clock::now();

        // 200 pinpoint edits (single-char insert/delete) at random positions
        for (int i = 0; i < 200; ++i) {
            std::string text = buf.text();
            uint32_t pos = random_byte_offset(rng, text);
            if (rng() % 2 == 0 && pos < buf.visible_length()) {
                // Delete 1 character
                uint32_t end = pos;
                unsigned char c = static_cast<unsigned char>(text[pos]);
                if (c < 0x80) end += 1;
                else if ((c & 0xE0) == 0xC0) end += 2;
                else if ((c & 0xF0) == 0xE0) end += 3;
                else end += 4;
                buf.apply_local_edit({{pos, end}}, {""});
            } else {
                // Insert 1 character
                buf.apply_local_edit({{pos, pos}}, {std::string(1, 'X')});
            }
        }

        auto end_time = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start).count();
        qDebug() << "200 pinpoint edits on 10KB doc:" << ms << "ms";

        check_invariants(buf, "after_pinpoint_edits");
        // Should complete in reasonable time (cursor.slice optimization)
        QVERIFY2(ms < 10000, "Pinpoint edits on large doc too slow");
    }

    // =======================================================================
    // ADVERSARIAL: Duplicate operations (network retransmission simulation)
    // =======================================================================

    void duplicate_ops_stress() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        std::mt19937 rng(seed);

        Buffer bufA(1), bufB(2);
        std::vector<Operation> ops;

        for (int i = 0; i < 30; ++i) {
            auto op = random_edit(bufA, rng);
            ops.push_back(op);
        }

        // Send each op 1-5 times to B in random order
        std::vector<Operation> duplicated;
        for (auto& op : ops) {
            int copies = 1 + (rng() % 5);
            for (int c = 0; c < copies; ++c)
                duplicated.push_back(op);
        }
        std::shuffle(duplicated.begin(), duplicated.end(), rng);

        bufB.apply_ops(duplicated);
        for (int pass = 0; pass < 20; ++pass) bufB.apply_ops({});

        check_invariants(bufB, "after_duplicates");
        QCOMPARE(bufA.text(), bufB.text());
    }

    // =======================================================================
    // ADVERSARIAL: Network partition then merge
    // =======================================================================

    void partition_then_merge() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        std::mt19937 rng(seed);

        Buffer bufA(1), bufB(2);

        // Shared initial state
        auto init = bufA.apply_local_edit({{0, 0}}, {"shared initial state"});
        bufB.apply_ops({init});

        // PARTITION: both edit independently for 30 ops
        std::vector<Operation> a_ops, b_ops;
        for (int i = 0; i < 30; ++i) {
            a_ops.push_back(random_edit(bufA, rng));
            b_ops.push_back(random_edit(bufB, rng));
        }

        check_invariants(bufA, "partitioned_a");
        check_invariants(bufB, "partitioned_b");

        // MERGE: exchange all ops
        bufA.apply_ops(b_ops);
        bufB.apply_ops(a_ops);
        for (int pass = 0; pass < 20; ++pass) {
            bufA.apply_ops({});
            bufB.apply_ops({});
        }

        check_invariants(bufA, "merged_a");
        check_invariants(bufB, "merged_b");
        QCOMPARE(bufA.text(), bufB.text());
    }

    // =======================================================================
    // ADVERSARIAL: UTF-8 at every possible split point
    // =======================================================================

    void utf8_split_at_every_boundary() {
        // Create a document with every UTF-8 character width, then insert
        // at every single byte boundary to ensure no mid-character splits
        Buffer buf(1);
        // a(1) é(2) 中(3) 🚀(4) b(1) ñ(2) 日(3) 😀(4)
        std::string text = "a\xc3\xa9\xe4\xb8\xad\xf0\x9f\x9a\x80"
                           "b\xc3\xb1\xe6\x97\xa5\xf0\x9f\x98\x80";
        buf.apply_local_edit({{0, 0}}, {text});
        QCOMPARE(buf.text(), text);
        check_invariants(buf, "utf8_mix_initial");

        // Find all valid character boundaries
        std::vector<uint32_t> boundaries = {0};
        for (size_t b = 0; b < text.size(); ) {
            unsigned char c = static_cast<unsigned char>(text[b]);
            if (c < 0x80) b += 1;
            else if ((c & 0xE0) == 0xC0) b += 2;
            else if ((c & 0xF0) == 0xE0) b += 3;
            else b += 4;
            boundaries.push_back(static_cast<uint32_t>(b));
        }

        // Insert "X" at every valid boundary
        for (size_t i = boundaries.size(); i > 0; --i) {
            // Work backwards so byte offsets remain valid
            uint32_t pos = boundaries[i - 1];
            Buffer test_buf(1);
            test_buf.apply_local_edit({{0, 0}}, {text});
            test_buf.apply_local_edit({{pos, pos}}, {"X"});
            check_invariants(test_buf, qPrintable(
                QString("insert_X_at_byte_%1").arg(pos)));

            // Verify the X is at the right position
            std::string result = test_buf.text();
            QCOMPARE(result.substr(pos, 1), std::string("X"));
        }

        // Delete each single character
        for (size_t i = 0; i + 1 < boundaries.size(); ++i) {
            Buffer test_buf(1);
            test_buf.apply_local_edit({{0, 0}}, {text});
            test_buf.apply_local_edit({{boundaries[i], boundaries[i+1]}}, {""});
            check_invariants(test_buf, qPrintable(
                QString("delete_char_at_%1").arg(boundaries[i])));
            QCOMPARE(test_buf.visible_length(),
                static_cast<uint32_t>(text.size()) - (boundaries[i+1] - boundaries[i]));
        }
    }

    // =======================================================================
    // STRESS: Many replicas, maximum chaos
    // =======================================================================

    void ten_replica_chaos() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        std::mt19937 rng(seed);

        constexpr int N = 10;
        std::vector<Buffer> bufs;
        for (int i = 0; i < N; ++i)
            bufs.emplace_back(static_cast<uint16_t>(i + 1));

        std::vector<std::vector<Operation>> queues(N);

        auto broadcast = [&](const Operation& op, int source) {
            for (int r = 0; r < N; ++r) {
                if (r == source) continue;
                int copies = 1 + (rng() % 2);
                for (int c = 0; c < copies; ++c) {
                    size_t pos = queues[r].empty() ? 0 :
                        (rng() % (queues[r].size() + 1));
                    queues[r].insert(
                        queues[r].begin() + static_cast<ptrdiff_t>(pos), op);
                }
            }
        };

        for (int step = 0; step < 200; ++step) {
            int action = rng() % 100;
            if (action < 40) {
                int r = rng() % N;
                auto op = random_edit(bufs[r], rng);
                broadcast(op, r);
            } else if (action < 55) {
                int r = rng() % N;
                auto op = (rng() % 2) ? bufs[r].undo() : bufs[r].redo();
                if (op) broadcast(*op, r);
            } else {
                int r = rng() % N;
                if (!queues[r].empty()) {
                    int count = 1 + (rng() % std::min<int>(8,
                        static_cast<int>(queues[r].size())));
                    std::vector<Operation> batch;
                    for (int c = 0; c < count && !queues[r].empty(); ++c) {
                        batch.push_back(queues[r].front());
                        queues[r].erase(queues[r].begin());
                    }
                    bufs[r].apply_ops(batch);
                }
            }
        }

        // Drain
        for (int r = 0; r < N; ++r) {
            if (!queues[r].empty()) {
                bufs[r].apply_ops(queues[r]);
                queues[r].clear();
            }
        }
        for (int pass = 0; pass < 30; ++pass) {
            for (auto& b : bufs) b.apply_ops({});
        }

        // Check invariants on all
        for (int r = 0; r < N; ++r) {
            check_invariants(bufs[r], qPrintable(QString("chaos_final_%1").arg(r)));
        }

        // Convergence
        for (int r = 1; r < N; ++r) {
            if (bufs[r].text() != bufs[0].text()) {
                std::cerr << "10-REPLICA CONVERGENCE FAILURE (seed " << seed << ")\n";
                std::cerr << "R0: \"" << bufs[0].text().substr(0, 100) << "...\"\n";
                std::cerr << "R" << r << ": \"" << bufs[r].text().substr(0, 100) << "...\"\n";
            }
            QCOMPARE(bufs[r].text(), bufs[0].text());
        }
    }
    // =======================================================================
    // ADVERSARIAL: High-frequency undo/redo to stress UndoMap SumTree
    // =======================================================================

    void undo_redo_storm() {
        uint64_t seed = std::random_device{}();
        qDebug() << "Seed:" << seed;
        std::mt19937 rng(seed);

        constexpr int N = 3;
        Buffer bufs[N] = {Buffer(1), Buffer(2), Buffer(3)};

        // Each replica builds a base of 5 edits
        std::vector<std::vector<Operation>> all_ops(N);
        for (int r = 0; r < N; ++r) {
            for (int i = 0; i < 5; ++i) {
                auto op = random_edit(bufs[r], rng);
                all_ops[r].push_back(op);
            }
        }

        // Cross-apply base edits
        for (int r = 0; r < N; ++r) {
            for (int s = 0; s < N; ++s) {
                if (r != s) bufs[r].apply_ops(all_ops[s]);
            }
            check_invariants(bufs[r], qPrintable(QString("base_%1").arg(r)));
        }

        // Storm: 200 steps, ~50% undo/redo, ~30% edit, ~20% sync
        // Track ALL ops per replica for final convergence sync.
        std::vector<std::vector<Operation>> all_storm_ops(N);
        for (int step = 0; step < 200; ++step) {
            int r = rng() % N;
            int action = rng() % 100;

            if (action < 25) {
                // Undo
                auto op = bufs[r].undo();
                if (op) all_storm_ops[r].push_back(*op);
            } else if (action < 50) {
                // Redo
                auto op = bufs[r].redo();
                if (op) all_storm_ops[r].push_back(*op);
            } else if (action < 80) {
                // New edit (clears redo stack, grows undo stack)
                auto op = random_edit(bufs[r], rng);
                all_storm_ops[r].push_back(op);
            } else {
                // Partial sync: random pair exchanges all ops so far
                int s = rng() % N;
                if (s != r) {
                    bufs[r].apply_ops(all_storm_ops[s]);
                    bufs[s].apply_ops(all_storm_ops[r]);
                }
            }

            check_invariants(bufs[r],
                qPrintable(QString("storm_%1_r%2").arg(step).arg(r)));
        }

        // Final sync: everyone gets everything (idempotent — dupes are no-ops)
        for (int r = 0; r < N; ++r) {
            for (int s = 0; s < N; ++s) {
                if (r != s) bufs[r].apply_ops(all_storm_ops[s]);
            }
        }

        for (int r = 0; r < N; ++r) {
            check_invariants(bufs[r],
                qPrintable(QString("storm_final_%1").arg(r)));
        }

        // Convergence
        for (int r = 1; r < N; ++r) {
            QCOMPARE(bufs[r].text(), bufs[0].text());
        }
    }
};

QTEST_MAIN(TestFuzz)
#include "tst_fuzz.moc"
