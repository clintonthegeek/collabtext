// S02 executable evidence: the independent sequence interpreter runs every S01
// fixture in every supplied arrival order, and the total order it produces is
// handed to the independent BoundaryModel for the named token cases. Production
// Buffer, IdList, Locator, identity allocation and codecs are not linked, so a
// defect shared with production cannot make these pass.
#include "reference/SequenceModel.h"
#include "reference/BoundaryModel.h"

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <map>
#include <numeric>
#include <set>
#include <string>

using namespace SequenceReference;
namespace {

std::string str(const QJsonValue& value) { return value.toString().toStdString(); }
std::string join(const std::vector<std::string>& values) {
    std::string out;
    for (const auto& value : values) { out += value; out += ' '; }
    return out;
}
std::string join(const QString& root, const std::vector<std::string>& values) {
    return join(values);
}

QJsonObject read(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) qFatal("Cannot read %s", qPrintable(path));
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        qFatal("Invalid fixture %s: %s", qPrintable(path), qPrintable(error.errorString()));
    return document.object();
}

std::vector<std::string> strings(const QJsonArray& array) {
    std::vector<std::string> out;
    out.reserve(static_cast<size_t>(array.size()));
    for (const auto& value : array) out.push_back(str(value));
    return out;
}

struct Fixture {
    QString id;
    bool list = false;
    std::map<std::string, std::string> actor_bytes;
    BoundaryReference::Context context;
    std::vector<std::pair<std::string, std::string>> initial;
    std::vector<Operation> operations;
    std::vector<std::vector<std::string>> arrival_orders;
    QJsonArray tokens;
    QJsonObject expected;
};

Operation operation_from(const QJsonObject& object) {
    Operation op;
    op.id = str(object["id"]);
    const auto kind = str(object["kind"]);
    op.kind = kind == "insert" ? Operation::Kind::insert
              : kind == "delete" ? Operation::Kind::remove
              : kind == "undo" ? Operation::Kind::undo
                               : Operation::Kind::redo;
    op.after = strings(object["after"].toArray());
    if (object.contains("between")) {
        const auto between = object["between"].toArray();
        if (between.size() != 2) qFatal("between must name exactly two boundaries");
        op.between_left = str(between[0]);
        op.between_right = str(between[1]);
    }
    for (const auto& item : object["items"].toArray()) {
        const auto entry = item.toObject();
        op.items.emplace_back(str(entry["id"]), str(entry["value"]));
    }
    op.targets = strings(object["targets"].toArray());
    if (object.contains("target")) op.target = str(object["target"]);
    return op;
}

Fixture load(const QString& path) {
    const auto document = read(path);
    Fixture fixture;
    fixture.id = document["id"].toString();
    fixture.list = document["primitive"].toString() == QLatin1String("list");
    const auto actors = document["actors"].toObject();
    for (auto at = actors.begin(); at != actors.end(); ++at)
        fixture.actor_bytes[at.key().toStdString()] = at.value().toString().toStdString();
    const auto context = document["context"].toObject();
    fixture.context = {str(context["document"]), str(context["stream"]),
                       fixture.list ? BoundaryReference::Primitive::list : BoundaryReference::Primitive::text,
                       uint64_t(context["epoch"].toInteger())};
    for (const auto& item : document["initial"].toArray()) {
        const auto entry = item.toObject();
        fixture.initial.emplace_back(str(entry["id"]), str(entry["value"]));
    }
    for (const auto& operation : document["operations"].toArray())
        fixture.operations.push_back(operation_from(operation.toObject()));
    for (const auto& order : document["arrival_orders"].toArray())
        fixture.arrival_orders.push_back(strings(order.toArray()));
    fixture.tokens = document["tokens"].toArray();
    fixture.expected = document["expected"].toObject();
    return fixture;
}

Document build(const Fixture& fixture) {
    Document document(fixture.list, IdentityOrder(fixture.actor_bytes));
    document.seed(fixture.initial);
    return document;
}

