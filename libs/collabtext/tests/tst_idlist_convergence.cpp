#include <QTest>
#include "crdt/IdList.h"

using namespace CollabText::Crdt;

class TestIdListConvergence : public QObject {
    Q_OBJECT
private slots:
    void two_replicas_converge_on_disjoint_inserts() {
        IdList alice(1), bob(2);

        auto op_a = alice.insert_after(Anchor::min(), 0xAA);
        auto op_b = bob.insert_after(Anchor::min(), 0xBB);

        alice.apply_ops({op_b});
        bob.apply_ops({op_a});

        QCOMPARE(alice.ids(), bob.ids());
        QCOMPARE(alice.size(), 2u);
    }
};

QTEST_GUILESS_MAIN(TestIdListConvergence)
#include "tst_idlist_convergence.moc"
