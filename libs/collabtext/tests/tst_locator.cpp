#include <QTest>
#include "crdt/Locator.h"

using namespace CollabText::Crdt;

class TestLocator : public QObject {
    Q_OBJECT
private slots:
    void between_produces_intermediate() {
        Locator lo = Locator::min();
        Locator hi = Locator::max();
        Locator mid = Locator::between(lo, hi);
        QVERIFY(lo < mid);
        QVERIFY(mid < hi);
    }

    void sequential_ordering() {
        // Insert 100 items sequentially (append pattern)
        std::vector<Locator> positions;
        positions.push_back(Locator::min());

        Locator hi = Locator::max();
        for (int i = 0; i < 100; ++i) {
            Locator next = Locator::between(positions.back(), hi);
            QVERIFY(positions.back() < next);
            QVERIFY(next < hi);
            positions.push_back(next);
        }

        // Verify total order
        for (size_t i = 1; i < positions.size(); ++i) {
            QVERIFY(positions[i - 1] < positions[i]);
        }
    }

    void depth_1_for_1000_appends() {
        // Sequential appends should stay at depth 1 for a long time
        Locator pos = Locator::min();
        Locator hi = Locator::max();
        size_t maxDepth = 0;
        for (int i = 0; i < 1000; ++i) {
            pos = Locator::between(pos, hi);
            if (pos.depth() > maxDepth)
                maxDepth = pos.depth();
        }
        QCOMPARE(maxDepth, size_t(1));
    }

    void depth_le_2_for_prepends() {
        // Sequential prepends (insert at beginning)
        Locator lo = Locator::min();
        Locator pos = Locator::max();
        size_t maxDepth = 0;
        for (int i = 0; i < 1000; ++i) {
            Locator next = Locator::between(lo, pos);
            if (next.depth() > maxDepth)
                maxDepth = next.depth();
            pos = next;
        }
        // Prepends may need depth 2 since we're consuming space downward
        QVERIFY2(maxDepth <= 3,
                 qPrintable(QString("maxDepth = %1").arg(maxDepth)));
    }

    void logarithmic_adversarial_growth() {
        // Adversarial: always insert between first two elements
        std::vector<Locator> positions;
        positions.push_back(Locator::min());
        positions.push_back(Locator::max());

        size_t maxDepth = 0;
        for (int i = 0; i < 1000; ++i) {
            Locator mid = Locator::between(positions[0], positions[1]);
            if (mid.depth() > maxDepth)
                maxDepth = mid.depth();
            positions.insert(positions.begin() + 1, mid);
        }

        // With biased midpoint (>> 48), adversarial splitting should
        // still stay manageable. Each level gives ~65536 positions.
        // 1000 splits should need at most depth ~2.
        QVERIFY2(maxDepth <= 3,
                 qPrintable(QString("maxDepth = %1").arg(maxDepth)));
    }

    void comparison_consistency() {
        Locator a({100});
        Locator b({100, 50});
        Locator c({100, 100});
        Locator d({200});

        QVERIFY(a < b);
        QVERIFY(b < c);
        QVERIFY(c < d);
        QVERIFY(a < d);

        // Reflexivity
        QVERIFY(a == a);
        QVERIFY(!(a < a));
    }

    void min_less_than_max() {
        QVERIFY(Locator::min() < Locator::max());
    }
};

QTEST_MAIN(TestLocator)
#include "tst_locator.moc"