/// BoundaryModel consumes a supplied total order; this is the hand-off between
/// two independent models. Hidden items stay in the order: an undone or deleted
/// item must keep its identity and its place, which is what a retained token and
/// a retained undo entry both need.
BoundaryReference::State boundary_state(const Fixture& fixture, const Document& document,
                                        const std::vector<std::string>& order) {
    BoundaryReference::State state{fixture.context, {}, {}};
    for (const auto& id : order) {
        const auto& item = document.items().at(id);
        BoundaryReference::Item entry;
        entry.id = id;
        entry.width = fixture.list ? 1u : uint32_t(std::max<size_t>(1, item.value.size()));
        entry.visible = item.visible();
        state.order.push_back(entry);
        state.integrated.insert(id);
    }
    if (!fixture.initial.empty()) state.integrated.insert("__initial_seed");
    return state;
}

BoundaryReference::Token token_from(const QJsonObject& object, const BoundaryReference::Context& context) {
    const auto between = object["between"].toArray();
    const auto left = str(between[0]);
    const auto right = str(between[1]);
    BoundaryReference::Token token;
    token.context = context;
    token.left = left == "START" ? std::nullopt : std::optional<std::string>(left);
    token.right = right == "END" ? std::nullopt : std::optional<std::string>(right);
    token.affinity = str(object["affinity"]) == "right" ? BoundaryReference::Affinity::right
                                                        : BoundaryReference::Affinity::left;
    return token;
}

QStringList fixture_names(const QString& root) {
    QDir directory(root);
    return directory.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
}

/// One actor typing `abc`, forward or in reverse, as its own operation per
/// scalar. Forward typing parents each scalar to the previous one; reverse
/// typing parents every scalar to START and names the previous scalar as the
/// right boundary, which is what puts it to the left. Both must read `abc`.
void append_typed_group(std::vector<Operation>& operations, const std::string& actor,
                        const std::string& word, bool reverse) {
    for (size_t at = 0; at < word.size(); ++at) {
        const auto letter = std::string(1, word[reverse ? word.size() - 1 - at : at]);
        const auto item = actor + "/" + std::to_string(at * 2 + 1);
        Operation op;
        op.id = actor + "/" + std::to_string(at * 2 + 4);
        op.items.emplace_back(item, letter);
        if (reverse) {
            op.between_left = "START";
            op.between_right = at == 0 ? "END" : actor + "/" + std::to_string((at - 1) * 2 + 1);
        } else {
            op.between_left = at == 0 ? "START" : actor + "/" + std::to_string((at - 1) * 2 + 1);
            op.between_right = "END";
        }
        if (at) op.after.push_back(actor + "/" + std::to_string((at - 1) * 2 + 4));
        operations.push_back(op);
    }
}

void enumerate_causal_orders(const std::vector<Operation>& operations,
                             std::vector<std::string>& prefix,
                             std::set<std::string>& delivered,
                             std::vector<std::vector<std::string>>& out) {
    if (prefix.size() == operations.size()) {
        out.push_back(prefix);
        return;
    }
    for (const auto& op : operations) {
        if (delivered.contains(op.id)) continue;
        if (!std::all_of(op.after.begin(), op.after.end(),
                         [&](const auto& dependency) { return delivered.contains(dependency); }))
            continue;
        delivered.insert(op.id);
        prefix.push_back(op.id);
        enumerate_causal_orders(operations, prefix, delivered, out);
        prefix.pop_back();
        delivered.erase(op.id);
    }
}

std::vector<std::vector<std::string>> causal_orders(const std::vector<Operation>& operations) {
    std::vector<std::vector<std::string>> out;
    std::vector<std::string> prefix;
    std::set<std::string> delivered;
    enumerate_causal_orders(operations, prefix, delivered, out);
    return out;
}

} // namespace

class SequenceModelTest : public QObject {
    Q_OBJECT
private slots:
    void everyFixtureOrderAndText();
    void everyArrivalOrderConverges();
    void duplicateAndReversedDelivery();
    void namedTokenPositionsAndComparisons();
    void sameTokenAcrossEveryCausalArrivalAndReplay();
    void undoRedoRestoresDeletedItems();
    void boundedEnumeration();
    void productionAtomizingRuleIsRejected();
};

