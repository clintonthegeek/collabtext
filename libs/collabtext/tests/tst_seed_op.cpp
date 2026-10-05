#include <QTest>
#include "crdt/SeedOp.h"
#include "crdt/Buffer.h"
#include "TestActor.h"

using CollabText::Test::actor;

using namespace CollabText::Crdt;

class TestSeedOp : public QObject {
    Q_OBJECT
private slots:
    void empty_seed_produces_empty_buffer() {
        Buffer buf(CollabText::Test::test_counter(7));
        buf.apply_ops({op_for_seed("")});
        QCOMPARE(buf.text(), std::string(""));
    }

    void seed_text_visible_after_apply() {
        Buffer buf(CollabText::Test::test_counter(42));
        buf.apply_ops({op_for_seed("hello world")});
        QCOMPARE(buf.text(), std::string("hello world"));
    }

    void seed_is_deterministic_across_replicas() {
        Buffer a(CollabText::Test::test_counter(1));
        Buffer b(CollabText::Test::test_counter(2));
        const std::string seed = "Line one\nLine two\n";
        auto op_a = op_for_seed(seed);
        auto op_b = op_for_seed(seed);
        a.apply_ops({op_a});
        b.apply_ops({op_b});
        QCOMPARE(a.text(), seed);
        QCOMPARE(b.text(), seed);
    }

    void seed_actor_is_derived_from_content_not_from_a_default() {
        // The seed used to be stamped replica 0, which was also the default
        // replica of every engine that did not configure one — so a seed
        // operation and an ordinary local edit could claim the same identity.
        // The actor is now derived from the content, which keeps two peers
        // agreeing (same content, same actor) while making a seed and a real
        // edit distinct facts.
        auto op = op_for_seed("anything");
        const auto* edit = std::get_if<EditOperation>(&op);
        QVERIFY(edit != nullptr);
        QVERIFY(edit->timestamp.actor.valid());
        QCOMPARE(edit->timestamp.actor, seed_actor("anything"));
        QVERIFY(seed_actor("anything") != seed_actor("something else"));
    }

    void edits_compose_with_seed() {
        Buffer buf(CollabText::Test::test_counter(5));
        buf.apply_ops({op_for_seed("abc")});
        buf.apply_local_edit({{3, 3}}, {"def"});
        QCOMPARE(buf.text(), std::string("abcdef"));
    }
};

QTEST_GUILESS_MAIN(TestSeedOp)
#include "tst_seed_op.moc"
