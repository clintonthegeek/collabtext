#include <QTest>
#include "crdt/Buffer.h"
#include "crdt/OperationQueue.h"

using namespace CollabText::Crdt;

class TestOpQueue : public QObject {
    Q_OBJECT
private slots:

    void empty_queue_noop() {
        Buffer buf(1);
        buf.apply_ops({});
        QCOMPARE(buf.text(), std::string(""));
        QCOMPARE(buf.visible_length(), 0u);
    }
};

QTEST_MAIN(TestOpQueue)
#include "tst_opqueue.moc"
