#include <QTest>
#include "TestSeed.h"
#include "TraceHarness.h"
#include "crdt/Buffer.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <random>
#include <set>
#include "TestActor.h"

using CollabText::Test::actor;

using namespace CollabText::Crdt;
using namespace CollabText::Test;

// ============================================================================
// Invariant checkers — these verify structural properties after every operation
// ============================================================================

// Same-locator ties are ordered replica-major (actor, then counter): the
// sequence contract keeps one actor's consecutive items adjacent.
static bool groupBefore(const CollabText::Crdt::Identity& a, const CollabText::Crdt::Identity& b) {
    if (const int by_actor = a.actor.compare(b.actor); by_actor != 0) return by_actor < 0;
    return a.counter < b.counter;
}

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
            if (groupBefore(frags[i].origin, frags[i-1].origin)) {
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
// Seeded helpers
// ============================================================================
//
// Every randomized scenario announces its effective seed, and when
// COLLABTEXT_TRACE_OUTPUT is set a failure keeps its trace. The default seed is
// fixed, so an ordinary run is a deterministic CI sample; a random campaign is
// opt-in and is made replayable by its trace, not by its seed.

/// Announce the campaign and scenario seed, then derive the scenario's RNG.
static std::mt19937 scenario_rng(const char* scenario, uint64_t* out_seed = nullptr) {
    const uint64_t seed = effective_seed(scenario);
    if (out_seed) *out_seed = seed;
    announce_seed("tst_fuzz", scenario, "buffer");
    return std::mt19937(static_cast<uint32_t>(seed));
}

/// Generate a random UTF-8 string of 1-maxChars characters.
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

/// Find a valid UTF-8 byte offset in text (don't split multi-byte chars).
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

/// Generate a random edit on a buffer without recording it. Used only by the
/// two adversarial scenarios that are not yet expressible in trace format v1
/// (multi-range local edits, and the timed pinpoint-edit measurement).
static Operation random_edit(Buffer& buf, std::mt19937& rng) {
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

/// A random local edit recorded through the harness, so the trace names the
/// exact decision (replica, byte range, replacement) rather than a roll.
static int recorded_edit(BufferRecorder& rec, int replica, std::mt19937& rng) {
    const std::string text = rec.buffer(replica).text();
    const uint32_t len = static_cast<uint32_t>(text.size());
    const uint32_t start = random_byte_offset(rng, text);
    uint32_t end = start;
    if (start < len) end = start + random_byte_offset(rng, text.substr(start));
    std::string replacement;
    if (rng() % 3 != 0) replacement = random_text(rng, 5);
    return rec.edit(replica, start, end, replacement);
}

/// Enqueue a produced operation for a peer at a random position, so delivery
/// order and duplication stay part of the recorded scenario.
static void enqueue(std::vector<int>& pending, int op_index, std::mt19937& rng,
                    int copies = 1) {
    for (int copy = 0; copy < copies; ++copy) {
        const size_t at = pending.empty() ? 0 : (rng() % (pending.size() + 1));
        pending.insert(pending.begin() + static_cast<std::ptrdiff_t>(at), op_index);
    }
}

/// Deliver up to `count` queued operations from the front of a peer's inbox.
static void deliver_front(BufferRecorder& rec, int replica, std::vector<int>& pending,
                          int count) {
    if (pending.empty()) return;
    count = std::min<int>(count, static_cast<int>(pending.size()));
    for (int i = 0; i < count; ++i) {
        rec.deliver(replica, pending.front());
        pending.erase(pending.begin());
    }
}

/// Assert convergence, keeping the trace when artifact output is configured.
/// A failing CI sample with no artifact is exactly what this harness exists to
/// stop producing.
static void assert_converged(BufferRecorder& rec, const char* context) {
    for (int r = 1; r < rec.replicas(); ++r) {
        if (rec.buffer(r).text() != rec.buffer(0).text()) {
            const std::string artifact = rec.save("failed");
            QFAIL(qPrintable(QString("CONVERGENCE FAILURE at %1: replica %2 differs "
                                     "(seed %3)%4")
                                 .arg(context).arg(r)
                                 .arg(effective_seed(context))
                                 .arg(artifact.empty()
                                          ? std::string()
                                          : (" artifact=" + artifact))));
        }
    }
}

// ============================================================================
// Test class
// ============================================================================

class TestFuzz : public QObject {
    Q_OBJECT

private slots:

    // =======================================================================
    // PROPERTY: Invariants hold after every single operation
    // =======================================================================

    void invariants_after_random_edits() {
        std::mt19937 rng = scenario_rng("invariants_after_random_edits");

        BufferRecorder rec(1, effective_seed("invariants_after_random_edits"),
                           "invariants_after_random_edits");
        check_invariants(rec.buffer(0), "initial");

        for (int i = 0; i < 200; ++i) {
            recorded_edit(rec, 0, rng);
            check_invariants(rec.buffer(0), qPrintable(QString("edit_%1").arg(i)));
        }

        rec.finish();
    }

    void invariants_after_undo_redo_cycles() {
        std::mt19937 rng = scenario_rng("invariants_after_undo_redo_cycles");
        BufferRecorder rec(1, effective_seed("invariants_after_undo_redo_cycles"),
                           "invariants_after_undo_redo_cycles");

        // Build up some state
        for (int i = 0; i < 30; ++i) {
            recorded_edit(rec, 0, rng);
        }
        check_invariants(rec.buffer(0), "after_30_edits");

        // Undo everything
        for (int i = 0; i < 30; ++i) {
            if (rec.undo(0) < 0) break;
            check_invariants(rec.buffer(0), qPrintable(QString("undo_%1").arg(i)));
        }

        // Redo everything
        for (int i = 0; i < 30; ++i) {
            if (rec.redo(0) < 0) break;
            check_invariants(rec.buffer(0), qPrintable(QString("redo_%1").arg(i)));
        }

        rec.finish();
    }

    // =======================================================================
    // PROPERTY: Convergence with multi-byte UTF-8
    // =======================================================================

    void convergence_utf8_stress() {
        uint64_t seed = 0;
        std::mt19937 rng = scenario_rng("convergence_utf8_stress", &seed);
        BufferRecorder rec(3, seed, "convergence_utf8_stress");
        std::vector<std::vector<int>> pending(3);

        for (int i = 0; i < 100; ++i) {
            const int action = static_cast<int>(rng() % 100);
            if (action < 50) {
                // Random edit with UTF-8 text
                const int r = static_cast<int>(rng() % 3);
                const int op = recorded_edit(rec, r, rng);
                if (op < 0) continue;
                for (int peer = 0; peer < 3; ++peer)
                    if (peer != r) enqueue(pending[peer], op, rng);
            } else if (action < 70) {
                // Undo/redo
                const int r = static_cast<int>(rng() % 3);
                const int op = (rng() % 2 == 0) ? rec.undo(r) : rec.redo(r);
                if (op < 0) continue;
                for (int peer = 0; peer < 3; ++peer)
                    if (peer != r) enqueue(pending[peer], op, rng);
            } else {
                // Deliver some ops
                const int r = static_cast<int>(rng() % 3);
                deliver_front(rec, r, pending[r],
                              1 + static_cast<int>(rng() % 5));
            }

            // Check invariants on all replicas periodically
            if (i % 20 == 0) {
                for (int r = 0; r < 3; ++r) {
                    check_invariants(rec.buffer(r), qPrintable(
                        QString("replica_%1_step_%2").arg(r).arg(i)));
                }
            }
        }

        // Drain all queues
        for (int r = 0; r < 3; ++r) {
            while (!pending[r].empty()) {
                rec.deliver(r, pending[r].front());
                pending[r].erase(pending[r].begin());
            }
        }
        rec.settle();

        // Final invariant check
        for (int r = 0; r < 3; ++r) {
            check_invariants(rec.buffer(r), qPrintable(QString("final_replica_%1").arg(r)));
        }

        assert_converged(rec, "convergence_utf8_stress");
        rec.finish();
    }

    // =======================================================================
    // ADVERSARIAL: All replicas insert at position 0
    // =======================================================================

    void adversarial_all_insert_at_zero() {
        uint64_t seed = 0;
        std::mt19937 rng = scenario_rng("adversarial_all_insert_at_zero", &seed);
        BufferRecorder rec(5, seed, "adversarial_all_insert_at_zero");

        constexpr int N_REPLICAS = 5;
        constexpr int N_OPS = 50;

        // Every replica inserts at position 0
        std::vector<int> produced;
        for (int round = 0; round < N_OPS; ++round) {
            const int r = static_cast<int>(rng() % N_REPLICAS);
            const int op = rec.edit(r, 0, 0, random_text(rng, 3));
            if (op < 0) continue;
            produced.push_back(op);
            check_invariants(rec.buffer(r), qPrintable(
                QString("insert_at_0_r%1_round%2").arg(r).arg(round)));
        }

        // Deliver all ops to all replicas (random order per replica)
        for (int r = 0; r < N_REPLICAS; ++r) {
            auto shuffled = produced;
            std::shuffle(shuffled.begin(), shuffled.end(), rng);
            for (const int op : shuffled) rec.deliver(r, op);
            rec.settle();
            check_invariants(rec.buffer(r), qPrintable(QString("after_merge_r%1").arg(r)));
        }

        assert_converged(rec, "adversarial_all_insert_at_zero");
        rec.finish();
    }

    // =======================================================================
    // ADVERSARIAL: All replicas edit the same single character
    // =======================================================================

    void adversarial_fight_over_one_char() {
        uint64_t seed = 0;
        std::mt19937 rng = scenario_rng("adversarial_fight_over_one_char", &seed);
        BufferRecorder rec(3, seed, "adversarial_fight_over_one_char");

        // Start with "X" on all replicas
        const int init = rec.edit(0, 0, 0, "X");
        rec.deliver(1, init);
        rec.deliver(2, init);

        std::vector<int> produced;
        // Each replica repeatedly deletes everything and inserts something new
        for (int round = 0; round < 30; ++round) {
            for (int r = 0; r < 3; ++r) {
                const uint32_t len = rec.buffer(r).visible_length();
                const int op = (len > 0)
                    ? rec.edit(r, 0, len, random_text(rng, 1))
                    : rec.edit(r, 0, 0, random_text(rng, 1));
                if (op >= 0) produced.push_back(op);
            }
        }

        // Deliver all ops to all replicas
        for (int r = 0; r < 3; ++r) {
            auto shuffled = produced;
            std::shuffle(shuffled.begin(), shuffled.end(), rng);
            for (const int op : shuffled) rec.deliver(r, op);
        }
        rec.settle();

        for (int r = 0; r < 3; ++r) {
            check_invariants(rec.buffer(r), "fight_over_one_char_final");
        }

        assert_converged(rec, "adversarial_fight_over_one_char");
        rec.finish();
    }

    // =======================================================================
    // ADVERSARIAL: Deep undo/redo chains with interleaved remote ops
    // =======================================================================

    void deep_undo_redo_with_remote() {
        uint64_t seed = 0;
        std::mt19937 rng = scenario_rng("deep_undo_redo_with_remote", &seed);
        BufferRecorder rec(2, seed, "deep_undo_redo_with_remote");
        std::vector<int> a_ops, b_ops;

        // A builds up 20 edits
        for (int i = 0; i < 20; ++i) {
            const int op = recorded_edit(rec, 0, rng);
            if (op >= 0) a_ops.push_back(op);
        }

        // B builds up 20 edits
        for (int i = 0; i < 20; ++i) {
            const int op = recorded_edit(rec, 1, rng);
            if (op >= 0) b_ops.push_back(op);
        }

        // A undoes 10
        std::vector<int> a_undos;
        for (int i = 0; i < 10; ++i) {
            const int op = rec.undo(0);
            if (op < 0) break;
            a_undos.push_back(op);
        }
        check_invariants(rec.buffer(0), "after_10_undos");

        // Now A receives all of B's ops
        for (const int op : b_ops) rec.deliver(0, op);
        check_invariants(rec.buffer(0), "after_receiving_b_ops");

        // A redoes 5
        std::vector<int> a_redos;
        for (int i = 0; i < 5; ++i) {
            const int op = rec.redo(0);
            if (op >= 0) a_redos.push_back(op);
            check_invariants(rec.buffer(0), qPrintable(QString("redo_%1").arg(i)));
        }

        // B receives all of A's ops + undos + redos
        for (const int op : a_ops) rec.deliver(1, op);
        for (const int op : a_undos) rec.deliver(1, op);
        for (const int op : a_redos) rec.deliver(1, op);
        rec.settle();

        check_invariants(rec.buffer(0), "final_a");
        check_invariants(rec.buffer(1), "final_b");
        assert_converged(rec, "deep_undo_redo_with_remote");
        rec.finish();
    }

    // =======================================================================
    // ADVERSARIAL: Rapid multi-range edits on the same region
    // =======================================================================
    //
    // Not yet recorded: trace format v1 expresses one byte range per local
    // edit, and this scenario deliberately issues 2-3 disjoint ranges in a
    // single operation. It stays seeded and announces its seed.

    void rapid_multi_range_edits() {
        uint64_t seed = 0;
        std::mt19937 rng = scenario_rng("rapid_multi_range_edits", &seed);

        Buffer buf(CollabText::Test::test_counter(1));
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
        uint64_t seed = 0;
        std::mt19937 rng = scenario_rng("anchor_stability_under_chaos", &seed);
        BufferRecorder rec(1, seed, "anchor_stability_under_chaos");

        rec.edit(0, 0, 0, "the quick brown fox jumps over the lazy dog");

        // Create anchors at every character boundary
        std::vector<Anchor> anchors;
        std::string text = rec.buffer(0).text();
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
                anchors.push_back(rec.buffer(0).anchor_at(off, Bias::Left));
        }

        // Hammer the buffer with random edits
        for (int i = 0; i < 100; ++i) {
            recorded_edit(rec, 0, rng);
            const Buffer& buf = rec.buffer(0);

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
                    const std::string artifact = rec.save("anchor-inversion");
                    std::cerr << "ANCHOR INVERSION at step " << i
                              << " (seed " << seed << ")"
                              << (artifact.empty() ? "" : (" artifact=" + artifact))
                              << "\n"
                              << "  anchor[" << (a-1) << "] (replica="
                              << anchors[a-1].position.actor << " cv="
                              << anchors[a-1].position.counter << ") = " << pos_prev << "\n"
                              << "  anchor[" << a << "] (replica="
                              << anchors[a].position.actor << " cv="
                              << anchors[a].position.counter << ") = " << pos_curr << "\n"
                              << "  text=\"" << buf.text() << "\"\n"
                              << "  visible_length=" << buf.visible_length() << "\n"
                              << "  fragments:\n";
                    auto frags = buf.fragments();
                    for (size_t fi = 0; fi < frags.size(); ++fi) {
                        std::cerr << "    [" << fi << "] loc_depth="
                                  << frags[fi].locator.depth()
                                  << " origin=(" << frags[fi].origin.actor
                                  << "," << frags[fi].origin.counter
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

        rec.finish();
    }

    // =======================================================================
    // ADVERSARIAL: Massive document with pinpoint edits
    // =======================================================================

    void large_document_pinpoint_edits() {
        BufferRecorder rec(1, effective_seed("large_document_pinpoint_edits"),
                           "large_document_pinpoint_edits");
        // Build a 10KB document from 100 separate insertions
        for (int i = 0; i < 100; ++i) {
            const uint32_t pos = rec.buffer(0).visible_length();
            rec.edit(0, pos, pos, std::string(100, 'a' + (i % 26)));
        }
        QCOMPARE(rec.buffer(0).visible_length(), 10000u);
        check_invariants(rec.buffer(0), "large_doc_built");

        std::mt19937 rng = scenario_rng("large_document_pinpoint_edits");

        auto start = std::chrono::steady_clock::now();

        // 200 pinpoint edits (single-char insert/delete) at random positions
        for (int i = 0; i < 200; ++i) {
            std::string text = rec.buffer(0).text();
            uint32_t pos = random_byte_offset(rng, text);
            if (rng() % 2 == 0 && pos < rec.buffer(0).visible_length()) {
                // Delete 1 character
                uint32_t end = pos;
                unsigned char c = static_cast<unsigned char>(text[pos]);
                if (c < 0x80) end += 1;
                else if ((c & 0xE0) == 0xC0) end += 2;
                else if ((c & 0xF0) == 0xE0) end += 3;
                else end += 4;
                rec.edit(0, pos, end, "");
            } else {
                // Insert 1 character
                rec.edit(0, pos, pos, std::string(1, 'X'));
            }
        }

        auto end_time = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start).count();
        qDebug() << "200 pinpoint edits on 10KB doc:" << ms << "ms";

        check_invariants(rec.buffer(0), "after_pinpoint_edits");
        // Should complete in reasonable time (cursor.slice optimization)
        QVERIFY2(ms < 10000, "Pinpoint edits on large doc too slow");

        rec.finish();
    }

    // =======================================================================
    // ADVERSARIAL: Duplicate operations (network retransmission simulation)
    // =======================================================================

    void duplicate_ops_stress() {
        uint64_t seed = 0;
        std::mt19937 rng = scenario_rng("duplicate_ops_stress", &seed);
        BufferRecorder rec(2, seed, "duplicate_ops_stress");

        std::vector<int> ops;
        for (int i = 0; i < 30; ++i) {
            const int op = recorded_edit(rec, 0, rng);
            if (op >= 0) ops.push_back(op);
        }

        // Send each op 1-5 times to B in random order
        std::vector<int> duplicated;
        for (const int op : ops) {
            const int copies = 1 + (rng() % 5);
            for (int c = 0; c < copies; ++c) duplicated.push_back(op);
        }
        std::shuffle(duplicated.begin(), duplicated.end(), rng);

        for (const int op : duplicated) rec.deliver(1, op);
        rec.settle();

        check_invariants(rec.buffer(1), "after_duplicates");
        assert_converged(rec, "duplicate_ops_stress");
        rec.finish();
    }

    // =======================================================================
    // ADVERSARIAL: Network partition then merge
    // =======================================================================

    void partition_then_merge() {
        uint64_t seed = 0;
        std::mt19937 rng = scenario_rng("partition_then_merge", &seed);
        BufferRecorder rec(2, seed, "partition_then_merge");

        // Shared initial state
        const int init = rec.edit(0, 0, 0, "shared initial state");
        rec.deliver(1, init);

        // PARTITION: both edit independently for 30 ops. Deliveries to the
        // disconnected replica are recorded and blocked, so the trace shows
        // exactly which operations a partition swallowed.
        rec.disconnect(1);
        std::vector<int> a_ops, b_ops;
        for (int i = 0; i < 30; ++i) {
            const int a = recorded_edit(rec, 0, rng);
            if (a >= 0) { a_ops.push_back(a); rec.deliver(1, a); }
            const int b = recorded_edit(rec, 1, rng);
            if (b >= 0) { b_ops.push_back(b); rec.deliver(0, b); }
        }

        check_invariants(rec.buffer(0), "partitioned_a");
        check_invariants(rec.buffer(1), "partitioned_b");

        // MERGE: reconnect and exchange all ops
        rec.reconnect(1);
        for (const int op : b_ops) rec.deliver(0, op);
        for (const int op : a_ops) rec.deliver(1, op);
        rec.settle();

        check_invariants(rec.buffer(0), "merged_a");
        check_invariants(rec.buffer(1), "merged_b");
        assert_converged(rec, "partition_then_merge");
        rec.finish();
    }

    // =======================================================================
    // ADVERSARIAL: UTF-8 at every possible split point
    // =======================================================================

    void utf8_split_at_every_boundary() {
        // Deterministic: no randomness, no seed to announce.
        // a(1) é(2) 中(3) 🚀(4) b(1) ñ(2) 日(3) 😀(4)
        std::string text = "a\xc3\xa9\xe4\xb8\xad\xf0\x9f\x9a\x80"
                           "b\xc3\xb1\xe6\x97\xa5\xf0\x9f\x98\x80";
        Buffer buf(CollabText::Test::test_counter(1));
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
            Buffer test_buf(CollabText::Test::test_counter(1));
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
            Buffer test_buf(CollabText::Test::test_counter(1));
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
        uint64_t seed = 0;
        std::mt19937 rng = scenario_rng("ten_replica_chaos", &seed);
        BufferRecorder rec(10, seed, "ten_replica_chaos");
        std::vector<std::vector<int>> pending(10);

        for (int step = 0; step < 200; ++step) {
            const int action = static_cast<int>(rng() % 100);
            if (action < 40) {
                const int r = static_cast<int>(rng() % 10);
                const int op = recorded_edit(rec, r, rng);
                if (op < 0) continue;
                for (int peer = 0; peer < 10; ++peer)
                    if (peer != r) enqueue(pending[peer], op, rng, 1 + (rng() % 2));
            } else if (action < 55) {
                const int r = static_cast<int>(rng() % 10);
                const int op = (rng() % 2) ? rec.undo(r) : rec.redo(r);
                if (op < 0) continue;
                for (int peer = 0; peer < 10; ++peer)
                    if (peer != r) enqueue(pending[peer], op, rng);
            } else {
                const int r = static_cast<int>(rng() % 10);
                deliver_front(rec, r, pending[r],
                              1 + static_cast<int>(rng() % 8));
            }
        }

        // Drain
        for (int r = 0; r < 10; ++r) {
            while (!pending[r].empty()) {
                rec.deliver(r, pending[r].front());
                pending[r].erase(pending[r].begin());
            }
        }
        rec.settle();

        // Check invariants on all
        for (int r = 0; r < 10; ++r) {
            check_invariants(rec.buffer(r), qPrintable(QString("chaos_final_%1").arg(r)));
        }

        assert_converged(rec, "ten_replica_chaos");
        rec.finish();
    }

    // =======================================================================
    // ADVERSARIAL: High-frequency undo/redo to stress UndoMap SumTree
    // =======================================================================

    void undo_redo_storm() {
        uint64_t seed = 0;
        std::mt19937 rng = scenario_rng("undo_redo_storm", &seed);
        BufferRecorder rec(3, seed, "undo_redo_storm");

        // Each replica builds a base of 5 edits
        std::vector<std::vector<int>> all_ops(3);
        for (int r = 0; r < 3; ++r) {
            for (int i = 0; i < 5; ++i) {
                const int op = recorded_edit(rec, r, rng);
                if (op >= 0) all_ops[r].push_back(op);
            }
        }

        // Cross-apply base edits
        for (int r = 0; r < 3; ++r) {
            for (int s = 0; s < 3; ++s) {
                if (r == s) continue;
                for (const int op : all_ops[s]) rec.deliver(r, op);
            }
            check_invariants(rec.buffer(r), qPrintable(QString("base_%1").arg(r)));
        }

        // Storm: 200 steps, ~50% undo/redo, ~30% edit, ~20% sync
        std::vector<std::vector<int>> all_storm_ops(3);
        for (int step = 0; step < 200; ++step) {
            const int r = static_cast<int>(rng() % 3);
            const int action = static_cast<int>(rng() % 100);

            if (action < 25) {
                const int op = rec.undo(r);
                if (op >= 0) all_storm_ops[r].push_back(op);
            } else if (action < 50) {
                const int op = rec.redo(r);
                if (op >= 0) all_storm_ops[r].push_back(op);
            } else if (action < 80) {
                // New edit (clears redo stack, grows undo stack)
                const int op = recorded_edit(rec, r, rng);
                if (op >= 0) all_storm_ops[r].push_back(op);
            } else {
                // Partial sync: random pair exchanges all ops so far
                const int s = static_cast<int>(rng() % 3);
                if (s != r) {
                    for (const int op : all_storm_ops[s]) rec.deliver(r, op);
                    for (const int op : all_storm_ops[r]) rec.deliver(s, op);
                }
            }

            check_invariants(rec.buffer(r),
                qPrintable(QString("storm_%1_r%2").arg(step).arg(r)));
        }

        // Final sync: everyone gets everything (idempotent — dupes are no-ops)
        for (int r = 0; r < 3; ++r) {
            for (int s = 0; s < 3; ++s) {
                if (r == s) continue;
                for (const int op : all_storm_ops[s]) rec.deliver(r, op);
            }
        }
        rec.settle();

        for (int r = 0; r < 3; ++r) {
            check_invariants(rec.buffer(r),
                qPrintable(QString("storm_final_%1").arg(r)));
        }

        assert_converged(rec, "undo_redo_storm");
        rec.finish();
    }
};

QTEST_MAIN(TestFuzz)
#include "tst_fuzz.moc"
