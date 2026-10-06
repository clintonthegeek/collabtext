// S03 executable evidence for the PRODUCTION ordering rule.
//
// tst_sequence_model.cpp runs the S01 fixtures through the independent reference
// interpreter. This file runs the same question through the production Buffer:
// concurrent whole passages must stay contiguous and land in the exact order the
// S02 rule specifies -- the first inserted item of each operation, counter
// ascending then actor bytes ascending.
//
// This deliberately does not rebuild the fixtures' causal histories. Driving
// Buffer from fixture `between` boundaries needs a per-actor causal delivery rig
// (whose own operations are local so they enter the undo stack, and whose peers'
// arrive only when the fixture says they were observed). That rig is worth having,
// but it is test scaffolding for S04's installed replay rather than evidence that
// the ordering rule was applied, and a bug in it would look exactly like a bug in
// the engine. The property below reaches the same rule through the public API.
#include "crdt/Buffer.h"
#include "crdt/IdList.h"

#include <collabtext/Operations.h>

#include "TestActor.h"

#include <QtTest>
#include <QtGlobal>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace CollabText;
using namespace CollabText::Crdt;
using namespace CollabText::Test;

namespace {

/// The typing rows are a known, *documented* failure rather than a broken test:
/// S03 could not express group-atomic typing with the flat locator chain, and
/// S01's oracle is deliberately left intact so the requirement stays visible.
/// Strict mode turns the marker back into a hard failure, so a fix has to be
/// promoted deliberately rather than arriving silently.
#define SEQUENCE_EXPECT_FAIL(reason) \
    do { \
        if (!qEnvironmentVariableIsSet("COLLABTEXT_STRICT_EVALUATION")) \
            QEXPECT_FAIL("", reason, Continue); \
    } while (false)

/// One actor's whole burst, minted as a single local edit at `at`. Production
/// reserves n+1 identities for n scalars, so the run's items are contiguous and
/// its operation identity sits above them -- the interval S02's rule ranks on.
Operation paste(Buffer& buffer, uint32_t at, const std::string& run) {
    return buffer.apply_local_edit({{int(at), int(at)}}, {run});
}

/// Every scalar of `run` must still be adjacent to its neighbours: no other
/// actor's scalar may appear between two of them.
bool contiguous(const std::string& text, const std::string& run) {
    const auto at = text.find(run);
    return at != std::string::npos;
}

} // namespace

class SequenceSemanticsTest : public QObject {
    Q_OBJECT
private slots:
    void concurrentWholePassagesStayContiguous_data();
    void concurrentWholePassagesStayContiguous();
    void concurrentTypingStaysGrouped_data();
    void concurrentTypingStaysGrouped();
    void insertionInsideTypedRunConverges();
    void everyS01FixtureMatchesProductionInEveryArrivalOrder();
    void listTieIsDeterministic_data();
    void listTieIsDeterministic();
};

void SequenceSemanticsTest::concurrentWholePassagesStayContiguous_data() {
    QTest::addColumn<QString>("first");
    QTest::addColumn<QString>("second");
    QTest::addColumn<QString>("third");
    QTest::newRow("two-pastes") << "abc" << "XYZ" << QString();
    QTest::newRow("unequal-lengths") << "ab" << "WXYZ" << QString();
    QTest::newRow("three-pastes") << "abc" << "XYZ" << "mnq";
}

void SequenceSemanticsTest::concurrentWholePassagesStayContiguous() {
    QFETCH(QString, first);
    QFETCH(QString, second);
    QFETCH(QString, third);
    const std::array<QString, 3> runs{first, second, third};
    const auto actors = runs.size();

    // Each actor pastes its whole run into an empty document, so every run sits
    // at the same position and the tie-break is the only thing that can order
    // them. Actors 1..n; Test::actor is big-endian in the low byte, so actor byte
    // order is actor index order.
    std::vector<Buffer> buffers;
    std::vector<Operation> ops;
    for (size_t n = 0; n < actors; ++n) {
        buffers.emplace_back(test_counter(uint8_t(n + 1)));
        if (runs[n].isEmpty()) continue;
        ops.push_back(paste(buffers.back(), 0, runs[n].toStdString()));
    }
    // Deliver every operation to every replica, including its own author, so each
    // replica holds the same set.
    for (auto& buffer : buffers)
        for (const auto& op : ops) QVERIFY(buffer.apply_ops({op})[0].kind() != ApplyOutcomeKind::rejected);

    // S02 R2: groups at the same position are ordered by their first inserted
    // item, counter then actor bytes. All three counters are 1 here, so the actor
    // bytes decide and the order is the run order.
    std::string oracle;
    for (size_t n = 0; n < actors; ++n) oracle += runs[n].toStdString();

    for (auto& buffer : buffers) {
        const auto text = buffer.text();
        QVERIFY2(text == oracle,
                 qPrintable(QStringLiteral("replicas produced \"%1\", oracle \"%2\"")
                                .arg(QString::fromStdString(text), QString::fromStdString(oracle))));
        // Contiguity is the actual F8 defect: `aXbYcZ` was perfectly convergent
        // and destroyed both passages.
        for (size_t n = 0; n < actors; ++n)
            QVERIFY2(contiguous(text, runs[n].toStdString()),
                     qPrintable(QStringLiteral("run \"%1\" is not contiguous in \"%2\"")
                                    .arg(runs[n], QString::fromStdString(text))));
    }
}

