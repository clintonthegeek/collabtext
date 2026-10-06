// S03 design-gate evidence for the FugueMax placement decision.
//
// FugueMaxModel.h is a literal transcription of the published algorithm. This
// file (1) checks it against the worked examples in the paper, (2) checks the
// properties the paper proves -- convergence, forward non-interleaving, and the
// equal-origin tie rule -- on seeded random multi-replica histories, and (3)
// cross-checks the S02 SequenceReference model against it, so the two
// independent references cannot silently disagree about document order.
//
// Neither model links the production engine.
#include "reference/FugueMaxModel.h"
#include "reference/SequenceModel.h"

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace FugueReference;

namespace {

/// Counter first, then actor bytes: the engine's `Identity` order, and S02's R2.
bool id_less(const Id& a, const Id& b) {
    if (a.counter != b.counter) return a.counter < b.counter;
    return a.actor < b.actor;
}

std::string name(const Id& id) { return id.actor + "/" + std::to_string(id.counter); }

std::vector<std::string> names(const std::vector<Id>& ids) {
    std::vector<std::string> out;
    for (const auto& id : ids) out.push_back(name(id));
    return out;
}

/// One broadcast operation: a chain of nodes and/or deletions, and the set of
/// operations its author had observed.
struct Op {
    std::string id;
    std::vector<NodeMsg> nodes;
    std::vector<Id> removes;
    std::set<int> deps;
};

struct Replica {
    std::string actor;
    Tree tree{id_less};
    std::set<int> seen;
    uint64_t counter = 1;   // next item identity; an op's own identity follows its items
};

void apply(Replica& to, const Op& op, int index) {
    for (const auto& node : op.nodes) to.tree.deliver(node);
    for (const auto& id : op.removes) to.tree.remove(id);
    to.seen.insert(index);
}

struct History {
    std::vector<Op> ops;
    std::vector<Replica> replicas;
};

/// A seeded random history: local inserts and removes interleaved with causal
/// delivery. `removes` toggles deletion, which the property checks leave off.
History random_history(uint32_t seed, int actors, int steps, bool removes) {
    std::mt19937 rng(seed);
    History h;
    for (int a = 0; a < actors; ++a) {
        Replica r;
        r.actor = std::string(1, char('A' + a));
        h.replicas.push_back(std::move(r));
    }
    auto pick = [&](size_t n) { return size_t(rng() % n); };
    for (int step = 0; step < steps; ++step) {
        const size_t who = pick(h.replicas.size());
        Replica& r = h.replicas[who];
        if (pick(3) != 0) {
            // Deliver something deliverable.
            std::vector<int> ready;
            for (size_t i = 0; i < h.ops.size(); ++i) {
                if (r.seen.count(int(i))) continue;
                if (std::includes(r.seen.begin(), r.seen.end(),
                                  h.ops[i].deps.begin(), h.ops[i].deps.end()))
                    ready.push_back(int(i));
            }
            if (!ready.empty()) {
                const int next = ready[pick(ready.size())];
                apply(r, h.ops[size_t(next)], next);
                continue;
            }
        }
        Op op;
        op.deps = r.seen;
        const size_t visible = r.tree.visible().size();
        if (removes && visible > 0 && pick(4) == 0) {
            const size_t at = pick(visible);
            op.removes.push_back(*r.tree.visible_at(at));
            r.counter += 1;   // the operation's own identity
        } else {
            const size_t at = pick(visible + 1);
            const size_t count = 1 + pick(4);
            std::vector<std::string> values;
            for (size_t n = 0; n < count; ++n)
                values.push_back(std::string(1, char('a' + pick(26))));
            op.nodes = r.tree.insert(at, values, Id{r.actor, r.counter});
            r.counter += count + 1;
        }
        op.id = r.actor + "/" + std::to_string(r.counter - 1);
        const int index = int(h.ops.size());
        h.ops.push_back(std::move(op));
        r.seen.insert(index);
        for (const auto& node : h.ops.back().nodes) (void)node;
    }
    // Final delivery: everything to everyone, in causal order.
    for (auto& r : h.replicas) {
        for (bool progress = true; progress;) {
            progress = false;
            for (size_t i = 0; i < h.ops.size(); ++i) {
                if (r.seen.count(int(i))) continue;
                if (!std::includes(r.seen.begin(), r.seen.end(),
                                   h.ops[i].deps.begin(), h.ops[i].deps.end()))
                    continue;
                apply(r, h.ops[i], int(i));
                progress = true;
            }
        }
    }
    return h;
}

}  // namespace

