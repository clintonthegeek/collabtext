#include <QTest>
#include "crdt/IdList.h"

using namespace CollabText::Crdt;

class TestIdList : public QObject {
    Q_OBJECT
private slots:
    void empty_list_has_no_ids() {
        IdList list(1);
        QCOMPARE(list.size(), 0u);
        QCOMPARE(list.ids().size(), size_t{0});
    }

    void replica_id_round_trips() {
        IdList list(42);
        QCOMPARE(list.replica_id(), quint16(42));
    }

    void initial_version_is_empty() {
        IdList list(1);
        // Version starts unobserved — all entries zero or vector empty
        const Global& v = list.version();
        bool all_zero = true;
        for (size_t i = 0; i < v.size(); ++i) {
            if (v[i] != 0) { all_zero = false; break; }
        }
        QVERIFY(all_zero || v.size() == 0);
    }

    void insert_into_empty_list() {
        IdList list(1);
        list.insert_after(Anchor::min(), 0xAA);
        QCOMPARE(list.size(), 1u);
        QCOMPARE(list.ids(), std::vector<uint64_t>{0xAA});
    }

    void insert_at_start_pushes_existing_back() {
        IdList list(1);
        list.insert_after(Anchor::min(), 0xAA);  // list: [0xAA]
        list.insert_after(Anchor::min(), 0xBB);  // list: [0xBB, 0xAA]
        QCOMPARE(list.size(), 2u);
        QCOMPARE(list.ids(), (std::vector<uint64_t>{0xBB, 0xAA}));
    }

    void multiple_inserts_at_start_reverse_order() {
        IdList list(1);
        list.insert_after(Anchor::min(), 1);
        list.insert_after(Anchor::min(), 2);
        list.insert_after(Anchor::min(), 3);
        QCOMPARE(list.ids(), (std::vector<uint64_t>{3, 2, 1}));
    }

    void same_id_value_can_appear_twice() {
        IdList list(1);
        list.insert_after(Anchor::min(), 0xAA);
        list.insert_after(Anchor::min(), 0xAA);
        QCOMPARE(list.size(), 2u);
        QCOMPARE(list.ids(), (std::vector<uint64_t>{0xAA, 0xAA}));
    }

    void remove_only_element() {
        IdList list(1);
        list.insert_after(Anchor::min(), 0xAA);  // origin=(1,1)
        Anchor at_aa(1, 1, Bias::Left);
        list.remove_at(at_aa);
        QCOMPARE(list.size(), 0u);
        QCOMPARE(list.ids().size(), size_t{0});
        QCOMPARE(list.tombstone_count(), size_t{1});
    }

    void remove_middle_keeps_neighbours() {
        IdList list(1);
        list.insert_after(Anchor::min(), 1);  // origin (1,1) → [1]
        list.insert_after(Anchor::min(), 2);  // origin (1,2) → [2, 1]
        list.insert_after(Anchor::min(), 3);  // origin (1,3) → [3, 2, 1]
        list.remove_at(Anchor(1, 2, Bias::Left));  // remove "2"
        QCOMPARE(list.ids(), (std::vector<uint64_t>{3, 1}));
        QCOMPARE(list.tombstone_count(), size_t{1});
    }

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

QTEST_GUILESS_MAIN(TestIdList)
#include "tst_idlist.moc"
