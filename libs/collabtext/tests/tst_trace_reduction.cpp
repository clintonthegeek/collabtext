// tst_trace_reduction.cpp — V04 acceptance: randomized failures are replayable
// and reducible.
//
// Covers the claims in the task:
//   1. COLLABTEXT_TEST_SEED is a checked uint64 with a documented fixed default.
//   2. Two runs at the same seed produce identical, replayable event traces.
//   3. An injected convergence failure produces a replayable artifact.
//   4. Delta reduction yields a smaller valid reproduction of that failure,
//      accepting a removal only when identity/causality stay valid and the same
//      failure predicate still fires.

#include <QDir>
#include <QTemporaryDir>
#include <QTest>

#include "TestSeed.h"
#include "TraceHarness.h"

using namespace CollabText::Test;

namespace {

struct Scenario {
    Trace trace;
    std::vector<std::string> live;   ///< replica states the recorded run ended with
};

/// A fixed multi-replica chaos scenario driven through a recorder. Every
/// decision (which replica, which range, which delivery, which restart) is
/// recorded, so the same seed produces the same trace and the same final texts.
Scenario build_chaos_scenario(uint64_t seed, const std::string& label,
                              bool with_restart = false) {
    BufferRecorder rec(3, seed, label);
    std::mt19937 rng(static_cast<uint32_t>(seed));

    for (int step = 0; step < 60; ++step) {
        const int r = static_cast<int>(rng() % 3);
        const std::string text = rec.buffer(r).text();
        const uint32_t len = static_cast<uint32_t>(text.size());
        uint32_t start = len ? static_cast<uint32_t>(rng() % (len + 1)) : 0u;
        if (start < len && (rng() % 2 == 0)) start += 1;
        const uint32_t end = (start < len && (rng() % 3 == 0)) ? start + 1 : start;
        const std::string payload = (rng() % 4 == 0) ? std::string() : std::string("x");
        const int op = rec.edit(r, start, end, payload);
        if (op < 0) continue;
        rec.broadcast(r, op);
        if (rng() % 5 == 0) rec.deliver(static_cast<int>(rng() % 3), op);  // duplicate
        if (rng() % 7 == 0) {
            const int undid = rec.undo(r);
            if (undid >= 0) rec.broadcast(r, undid);
        }
        if (with_restart && step % 23 == 22) rec.restart(r);
    }

    Scenario scenario;
    scenario.trace = rec.trace();
    for (int r = 0; r < rec.replicas(); ++r)
        scenario.live.push_back(rec.buffer(r).text());
    return scenario;
}

Scenario build_idlist_scenario(uint64_t seed, const std::string& label) {
    IdListRecorder rec(2, seed, label);
    std::mt19937 rng(static_cast<uint32_t>(seed));
    for (int step = 0; step < 40; ++step) {
        const int r = static_cast<int>(rng() % 2);
        const uint32_t size = rec.list(r).size();
        const uint32_t index = size ? static_cast<uint32_t>(rng() % (size + 1)) : 0u;
        int op = -1;
        if (size == 0 || rng() % 3 != 0)
            op = rec.insert_after(r, index, 1000 + static_cast<uint64_t>(step));
        else
            op = rec.remove_at(r, index);
        if (op < 0) continue;
        rec.broadcast(r, op);
    }
    Scenario scenario;
    scenario.trace = rec.trace();
    for (int r = 0; r < rec.replicas(); ++r) {
        std::string ids;
        for (const uint64_t id : rec.list(r).ids()) ids += std::to_string(id) + ",";
        scenario.live.push_back(ids);
    }
    return scenario;
}

/// A scenario whose convergence failure is injected, not an engine defect: one
/// delivery is marked lost, so the replicas legitimately disagree.
Trace build_injected_failure(uint64_t seed, const std::string& label) {
    BufferRecorder rec(2, seed, label);
    const int first = rec.edit(0, 0, 0, "hello");
    rec.deliver(1, first);
    for (int i = 0; i < 6; ++i) {
        const int op = rec.edit(0, static_cast<uint32_t>(5 + i), static_cast<uint32_t>(5 + i), ".");
        rec.deliver(1, op);
    }
    // Injected transport loss: replica 1 never receives this operation.
    const int lost = rec.edit(0, 5, 5, "L");
    rec.deliver(1, lost, /*lose=*/true);
    rec.deliver(0, lost);
    // Replica 1 makes its own conflicting edit, delivered normally.
    const int other = rec.edit(1, 0, 0, "!");
    rec.deliver(0, other);
    return rec.trace();
}

}  // namespace