/// The oracle has teeth, or it proves nothing. This reconstructs the rule the
/// engine uses today -- atomize a same-position run into single scalars and
/// order them by per-scalar identity (counter first, then actor) -- and shows
/// the S01 fixtures reject it. If this test ever stopped reproducing the
/// interleave, the fixtures would have stopped being able to detect the defect
/// they exist to detect.
namespace {
std::string production_atomized_text(const Fixture& fixture) {
    // Flatten every operation's items into single scalars, then order those
    // scalars by (counter, actor). This is what normalize_fragments does to two
    // concurrent multi-scalar pastes.
    std::vector<std::pair<ItemId, std::string>> scalars;
    for (const auto& op : fixture.operations) {
        if (op.kind != Operation::Kind::insert) continue;
        for (const auto& [id, value] : op.items) {
            const auto parsed = parse_id(id);
            if (parsed) scalars.emplace_back(*parsed, value);
        }
    }
    const IdentityOrder order(fixture.actor_bytes);
    std::stable_sort(scalars.begin(), scalars.end(),
                     [&](const auto& a, const auto& b) { return order(a.first, b.first); });
    std::string out;
    for (const auto& scalar : scalars) out += scalar.second;
    return out;
}
} // namespace

void SequenceModelTest::productionAtomizingRuleIsRejected() {
    const auto root = QString::fromUtf8(COLLABTEXT_SEQUENCE_FIXTURE_DIR);
    const auto paste = load(root + "/whole-concurrent-pastes.json");
    // The fixture's oracle, and what production actually produces today.
    QCOMPARE(QString::fromStdString(str(paste.expected["text"])), QStringLiteral("abcXYZ"));
    QCOMPARE(QString::fromStdString(production_atomized_text(paste)), QStringLiteral("aXbYcZ"));

    auto document = build(paste);
    QVERIFY(document.replay(paste.operations, paste.arrival_orders.front()).complete);
    QCOMPARE(QString::fromStdString(document.text()), QStringLiteral("abcXYZ"));

    // Only fixtures with no seed and no deletion are a fair comparison: for the
    // others the atomizing sketch ignores the seed and the deletes, so its
    // output would differ for reasons that have nothing to do with ordering.
    // These four are exactly the ones where the two rules disagree and nothing
    // else does.
    int rejected = 0;
    for (const auto& name : fixture_names(root)) {
        const auto fixture = load(root + QLatin1Char('/') + name);
        if (!fixture.initial.empty() || fixture.expected["text"].isNull()) continue;
        const bool inserts_only = std::all_of(fixture.operations.begin(), fixture.operations.end(),
                                              [](const Operation& op) { return op.kind == Operation::Kind::insert; });
        if (!inserts_only) continue;
        const auto atomized = production_atomized_text(fixture);
        const auto wanted = str(fixture.expected["text"]);
        if (atomized == wanted) continue;
        ++rejected;
        auto model = build(fixture);
        QVERIFY(model.replay(fixture.operations, fixture.arrival_orders.front()).complete);
        QCOMPARE(QString::fromStdString(model.text()), QString::fromStdString(wanted));
    }
    // forward-typing, nested-post-merge-edit, reverse-typing and
    // whole-concurrent-pastes: the atomizing rule interleaves every one of them
    // (`aXbYcZ`, `aXbYcZ!`, `cZbYaX`, `aXbYcZ`). typing-into-split-run is the
    // fifth: a peer splitting a typed run while its author keeps typing at the
    // end. If the fixtures stopped rejecting these, the oracle would have lost
    // the defect it exists for.
    QCOMPARE(rejected, 5);

    const auto reverse = load(root + "/reverse-typing.json");
    QCOMPARE(QString::fromStdString(production_atomized_text(reverse)), QStringLiteral("cZbYaX"));
    auto reverse_model = build(reverse);
    QVERIFY(reverse_model.replay(reverse.operations, reverse.arrival_orders.front()).complete);
    QCOMPARE(QString::fromStdString(reverse_model.text()), QStringLiteral("abcXYZ"));
}