/// The same question for typing rather than pasting: three scalars inserted one at
/// a time, forward and in reverse. The S01 oracle is `abcXYZ` in every case.
void SequenceSemanticsTest::concurrentTypingStaysGrouped_data() {
    QTest::addColumn<bool>("reverse");
    QTest::newRow("forward") << false;
    QTest::newRow("reverse") << true;
}

void SequenceSemanticsTest::concurrentTypingStaysGrouped() {
    QFETCH(bool, reverse);
    const std::string word = "abc";
    std::vector<Buffer> buffers;
    std::vector<Operation> ops;
    for (uint8_t actor = 1; actor <= 3; ++actor) {
        buffers.emplace_back(test_counter(actor));
        // Each actor types its own word one scalar at a time into its own empty
        // view, then the three are delivered. Forward typing appends; reverse
        // typing prepends, which is the S01 `reverse-typing` shape -- both must
        // read `abc`.
        auto& buffer = buffers.back();
        for (size_t n = 0; n < word.size(); ++n) {
            const auto letter = reverse ? std::string(1, word[word.size() - 1 - n]) : std::string(1, word[n]);
            const uint32_t at = reverse ? 0u : uint32_t(buffer.text().size());
            ops.push_back(paste(buffer, at, letter));
        }
    }
    for (auto& buffer : buffers)
        for (const auto& op : ops) QVERIFY(buffer.apply_ops({op})[0].kind() != ApplyOutcomeKind::rejected);

    std::string oracle;
    for (int actor = 0; actor < 3; ++actor) oracle += word;
    // One assertion for every replica: a marker covers a single failure, and two
    // failing QCOMPAREs in one row would report the second as a real defect.
    std::string observed;
    for (auto& buffer : buffers) observed += buffer.text();
    const auto repeated = [&] {
        std::string out;
        for (int n = 0; n < 3; ++n) out += oracle;
        return out;
    }();
    QVERIFY2(observed == repeated,
             qPrintable(QStringLiteral("%1 typing produced \"%2\", oracle \"%3\"")
                            .arg(reverse ? QStringLiteral("reverse") : QStringLiteral("forward"),
                                 QString::fromStdString(observed), QString::fromStdString(repeated))));
}

/// A peer inserting inside a reused-locator typed run must relocate the WHOLE tail
/// of the run, not just its first fragment. Typed runs are not counter-contiguous
/// (each operation's own identity takes a counter), which once stranded the tail
/// of the run behind the insertion. Covers the local path, the remote path, and a
/// concurrent continuation by the run's author.
void SequenceSemanticsTest::insertionInsideTypedRunConverges() {
    Buffer alice(test_counter(1)), bob(test_counter(2));
    std::vector<Operation> shared;
    const std::string word = "abcdef";
    for (size_t n = 0; n < word.size(); ++n) {
        auto op = paste(alice, uint32_t(n), std::string(1, word[n]));
        QVERIFY(bob.apply_ops({op})[0].kind() != ApplyOutcomeKind::rejected);
    }
    QCOMPARE(bob.text(), word);

    // Bob types inside the run while Alice, concurrently, keeps typing at its end.
    const auto bob1 = paste(bob, 3, "X");
    const auto bob2 = paste(bob, 4, "Y");
    QCOMPARE(bob.text(), std::string("abcXYdef"));
    const auto alice1 = paste(alice, 6, "g");
    const auto alice2 = paste(alice, 7, "h");

    for (const auto& op : {bob1, bob2})
        QVERIFY(alice.apply_ops({op})[0].kind() != ApplyOutcomeKind::rejected);
    for (const auto& op : {alice1, alice2})
        QVERIFY(bob.apply_ops({op})[0].kind() != ApplyOutcomeKind::rejected);

    // Replicas agree, and the concurrent continuation follows the run it continued:
    // `gh` was typed after `f`, so it lands after `def` however the peer's `XY`
    // split the run. A reused or fresh locator could not say that; the tree link can.
    QCOMPARE(alice.text(), bob.text());
    QCOMPARE(alice.text(), std::string("abcXYdefgh"));
}