class FugueMaxModelTest : public QObject {
    Q_OBJECT
private slots:
    void paperFigure7();
    void paperFigure6();
    void shoppingListBackwardInsertions();
    void concurrentTypingStaysGrouped();
    void randomHistoriesConverge();
    void forwardNonInterleavingAndEqualOriginTie();
    void s02ModelIsSupersededByFugueMax();
    void everyS01FixtureHoldsUnderFugueMax();
};

// Figure 7: three replicas concurrently insert A, B, C. Replica 1 then inserts X
// between A and C, replica 2 inserts Y between A and B. Maximal non-interleaving
// forces AXYBC.
void FugueMaxModelTest::paperFigure7() {
    Replica r1{"1"}, r2{"2"}, r3{"3"};
    // Actor labels only name the replicas; the element ids carry the letters'
    // order so that A < B < C as in the paper.
    auto a = r1.tree.insert(0, {"A"}, Id{"A", 1});
    auto b = r2.tree.insert(0, {"B"}, Id{"B", 1});
    auto c = r3.tree.insert(0, {"C"}, Id{"C", 1});
    for (auto& m : a) { r2.tree.deliver(m); r3.tree.deliver(m); }
    for (auto& m : c) r1.tree.deliver(m);
    for (auto& m : b) r1.tree.deliver(m), r3.tree.deliver(m);
    for (auto& m : c) r2.tree.deliver(m);
    // r1 has A,B,C in list order; the paper's replica 1 only has A,C when it
    // inserts X, so rebuild that state explicitly.
    Replica p1{"1"}, p2{"2"};
    for (auto& m : a) { p1.tree.deliver(m); p2.tree.deliver(m); }
    for (auto& m : c) p1.tree.deliver(m);
    for (auto& m : b) p2.tree.deliver(m);
    QCOMPARE(QString::fromStdString(p1.tree.text()), QStringLiteral("AC"));
    QCOMPARE(QString::fromStdString(p2.tree.text()), QStringLiteral("AB"));
    auto x = p1.tree.insert(1, {"X"}, Id{"X", 2});
    auto y = p2.tree.insert(1, {"Y"}, Id{"Y", 2});
    for (auto& m : b) p1.tree.deliver(m);
    for (auto& m : c) p2.tree.deliver(m);
    for (auto& m : y) p1.tree.deliver(m);
    for (auto& m : x) p2.tree.deliver(m);
    QCOMPARE(QString::fromStdString(p1.tree.text()), QStringLiteral("AXYBC"));
    QCOMPARE(QString::fromStdString(p2.tree.text()), QStringLiteral("AXYBC"));
    QCOMPARE(QString::fromStdString(r3.tree.text()), QStringLiteral("ABC"));
}

// Figure 6: A, B, C concurrent; replica 1 sees A and C and inserts X between
// them. The only list order that keeps AX and the A<B<C total order is AXBC.
void FugueMaxModelTest::paperFigure6() {
    Replica r1{"1"}, r3{"3"};
    auto a = r1.tree.insert(0, {"A"}, Id{"A", 1});
    Replica r2{"2"};
    auto b = r2.tree.insert(0, {"B"}, Id{"B", 1});
    auto c = r3.tree.insert(0, {"C"}, Id{"C", 1});
    for (auto& m : a) r3.tree.deliver(m);
    for (auto& m : b) r3.tree.deliver(m);
    for (auto& m : a) r2.tree.deliver(m);
    for (auto& m : c) r1.tree.deliver(m);
    QCOMPARE(QString::fromStdString(r1.tree.text()), QStringLiteral("AC"));
    auto x = r1.tree.insert(1, {"X"}, Id{"X", 2});
    for (auto& m : b) r1.tree.deliver(m);
    for (auto& m : x) r3.tree.deliver(m);
    QCOMPARE(QString::fromStdString(r1.tree.text()), QStringLiteral("AXBC"));
    QCOMPARE(QString::fromStdString(r3.tree.text()), QStringLiteral("AXBC"));
}

