// S03 acceptance evidence: the PRODUCTION engines against the literal FugueMax
// reference, on seeded random multi-replica histories.
//
// Each trace drives real Buffers (or IdLists) and, in lockstep, reference replicas
// that follow the paper's Algorithm 1 over its own tree. They are fed the same
// local edits and the same deliveries. After every step the touched replica must
//   * produce the same text / list,
//   * hold the same ITEM ORDER including tombstones, and
//   * for a local edit, have minted the same links (parent, side, right origin)
//     the reference mints.
// The links are compared one by one so a placement bug is caught where it is made,
// not where it later shows up as a reordering.
//
// This is the roadmap's "production matches the reference model on every history"
// check made executable over arbitrary histories, rather than only the S01 fixtures.
#include "reference/FugueMaxModel.h"

#include "crdt/Buffer.h"
#include "crdt/IdList.h"
#include "crdt/SequenceOrder.h"

#include "TestActor.h"
#include "TestSeed.h"

#include <QtTest>

#include <algorithm>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace CollabText;
using namespace CollabText::Crdt;
namespace FR = FugueReference;

namespace {

constexpr int kReplicas = 3;

FR::Id model_id(const Identity& id) {
    // Test actors are big-endian in the low byte, so byte order is index order and a
    // one-letter name sorts the same way.
    const auto text = id.actor.to_string();
    const int index = std::stoi(text.substr(text.size() - 2), nullptr, 16);
    return FR::Id{std::string(1, char('A' + index - 1)), id.counter};
}
Identity prod_id(const FR::Id& id) {
    return Identity(CollabText::Test::actor(uint8_t(id.actor[0] - 'A' + 1)), id.counter);
}
bool id_less(const FR::Id& a, const FR::Id& b) {
    if (a.counter != b.counter) return a.counter < b.counter;
    return a.actor < b.actor;
}
std::string name(const FR::Id& id) { return id.actor + "/" + std::to_string(id.counter); }

const std::vector<std::string>& alphabet() {
    static const std::vector<std::string> a = {
        "a", "b", "c", "d", "e", "f", "g", "h", "x", "y", "z", "\xC3\xA9" /*é*/,
        "\xE2\x82\xAC" /*€*/, "\xF0\x9F\x98\x80" /*😀*/};
    return a;
}

/// A scalar-index view of a UTF-8 string: the starting byte of each scalar.
std::vector<uint32_t> scalar_starts(const std::string& text) {
    std::vector<uint32_t> starts;
    for (uint32_t b = 0; b < text.size();) {
        starts.push_back(b);
        const auto c = static_cast<unsigned char>(text[b]);
        b += c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
    }
    starts.push_back(uint32_t(text.size()));
    return starts;
}

std::string link_text(const Link& l) {
    auto id = [](const std::optional<Identity>& i) {
        return i ? i->actor.to_string().substr(30) + "/" + std::to_string(i->counter) : std::string("-");
    };
    return "parent=" + id(l.parent) + (l.side == Side::Left ? " L" : " R") + " ro=" + id(l.right_origin);
}

Link model_link(const FR::NodeMsg& m) {
    Link out;
    if (!m.parent_is_root) out.parent = prod_id(m.parent);
    out.side = m.side == FR::Side::Left ? Side::Left : Side::Right;
    if (!m.ro_is_end) out.right_origin = prod_id(m.right_origin);
    return out;
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

struct TextOp {
    Operation prod;
    std::vector<FR::NodeMsg> nodes;
    std::vector<FR::Id> removes;
    std::set<int> deps;
};

struct TextReplica {
    std::unique_ptr<Buffer> buffer;
    FR::Tree tree{id_less};
    std::set<int> seen;
};

std::string order_of(const std::vector<Fragment>& frags) {
    std::string out;
    for (const auto& f : frags)
        for (uint32_t i = 0; i < f.length; ++i) out += name(model_id(f.item_at(i))) + " ";
    return out;
}
std::string order_of(const FR::Tree& tree) {
    std::string out;
    for (const auto& id : tree.traverse_all()) out += name(id) + " ";
    return out;
}

}  // namespace

class SequenceDifferentialTest : public QObject {
    Q_OBJECT
private slots:
    void textMatchesFugueMax_data() {
        QTest::addColumn<uint32_t>("seed");
        const uint32_t base = uint32_t(CollabText::Test::effective_seed("tst_sequence_differential"));
        for (uint32_t i = 0; i < 120; ++i) QTest::newRow(qPrintable(QString::number(i))) << (base + i * 7919u);
    }
    void textMatchesFugueMax();
    void listMatchesFugueMax_data() {
        QTest::addColumn<uint32_t>("seed");
        const uint32_t base = uint32_t(CollabText::Test::effective_seed("tst_sequence_differential"));
        for (uint32_t i = 0; i < 120; ++i) QTest::newRow(qPrintable(QString::number(i))) << (base + i * 104729u);
    }
    void listMatchesFugueMax();
};

void SequenceDifferentialTest::textMatchesFugueMax() {
    QFETCH(uint32_t, seed);
    std::mt19937 rng(seed);
    auto pick = [&](size_t n) { return size_t(rng() % n); };

    std::vector<TextReplica> replicas(kReplicas);
    for (int r = 0; r < kReplicas; ++r)
        replicas[size_t(r)].buffer = std::make_unique<Buffer>(CollabText::Test::test_counter(uint8_t(r + 1)));
    std::vector<TextOp> ops;

    auto fail = [&](const QString& why, int r) {
        QFAIL(qPrintable(QStringLiteral("seed %1 replica %2: %3\n  prod: %4\n  ref : %5")
                             .arg(seed).arg(r).arg(why,
                                 QString::fromStdString(order_of(replicas[size_t(r)].buffer->fragments())),
                                 QString::fromStdString(order_of(replicas[size_t(r)].tree)))));
    };
    auto check = [&](int r) {
        auto& rep = replicas[size_t(r)];
        if (rep.buffer->text() != rep.tree.text()) fail("text differs", r);
        if (order_of(rep.buffer->fragments()) != order_of(rep.tree)) fail("item order differs", r);
        if (const auto bad = SequenceOrder::check_consistency(rep.buffer->fragments()))
            fail(QStringLiteral("structure: ") + QString::fromStdString(*bad), r);
    };

    const int steps = 70;
    for (int step = 0; step < steps; ++step) {
        const int r = int(pick(kReplicas));
        auto& rep = replicas[size_t(r)];

        if (pick(5) < 2) {   // deliver
            std::vector<int> ready;
            for (size_t i = 0; i < ops.size(); ++i) {
                if (rep.seen.count(int(i))) continue;
                if (std::includes(rep.seen.begin(), rep.seen.end(), ops[i].deps.begin(), ops[i].deps.end()))
                    ready.push_back(int(i));
            }
            if (ready.empty()) continue;
            const int next = ready[pick(ready.size())];
            const auto outcome = rep.buffer->apply_ops({ops[size_t(next)].prod})[0];
            if (outcome.kind() != ApplyOutcomeKind::applied)
                fail(QStringLiteral("delivery of op %1 was not applied").arg(next), r);
            for (const auto& n : ops[size_t(next)].nodes) rep.tree.deliver(n);
            for (const auto& id : ops[size_t(next)].removes) rep.tree.remove(id);
            rep.seen.insert(next);
            check(r);
            continue;
        }

        // Local edit: up to two disjoint ranges against the current visible text.
        const std::string text = rep.buffer->text();
        const auto starts = scalar_starts(text);
        const size_t scalars = starts.size() - 1;
        std::vector<std::pair<uint32_t, uint32_t>> ranges;     // byte offsets
        std::vector<std::pair<size_t, size_t>> sranges;        // scalar indices
        std::vector<std::string> replacement;
        const size_t want = 1 + pick(2);
        size_t cursor = 0;
        for (size_t n = 0; n < want && cursor <= scalars; ++n) {
            const size_t a = cursor + pick(scalars - cursor + 1);
            const size_t b = std::min(scalars, a + pick(3));
            std::string text_in;
            const size_t put = pick(4);
            for (size_t k = 0; k < put; ++k) text_in += alphabet()[pick(alphabet().size())];
            if (a == b && text_in.empty()) continue;   // a no-op range
            ranges.emplace_back(starts[a], starts[b]);
            sranges.emplace_back(a, b);
            replacement.push_back(text_in);
            cursor = b + (a == b ? 0 : 0);
            if (cursor == scalars) break;
            cursor += 1;   // keep ranges non-adjacent so each is its own edit
        }
        if (ranges.empty()) continue;

        auto result = rep.buffer->try_apply_local_edit(ranges, replacement);
        if (!result.ok()) continue;   // a generated range the validator refuses: not interesting
        const Operation produced = result.value();
        const auto& edit = operation_get<EditOperation>(produced);

        TextOp op;
        op.prod = produced;
        op.deps = rep.seen;

        // Reference: mint every insertion against the OLD state, then delete, then place.
        std::vector<FR::Id> visible_before;
        for (const auto& id : rep.tree.visible()) visible_before.push_back(id);
        size_t frag = 0;
        std::vector<std::vector<FR::NodeMsg>> chains;
        for (size_t k = 0; k < ranges.size(); ++k) {
            const auto& [a, b] = sranges[k];
            if (replacement[k].empty()) continue;
            if (frag >= edit.inserted_fragments.size()) fail("production minted fewer fragments than ranges insert", r);
            const auto& produced_frag = edit.inserted_fragments[frag++];
            const auto starts_in = scalar_starts(replacement[k]);
            std::vector<std::string> values;
            for (size_t s = 0; s + 1 < starts_in.size(); ++s)
                values.push_back(replacement[k].substr(starts_in[s], starts_in[s + 1] - starts_in[s]));
            const FR::Id first = model_id(produced_frag.origin);
            const bool at_start = (a == 0);
            FR::Id left;
            if (!at_start) left = visible_before[a - 1];
            std::vector<FR::NodeMsg> chain;
            FR::NodeMsg head = rep.tree.plan_after(at_start, left, values[0], first);
            chain.push_back(head);
            for (size_t s = 1; s < values.size(); ++s) {
                FR::NodeMsg m;
                m.id = FR::Id{first.actor, first.counter + s};
                m.value = values[s];
                m.parent_is_root = false;
                m.parent = chain.back().id;
                m.side = FR::Side::Right;
                m.ro_is_end = head.ro_is_end;
                m.right_origin = head.right_origin;
                m.left_origin_is_start = false;
                m.left_origin = chain.back().id;
                chain.push_back(m);
            }
            if (!(model_link(head) == produced_frag.link))
                fail(QStringLiteral("minted a different link: production {%1}, reference {%2}")
                         .arg(QString::fromStdString(link_text(produced_frag.link)),
                              QString::fromStdString(link_text(model_link(head)))), r);
            chains.push_back(std::move(chain));
        }
        if (frag != edit.inserted_fragments.size()) fail("production minted more fragments than the ranges insert", r);
        for (const auto& [a, b] : sranges)
            for (size_t k = a; k < b; ++k) op.removes.push_back(visible_before[k]);
        for (const auto& id : op.removes) rep.tree.remove(id);
        for (auto& chain : chains) {
            for (const auto& n : chain) { op.nodes.push_back(n); rep.tree.deliver(n); }
        }

        const int index = int(ops.size());
        ops.push_back(std::move(op));
        rep.seen.insert(index);
        check(r);
    }

    // Drain every operation to every replica; all must converge on the reference.
    for (int r = 0; r < kReplicas; ++r) {
        auto& rep = replicas[size_t(r)];
        for (bool progress = true; progress;) {
            progress = false;
            for (size_t i = 0; i < ops.size(); ++i) {
                if (rep.seen.count(int(i))) continue;
                if (!std::includes(rep.seen.begin(), rep.seen.end(), ops[i].deps.begin(), ops[i].deps.end()))
                    continue;
                if (rep.buffer->apply_ops({ops[i].prod})[0].kind() != ApplyOutcomeKind::applied)
                    fail(QStringLiteral("final delivery of op %1 was not applied").arg(i), r);
                for (const auto& n : ops[i].nodes) rep.tree.deliver(n);
                for (const auto& id : ops[i].removes) rep.tree.remove(id);
                rep.seen.insert(int(i));
                progress = true;
            }
        }
        check(r);
    }
    for (int r = 1; r < kReplicas; ++r)
        QCOMPARE(replicas[size_t(r)].buffer->text(), replicas[0].buffer->text());
}

// ---------------------------------------------------------------------------
// List
// ---------------------------------------------------------------------------

namespace {

struct ListOp {
    IdListOperation prod;
    std::vector<FR::NodeMsg> nodes;
    std::vector<FR::Id> removes;
    std::set<int> deps;
};
struct ListReplica {
    std::unique_ptr<IdList> list;
    FR::Tree tree{id_less};
    std::set<int> seen;
};
std::string list_order_of(const std::vector<IdListEntry>& entries) {
    std::string out;
    for (const auto& e : entries) out += name(model_id(e.origin)) + " ";
    return out;
}

}  // namespace

void SequenceDifferentialTest::listMatchesFugueMax() {
    QFETCH(uint32_t, seed);
    std::mt19937 rng(seed);
    auto pick = [&](size_t n) { return size_t(rng() % n); };

    std::vector<ListReplica> replicas(kReplicas);
    for (int r = 0; r < kReplicas; ++r)
        replicas[size_t(r)].list = std::make_unique<IdList>(CollabText::Test::test_counter(uint8_t(r + 1)));
    std::vector<ListOp> ops;
    uint64_t next_value = 1;

    auto fail = [&](const QString& why, int r) {
        QFAIL(qPrintable(QStringLiteral("seed %1 replica %2: %3\n  prod: %4\n  ref : %5")
                             .arg(seed).arg(r).arg(why,
                                 QString::fromStdString(list_order_of(replicas[size_t(r)].list->entries())),
                                 QString::fromStdString(order_of(replicas[size_t(r)].tree)))));
    };
    auto check = [&](int r) {
        auto& rep = replicas[size_t(r)];
        if (list_order_of(rep.list->entries()) != order_of(rep.tree)) fail("entry order differs", r);
        if (const auto bad = SequenceOrder::check_consistency(rep.list->entries()))
            fail(QStringLiteral("structure: ") + QString::fromStdString(*bad), r);
        QCOMPARE(uint32_t(rep.tree.visible().size()), rep.list->size());
    };

    for (int step = 0; step < 80; ++step) {
        const int r = int(pick(kReplicas));
        auto& rep = replicas[size_t(r)];

        if (pick(5) < 2) {
            std::vector<int> ready;
            for (size_t i = 0; i < ops.size(); ++i) {
                if (rep.seen.count(int(i))) continue;
                if (std::includes(rep.seen.begin(), rep.seen.end(), ops[i].deps.begin(), ops[i].deps.end()))
                    ready.push_back(int(i));
            }
            if (ready.empty()) continue;
            const int next = ready[pick(ready.size())];
            if (rep.list->apply_ops({ops[size_t(next)].prod})[0].kind() != ApplyOutcomeKind::applied)
                fail(QStringLiteral("delivery of op %1 was not applied").arg(next), r);
            for (const auto& n : ops[size_t(next)].nodes) rep.tree.deliver(n);
            for (const auto& id : ops[size_t(next)].removes) rep.tree.remove(id);
            rep.seen.insert(next);
            check(r);
            continue;
        }

        const auto entries = rep.list->entries();
        ListOp op;
        op.deps = rep.seen;
        if (!entries.empty() && pick(4) == 0) {
            // Remove any entry (the list anchors on origins, tombstones included, but only
            // a visible one can be removed meaningfully).
            std::vector<const IdListEntry*> visible;
            for (const auto& e : entries) if (e.visible) visible.push_back(&e);
            if (visible.empty()) continue;
            const auto* target = visible[pick(visible.size())];
            auto result = rep.list->try_remove_at(Anchor(target->origin, Bias::Left));
            if (!result.ok()) continue;
            op.prod = result.value();
            op.removes.push_back(model_id(target->origin));
            rep.tree.remove(model_id(target->origin));
        } else {
            // Insert after any entry (tombstones included) or at the head.
            const bool at_head = entries.empty() || pick(4) == 0;
            const IdListEntry* after = at_head ? nullptr : &entries[pick(entries.size())];
            const Anchor anchor = at_head ? Anchor::min() : Anchor(after->origin, Bias::Left);
            auto result = rep.list->try_insert_after(anchor, next_value++);
            if (!result.ok()) continue;
            op.prod = result.value();
            const auto& insert = operation_get<IdListInsertOp>(op.prod);
            const FR::Id id = model_id(insert.timestamp);
            const FR::NodeMsg planned = rep.tree.plan_after(
                at_head, at_head ? FR::Id{} : model_id(after->origin), "v", id);
            if (!(model_link(planned) == insert.link))
                fail(QStringLiteral("minted a different link: production {%1}, reference {%2}")
                         .arg(QString::fromStdString(link_text(insert.link)),
                              QString::fromStdString(link_text(model_link(planned)))), r);
            op.nodes.push_back(planned);
            rep.tree.deliver(planned);
        }
        const int index = int(ops.size());
        ops.push_back(std::move(op));
        rep.seen.insert(index);
        check(r);
    }

    for (int r = 0; r < kReplicas; ++r) {
        auto& rep = replicas[size_t(r)];
        for (bool progress = true; progress;) {
            progress = false;
            for (size_t i = 0; i < ops.size(); ++i) {
                if (rep.seen.count(int(i))) continue;
                if (!std::includes(rep.seen.begin(), rep.seen.end(), ops[i].deps.begin(), ops[i].deps.end()))
                    continue;
                if (rep.list->apply_ops({ops[i].prod})[0].kind() != ApplyOutcomeKind::applied)
                    fail(QStringLiteral("final delivery of op %1 was not applied").arg(i), r);
                for (const auto& n : ops[i].nodes) rep.tree.deliver(n);
                for (const auto& id : ops[i].removes) rep.tree.remove(id);
                rep.seen.insert(int(i));
                progress = true;
            }
        }
        check(r);
    }
    for (int r = 1; r < kReplicas; ++r)
        QCOMPARE(replicas[size_t(r)].list->ids(), replicas[0].list->ids());
}

QTEST_GUILESS_MAIN(SequenceDifferentialTest)
#include "tst_sequence_differential.moc"