class TestTraceReduction : public QObject {
    Q_OBJECT

private slots:

    // ---- 1. the seed contract ----

    void seed_parsing_is_checked() {
        uint64_t value = 0;
        QVERIFY(detail::parse_u64_strict("0", value));
        QCOMPARE(value, 0ull);
        QVERIFY(detail::parse_u64_strict("18446744073709551615", value));
        QCOMPARE(value, UINT64_MAX);
        QVERIFY(detail::parse_u64_strict("42", value));
        QCOMPARE(value, 42ull);

        // Rejected: whitespace, sign, suffix, hex, empty, overflow.
        QVERIFY(!detail::parse_u64_strict(" 42", value));
        QVERIFY(!detail::parse_u64_strict("42 ", value));
        QVERIFY(!detail::parse_u64_strict("-1", value));
        QVERIFY(!detail::parse_u64_strict("42abc", value));
        QVERIFY(!detail::parse_u64_strict("0x2a", value));
        QVERIFY(!detail::parse_u64_strict("", value));
        QVERIFY(!detail::parse_u64_strict("18446744073709551616", value));
    }

    void default_seed_is_documented_and_stable() {
        // kDefaultTestSeed is the CI default named by the roadmap's fixed-seed
        // runs; effective_seed() derives per-scenario seeds from the campaign.
        QCOMPARE(kDefaultTestSeed, 20261003ull);
        QCOMPARE(effective_seed("scenario"), effective_seed("scenario"));
        QVERIFY(effective_seed("a") != effective_seed("b"));

        // A malformed COLLABTEXT_TEST_SEED is reported, never silently obeyed.
        uint64_t parsed = 0;
        if (seed_config().seed_raw.empty()) {
            QVERIFY(!seed_config().seed_rejected);
            QCOMPARE(seed_config().seed, kDefaultTestSeed);
        } else {
            const bool valid = detail::parse_u64_strict(seed_config().seed_raw, parsed);
            QCOMPARE(valid, !seed_config().seed_rejected);
        }
    }

    void effective_seed_is_derived_from_the_campaign_seed() {
        QCOMPARE(detail::mix_label(42, "chaos"), detail::mix_label(42, "chaos"));
        QVERIFY(detail::mix_label(42, "chaos") != detail::mix_label(43, "chaos"));
        QVERIFY(detail::mix_label(42, "chaos") != detail::mix_label(42, "other"));
    }

    // ---- 2. identical traces and faithful replay ----

    void same_seed_produces_identical_traces() {
        const Scenario first = build_chaos_scenario(42, "chaos");
        const Scenario second = build_chaos_scenario(42, "chaos");
        QCOMPARE(first.trace.to_text(), second.trace.to_text());
        QCOMPARE(first.live, second.live);
        QVERIFY(first.trace.events.size() > 20);
    }

    void different_seeds_produce_different_traces() {
        const Scenario a = build_chaos_scenario(42, "chaos");
        const Scenario b = build_chaos_scenario(20261003, "chaos");
        QVERIFY(a.trace.to_text() != b.trace.to_text());
    }

    void replay_reproduces_the_recorded_run() {
        for (const bool with_restart : {false, true}) {
            const Scenario scenario =
                build_chaos_scenario(42, "chaos", with_restart);
            const ReplayResult replayed = replay(scenario.trace);
            QVERIFY2(replayed.status == ReplayStatus::Completed,
                     qPrintable(QString::fromStdString(replayed.error)));
            QCOMPARE(replayed.replica_state, scenario.live);
            QVERIFY(replayed.applied > 0);
            QCOMPARE(replayed.invariant_error, std::string());
        }
    }

    void trace_round_trips_through_a_file() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const Scenario scenario = build_chaos_scenario(42, "chaos");
        const std::string path =
            QDir(dir.path()).filePath("chaos.trace").toStdString();
        QVERIFY(scenario.trace.save(path));