// ---------------------------------------------------------------------------
// S01 fixtures through the production engines
// ---------------------------------------------------------------------------
//
// tst_sequence_model runs these histories through the reference interpreters and
// tst_fuguemax_model through the literal FugueMax. This runs them through the
// engine. The fixtures describe operations by *boundaries* ("between A/3 and
// B/1"), so each operation is minted by a persistent per-actor replica that has
// observed exactly its causal prerequisites, at the offset that boundary names;
// the engine's own identity allocation must then reproduce the fixture's operation
// identity (A/4, B/6, ...), which is checked rather than assumed. Every arrival
// order is then replayed into a fresh replica and the visible order compared with
// the fixture's exact oracle.

namespace {

using Json = QJsonObject;

uint8_t actor_index_of(const Json& actors, const QString& alias) {
    return uint8_t(actors.value(alias).toString().right(2).toUInt(nullptr, 16));
}

Identity fixture_identity(const Json& actors, const QString& text) {
    const auto parts = text.split('/');
    return Identity(actor(actor_index_of(actors, parts.at(0))), parts.at(1).toULongLong());
}

QString identity_name(const Json& actors, const Identity& id) {
    for (auto it = actors.begin(); it != actors.end(); ++it)
        if (actor(actor_index_of(actors, it.key())) == id.actor)
            return it.key() + "/" + QString::number(id.counter);
    return QStringLiteral("?/") + QString::number(id.counter);
}

/// Visible item identities of a Buffer in document order.
std::vector<Identity> visible_items(const Buffer& buffer) {
    std::vector<Identity> out;
    for (const auto& f : buffer.fragments())
        if (f.visible)
            for (uint32_t i = 0; i < f.length; ++i) out.push_back(f.item_at(i));
    return out;
}

/// Byte offset just after `item` in `buffer`'s visible text, or 0 for START. An
/// item that is not visible contributes nothing, so a boundary naming a tombstone
/// resolves to "after the last visible item before it".
uint32_t offset_after(const Buffer& buffer, const QString& boundary, const Json& actors) {
    if (boundary == "START") return 0;
    const Identity target = fixture_identity(actors, boundary);
    uint32_t offset = 0;
    for (const auto& f : buffer.fragments()) {
        uint32_t scalar_offset = 0;
        for (uint32_t i = 0; i < f.length; ++i) {
            const uint32_t width = [&] {
                const auto lead = static_cast<unsigned char>(f.text[scalar_offset]);
                return lead < 0x80 ? 1u : (lead & 0xE0) == 0xC0 ? 2u : (lead & 0xF0) == 0xE0 ? 3u : 4u;
            }();
            if (f.visible) offset += width;
            scalar_offset += width;
            if (f.item_at(i) == target) return offset;
        }
    }
    return offset;
}

/// Byte range [start, end) of a visible `item` in `buffer`, if it is visible.
std::optional<std::pair<uint32_t, uint32_t>> range_of(const Buffer& buffer, const Identity& item) {
    uint32_t offset = 0;
    for (const auto& f : buffer.fragments()) {
        uint32_t scalar_offset = 0;
        for (uint32_t i = 0; i < f.length; ++i) {
            const auto lead = static_cast<unsigned char>(f.text[scalar_offset]);
            const uint32_t width = lead < 0x80 ? 1u : (lead & 0xE0) == 0xC0 ? 2u : (lead & 0xF0) == 0xE0 ? 3u : 4u;
            if (f.item_at(i) == item) {
                if (!f.visible) return std::nullopt;
                return std::make_pair(offset, offset + width);
            }
            if (f.visible) offset += width;
            scalar_offset += width;
        }
    }
    return std::nullopt;
}

}  // namespace

