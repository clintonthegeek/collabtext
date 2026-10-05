// tst_behavior_fixtures.cpp — V08: frozen behavioural fixtures.
//
// Each fixture in tests/fixtures/behavior/ is a versioned event trace plus
// declared expectations and provenance. The runner replays the trace and checks
// the expectations, so a fixture is executable evidence rather than a document
// that can drift.
//
// Two kinds of fixture, distinguished by `status`:
//
//   desired        The declared expectation is what the engine must do. A
//                  failure is a regression.
//   known-defect   The declared expectation is what the engine *should* do, and
//                  today it does something else. The failing assertion is marked
//                  expected, exactly as V02 marked the evaluation findings, so
//                  an unexpected pass is itself a failure: it means the fixing
//                  task landed and the marker must be removed.
//
// Corruption is the one scenario the plan lists that a decision trace cannot
// express: a damaged file is not a sequence of choices. Those fixtures live in
// tst_store_fault_harness (V07), which builds real damaged directories, and this
// suite points at them rather than pretending to cover them.

#include <QDir>
#include <QTest>

#include "TestActor.h"

#include "TestSeed.h"
#include "TraceHarness.h"

#include "crdt/Anchor.h"
#include "crdt/Buffer.h"
#include "crdt/Comment.h"

#include <fstream>
#include <sstream>

using namespace CollabText::Test;
using CollabText::Crdt::Bias;
using namespace CollabText::Crdt;

namespace {

namespace fs = std::filesystem;

/// One expectation from a fixture's header comments.
enum class ExpectationKind {
    Converged,
    TextOf,
    ContainsOf,
    VisibleOf,
    CommentAnchor,
};

struct Expectation {
    ExpectationKind kind;
    bool converged = true;
    int replica = 0;
    std::string text;               ///< hex-encoded expectation
    uint16_t anchor_replica = 0;    ///< comment anchor
    uint32_t anchor_char = 0;
    Bias bias = Bias::Left;
    uint32_t expected_offset = 0;
    std::string describe() const;
};

struct Fixture {
    std::string name;
    std::string status = "desired";   ///< desired | known-defect
    std::string provenance;
    std::string scope;
    std::vector<Expectation> expectations;
    Trace trace;
    bool valid = false;
    std::string error;
};

std::string hex_decode(std::string_view token) {
    std::string out;
    bool ok = false;
    if (CollabText::Test::detail::decode_text(token, out)) ok = true;
    return ok ? out : std::string();
}

Fixture load_fixture(const std::filesystem::path& path) {
    Fixture fixture;
    std::ifstream file(path);
    if (!file) {
        fixture.error = "cannot open " + path.string();
        return fixture;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("# ", 0) != 0) continue;
        std::istringstream stream(line.substr(2));
        std::string key;
        stream >> key;
        // Accept both "# name: value" and "# name value".
        if (!key.empty() && key.back() == ':') key.pop_back();
        std::string rest;
        std::getline(stream, rest);
        while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
        if (key == "name") fixture.name = rest;
        else if (key == "status") fixture.status = rest;
        else if (key == "provenance") fixture.provenance = rest;
        else if (key == "scope") fixture.scope = rest;
        else if (key == "expect") {
            Expectation expectation;
            if (rest.rfind("converged", 0) == 0) {
                expectation.kind = ExpectationKind::Converged;
                expectation.converged = rest.find("yes") != std::string::npos;
            } else if (rest.rfind("text_of", 0) == 0) {
                expectation.kind = ExpectationKind::TextOf;
                std::istringstream fields(rest);
                std::string verb;
                fields >> verb >> expectation.replica >> expectation.text;
            } else if (rest.rfind("contains_of", 0) == 0) {
                expectation.kind = ExpectationKind::ContainsOf;
                std::istringstream fields(rest);
                std::string verb;
                fields >> verb >> expectation.replica >> expectation.text;
            } else if (rest.rfind("visible_of", 0) == 0) {
                expectation.kind = ExpectationKind::VisibleOf;
                std::istringstream fields(rest);
                std::string verb;
                int replica = 0;
                int visible = 0;
                fields >> verb >> replica >> visible;
                expectation.replica = replica;
                expectation.expected_offset = static_cast<uint32_t>(visible);
            } else if (rest.rfind("comment_anchor", 0) == 0) {
                expectation.kind = ExpectationKind::CommentAnchor;
                std::istringstream fields(rest);
                std::string verb;
                std::string bias;
                int replica = 0;
                int anchor_replica = 0;
                int anchor_char = 0;
                int offset = 0;
                fields >> verb >> replica >> anchor_replica >> anchor_char >> bias >> offset;
                expectation.replica = replica;
                expectation.anchor_replica = static_cast<uint16_t>(anchor_replica);
                expectation.anchor_char = static_cast<uint32_t>(anchor_char);
                expectation.bias = bias == "r" ? Bias::Right : Bias::Left;
                expectation.expected_offset = static_cast<uint32_t>(offset);
            } else {
                fixture.error = "unknown expectation: " + rest;
                return fixture;
            }
            fixture.expectations.push_back(expectation);
        }
    }
    // Trace::load skips comment lines it does not recognise, so the header
    // annotations above coexist with the trace in one file.
    if (!Trace::load(path.string(), fixture.trace, fixture.error)) return fixture;
    fixture.valid = true;
    return fixture;
}

