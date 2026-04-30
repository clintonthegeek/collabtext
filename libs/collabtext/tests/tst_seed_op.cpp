#include <QTest>
#include "crdt/SeedOp.h"
#include "crdt/Buffer.h"

using namespace CollabText::Crdt;

class TestSeedOp : public QObject {
    Q_OBJECT
private slots:
    void empty_seed_produces_empty_buffer() {
        Buffer buf(7);
        buf.apply_ops({op_for_seed("")});
        QCOMPARE(buf.text(), std::string(""));
    }

    void seed_text_visible_after_apply() {
        Buffer buf(42);
        buf.apply_ops({op_for_seed("hello world")});
        QCOMPARE(buf.text(), std::string("hello world"));
    }

    void seed_is_deterministic_across_replicas() {
        Buffer a(1);
        Buffer b(2);
        const std::string seed = "Line one\nLine two\n";
        auto op_a = op_for_seed(seed);
        auto op_b = op_for_seed(seed);
        a.apply_ops({op_a});
        b.apply_ops({op_b});
        QCOMPARE(a.text(), seed);
        QCOMPARE(b.text(), seed);
    }

    void seed_op_uses_replica_zero() {
        auto op = op_for_seed("anything");
        const auto* edit = std::get_if<EditOperation>(&op);
        QVERIFY(edit != nullptr);
        QCOMPARE(edit->timestamp.replica_id, uint16_t(0));
    }

    void edits_compose_with_seed() {
        Buffer buf(5);
        buf.apply_ops({op_for_seed("abc")});
        buf.apply_local_edit({{3, 3}}, {"def"});
        QCOMPARE(buf.text(), std::string("abcdef"));
    }
};

QTEST_GUILESS_MAIN(TestSeedOp)
#include "tst_seed_op.moc"