/// The S01 oracle: exact order, exact hidden set, exact text. Not "the replicas
/// agree" -- agreement is what let `aXbYcZ` pass as convergence.
void SequenceModelTest::everyFixtureOrderAndText() {
    const auto names = fixture_names(QString::fromUtf8(COLLABTEXT_SEQUENCE_FIXTURE_DIR));
    QCOMPARE(names.size(), 18);
    for (const auto& name : names) {
        const auto fixture = load(QString::fromUtf8(COLLABTEXT_SEQUENCE_FIXTURE_DIR) + QLatin1Char('/') + name);
        auto document = build(fixture);
        const auto report = document.replay(fixture.operations, fixture.arrival_orders.front());
        QVERIFY2(report.complete, qPrintable(fixture.id + ": operations did not drain"));
        QVERIFY2(document.deferred().empty(), qPrintable(fixture.id + ": operations left deferred"));

        const auto& expected = fixture.expected;
        // `order` is the visible order; retained-but-hidden items are named
        // separately, and the total order including them is checked below.
        QCOMPARE(QString::fromStdString(join(document.visible_order())),
                 QString::fromStdString(join(strings(expected["order"].toArray()))));
        QCOMPARE(QString::fromStdString(join(document.hidden())),
                 QString::fromStdString(join(strings(expected["hidden"].toArray()))));
        if (expected.contains("text") && !expected["text"].isNull())
            QCOMPARE(QString::fromStdString(document.text()),
                     QString::fromStdString(str(expected["text"])));
        // No item is ever lost from the total order: visible plus hidden is the
        // whole set, which is what a retained token and a retained undo need.
        QCOMPARE(document.visible_order().size() + document.hidden().size(),
                 document.order().size());
    }
}

/// Every supplied arrival order, including the deliberately reversed ones, must
/// reach the same order, the same hidden set and the same text.
void SequenceModelTest::everyArrivalOrderConverges() {
    const auto root = QString::fromUtf8(COLLABTEXT_SEQUENCE_FIXTURE_DIR);
    int orders = 0;
    for (const auto& name : fixture_names(root)) {
        const auto fixture = load(root + QLatin1Char('/') + name);
        std::vector<std::string> reference_order, reference_hidden;
        std::string reference_text;
        bool first = true;
        for (const auto& arrival : fixture.arrival_orders) {
            auto document = build(fixture);
            const auto report = document.replay(fixture.operations, arrival);
            QVERIFY2(report.complete, qPrintable(fixture.id + ": arrival order did not drain"));
            if (first) {
                reference_order = document.order();
                reference_hidden = document.hidden();
                reference_text = document.text();
                first = false;
            } else {
                QCOMPARE(QString::fromStdString(join(document.order())),
                         QString::fromStdString(join(reference_order)));
                QCOMPARE(QString::fromStdString(join(document.hidden())),
                         QString::fromStdString(join(reference_hidden)));
                QCOMPARE(QString::fromStdString(document.text()),
                         QString::fromStdString(reference_text));
            }
            ++orders;
        }
    }
    QCOMPARE(orders, 32);
}

/// Duplicates must be counted, not silently reapplied, and delivering an
/// operation before its prerequisite must defer and drain rather than invent a
/// different causal history.
void SequenceModelTest::duplicateAndReversedDelivery() {
    const auto root = QString::fromUtf8(COLLABTEXT_SEQUENCE_FIXTURE_DIR);
    const auto paste = load(root + "/whole-concurrent-pastes.json");

    auto straight = build(paste);
    const auto first = straight.replay(paste.operations, {"A/4", "B/4"});

    auto noisy = build(paste);
    const auto second = noisy.replay(paste.operations, {"B/4", "A/4", "B/4", "A/4"});
    QCOMPARE(second.integrated, first.integrated);
    QCOMPARE(second.duplicate, 2u);
    QCOMPARE(QString::fromStdString(noisy.text()), QString::fromStdString(straight.text()));

    const auto nested = load(root + "/nested-post-merge-edit.json");
    auto causal = build(nested);
    const auto report = causal.replay(nested.operations, {"B/6", "A/4", "B/4"});
    QVERIFY(report.deferred > 0);
    QVERIFY(report.complete);
    QCOMPARE(QString::fromStdString(causal.text()), QStringLiteral("a!bcXYZ"));
}