void SequenceSemanticsTest::everyS01FixtureMatchesProductionInEveryArrivalOrder() {
    const QString root = QStringLiteral(COLLABTEXT_SEQUENCE_FIXTURE_DIR);
    const auto files = QDir(root).entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    QVERIFY(files.size() >= 18);

    for (const auto& file : files) {
        QFile in(root + "/" + file);
        QVERIFY(in.open(QIODevice::ReadOnly));
        const Json doc = QJsonDocument::fromJson(in.readAll()).object();
        const QString fixture = doc["id"].toString();
        const Json actors = doc["actors"].toObject();
        const bool list = doc["primitive"].toString() == "list";
        const QString where = fixture + ": ";

        // The seed: initial items are observed through one operation by the seed actor.
        std::optional<Operation> seed_text;
        std::optional<IdListOperation> seed_list;
        const auto initial = doc["initial"].toArray();
        if (!initial.isEmpty()) {
            const QString seed_alias = initial[0].toObject()["id"].toString().section('/', 0, 0);
            if (list) {
                QSKIP("list fixtures in S01 carry no initial items");
            } else {
                Buffer seeder(test_counter(actor_index_of(actors, seed_alias)));
                std::string run;
                for (const auto& item : initial) run += item.toObject()["value"].toString().toStdString();
                seed_text = seeder.apply_local_edit({{0, 0}}, {run});
            }
        }

        // One persistent replica per actor, so undo/redo stacks are the author's.
        std::map<QString, std::unique_ptr<Buffer>> text_replicas;
        std::map<QString, std::unique_ptr<IdList>> list_replicas;
        std::map<QString, std::set<QString>> delivered;
        std::map<QString, Operation> text_ops;
        std::map<QString, IdListOperation> list_ops;
        std::map<QString, std::set<QString>> closure;
        std::vector<QString> op_order;

        auto replica_text = [&](const QString& alias) -> Buffer& {
            auto& slot = text_replicas[alias];
            if (!slot) {
                slot = std::make_unique<Buffer>(test_counter(actor_index_of(actors, alias)));
                if (seed_text) slot->apply_ops({*seed_text});
            }
            return *slot;
        };
        auto replica_list = [&](const QString& alias) -> IdList& {
            auto& slot = list_replicas[alias];
            if (!slot) slot = std::make_unique<IdList>(test_counter(actor_index_of(actors, alias)));
            return *slot;
        };

        for (const auto& value : doc["operations"].toArray()) {
            const Json op = value.toObject();
            const QString id = op["id"].toString();
            const QString alias = id.section('/', 0, 0);
            const QString kind = op["kind"].toString();

            std::set<QString> deps;
            for (const auto& a : op["after"].toArray()) {
                const QString dep = a.toString();
                deps.insert(dep);
                deps.insert(closure[dep].begin(), closure[dep].end());
            }
            closure[id] = deps;

            // Deliver exactly the causal prerequisites this actor has not yet seen.
            for (const auto& prior : op_order) {
                if (!deps.count(prior) || delivered[alias].count(prior)) continue;
                if (list) {
                    QVERIFY2(replica_list(alias).apply_ops({list_ops.at(prior)})[0].kind() == ApplyOutcomeKind::applied,
                             qPrintable(where + "prerequisite " + prior + " not applied"));
                } else {
                    QVERIFY2(replica_text(alias).apply_ops({text_ops.at(prior)})[0].kind() == ApplyOutcomeKind::applied,
                             qPrintable(where + "prerequisite " + prior + " not applied"));
                }
                delivered[alias].insert(prior);
            }

            Identity produced_id;
            if (list) {
                IdList& rep = replica_list(alias);
                std::optional<IdListOperation> made;
                if (kind == "insert") {
                    const QString left = op["between"].toArray()[0].toString();
                    const auto item = op["items"].toArray()[0].toObject();
                    const Anchor anchor = left == "START" ? Anchor::min()
                                                          : Anchor(fixture_identity(actors, left), Bias::Left);
                    auto r = rep.try_insert_after(anchor, uint64_t(item["value"].toInteger()));
                    QVERIFY2(r.ok(), qPrintable(where + "insert refused"));
                    made = r.value();
                } else if (kind == "undo") {
                    made = rep.undo();
                } else if (kind == "redo") {
                    made = rep.redo();
                }
                QVERIFY2(made.has_value(), qPrintable(where + id + " produced no operation"));
                produced_id = idlist_op_identity(*made);
                list_ops[id] = *made;
            } else {
                Buffer& rep = replica_text(alias);
                std::optional<Operation> made;
                if (kind == "insert") {
                    const QString left = op["between"].toArray()[0].toString();
                    std::string content;
                    for (const auto& item : op["items"].toArray())
                        content += item.toObject()["value"].toString().toStdString();
                    const uint32_t at = offset_after(rep, left, actors);
                    made = rep.apply_local_edit({{at, at}}, {content});
                } else if (kind == "delete") {
                    std::vector<std::pair<uint32_t, uint32_t>> ranges;
                    for (const auto& t : op["targets"].toArray()) {
                        if (const auto r = range_of(rep, fixture_identity(actors, t.toString()))) {
                            if (!ranges.empty() && ranges.back().second == r->first) ranges.back().second = r->second;
                            else ranges.push_back(*r);
                        }
                    }
                    std::sort(ranges.begin(), ranges.end());
                    std::vector<std::string> blanks(ranges.size());
                    made = rep.try_apply_local_edit(ranges, blanks).value();
                } else if (kind == "undo") {
                    made = rep.undo();
                } else if (kind == "redo") {
                    made = rep.redo();
                }
                QVERIFY2(made.has_value(), qPrintable(where + id + " produced no operation"));
                produced_id = op_identity(*made);
                text_ops[id] = *made;
            }
            QCOMPARE(identity_name(actors, produced_id), id);   // the engine reproduced the fixture's identity
            delivered[alias].insert(id);
            op_order.push_back(id);
        }

        // Every arrival order, replayed into a fresh replica.
        QStringList want;
        for (const auto& e : doc["expected"].toObject()["order"].toArray()) want << e.toString();
        std::set<QString> hidden;
        for (const auto& e : doc["expected"].toObject()["hidden"].toArray()) hidden.insert(e.toString());

        for (const auto& arrival : doc["arrival_orders"].toArray()) {
            QStringList got;
            if (list) {
                IdList fresh(test_counter(99));
                for (const auto& v : arrival.toArray()) fresh.apply_ops({list_ops.at(v.toString())});
                QCOMPARE(fresh.apply_ops({}).size(), size_t(0));
                for (const auto& e : fresh.entries())
                    if (e.visible) got << identity_name(actors, e.origin);
            } else {
                Buffer fresh(test_counter(99));
                if (seed_text) fresh.apply_ops({*seed_text});
                for (const auto& v : arrival.toArray()) fresh.apply_ops({text_ops.at(v.toString())});
                QCOMPARE(fresh.deferred_count(), size_t(0));
                for (const auto& id : visible_items(fresh)) got << identity_name(actors, id);
            }
            QVERIFY2(got == want, qPrintable(where + "visible order " + got.join(' ') + ", oracle " + want.join(' ')));
        }
    }
}

