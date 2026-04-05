#include <QTest>
#include "crdt/Buffer.h"

using namespace CollabText::Crdt;

class TestGC : public QObject {
    Q_OBJECT
private slots:

    void fragment_count_empty() {
        Buffer buf(1);
        QCOMPARE(buf.fragment_count(), size_t(0));
        QCOMPARE(buf.tombstone_count(), size_t(0));
    }

    void fragment_count_after_insert() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        QVERIFY(buf.fragment_count() > 0);
        QCOMPARE(buf.tombstone_count(), size_t(0));
    }

    void tombstone_count_after_delete() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        buf.apply_local_edit({{0, 5}}, {""});  // delete all
        QCOMPARE(buf.text(), std::string(""));
        QVERIFY(buf.tombstone_count() > 0);
    }

    void max_undo_depth_default() {
        Buffer buf(1);
        QCOMPARE(buf.max_undo_depth(), size_t(1000));
    }

    void set_max_undo_depth() {
        Buffer buf(1);
        buf.set_max_undo_depth(50);
        QCOMPARE(buf.max_undo_depth(), size_t(50));
    }
};

QTEST_MAIN(TestGC)
#include "tst_gc.moc"
