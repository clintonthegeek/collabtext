#include "crdt/DeletionIndex.h"
#include "crdt/OriginIndex.h"
#include "crdt/RunDirectory.h"

#include <QtTest>
#include <array>
#include <random>
#include <vector>

using namespace CollabText;
using namespace CollabText::Crdt;

class CrdtIndexesTest final : public QObject {
    Q_OBJECT
private slots:
    void originIntervalsAreCheckedAndPersistent();
    void deletionQueriesReturnOnlyMatchingIncidences();
    void originLookupMatchesFlatScanAcrossManyIntervals();
    void stableRunHandlesResolveNewOrdinalsWithoutChangingOldSnapshot();
};

static ActorId actor(uint8_t value) {
    std::array<uint8_t, ActorId::kBytes> bytes{};
    bytes.back() = value;
    return ActorId::from_bytes(bytes);
}

void CrdtIndexesTest::originIntervalsAreCheckedAndPersistent() {
    OriginIndex empty;
    auto first = empty;
    QVERIFY(first.insert(actor(1), 1, 8, 10));
    QVERIFY(first.insert(actor(1), 8, 12, 11));
    QVERIFY(first.insert(actor(2), 1, 3, 12));
    QVERIFY(!first.insert(actor(1), 7, 9, 13));
    QVERIFY(!first.insert(actor(1), 2, 4, 14));
    QVERIFY(!first.insert(actor(1), 4, 4, 15));
    QCOMPARE(first.size(), size_t(3));
    QCOMPARE(empty.size(), size_t(0));

    const auto hit = first.locate(Identity(actor(1), 9));
    QVERIFY(hit.has_value());
    QCOMPARE(hit->run, RunId(11));
    QCOMPARE(first.find_interval_start(actor(1), 8).value(), RunId(11));
    QVERIFY(!first.find_interval_start(actor(1), 9).has_value());
    QVERIFY(!first.locate(Identity(actor(1), 12)).has_value());
    QVERIFY(!first.locate(Identity(actor(3), 1)).has_value());
    const auto crossing = first.overlapping(actor(1), 6, 10);
    QCOMPARE(crossing.size(), size_t(2));
    QCOMPARE(crossing[0].run, RunId(10));
    QCOMPARE(crossing[1].run, RunId(11));

    auto second = first;
    QVERIFY(second.erase(actor(1), 1));
    QVERIFY(!second.locate(Identity(actor(1), 1)).has_value());
    QVERIFY(first.locate(Identity(actor(1), 1)).has_value());
}

void CrdtIndexesTest::stableRunHandlesResolveNewOrdinalsWithoutChangingOldSnapshot() {
    RunDirectory old_state;
    QVERIFY(old_state.rebuild({{101, 0}, {303, 1}}));
    QCOMPARE(old_state.ordinal(303).value(), uint64_t{1});

    auto appended = old_state;
    QVERIFY(appended.insert(707, 2));
    QCOMPARE(appended.ordinal(707).value(), uint64_t{2});
    QVERIFY(!appended.insert(303, 3));
    QVERIFY(!appended.insert(0, 3));
    QVERIFY(appended.erase(707));
    QVERIFY(!appended.ordinal(707).has_value());
    QCOMPARE(old_state.ordinal(303).value(), uint64_t{1});

    RunDirectory next_state;
    QVERIFY(next_state.rebuild({{101, 0}, {707, 1}, {303, 2}}));
    QCOMPARE(next_state.ordinal(303).value(), uint64_t{2});
    QCOMPARE(old_state.ordinal(303).value(), uint64_t{1});
    QVERIFY(!next_state.ordinal(999).has_value());
    QVERIFY(!next_state.rebuild({{101, 0}, {101, 1}}));
}

void CrdtIndexesTest::deletionQueriesReturnOnlyMatchingIncidences() {
    DeletionIndex empty;
    const Identity d1(actor(9), 40), d2(actor(9), 41);
    auto first = empty;
    QVERIFY(first.insert(d1, actor(1), 1, 5, 101));
    QVERIFY(first.insert(d1, actor(1), 5, 8, 102));
    QVERIFY(first.insert(d1, actor(2), 1, 3, 103));
    QVERIFY(first.insert(d2, actor(1), 1, 8, 104));
    QVERIFY(!first.insert(d1, actor(1), 4, 6, 105));

    const auto records = first.incidences(d1);
    QCOMPARE(records.size(), size_t(3));
    QCOMPARE(records[0].run, RunId(101));
    QCOMPARE(records[1].run, RunId(102));
    QCOMPARE(records[2].run, RunId(103));
    QCOMPARE(first.incidences(Identity(actor(7), 99)).size(), size_t(0));

    auto after_undo = first; // undo changes visibility, never the incidence map
    QVERIFY(after_undo.erase(d1, actor(1), 1));
    QCOMPARE(first.incidences(d1).size(), size_t(3));
    QCOMPARE(after_undo.incidences(d1).size(), size_t(2));
}

void CrdtIndexesTest::originLookupMatchesFlatScanAcrossManyIntervals() {
    OriginIndex index;
    struct Interval { ActorId actor; uint64_t first; uint64_t end; RunId run; };
    std::vector<Interval> scan;
    for (uint8_t a = 1; a <= 7; ++a) {
        for (uint64_t i = 0; i < 500; ++i) {
            const uint64_t first = 1 + i * 4;
            const uint64_t end = first + 3;
            const RunId run = 10000 + uint64_t(a) * 1000 + i;
            QVERIFY(index.insert(actor(a), first, end, run));
            scan.push_back({actor(a), first, end, run});
        }
    }
    std::mt19937_64 random(0x58404ULL);
    for (size_t i = 0; i < 25000; ++i) {
        const auto a = static_cast<uint8_t>(1 + random() % 9);
        const uint64_t counter = 1 + random() % 2200;
        std::optional<RunId> expected;
        for (const auto& row : scan) {
            if (row.actor == actor(a) && row.first <= counter && counter < row.end) {
                expected = row.run;
                break;
            }
        }
        const auto actual = index.locate(Identity(actor(a), counter));
        QCOMPARE(actual.has_value(), expected.has_value());
        if (expected) QCOMPARE(actual->run, *expected);
    }

    const OriginIndex snapshot = index;
    for (uint8_t a = 1; a <= 7; ++a)
        for (uint64_t i = 0; i < 500; i += 2)
            QVERIFY(index.erase(actor(a), 1 + i * 4));
    QCOMPARE(index.size(), size_t(1750));
    for (uint8_t a = 1; a <= 7; ++a) {
        for (uint64_t i = 0; i < 500; ++i) {
            const auto current = index.locate(Identity(actor(a), 1 + i * 4));
            QCOMPARE(current.has_value(), (i % 2) == 1);
            const auto old = snapshot.locate(Identity(actor(a), 1 + i * 4));
            QVERIFY(old.has_value());
        }
    }
}

QTEST_APPLESS_MAIN(CrdtIndexesTest)
#include "tst_crdt_indexes.moc"
