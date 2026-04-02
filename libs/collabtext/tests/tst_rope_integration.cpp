#include <QTest>
#include "crdt/Buffer.h"

using namespace CollabText::Crdt;

class TestRopeIntegration : public QObject {
    Q_OBJECT
private slots:

    void rope_tracks_inserts() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        QCOMPARE(buf.visible_rope_len(), 5u);
        QCOMPARE(buf.deleted_rope_len(), 0u);
        QCOMPARE(buf.text(), std::string("hello"));

        buf.apply_local_edit({{5, 5}}, {" world"});
        QCOMPARE(buf.visible_rope_len(), 11u);
        QCOMPARE(buf.deleted_rope_len(), 0u);
        QCOMPARE(buf.text(), std::string("hello world"));
    }
};

QTEST_MAIN(TestRopeIntegration)
#include "tst_rope_integration.moc"