        Trace loaded;
        std::string error;
        QVERIFY2(Trace::load(path, loaded, error), qPrintable(QString::fromStdString(error)));
        QCOMPARE(loaded.to_text(), scenario.trace.to_text());
        QCOMPARE(replay(loaded).replica_state, scenario.live);
    }

    void idlist_traces_round_trip_and_replay() {
        const Scenario scenario = build_idlist_scenario(42, "idlist");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::string path = QDir(dir.path()).filePath("idlist.trace").toStdString();
        QVERIFY(scenario.trace.save(path));

        Trace loaded;
        std::string error;
        QVERIFY2(Trace::load(path, loaded, error), qPrintable(QString::fromStdString(error)));
        QCOMPARE(loaded.to_text(), scenario.trace.to_text());
        const ReplayResult replayed = replay(loaded);
        QVERIFY2(replayed.status == ReplayStatus::Completed,
                 qPrintable(QString::fromStdString(replayed.error)));
        QCOMPARE(replayed.idlists, 2u);
        QCOMPARE(replayed.replica_state, scenario.live);
    }

    // ---- 3. the injected failure is replayable ----

    void injected_lost_delivery_breaks_convergence_and_replays() {
        const Trace trace = build_injected_failure(42, "injected");
        const ReplayResult recorded = replay(trace);
        QVERIFY(recorded.status == ReplayStatus::Completed);
        QVERIFY2(!recorded.converged, "the injected loss must actually diverge");
        QCOMPARE(recorded.lost, 1u);
        QCOMPARE(recorded.invariant_error, std::string());

        // Replaying the artifact reproduces the same divergence, not a new one.
        const ReplayResult again = replay(trace);
        QVERIFY(!again.converged);
        QCOMPARE(again.replica_state, recorded.replica_state);
        QCOMPARE(again.divergence, recorded.divergence);
    }

    void disconnect_blocks_delivery_until_reconnect() {
        BufferRecorder rec(2, 42, "partition");
        const int op = rec.edit(0, 0, 0, "blocked");
        rec.disconnect(1);
        rec.deliver(1, op);
        QCOMPARE(rec.buffer(1).text(), std::string());
        rec.reconnect(1);
        rec.deliver(1, op);
        QCOMPARE(rec.buffer(1).text(), std::string("blocked"));

        const ReplayResult replayed = replay(rec.trace());
        QVERIFY(replayed.status == ReplayStatus::Completed);
        QCOMPARE(replayed.blocked, 1u);
        QVERIFY(replayed.converged);
    }

    // ---- 4. reduction yields a smaller valid reproduction ----

    void injected_failure_reduces_to_a_smaller_valid_reproduction() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const Trace failing = build_injected_failure(42, "injected");
        // The unreduced trace is saved first: it is the primary artifact.
        const std::string full_path = QDir(dir.path()).filePath("injected-full.trace").toStdString();
        QVERIFY(failing.save(full_path));

        Trace loaded;
        std::string error;
        QVERIFY2(Trace::load(full_path, loaded, error),
                 qPrintable(QString::fromStdString(error)));
        const ReplayResult before = replay(loaded);
        QVERIFY(before.status == ReplayStatus::Completed);
        QVERIFY(!before.converged);

        const ReduceOutcome reduced = reduce_trace(loaded, FailurePredicate::Convergence);
        QVERIFY2(reduced.ok, qPrintable(QString::fromStdString(reduced.note)));
        QVERIFY(reduced.removed > 0);
        QVERIFY(reduced.trace.events.size() < loaded.events.size());

        // Validity: every surviving event still applies, so the reduced trace
        // is a legal execution rather than a clamped approximation.
        const ReplayResult after = replay(reduced.trace);
        QVERIFY2(after.status == ReplayStatus::Completed,
                 qPrintable(QString::fromStdString(after.error)));
        // The *predicate* is preserved, not the mechanism: a smaller failing
        // trace may diverge for a different reason than the injected loss.
        QVERIFY2(!after.converged, "the reduction must preserve the failure");
        QVERIFY(!after.divergence.empty());

        // The reduced artifact is itself replayable from disk.
        const std::string small_path =
            QDir(dir.path()).filePath("injected-reduced.trace").toStdString();
        QVERIFY(reduced.trace.save(small_path));
        Trace reloaded;
        QVERIFY2(Trace::load(small_path, reloaded, error),
                 qPrintable(QString::fromStdString(error)));
        const ReplayResult replayed_again = replay(reloaded);
        QVERIFY(replayed_again.status == ReplayStatus::Completed);
        QVERIFY(!replayed_again.converged);
        QCOMPARE(replayed_again.replica_state, after.replica_state);
    }

    void reduction_removes_irrelevant_traffic() {
        // A noisy run — 30 retransmissions of one operation — whose only
        // failure is an injected loss must reduce to a handful of events.
        BufferRecorder rec(2, 42, "noisy");
        const int a = rec.edit(0, 0, 0, "a");
        for (int i = 0; i < 30; ++i) rec.deliver(1, a);
        const int lost = rec.edit(0, 1, 1, "Z");
        rec.deliver(1, lost, /*lose=*/true);
        const int b = rec.edit(1, 0, 0, "b");
        rec.deliver(0, b);
        QVERIFY(rec.trace().events.size() > 30);

        const ReduceOutcome reduced = reduce_trace(rec.trace(), FailurePredicate::Convergence);
        QVERIFY2(reduced.ok, qPrintable(QString::fromStdString(reduced.note)));
        QVERIFY2(reduced.removed >= 25,
                 qPrintable(QString("removed %1 of %2")
                                .arg(reduced.removed)
                                .arg(rec.trace().events.size())));
        QVERIFY(reduced.trace.events.size() < rec.trace().events.size());

        // The reproduction is a legal execution, still fails, and still
        // exercises both replicas as delivery targets.
        const ReplayResult after = replay(reduced.trace);
        QVERIFY(after.status == ReplayStatus::Completed);
        QVERIFY(!after.converged);
        QVERIFY(!after.divergence.empty());
        QCOMPARE(after.delivery_targets, 0b11u);
    }

    void reduction_rejects_removals_that_break_causality() {
        // A delivery names a produced operation by index. Removing the edit
        // that produced it must be rejected, not silently tolerated.
        BufferRecorder rec(2, 42, "causality");
        const int op = rec.edit(0, 0, 0, "abc");
        rec.deliver(1, op);
        const Trace trace = rec.trace();
        QCOMPARE(trace.events.size(), 2u);

        Trace without_edit = trace;
        without_edit.events.erase(without_edit.events.begin());
        const ReplayResult broken = replay(without_edit);
        QCOMPARE(broken.status, ReplayStatus::NotApplicable);
        QCOMPARE(broken.error_event, 0u);
        QVERIFY(broken.error.find("never produced") != std::string::npos);
    }

    void reduction_rejects_removals_that_break_edit_ranges() {
        // Byte ranges are decisions about the replica's text at that moment.
        // Dropping an earlier edit invalidates a later range, and the candidate
        // must be rejected rather than clamped.
        BufferRecorder rec(1, 42, "ranges");
        rec.edit(0, 0, 0, "abcd");
        rec.edit(0, 4, 4, "efgh");
        const Trace trace = rec.trace();

        Trace without_first = trace;
        without_first.events.erase(without_first.events.begin());
        const ReplayResult broken = replay(without_first);
        QCOMPARE(broken.status, ReplayStatus::NotApplicable);
        QVERIFY(broken.error.find("outside the replica text") != std::string::npos);
    }

    void reduction_rejects_removals_that_break_idlist_indices() {
        IdListRecorder rec(2, 42, "idlist-indices");
        const int a = rec.insert_after(0, 0, 1);
        rec.deliver(1, a);
        const int b = rec.insert_after(1, 1, 2);
        rec.deliver(0, b);
        const int lost = rec.insert_after(0, 0, 3);
        rec.deliver(1, lost, /*lose=*/true);

        const Trace trace = rec.trace();
        // Drop the first insert and its delivery: replica 1 now holds no
        // entries, so the recorded visible index 1 no longer exists and the
        // candidate must be rejected rather than clamped.
        Trace without_first = trace;
        without_first.events.erase(without_first.events.begin(),
                                   without_first.events.begin() + 2);
        const ReplayResult broken = replay(without_first);
        QCOMPARE(broken.status, ReplayStatus::NotApplicable);
        QVERIFY(broken.error.find("beyond the replica's list size") != std::string::npos);
    }

    void reduction_refuses_a_passing_trace() {
        BufferRecorder rec(2, 42, "passing");
        const int op = rec.edit(0, 0, 0, "abc");
        rec.deliver(1, op);
        const ReduceOutcome outcome = reduce_trace(rec.trace(), FailurePredicate::Convergence);
        QVERIFY(!outcome.ok);
        QVERIFY(outcome.note.find("does not fail") != std::string::npos);
        QCOMPARE(outcome.removed, 0u);
    }

    void predicate_selection_is_independent() {
        ReplayResult diverged;
        diverged.converged = false;
        diverged.divergence = "replicas 0 and 1 differ";
        ReplayResult broken;
        broken.converged = true;
        broken.invariant_error = "visible_length != text().size()";

        QVERIFY(predicate_fails(diverged, FailurePredicate::Convergence));
        QVERIFY(!predicate_fails(diverged, FailurePredicate::Invariant));
        QVERIFY(predicate_fails(broken, FailurePredicate::Invariant));
        QVERIFY(!predicate_fails(broken, FailurePredicate::Convergence));
        QVERIFY(predicate_fails(diverged, FailurePredicate::Any));
        QVERIFY(predicate_fails(broken, FailurePredicate::Any));

        // Containment (V03) means an ordinary replay satisfies every predicate,
        // so reduction must refuse it rather than invent a failure.
        Trace empty;
        empty.model = TraceModel::Buffer;
        empty.label = "empty";
        empty.seed = 42;
        empty.replicas = 2;
        const ReplayResult clean = replay(empty);
        QVERIFY(clean.status == ReplayStatus::Completed);
        QVERIFY(clean.converged);
        QVERIFY(clean.invariant_error.empty());
        QVERIFY(!predicate_fails(clean, FailurePredicate::Any));
    }

    // ---- restart modelling ----

    void restart_preserves_document_and_history() {
        BufferRecorder rec(1, 42, "restart");
        rec.edit(0, 0, 0, "one");
        rec.edit(0, 3, 3, "two");
        const int undone = rec.undo(0);
        QVERIFY(undone >= 0);
        rec.redo(0);
        const std::string before = rec.buffer(0).text();
        rec.restart(0);
        QCOMPARE(rec.buffer(0).text(), before);

        // The rebuilt replica is a real engine, not a snapshot: it still undoes.
        const int undone_again = rec.undo(0);
        QVERIFY(undone_again >= 0);
        QVERIFY(rec.buffer(0).text() != before);

        const ReplayResult replayed = replay(rec.trace());
        QVERIFY2(replayed.status == ReplayStatus::Completed,
                 qPrintable(QString::fromStdString(replayed.error)));
        QCOMPARE(replayed.replica_state.front(), rec.buffer(0).text());
    }

    void trace_rejects_unknown_versions_and_verbs() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        std::string error;
        Trace parsed;

        const std::string wrong_version = QDir(dir.path()).filePath("v9.trace").toStdString();
        QVERIFY(Trace{}.save(wrong_version));
        QVERIFY(Trace::load(wrong_version, parsed, error));
        {
            std::ofstream bump(wrong_version, std::ios::trunc);
            bump << "#collabtext-trace 9\n#model buffer\n#replicas 1\n";
        }
        QVERIFY(!Trace::load(wrong_version, parsed, error));
        QVERIFY(error.find("unsupported trace version") != std::string::npos);

        const std::string bad_verb = QDir(dir.path()).filePath("bad.trace").toStdString();
        {
            std::ofstream file(bad_verb, std::ios::trunc);
            file << "#collabtext-trace 1\n#model buffer\n#replicas 1\nlevitate 0\n";
        }
        QVERIFY(!Trace::load(bad_verb, parsed, error));
        QVERIFY(error.find("unknown event verb") != std::string::npos);

        const std::string no_magic = QDir(dir.path()).filePath("bare.trace").toStdString();
        {
            std::ofstream file(no_magic, std::ios::trunc);
            file << "edit 0 0 0 61\n";
        }
        QVERIFY(!Trace::load(no_magic, parsed, error));
        QVERIFY(error.find("header") != std::string::npos);
    }
};

QTEST_GUILESS_MAIN(TestTraceReduction)
#include "tst_trace_reduction.moc"
