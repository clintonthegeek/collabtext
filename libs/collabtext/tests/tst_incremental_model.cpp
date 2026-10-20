#include "incremental-model.h"

#include <QtTest>
#include <exception>

class IncrementalModelTest final : public QObject {
    Q_OBJECT
private slots:
    void summaryMonoidAndUnicodeLaws();
    void exhaustShortTransactions();
    void seededReplayAndFailureAtomicity();
};

void IncrementalModelTest::summaryMonoidAndUnicodeLaws() {
    try {
        IncrementalReference::check_summary_laws(0x584304aULL, 100000);
    } catch (const std::exception& error) {
        QFAIL(error.what());
    }
}

void IncrementalModelTest::exhaustShortTransactions() {
    try {
        const auto report = IncrementalReference::enumerate_short_traces();
        QCOMPARE(report.traces, 2401ULL);
        QCOMPARE(report.events, 9604ULL);
        QVERIFY(report.checks > report.events);
        QCOMPARE(report.injected_failures, 57624ULL);
        QCOMPARE(QString::fromStdString(report.digest), QString("9d75f9d96759adb3"));
        QVERIFY(!report.digest.empty());
        qInfo("short traces=%llu events=%llu checks=%llu refusals=%llu injected=%llu digest=%s",
              static_cast<unsigned long long>(report.traces),
              static_cast<unsigned long long>(report.events),
              static_cast<unsigned long long>(report.checks),
              static_cast<unsigned long long>(report.refusals),
              static_cast<unsigned long long>(report.injected_failures),
              report.digest.c_str());
    } catch (const std::exception& error) {
        QFAIL(error.what());
    }
}

void IncrementalModelTest::seededReplayAndFailureAtomicity() {
    try {
        const auto first = IncrementalReference::run_seeded_traces(20261020, 12, 300);
        const auto replay = IncrementalReference::run_seeded_traces(20261020, 12, 300);
        QCOMPARE(first.digest, replay.digest);
        QCOMPARE(first.traces, 12ULL);
        QCOMPARE(first.events, 3600ULL);
        QCOMPARE(first.injected_failures, 3600ULL);
        QCOMPARE(QString::fromStdString(first.digest), QString("475a667e6cee8102"));
        QVERIFY(first.checks > 10000);
        QVERIFY(!first.digest.empty());
        qInfo("seeded traces=%llu events=%llu checks=%llu refusals=%llu injected=%llu digest=%s",
              static_cast<unsigned long long>(first.traces),
              static_cast<unsigned long long>(first.events),
              static_cast<unsigned long long>(first.checks),
              static_cast<unsigned long long>(first.refusals),
              static_cast<unsigned long long>(first.injected_failures),
              first.digest.c_str());
    } catch (const std::exception& error) {
        QFAIL(error.what());
    }
}

QTEST_APPLESS_MAIN(IncrementalModelTest)
#include "tst_incremental_model.moc"