/// The W2 named cases: exact offsets and states from the fixture, plus the exact
/// comparison between originally minted tokens. This is where the sequence order
/// and the boundary contract meet.
void SequenceModelTest::namedTokenPositionsAndComparisons() {
    const auto root = QString::fromUtf8(COLLABTEXT_SEQUENCE_FIXTURE_DIR);
    int token_fixtures = 0, positions = 0, comparisons = 0;
    for (const auto& name : fixture_names(root)) {
        const auto fixture = load(root + QLatin1Char('/') + name);
        if (fixture.tokens.isEmpty()) continue;
        ++token_fixtures;
        auto document = build(fixture);
        QVERIFY(document.replay(fixture.operations, fixture.arrival_orders.front()).complete);

        const auto order = document.order();
        const auto state = boundary_state(fixture, document, order);
        std::map<std::string, BoundaryReference::Token> tokens;
        for (const auto& entry : fixture.tokens) {
            const auto object = entry.toObject();
            tokens[str(object["id"])] = token_from(object, state.context);
        }
        if (!fixture.initial.empty()) {
            for (auto& [id, token] : tokens) token.mint_state.insert("__initial_seed");
        }
        // Mint state is the causal history the token was minted against: the
        // seed when the document had one, and nothing otherwise. Claiming a seed
        // a fixture never had would make every lookup await state.
        if (!fixture.initial.empty()) {
            for (auto& [id, token] : tokens) token.mint_state.insert("__initial_seed");
        }
        const auto expected_positions = fixture.expected["positions"].toObject();
        for (const auto& entry : fixture.tokens) {
            const auto object = entry.toObject();
            const auto id = str(object["id"]);
            const auto& token = tokens.at(id);
            const auto resolution = BoundaryReference::resolve(state, token);
            const auto wanted = expected_positions.value(QString::fromStdString(id)).toObject();
            QVERIFY2(!wanted.isEmpty(), qPrintable(QStringLiteral("%1: token %2 has no expected position")
                                              .arg(fixture.id, QString::fromStdString(id))));
            // "collapsed" is a resolved result carrying explicit collapse, not
            // a separate outcome: the offset stays usable.
            const auto wanted_name = str(wanted["state"]);
            if (wanted_name == "collapsed") {
                QCOMPARE(QString::fromStdString(std::string(BoundaryReference::status_name(resolution.status))),
                         QStringLiteral("resolved"));
                QVERIFY(resolution.collapsed());
            } else {
                const auto wanted_status = BoundaryReference::status_from_name(wanted_name);
                QVERIFY2(wanted_status.has_value(),
                         qPrintable(QStringLiteral("%1: unknown expected state %2")
                                        .arg(fixture.id, QString::fromStdString(wanted_name))));
                QCOMPARE(QString::fromStdString(std::string(BoundaryReference::status_name(resolution.status))),
                         QString::fromStdString(wanted_name));
            }
            if (resolution.status == BoundaryReference::Status::resolved)
                QCOMPARE(resolution.offset, uint32_t(wanted["offset"].toInteger()));
            ++positions;
        }
        for (const auto& entry : fixture.expected["comparisons"].toArray()) {
            const auto object = entry.toObject();
            const auto result = BoundaryReference::compare(state, tokens.at(str(object["left"])),
                                                           tokens.at(str(object["right"])));
            QVERIFY(result.has_value());
            const auto observed = *result < 0 ? "less" : *result > 0 ? "greater" : "equal";
            QCOMPARE(QString::fromStdString(observed), QString::fromStdString(str(object["order"])));
            ++comparisons;
        }
    }
    // 8 of the 17 fixtures carry named tokens: 22 positions and 14 comparisons
    // in total. These are the S01 W2 named cases, not a sampled subset.
    QCOMPARE(token_fixtures, 8);
    QCOMPARE(positions, 22);
    QCOMPARE(comparisons, 14);
}

