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
};

QTEST_GUILESS_MAIN(TestIdList)
#include "tst_idlist.moc"
