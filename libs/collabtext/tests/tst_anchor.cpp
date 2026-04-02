#include <QTest>
#include "crdt/Buffer.h"

using namespace CollabText::Crdt;

class TestAnchor : public QObject {
    Q_OBJECT
private slots:

    void anchor_at_beginning() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});

        auto anchor = buf.anchor_at(0, Bias::Left);
        QCOMPARE(buf.resolve_anchor(anchor), 0u);
    }

    void anchor_at_end() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});

        auto anchor = buf.anchor_at(5, Bias::Left);
        uint32_t resolved = buf.resolve_anchor(anchor);
        QCOMPARE(resolved, 5u);
    }

    void anchor_in_middle() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});

        auto anchor = buf.anchor_at(2, Bias::Left);
        QCOMPARE(buf.resolve_anchor(anchor), 2u);
    }

    void anchor_survives_insert_before() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});

        // Anchor at position 2 (between 'e' and 'l')
        auto anchor = buf.anchor_at(2, Bias::Left);
        QCOMPARE(buf.resolve_anchor(anchor), 2u);

        // Insert "XX" at position 0 → "XXhello"
        buf.apply_local_edit({{0, 0}}, {"XX"});

        // Anchor should now resolve to 4 (shifted by 2)
        QCOMPARE(buf.resolve_anchor(anchor), 4u);
    }

    void anchor_survives_insert_after() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});

        // Anchor at position 2
        auto anchor = buf.anchor_at(2, Bias::Left);

        // Insert "XX" at position 4 → "hellXXo"
        buf.apply_local_edit({{4, 4}}, {"XX"});

        // Anchor should still resolve to 2 (not affected)
        QCOMPARE(buf.resolve_anchor(anchor), 2u);
    }

    void anchor_survives_delete_before() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});

        // Anchor at position 3 ('l')
        auto anchor = buf.anchor_at(3, Bias::Left);
        QCOMPARE(buf.resolve_anchor(anchor), 3u);

        // Delete first char → "ello"
        buf.apply_local_edit({{0, 1}}, {""});

        // Anchor should now resolve to 2
        QCOMPARE(buf.resolve_anchor(anchor), 2u);
    }

    void anchor_min_always_zero() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});

        auto anchor = Anchor::min();
        QCOMPARE(buf.resolve_anchor(anchor), 0u);

        buf.apply_local_edit({{0, 0}}, {"XX"});
        QCOMPARE(buf.resolve_anchor(anchor), 0u);
    }

    void anchor_max_always_end() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});

        auto anchor = Anchor::max();
        QCOMPARE(buf.resolve_anchor(anchor), 5u);

        buf.apply_local_edit({{5, 5}}, {" world"});
        QCOMPARE(buf.resolve_anchor(anchor), 11u);
    }

    void compare_anchors_ordering() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});

        auto a = buf.anchor_at(1, Bias::Left);
        auto b = buf.anchor_at(3, Bias::Left);

        QVERIFY(buf.compare_anchors(a, b) < 0);
        QVERIFY(buf.compare_anchors(b, a) > 0);
        QVERIFY(buf.compare_anchors(a, a) == 0);
    }

    void anchor_on_deleted_text() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});

        // Anchor at position 2 ('l')
        auto anchor = buf.anchor_at(2, Bias::Left);

        // Delete "ll" → "heo"
        buf.apply_local_edit({{2, 4}}, {""});

        // Anchor was on deleted text — should resolve to where 'l' was
        uint32_t resolved = buf.resolve_anchor(anchor);
        QVERIFY(resolved <= 2u); // resolves to the deletion point
    }

    void anchor_with_multiple_edits() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"abcdef"});

        // Anchor at position 3 ('d')
        auto anchor = buf.anchor_at(3, Bias::Left);
        QCOMPARE(buf.resolve_anchor(anchor), 3u);

        // Insert "XX" at position 1 → "aXXbcdef"
        buf.apply_local_edit({{1, 1}}, {"XX"});
        QCOMPARE(buf.resolve_anchor(anchor), 5u);

        // Delete 'b' (position 3 in new text) → "aXXcdef"
        buf.apply_local_edit({{3, 4}}, {""});
        QCOMPARE(buf.resolve_anchor(anchor), 4u);
    }
};

QTEST_MAIN(TestAnchor)
#include "tst_anchor.moc"
