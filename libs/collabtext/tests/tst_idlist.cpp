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
};

QTEST_GUILESS_MAIN(TestIdList)
#include "tst_idlist.moc"