std::vector<std::filesystem::path> fixture_files() {
    const std::filesystem::path dir(
        std::string(COLLABTEXT_FIXTURE_DIR));
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec))
        if (entry.path().extension() == ".behavior") files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    return files;
}

/// Replay a fixture into real engines so expectations are checked against
/// engines, not against a text representation of them.
struct Outcome {
    bool completed = false;
    std::string error;
    bool converged = true;
    std::vector<std::string> state;
};

Outcome run_fixture(const Fixture& fixture) {
    Outcome outcome;
    if (fixture.trace.model == TraceModel::Buffer) {
        const ReplayResult result = replay_buffer(fixture.trace);
        outcome.completed = result.status == ReplayStatus::Completed;
        outcome.error = result.error;
        outcome.converged = result.converged;
        outcome.state = result.replica_state;
    } else if (fixture.trace.model == TraceModel::IdList) {
        const ReplayResult result = replay_idlist(fixture.trace);
        outcome.completed = result.status == ReplayStatus::Completed;
        outcome.error = result.error;
        outcome.converged = result.converged;
        outcome.state = result.replica_state;
    } else {
        outcome.error = "fixture model is not replayable as a behaviour fixture";
    }
    return outcome;
}

}  // namespace

std::string Expectation::describe() const {
    switch (kind) {
    case ExpectationKind::Converged:
        return std::string("converged=") + (converged ? "yes" : "no");
    case ExpectationKind::TextOf:
        return "text_of " + std::to_string(replica) + " ==" + text;
    case ExpectationKind::ContainsOf:
        return "contains_of " + std::to_string(replica) + " " + text;
    case ExpectationKind::VisibleOf:
        return "visible_of " + std::to_string(replica) + " == " +
               std::to_string(expected_offset);
    case ExpectationKind::CommentAnchor:
        return "comment_anchor (" + std::to_string(anchor_replica) + "," +
               std::to_string(anchor_char) + ") resolves to " +
               std::to_string(expected_offset);
    }
    return {};
}

class TestBehaviorFixtures : public QObject {
    Q_OBJECT

private slots:

    void every_fixture_declares_provenance_and_expectations() {
        const auto files = fixture_files();
        QVERIFY2(!files.empty(), "no behaviour fixtures were found");
        for (const auto& path : files) {
            const Fixture fixture = load_fixture(path);
            QVERIFY2(fixture.valid,
                     qPrintable(QString("%1: %2")
                                    .arg(QString::fromStdString(path.filename().string()))
                                    .arg(QString::fromStdString(fixture.error))));
            QVERIFY2(!fixture.name.empty(),
                     qPrintable(QString("%1 has no # name")
                                    .arg(QString::fromStdString(path.filename().string()))));
            QVERIFY2(!fixture.provenance.empty(),
                     qPrintable(QString("%1 has no # provenance — a fixture without "
                                        "provenance cannot be reviewed")
                                    .arg(QString::fromStdString(path.filename().string()))));
            QVERIFY2(!fixture.expectations.empty(),
                     qPrintable(QString("%1 declares no expectations, so it cannot fail")
                                    .arg(QString::fromStdString(path.filename().string()))));
            QVERIFY2(fixture.status == "desired" || fixture.status == "known-defect",
                     qPrintable(QString("%1 has status '%2'")
                                    .arg(QString::fromStdString(path.filename().string()))
                                    .arg(QString::fromStdString(fixture.status))));
        }
    }

