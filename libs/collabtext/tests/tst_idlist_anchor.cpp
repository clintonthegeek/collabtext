#include <QTest>
#include "crdt/IdList.h"

using namespace CollabText::Crdt;

class TestIdListAnchor : public QObject {
    Q_OBJECT
private slots:
    // β4.1
    void anchor_at_index_picks_visible_entry() {
        IdList list(1);
        list.insert_after(Anchor::min(), 1);  // origin (1,1)
        list.insert_after(Anchor::min(), 2);  // origin (1,2) → ids: [2, 1]
        Anchor a0 = list.anchor_at_index(0, Bias::Left);
        Anchor a1 = list.anchor_at_index(1, Bias::Left);
        QCOMPARE(a0.replica_id, quint16(1));
        QCOMPARE(a0.char_value, quint32(2));  // "2" is at index 0
        QCOMPARE(a1.char_value, quint32(1));  // "1" is at index 1
    }

    // β4.2
    void anchor_of_finds_first_match() {
        IdList list(1);
        list.insert_after(Anchor::min(), 100);  // origin (1,1)
        list.insert_after(Anchor::min(), 200);  // origin (1,2) → [200, 100]
        Anchor a = list.anchor_of(100, Bias::Right);
        QCOMPARE(a.char_value, quint32(1));  // origin of "100" is (1,1)
        QCOMPARE(a.bias, Bias::Right);
    }

    // β4.3
    void resolve_anchor_returns_visible_index() {
        IdList list(1);
        list.insert_after(Anchor::min(), 1);  // origin (1,1) → idx 0
        list.insert_after(Anchor::min(), 2);  // origin (1,2) → [2,1]; "1" now at idx 1
        Anchor a = list.anchor_of(1, Bias::Left);
        QCOMPARE(list.resolve_anchor(a), quint32(1));
    }

    void resolve_min_max_anchors() {
        IdList list(1);
        list.insert_after(Anchor::min(), 1);
        list.insert_after(Anchor::min(), 2);
        QCOMPARE(list.resolve_anchor(Anchor::min()), quint32(0));
        QCOMPARE(list.resolve_anchor(Anchor::max()), list.size());
    }

    // β4.4
    void anchor_of_deleted_resolves_to_neighbour() {
        IdList list(1);
        list.insert_after(Anchor::min(), 1);  // origin (1,1)
        list.insert_after(Anchor::min(), 2);  // origin (1,2) → [2, 1]
        Anchor a_one = list.anchor_of(1, Bias::Left);
        list.remove_at(a_one);
        // After delete: [2]; anchor at deleted "1" with Left bias → resolves to 1 (end of list)
        QCOMPARE(list.resolve_anchor(a_one), quint32(1));
    }

    // β4.5
    void compare_anchors_orders_by_position() {
        IdList list(1);
        list.insert_after(Anchor::min(), 1);
        list.insert_after(Anchor::min(), 2);  // [2, 1]
        Anchor first = list.anchor_at_index(0, Bias::Left);
        Anchor second = list.anchor_at_index(1, Bias::Left);
        QVERIFY(list.compare_anchors(first, second) < 0);
        QVERIFY(list.compare_anchors(second, first) > 0);
        QCOMPARE(list.compare_anchors(first, first), 0);
    }
};

QTEST_GUILESS_MAIN(TestIdListAnchor)
#include "tst_idlist_anchor.moc"