// Figure 2: both users prepend one line at a time to the same list. The result
// must keep each user's items together, not shuffle them across categories.
void FugueMaxModelTest::shoppingListBackwardInsertions() {
    Replica base{"S"};
    auto shopping = base.tree.insert(0, {"S"}, Id{"S", 1});
    Replica a{"A"}, b{"B"};
    for (auto& m : shopping) { a.tree.deliver(m); b.tree.deliver(m); }
    std::vector<NodeMsg> from_a, from_b;
    // Each user prepends (insert at index 0) three single items, newest first.
    uint64_t ca = 2, cb = 2;
    for (const char* v : {"c", "b", "a"}) {
        auto m = a.tree.insert(0, {v}, Id{"A", ca}); ca += 2;
        from_a.insert(from_a.end(), m.begin(), m.end());
    }
    for (const char* v : {"z", "y", "x"}) {
        auto m = b.tree.insert(0, {v}, Id{"B", cb}); cb += 2;
        from_b.insert(from_b.end(), m.begin(), m.end());
    }
    for (auto& m : from_b) a.tree.deliver(m);
    for (auto& m : from_a) b.tree.deliver(m);
    QCOMPARE(QString::fromStdString(a.tree.text()), QString::fromStdString(b.tree.text()));
    const std::string text = a.tree.text();
    // Whatever the order of the two groups, each user's three items stay together.
    QVERIFY2(text == "abcxyzS" || text == "xyzabcS", text.c_str());
}

void FugueMaxModelTest::concurrentTypingStaysGrouped() {
    Replica a{"A"}, b{"B"}, c{"C"};
    std::vector<std::vector<NodeMsg>> sent(3);
    Replica* rs[3] = {&a, &b, &c};
    const char* word[3] = {"abc", "abc", "abc"};
    for (int i = 0; i < 3; ++i) {
        uint64_t counter = 1;
        for (size_t at = 0; at < 3; ++at) {
            auto m = rs[i]->tree.insert(at, {std::string(1, word[i][at])},
                                        Id{rs[i]->actor, counter});
            counter += 2;
            sent[size_t(i)].insert(sent[size_t(i)].end(), m.begin(), m.end());
        }
    }
    for (int to = 0; to < 3; ++to)
        for (int from = 0; from < 3; ++from)
            if (to != from)
                for (auto& m : sent[size_t(from)]) rs[to]->tree.deliver(m);
    for (int i = 0; i < 3; ++i)
        QCOMPARE(QString::fromStdString(rs[i]->tree.text()), QStringLiteral("abcabcabc"));
}

void FugueMaxModelTest::randomHistoriesConverge() {
    for (uint32_t seed = 1; seed <= 150; ++seed) {
        const auto h = random_history(seed, 3, 70, /*removes=*/true);
        const auto expected = names(h.replicas[0].tree.traverse_all());
        for (const auto& r : h.replicas) {
            const auto got = names(r.tree.traverse_all());
            QVERIFY2(got == expected, qPrintable(QStringLiteral("seed %1 diverged").arg(seed)));
        }
    }
}