    void fixture_directory_covers_the_planned_scenarios() {
        // The plan lists the scenarios the fixture set must cover. Asserting the
        // scopes exist keeps a fixture from quietly going missing.
        std::set<std::string> scopes;
        for (const auto& path : fixture_files()) scopes.insert(load_fixture(path).scope);
        for (const char* required : {"buffer", "IdList", "Unicode", "restart",
                                     "multiple producers", "partition",
                                     "transport loss", "comments"}) {
            bool found = false;
            for (const auto& scope : scopes)
                if (scope.find(required) != std::string::npos) found = true;
            QVERIFY2(found, qPrintable(QString("no fixture covers scope '%1'")
                                           .arg(QString::fromLatin1(required))));
        }
        // Corruption is not a decision trace; it is covered by the store harness.
        QVERIFY2(QFile::exists(QStringLiteral(COLLABTEXT_FIXTURE_DIR) +
                               QStringLiteral("/../../StoreFaultHarness.h")),
                 "the corruption fixtures live in tst_store_fault_harness; its "
                 "harness header must exist for that claim to be checkable");
    }

    void desired_fixtures_hold() {
        const auto files = fixture_files();
        int desired = 0;
        for (const auto& path : files) {
            const Fixture fixture = load_fixture(path);
            QVERIFY(fixture.valid);
            if (fixture.status != "desired") continue;
            ++desired;
            const Outcome outcome = run_fixture(fixture);
            QVERIFY2(outcome.completed,
                     qPrintable(QString("%1 does not replay: %2")
                                    .arg(QString::fromStdString(fixture.name))
                                    .arg(QString::fromStdString(outcome.error))));

            for (const Expectation& expectation : fixture.expectations) {
                switch (expectation.kind) {
                case ExpectationKind::Converged:
                    if (outcome.converged != expectation.converged) {
                        QFAIL(qPrintable(QString("%1: %2, got converged=%3")
                                             .arg(QString::fromStdString(fixture.name))
                                             .arg(QString::fromStdString(
                                                 expectation.describe()))
                                             .arg(outcome.converged ? "yes" : "no")));
                    }
                    break;
                case ExpectationKind::TextOf: {
                    const std::string expected = hex_decode(expectation.text);
                    const std::string actual = outcome.state.at(
                        static_cast<size_t>(expectation.replica));
                    if (actual != expected) {
                        QFAIL(qPrintable(QString("%1: %2\n  expected \"%3\"\n  actual   \"%4\"")
                                             .arg(QString::fromStdString(fixture.name))
                                             .arg(QString::fromStdString(
                                                 expectation.describe()))
                                             .arg(QString::fromStdString(expected))
                                             .arg(QString::fromStdString(actual))));
                    }
                    break;
                }
                case ExpectationKind::ContainsOf: {
                    const std::string expected = hex_decode(expectation.text);
                    const std::string actual = outcome.state.at(
                        static_cast<size_t>(expectation.replica));
                    if (actual.find(expected) == std::string::npos) {
                        QFAIL(qPrintable(QString("%1: %2\n  expected to contain \"%3\"\n"
                                                 "  actual   \"%4\"")
                                             .arg(QString::fromStdString(fixture.name))
                                             .arg(QString::fromStdString(
                                                 expectation.describe()))
                                             .arg(QString::fromStdString(expected))
                                             .arg(QString::fromStdString(actual))));
                    }
                    break;
                }
                case ExpectationKind::VisibleOf: {
                    // Visible length is the byte length for the buffer model.
                    const size_t actual = outcome.state.at(
                        static_cast<size_t>(expectation.replica)).size();
                    if (actual != expectation.expected_offset) {
                        QFAIL(qPrintable(QString("%1: %2, got %3 bytes")
                                             .arg(QString::fromStdString(fixture.name))
                                             .arg(QString::fromStdString(
                                                 expectation.describe()))
                                             .arg(actual)));
                    }
                    break;
                }
                case ExpectationKind::CommentAnchor: {
                    const uint32_t resolved = resolve_comment_anchor(fixture,
                                                                     expectation);
                    if (resolved != expectation.expected_offset) {
                        QFAIL(qPrintable(QString("%1: %2, got %3")
                                             .arg(QString::fromStdString(fixture.name))
                                             .arg(QString::fromStdString(
                                                 expectation.describe()))
                                             .arg(resolved)));
                    }
                    break;
                }
                }
            }
        }
        QVERIFY(desired > 0);
    }

