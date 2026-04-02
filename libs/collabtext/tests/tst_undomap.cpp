#include <QTest>
#include "crdt/UndoMap.h"
#include <chrono>

using namespace CollabText::Crdt;

class TestUndoMap : public QObject {
    Q_OBJECT
private slots:

    void undo_count_parity() {
        UndoMap map;
        Lamport edit_id(1, 10);
        Lamport undo1(1, 100);
        Lamport undo2(1, 200);
        Lamport undo3(1, 300);

        QCOMPARE(map.undo_count(edit_id), 0u);
        QVERIFY(!map.is_undone(edit_id));

        map.insert(UndoMapEntry{{edit_id, undo1}, 1});
        QCOMPARE(map.undo_count(edit_id), 1u);
        QVERIFY(map.is_undone(edit_id));

        map.insert(UndoMapEntry{{edit_id, undo2}, 2});
        QCOMPARE(map.undo_count(edit_id), 2u);
        QVERIFY(!map.is_undone(edit_id));

        map.insert(UndoMapEntry{{edit_id, undo3}, 3});
        QCOMPARE(map.undo_count(edit_id), 3u);
        QVERIFY(map.is_undone(edit_id));
    }

    void was_undone_version_filter() {
        UndoMap map;
        Lamport edit_id(1, 10);
        Lamport undoA(2, 50);
        Lamport undoB(3, 60);

        map.insert(UndoMapEntry{{edit_id, undoA}, 1});
        map.insert(UndoMapEntry{{edit_id, undoB}, 1});

        Global versionA;
        versionA.observe(undoA);
        QVERIFY(map.was_undone(edit_id, versionA));

        Global versionEmpty;
        QVERIFY(!map.was_undone(edit_id, versionEmpty));

        Global versionBoth;
        versionBoth.observe(undoA);
        versionBoth.observe(undoB);
        QVERIFY(map.was_undone(edit_id, versionBoth));
    }

    void concurrent_undo_both_survive() {
        UndoMap map;
        Lamport editE(1, 10);
        Lamport undoAlice(2, 100);
        Lamport undoBob(3, 100);

        map.insert(UndoMapEntry{{editE, undoAlice}, 1});
        map.insert(UndoMapEntry{{editE, undoBob}, 1});

        QCOMPARE(map.undo_count(editE), 1u);
        QVERIFY(map.is_undone(editE));
    }

    void concurrent_undo_then_redo() {
        UndoMap map;
        Lamport editE(1, 10);
        Lamport undoA1(2, 100);
        Lamport undoB(3, 100);
        Lamport redoA(2, 200);

        map.insert(UndoMapEntry{{editE, undoA1}, 1});
        map.insert(UndoMapEntry{{editE, undoB}, 1});
        map.insert(UndoMapEntry{{editE, redoA}, 2});

        QCOMPARE(map.undo_count(editE), 2u);
        QVERIFY(!map.is_undone(editE));
    }

    void legacy_undo_redo_shims() {
        UndoMap map;
        UndoMapKey key(1, 10);

        QVERIFY(!map.is_undone(key));
        QCOMPARE(map.count(key), 0u);

        map.undo(key);
        QVERIFY(map.is_undone(key));

        map.redo(key);
        QVERIFY(!map.is_undone(key));

        map.redo(key);
        QVERIFY(!map.is_undone(key));
    }

    void multiple_edits_independent() {
        UndoMap map;
        Lamport editA(1, 10);
        Lamport editB(1, 20);
        Lamport undo1(1, 100);

        map.insert(UndoMapEntry{{editA, undo1}, 1});

        QVERIFY(map.is_undone(editA));
        QVERIFY(!map.is_undone(editB));
    }

    void undo_map_cursor_seek() {
        UndoMap map;
        Lamport undo_ts(99, 1);
        for (uint32_t i = 0; i < 1000; ++i) {
            Lamport edit_id(1, i);
            map.insert(UndoMapEntry{{edit_id, undo_ts}, 1});
        }

        QVERIFY(map.is_undone(Lamport(1, 0)));
        QVERIFY(map.is_undone(Lamport(1, 500)));
        QVERIFY(map.is_undone(Lamport(1, 999)));
        QVERIFY(!map.is_undone(Lamport(1, 1000)));

        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < 10000; ++i) {
            map.undo_count(Lamport(1, i % 1000));
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - start);
        QVERIFY2(elapsed.count() < 1000,
                 qPrintable(QString("10000 lookups took %1ms").arg(elapsed.count())));
    }
};

QTEST_MAIN(TestUndoMap)
#include "tst_undomap.moc"
