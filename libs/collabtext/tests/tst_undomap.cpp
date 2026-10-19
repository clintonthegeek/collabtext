#include <QTest>
#include "crdt/UndoMap.h"
#include <chrono>
#include "TestActor.h"

using CollabText::Test::actor;

using namespace CollabText::Crdt;

class TestUndoMap : public QObject {
    Q_OBJECT
private slots:

    void undo_count_parity() {
        UndoMap map;
        Identity edit_id(actor(1), 10);
        Identity undo1(actor(1), 100);
        Identity undo2(actor(1), 200);
        Identity undo3(actor(1), 300);

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
        Identity edit_id(actor(1), 10);
        Identity undoA(actor(2), 50);
        Identity undoB(actor(3), 60);

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
        Identity editE(actor(1), 10);
        Identity undoAlice(actor(2), 100);
        Identity undoBob(actor(3), 100);

        map.insert(UndoMapEntry{{editE, undoAlice}, 1});
        map.insert(UndoMapEntry{{editE, undoBob}, 1});

        QCOMPARE(map.undo_count(editE), 1u);
        QVERIFY(map.is_undone(editE));
    }

    void concurrent_undo_then_redo() {
        UndoMap map;
        Identity editE(actor(1), 10);
        Identity undoA1(actor(2), 100);
        Identity undoB(actor(3), 100);
        Identity redoA(actor(2), 200);

        map.insert(UndoMapEntry{{editE, undoA1}, 1});
        map.insert(UndoMapEntry{{editE, undoB}, 1});
        map.insert(UndoMapEntry{{editE, redoA}, 2});

        QCOMPARE(map.undo_count(editE), 2u);
        QVERIFY(!map.is_undone(editE));
    }

    void insert_and_query() {
        UndoMap map;
        UndoMapKey key(Identity(actor(1), 10));
        Identity edit_id(actor(1), 10);
        Identity undo_ts(actor(2), 50);

        QVERIFY(!map.is_undone(key));

        map.insert(UndoMapEntry{{edit_id, undo_ts}, 1});
        QVERIFY(map.is_undone(key));

        map.insert(UndoMapEntry{{edit_id, Identity(actor(2), 60)}, 2});
        QVERIFY(!map.is_undone(key));
    }

    void multiple_edits_independent() {
        UndoMap map;
        Identity editA(actor(1), 10);
        Identity editB(actor(1), 20);
        Identity undo1(actor(1), 100);

        map.insert(UndoMapEntry{{editA, undo1}, 1});

        QVERIFY(map.is_undone(editA));
        QVERIFY(!map.is_undone(editB));
    }

    void checkpoint_entries_rebuild_and_reject_atomically() {
        UndoMap original;
        const Identity edit_a(actor(1), 10);
        const Identity edit_b(actor(1), 20);
        original.insert({{edit_a, Identity(actor(2), 30)}, 1});
        original.insert({{edit_a, Identity(actor(3), 40)}, 2});
        original.insert({{edit_b, Identity(actor(2), 50)}, 1});

        const auto snapshot = original.entries();
        UndoMap restored;
        QVERIFY(restored.restore_entries(snapshot));
        QCOMPARE(restored.entries().size(), snapshot.size());
        QCOMPARE(restored.entries()[0].key, snapshot[0].key);
        QCOMPARE(restored.entries()[1].undo_count, snapshot[1].undo_count);
        QCOMPARE(restored.entries()[2].key, snapshot[2].key);
        QCOMPARE(restored.is_undone(edit_a), original.is_undone(edit_a));
        QCOMPARE(restored.is_undone(edit_b), original.is_undone(edit_b));

        auto malformed = snapshot;
        malformed[1].key = malformed[0].key;
        const auto failed = restored.restore_entries(malformed);
        QVERIFY(!failed);
        QCOMPARE(failed.error().code(), CollabText::ErrorCode::malformed);
        QCOMPARE(restored.entries().size(), snapshot.size());
        QCOMPARE(restored.entries()[0].key, snapshot[0].key);
        QCOMPARE(restored.entries()[2].key, snapshot[2].key);
        QCOMPARE(restored.is_undone(edit_a), original.is_undone(edit_a));

        malformed = snapshot;
        malformed[0].undo_count = 0;
        QVERIFY(!restored.restore_entries(malformed));
        QCOMPARE(restored.entries().size(), snapshot.size());
    }

    void undo_map_cursor_seek() {
        UndoMap map;
        Identity undo_ts(actor(99), 1);
        for (uint32_t i = 0; i < 1000; ++i) {
            Identity edit_id(actor(1), i);
            map.insert(UndoMapEntry{{edit_id, undo_ts}, 1});
        }

        QVERIFY(map.is_undone(Identity(actor(1), 0)));
        QVERIFY(map.is_undone(Identity(actor(1), 500)));
        QVERIFY(map.is_undone(Identity(actor(1), 999)));
        QVERIFY(!map.is_undone(Identity(actor(1), 1000)));

        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < 10000; ++i) {
            map.undo_count(Identity(actor(1), i % 1000));
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - start);
        QVERIFY2(elapsed.count() < 1000,
                 qPrintable(QString("10000 lookups took %1ms").arg(elapsed.count())));
    }
};

QTEST_MAIN(TestUndoMap)
#include "tst_undomap.moc"