// Definition 4 (1) and (3) over the full list, on delete-free histories. (2) has
// the exception of Lemma 5 and is covered by the paper's own examples above.
void FugueMaxModelTest::forwardNonInterleavingAndEqualOriginTie() {
    for (uint32_t seed = 1; seed <= 100; ++seed) {
        const auto h = random_history(seed, 3, 60, /*removes=*/false);
        const auto& tree = h.replicas[0].tree;
        const auto list = tree.traverse_all();
        std::map<std::string, size_t> at;
        for (size_t i = 0; i < list.size(); ++i) at[name(list[i])] = i;

        std::map<std::string, NodeMsg> msg;
        for (const auto& op : h.ops)
            for (const auto& node : op.nodes) msg[name(node.id)] = node;

        // (1) If B's left origin is A and B is the earliest element with left
        // origin A, then A and B are consecutive.
        std::map<std::string, std::string> earliest;   // left origin -> earliest element
        for (const auto& [id, m] : msg) {
            const std::string origin = m.left_origin_is_start ? "START" : name(m.left_origin);
            auto found = earliest.find(origin);
            if (found == earliest.end() || at[id] < at[found->second]) earliest[origin] = id;
        }
        for (const auto& [origin, first] : earliest) {
            if (origin == "START") { QVERIFY2(at[first] == 0, "first element must open the list"); continue; }
            QVERIFY2(at[first] == at[origin] + 1,
                     qPrintable(QStringLiteral("seed %1: %2 not immediately after %3")
                                    .arg(seed).arg(QString::fromStdString(first),
                                                   QString::fromStdString(origin))));
        }

        // (3) Same left origin and same right origin: lower id earlier.
        for (const auto& [ia, ma] : msg) {
            for (const auto& [ib, mb] : msg) {
                if (ia >= ib) continue;
                if (ma.left_origin_is_start != mb.left_origin_is_start) continue;
                if (!ma.left_origin_is_start && !(ma.left_origin == mb.left_origin)) continue;
                if (ma.ro_is_end != mb.ro_is_end) continue;
                if (!ma.ro_is_end && !(ma.right_origin == mb.right_origin)) continue;
                const bool a_first = at[ia] < at[ib];
                QVERIFY2(a_first == id_less(ma.id, mb.id),
                         qPrintable(QStringLiteral("seed %1: %2 / %3 violate the equal-origin tie")
                                        .arg(seed).arg(QString::fromStdString(ia),
                                                       QString::fromStdString(ib))));
            }
        }
    }
}

// The S02 model's R1/R2 rule is NOT maximally non-interleaving, and this is the
// smallest history that shows it (found by searching seeded random histories:
// the two models disagree on about 2% of them). A and B both insert after A/2.
// B typed "kz" immediately before its own `q`, so a backward non-interleaving
// order keeps B/3 B/4 B/1 consecutive -- FugueMax does, via the reverse
// right-origin rule. R2 orders only by first-item identity, so S02 puts B's pair
// before A's whole subtree and leaves B/1 stranded far to the right.
//
// This is why production adopts FugueMax rather than the S02 rule, and why S02's
// spec carries an amendment. The pin is explicit so that neither model drifting
// goes unnoticed.
void FugueMaxModelTest::s02ModelIsSupersededByFugueMax() {
    using SequenceReference::Operation;
    auto op = [](const char* id, std::vector<const char*> after, const char* left, const char* right,
                 std::vector<std::pair<const char*, const char*>> items) {
        Operation o;
        o.id = id;
        o.kind = Operation::Kind::insert;
        for (auto* a : after) o.after.push_back(a);
        o.between_left = left;
        o.between_right = right;
        for (auto& [i, v] : items) o.items.emplace_back(i, v);
        return o;
    };
    const std::vector<Operation> ops = {
        op("op0", {}, "START", "END", {{"A/1", "v"}, {"A/2", "k"}}),
        op("op1", {"op0"}, "A/2", "END", {{"A/4", "z"}, {"A/5", "u"}, {"A/6", "w"}}),
        op("op2", {"op0", "op1"}, "A/2", "A/4", {{"A/8", "a"}, {"A/9", "s"}}),
        op("op3", {}, "START", "END", {{"B/1", "q"}}),
        op("op4", {}, "START", "END", {{"C/1", "h"}, {"C/2", "u"}}),
        op("op5", {"op0", "op3"}, "A/2", "B/1", {{"B/3", "k"}, {"B/4", "z"}}),
    };
    SequenceReference::Document s02(false, SequenceReference::IdentityOrder{});
    QVERIFY(s02.replay(ops, {"op0", "op1", "op2", "op3", "op4", "op5"}).complete);
    const std::vector<std::string> s02_order = s02.order();
    const std::vector<std::string> s02_expected = {"A/1", "A/2", "B/3", "B/4", "A/8", "A/9",
                                                   "A/4", "A/5", "A/6", "B/1", "C/1", "C/2"};
    QCOMPARE(s02_order, s02_expected);   // B/3 B/4 are far from B/1

    // The same history through FugueMax, minted from first principles.
    Replica a{"A"}, b{"B"}, c{"C"};
    auto m0 = a.tree.insert(0, {"v", "k"}, Id{"A", 1});
    auto m1 = a.tree.insert(2, {"z", "u", "w"}, Id{"A", 4});
    auto m2 = a.tree.insert(2, {"a", "s"}, Id{"A", 8});
    auto m3 = b.tree.insert(0, {"q"}, Id{"B", 1});
    auto m4 = c.tree.insert(0, {"h", "u"}, Id{"C", 1});
    for (auto& m : m0) b.tree.deliver(m);
    auto m5 = b.tree.insert(2, {"k", "z"}, Id{"B", 3});
    for (auto* tree : {&a.tree, &b.tree, &c.tree}) {
        for (auto* msgs : {&m0, &m1, &m2, &m3, &m4, &m5})
            for (auto& m : *msgs) tree->deliver(m);
    }
    const std::vector<std::string> fugue = names(a.tree.traverse_all());
    QCOMPARE(names(b.tree.traverse_all()), fugue);
    QCOMPARE(names(c.tree.traverse_all()), fugue);
    const std::vector<std::string> fugue_expected = {"A/1", "A/2", "A/8", "A/9", "A/4", "A/5",
                                                     "A/6", "B/3", "B/4", "B/1", "C/1", "C/2"};
    QCOMPARE(fugue, fugue_expected);     // B/3 B/4 B/1 consecutive
    QVERIFY(fugue != s02_order);
}