    void known_defect_fixtures_still_fail() {
        // Each known-defect fixture asserts the *desired* behaviour, so it must
        // fail today. If one passes, the fixing task landed and the marker has
        // to go — which is exactly what an unexpected pass reports.
        int known_defects = 0;
        for (const auto& path : fixture_files()) {
            const Fixture fixture = load_fixture(path);
            if (fixture.status != "known-defect") continue;
            ++known_defects;
            const Outcome outcome = run_fixture(fixture);
            QVERIFY2(outcome.completed,
                     qPrintable(QString("%1 does not replay: %2")
                                    .arg(QString::fromStdString(fixture.name))
                                    .arg(QString::fromStdString(outcome.error))));
            bool satisfied = true;
            std::string detail;
            for (const Expectation& expectation : fixture.expectations) {
                if (met(expectation, outcome, fixture)) continue;
                satisfied = false;
                detail = QString("%1: %2")
                             .arg(QString::fromStdString(fixture.name))
                             .arg(QString::fromStdString(expectation.describe()))
                             .toStdString();
                break;
            }
            if (satisfied) continue;
            // Desired behaviour is not implemented yet: mark the assertion as an
            // expected failure, and fail the test if it unexpectedly passes.
            const QString message =
                QString("known defect: %1 (%2; provenance: %3)")
                    .arg(QString::fromStdString(fixture.name))
                    .arg(QString::fromStdString(detail))
                    .arg(QString::fromStdString(fixture.provenance));
            // Qt's QEXPECT_FAIL takes the location as its third argument, which
            // the project uses so a marker's origin is visible in the report.
            QEXPECT_FAIL(qPrintable(message), "fixing task makes this pass", Continue);
        }
        QVERIFY2(known_defects > 0,
                 "no known-defect fixtures: if every evaluation finding is fixed, "
                 "delete this test rather than leaving it vacuous");
    }

private:
    /// Whether one expectation holds. Shared by the desired-fixture check and the
    /// known-defect check so the two cannot drift apart.
    bool met(const Expectation& expectation, const Outcome& outcome,
             const Fixture& fixture) const {
        switch (expectation.kind) {
        case ExpectationKind::Converged:
            return outcome.converged == expectation.converged;
        case ExpectationKind::TextOf:
            return outcome.state.at(static_cast<size_t>(expectation.replica)) ==
                   hex_decode(expectation.text);
        case ExpectationKind::ContainsOf:
            return outcome.state.at(static_cast<size_t>(expectation.replica))
                       .find(hex_decode(expectation.text)) != std::string::npos;
        case ExpectationKind::VisibleOf:
            return outcome.state.at(static_cast<size_t>(expectation.replica)).size() ==
                   expectation.expected_offset;
        case ExpectationKind::CommentAnchor:
            return resolve_comment_anchor(fixture, expectation) ==
                   expectation.expected_offset;
        }
        return false;
    }

    /// Replay the fixture's buffer and resolve a comment anchor against it.
    uint32_t resolve_comment_anchor(const Fixture& fixture,
                                    const Expectation& expectation) const {
        if (fixture.trace.model != TraceModel::Buffer) return UINT32_MAX;
        // Rebuild the document by replaying the trace's edits onto a real
        // Buffer, so the anchor is resolved by the engine rather than by
        // arithmetic on the expected text.
        Buffer buffer(CollabText::Test::actor(
            static_cast<uint8_t>(expectation.replica + 1)));
        std::vector<Operation> produced;
        for (const Event& event : fixture.trace.events) {
            switch (event.kind) {
            case EventKind::Edit:
                produced.push_back(buffer.apply_local_edit(
                    {{event.start, event.end}}, {event.text}));
                break;
            case EventKind::Deliver: {
                if (event.lose || event.op_index < 0 ||
                    static_cast<size_t>(event.op_index) >= produced.size())
                    break;
                buffer.apply_ops({produced[event.op_index]});
                break;
            }
            case EventKind::Undo: {
                if (auto op = buffer.undo()) produced.push_back(*op);
                break;
            }
            case EventKind::Redo: {
                if (auto op = buffer.redo()) produced.push_back(*op);
                break;
            }
            default:
                break;
            }
        }
        // The comment names the range boundaries; the range start is what a
        // reader resolves to position the comment.
        const Anchor start(
            Identity(CollabText::Test::actor(expectation.anchor_replica),
                     expectation.anchor_char),
            expectation.bias);
        return buffer.resolve_anchor(start);
    }
};

QTEST_GUILESS_MAIN(TestBehaviorFixtures)
#include "tst_behavior_fixtures.moc"