/// IdList's equal-boundary tie must be deterministic across replicas. This is the
/// same rule in miniature: two inserts at the document start, and both replicas
/// must agree on the order, with the values preserved exactly.
void SequenceSemanticsTest::listTieIsDeterministic_data() {
    QTest::addColumn<quint64>("second");
    QTest::newRow("small") << quint64(43);
    // Above 2^53: any double-precision path would silently round this.
    QTest::newRow("above-2-53") << quint64(9223372036854775815ULL);
}

void SequenceSemanticsTest::listTieIsDeterministic() {
    QFETCH(quint64, second);
    IdList a(test_counter(1)), b(test_counter(2));
    const auto first_op = a.insert_after(Anchor::min(), 42);
    const auto second_op = b.insert_after(Anchor::min(), second);
    QVERIFY(a.apply_ops({second_op})[0].kind() != ApplyOutcomeKind::rejected);
    QVERIFY(b.apply_ops({first_op})[0].kind() != ApplyOutcomeKind::rejected);

    // Actor 1's bytes sort before actor 2's at the same position, so 42 leads.
    const std::vector<uint64_t> oracle{42, uint64_t(second)};
    QCOMPARE(a.ids(), oracle);
    QCOMPARE(b.ids(), oracle);

    IdList replica(test_counter(1));
    QVERIFY(replica.apply_ops({first_op})[0].kind() != ApplyOutcomeKind::rejected);
    QVERIFY(replica.apply_ops({second_op})[0].kind() != ApplyOutcomeKind::rejected);
    QCOMPARE(replica.ids(), oracle);
}

QTEST_MAIN(SequenceSemanticsTest)
#include "tst_sequence_semantics.moc"