Id parse(const QString& text) {
    const auto parts = text.split('/');
    return Id{parts.at(0).toStdString(), parts.at(1).toULongLong()};
}

// Every S01 fixture's exact order is what FugueMax produces. The fixtures were
// written against the S02 rule, so this is the check that the decision to adopt
// FugueMax does not quietly redefine them. Each insert's side is derived from the
// state its causal ancestors give, and its declared right boundary is checked
// against what a FugueMax mint at that state would have recorded.
void FugueMaxModelTest::everyS01FixtureHoldsUnderFugueMax() {
    const QString root = QStringLiteral(COLLABTEXT_SEQUENCE_FIXTURE_DIR);
    const auto files = QDir(root).entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    QVERIFY(files.size() >= 18);
    for (const auto& file : files) {
        QFile in(root + "/" + file);
        QVERIFY(in.open(QIODevice::ReadOnly));
        const auto doc = QJsonDocument::fromJson(in.readAll()).object();
        const QString fixture = doc["id"].toString();

        std::map<std::string, std::vector<NodeMsg>> chains;     // op id -> nodes
        std::map<std::string, std::vector<Id>> removes;         // op id -> removed items
        std::map<std::string, std::set<std::string>> closure;   // op id -> transitive deps
        std::vector<std::string> op_order;

        std::vector<NodeMsg> seed;
        {
            std::vector<std::string> values;
            Id first;
            const auto initial = doc["initial"].toArray();
            for (int i = 0; i < initial.size(); ++i) {
                const auto item = initial[i].toObject();
                const Id id = parse(item["id"].toString());
                if (i == 0) first = id;
                values.push_back(item["value"].isString()
                                     ? item["value"].toString().toStdString()
                                     : std::to_string(item["value"].toInt()));
            }
            if (!values.empty()) {
                Tree scratch{id_less};
                seed = scratch.insert(0, values, first);
            }
        }

        auto build = [&](const std::set<std::string>& deps) {
            Tree tree{id_less};
            for (const auto& m : seed) tree.deliver(m);
            for (const auto& op : op_order) {
                if (!deps.count(op)) continue;
                for (const auto& m : chains[op]) tree.deliver(m);
                for (const auto& id : removes[op]) tree.remove(id);
            }
            return tree;
        };

        for (const auto& value : doc["operations"].toArray()) {
            const auto op = value.toObject();
            const std::string id = op["id"].toString().toStdString();
            std::set<std::string> deps;
            for (const auto& a : op["after"].toArray()) {
                const std::string dep = a.toString().toStdString();
                deps.insert(dep);
                deps.insert(closure[dep].begin(), closure[dep].end());
            }
            closure[id] = deps;
            const QString kind = op["kind"].toString();
            if (kind == "delete") {
                for (const auto& t : op["targets"].toArray()) removes[id].push_back(parse(t.toString()));
            } else if (kind == "insert") {
                const auto between = op["between"].toArray();
                const QString left = between[0].toString(), right = between[1].toString();
                const bool left_start = (left == "START");
                const Id lo = left_start ? Id{} : parse(left);
                Tree state = build(deps);
                const auto next = state.next_in_list(left_start, lo);
                const auto next_visible = state.next_visible_in_list(left_start, lo);
                const QString full_right = next ? QString::fromStdString(name(*next)) : QStringLiteral("END");
                const QString visible_right =
                    next_visible ? QString::fromStdString(name(*next_visible)) : QStringLiteral("END");
                // S01 spells a boundary as the live neighbour; FugueMax's right origin
                // is the next node *including tombstones*. The declared boundary must be
                // one of the two, and the message always carries the FugueMax one.
                QVERIFY2(right == full_right || right == visible_right,
                         qPrintable(QStringLiteral("%1/%2: declared right boundary %3 is neither the next node (%4) "
                                                   "nor the next live node (%5) at that state")
                                        .arg(fixture, QString::fromStdString(id), right, full_right, visible_right)));
                const QString right_origin = full_right;
                const auto items = op["items"].toArray();
                std::vector<NodeMsg> chain;
                for (int i = 0; i < items.size(); ++i) {
                    NodeMsg m;
                    const auto item = items[i].toObject();
                    m.id = parse(item["id"].toString());
                    m.value = item["value"].isString() ? item["value"].toString().toStdString()
                                                       : std::to_string(item["value"].toInt());
                    m.ro_is_end = (right_origin == "END");
                    if (!m.ro_is_end) m.right_origin = parse(right_origin);
                    if (i == 0) {
                        m.left_origin_is_start = left_start;
                        m.left_origin = lo;
                        if (!state.has_right_child(left_start, lo)) {
                            m.parent_is_root = left_start;
                            m.parent = lo;
                            m.side = Side::Right;
                        } else {
                            m.parent_is_root = false;
                            m.parent = m.right_origin;
                            m.side = Side::Left;
                        }
                    } else {
                        m.left_origin_is_start = false;
                        m.left_origin = chain.back().id;
                        m.parent_is_root = false;
                        m.parent = chain.back().id;
                        m.side = Side::Right;
                    }
                    chain.push_back(m);
                }
                chains[id] = std::move(chain);
            }
            op_order.push_back(id);
        }

        // Replay every arrival order, deferring until causally ready.
        for (const auto& arrival : doc["arrival_orders"].toArray()) {
            Tree tree{id_less};
            for (const auto& m : seed) tree.deliver(m);
            std::set<std::string> applied;
            std::vector<std::string> pending;
            for (const auto& v : arrival.toArray()) pending.push_back(v.toString().toStdString());
            for (bool progress = true; progress && !pending.empty();) {
                progress = false;
                for (size_t i = 0; i < pending.size();) {
                    const auto& op = pending[i];
                    if (!std::includes(applied.begin(), applied.end(), closure[op].begin(), closure[op].end())) { ++i; continue; }
                    for (const auto& m : chains[op]) tree.deliver(m);
                    for (const auto& id : removes[op]) tree.remove(id);
                    applied.insert(op);
                    pending.erase(pending.begin() + static_cast<ptrdiff_t>(i));
                    progress = true;
                }
            }
            QVERIFY2(pending.empty(), qPrintable(fixture + ": arrival order not deliverable"));
            // `expected.order` is the visible order; `hidden` lists items removed or
            // undone. Placement is the same either way, so compare the full list with
            // the hidden items filtered out.
            std::vector<std::string> want;
            for (const auto& e : doc["expected"].toObject()["order"].toArray()) want.push_back(e.toString().toStdString());
            std::set<std::string> hidden;
            for (const auto& e : doc["expected"].toObject()["hidden"].toArray()) hidden.insert(e.toString().toStdString());
            std::vector<std::string> got;
            for (const auto& id : tree.traverse_all())
                if (!hidden.count(name(id))) got.push_back(name(id));
            QVERIFY2(got == want,
                     qPrintable(QStringLiteral("%1: FugueMax gives %2, fixture expects %3")
                                    .arg(fixture,
                                         QString::fromStdString([&]{ std::string s; for (auto& x : got) s += x + " "; return s; }()),
                                         QString::fromStdString([&]{ std::string s; for (auto& x : want) s += x + " "; return s; }()))));
        }
    }
}

QTEST_GUILESS_MAIN(FugueMaxModelTest)
#include "tst_fuguemax_model.moc"