/// Carry each fixture's originally minted token values through every arrival
/// order and a fresh reference-document reconstruction. Duplicate each delivery
/// as well; the assertions target resolution of the same token, not a newly
/// minted boundary in each final state.
void SequenceModelTest::sameTokenAcrossEveryCausalArrivalAndReplay() {
    const auto root = QString::fromUtf8(COLLABTEXT_SEQUENCE_FIXTURE_DIR);
    size_t fixture_orders = 0, positions = 0, comparisons = 0, duplicate_deliveries = 0;
    for (const auto& name : fixture_names(root)) {
        const auto fixture = load(root + QLatin1Char('/') + name);
        if (fixture.tokens.isEmpty()) continue;
        auto seed_document = build(fixture);
        QVERIFY(seed_document.replay(fixture.operations,
                                     fixture.arrival_orders.front()).complete);
        const auto seed_state = boundary_state(fixture, seed_document,
                                               seed_document.order());
        std::map<std::string, BoundaryReference::Token> persisted;
        for (const auto& entry : fixture.tokens) {
            const auto object = entry.toObject();
            auto token = token_from(object, seed_state.context);
            if (!fixture.initial.empty()) token.mint_state.insert("__initial_seed");
            persisted.emplace(str(object["id"]), std::move(token));
        }

        for (const auto& arrival : fixture.arrival_orders) {
            auto restarted = build(fixture);
            std::vector<std::string> duplicate_order;
            duplicate_order.reserve(arrival.size() * 2);
            for (const auto& id : arrival) {
                duplicate_order.push_back(id);
                duplicate_order.push_back(id);
            }
            const auto report = restarted.replay(fixture.operations, duplicate_order);
            QVERIFY2(report.complete, qPrintable(fixture.id + ": duplicate replay did not drain"));
            QCOMPARE(report.duplicate, arrival.size());
            const auto state = boundary_state(fixture, restarted, restarted.order());
            for (const auto& entry : fixture.tokens) {
                const auto object = entry.toObject();
                const auto id = str(object["id"]);
                const auto actual = BoundaryReference::resolve(state, persisted.at(id));
                const auto wanted = fixture.expected["positions"].toObject()
                                        .value(QString::fromStdString(id)).toObject();
                const auto wanted_state = str(wanted["state"]);
                if (wanted_state == "collapsed") {
                    QCOMPARE(actual.status, BoundaryReference::Status::resolved);
                    QVERIFY(actual.collapsed());
                } else {
                    const auto status = BoundaryReference::status_from_name(wanted_state);
                    QVERIFY(status.has_value());
                    QCOMPARE(actual.status, *status);
                }
                if (actual.status == BoundaryReference::Status::resolved)
                    QCOMPARE(actual.offset, uint32_t(wanted["offset"].toInteger()));
                ++positions;
            }
            for (const auto& entry : fixture.expected["comparisons"].toArray()) {
                const auto object = entry.toObject();
                const auto comparison = BoundaryReference::compare(
                    state, persisted.at(str(object["left"])), persisted.at(str(object["right"])));
                QVERIFY(comparison.has_value());
                const auto actual = *comparison < 0 ? "less" : *comparison > 0 ? "greater" : "equal";
                QCOMPARE(QString::fromStdString(actual), QString::fromStdString(str(object["order"])));
                ++comparisons;
            }
            ++fixture_orders;
            duplicate_deliveries += arrival.size();
        }
    }
    QCOMPARE(fixture_orders, size_t(11));
    QCOMPARE(positions, size_t(28));
    QCOMPARE(comparisons, size_t(17));
    QCOMPARE(duplicate_deliveries, size_t(17));
}

/// A local deletion is an operation with an undoable effect. Keep the item's
/// identity and position while toggling that effect, and ensure redo restores
/// the deletion rather than restoring the item.
void SequenceModelTest::undoRedoRestoresDeletedItems() {
    Document document(false, IdentityOrder({{"A", std::string(31, '0') + "1"}}));
    document.seed({{"A/1", "x"}});
    Operation remove;
    remove.id = "A/4";
    remove.kind = Operation::Kind::remove;
    remove.targets = {"A/1"};
    Operation undo;
    undo.id = "A/6";
    undo.kind = Operation::Kind::undo;
    undo.after = {remove.id};
    undo.target = remove.id;
    Operation redo;
    redo.id = "A/8";
    redo.kind = Operation::Kind::redo;
    redo.after = {undo.id};
    redo.target = remove.id;

    QVERIFY(document.integrate(remove));
    QCOMPARE(QString::fromStdString(document.text()), QString());
    QVERIFY(document.integrate(undo));
    QCOMPARE(QString::fromStdString(document.text()), QStringLiteral("x"));
    QCOMPARE(document.order(), std::vector<std::string>{"A/1"});
    QVERIFY(document.integrate(redo));
    QCOMPARE(QString::fromStdString(document.text()), QString());
    QCOMPARE(document.order(), std::vector<std::string>{"A/1"});
}

/// Bounded enumeration over the shapes that decide the repair: two and three
/// concurrent typing groups, each typed forward or in reverse. Every group must
/// stay contiguous -- interleaving them is precisely the F8 defect.
void SequenceModelTest::boundedEnumeration() {
    const std::map<std::string, std::string> actors{
        {"A", std::string(31, '0') + "1"}, {"B", std::string(31, '0') + "2"}, {"C", std::string(31, '0') + "3"}};
    const std::vector<std::string> words{"abc", "XYZ", "mnq"};
    size_t shapes = 0, histories = 0, duplicate_histories = 0, deferred_histories = 0;
    for (int groups = 2; groups <= 3; ++groups) {
        for (int mask = 0; mask < (1 << groups); ++mask) {
            std::vector<Operation> operations;
            std::vector<std::string> arrival;
            for (int g = 0; g < groups; ++g) {
                const auto actor = std::string(1, char('A' + g));
                const auto before = size_t(operations.size());
                append_typed_group(operations, actor, words[static_cast<size_t>(g)], (mask >> g) & 1);
                for (size_t at = before; at < operations.size(); ++at) arrival.push_back(operations[at].id);
            }
            // Expected: groups in branch order -- first item counter, then actor
            // bytes -- each reading its own word.
            std::string expected;
            for (int g = 0; g < groups; ++g) expected += words[static_cast<size_t>(g)];

            ++shapes;
            const auto orders = causal_orders(operations);
            QVERIFY(!orders.empty());
            for (const auto& causal_order : orders) {
                auto document = Document(false, IdentityOrder(actors));
                const auto report = document.replay(operations, causal_order);
                QVERIFY(report.complete);
                QCOMPARE(QString::fromStdString(document.text()), QString::fromStdString(expected));
                const auto order = document.order();
                QCOMPARE(order.size(), size_t(groups) * 3);
                std::set<std::string> started;
                std::string previous_actor;
                for (const auto& id : order) {
                    const auto parsed = parse_id(id);
                    QVERIFY(parsed.has_value());
                    if (parsed->actor != previous_actor) {
                        QVERIFY2(started.insert(parsed->actor).second,
                                 qPrintable(QStringLiteral("actor ") + QString::fromStdString(parsed->actor)
                                            + QStringLiteral(" appears in two runs: ")
                                            + QString::fromStdString(join(order))));
                        previous_actor = parsed->actor;
                    }
                }
                auto with_duplicates = causal_order;
                for (const auto& id : causal_order) with_duplicates.push_back(id);
                auto duplicate_document = Document(false, IdentityOrder(actors));
                const auto duplicate_report = duplicate_document.replay(operations, with_duplicates);
                QVERIFY(duplicate_report.complete);
                QCOMPARE(duplicate_report.duplicate, causal_order.size());
                QCOMPARE(QString::fromStdString(join(duplicate_document.order())),
                         QString::fromStdString(join(document.order())));
                ++histories;
                ++duplicate_histories;
            }
            auto reverse_delivery = arrival;
            std::reverse(reverse_delivery.begin(), reverse_delivery.end());
            auto deferred = Document(false, IdentityOrder(actors));
            const auto deferred_report = deferred.replay(operations, reverse_delivery);
            QVERIFY(deferred_report.complete);
            QVERIFY(deferred_report.deferred > 0);
            QCOMPARE(QString::fromStdString(deferred.text()), QString::fromStdString(expected));
            ++deferred_histories;
        }
    }
    QCOMPARE(shapes, size_t(12));
    QCOMPARE(histories, size_t(13520));
    QCOMPARE(duplicate_histories, histories);
    QCOMPARE(deferred_histories, shapes);
}

QTEST_MAIN(SequenceModelTest)
#include "tst_sequence_model.moc"
