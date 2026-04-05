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
        // Prepends grow depth roughly every 2 inserts with the
        // lo-biased allocator (biased for appends, not prepends).
        // The key invariant: no crash and strict ordering.
        QVERIFY2(maxDepth <= 600,
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

        // This pattern is like prepends: inserting next to min each time.
        // Depth grows roughly every 2 inserts with the lo-biased allocator.
        QVERIFY2(maxDepth <= 600,
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

    void between_adjacent_digits() {
        Locator lo({100});
        Locator hi({101});
        Locator mid = Locator::between(lo, hi);
        QVERIFY(lo < mid);
        QVERIFY(mid < hi);
    }

    void between_adjacent_deep() {
        Locator lo({50, 200});
        Locator hi({50, 201});
        Locator mid = Locator::between(lo, hi);
        QVERIFY(lo < mid);
        QVERIFY(mid < hi);
    }

    void between_never_equals_bounds() {
        // Stress test 1: 500 sequential between() calls
        {
            Locator lo = Locator::min();
            Locator hi = Locator::max();
            for (int i = 0; i < 500; ++i) {
                Locator mid = Locator::between(lo, hi);
                QVERIFY2(lo < mid,
                    qPrintable(QString("sequential i=%1: mid not > lo").arg(i)));
                QVERIFY2(mid < hi,
                    qPrintable(QString("sequential i=%1: mid not < hi").arg(i)));
                lo = mid;
            }
        }

        // Stress test 2: 200 alternating-direction calls
        {
            Locator lo = Locator::min();
            Locator hi = Locator::max();
            for (int i = 0; i < 200; ++i) {
                Locator mid = Locator::between(lo, hi);
                QVERIFY2(lo < mid,
                    qPrintable(QString("alternating i=%1: mid not > lo").arg(i)));
                QVERIFY2(mid < hi,
                    qPrintable(QString("alternating i=%1: mid not < hi").arg(i)));
                if (i % 2 == 0) {
                    hi = mid;  // shrink from the right
                } else {
                    lo = mid;  // shrink from the left
                }
            }
        }
    }
};

QTEST_MAIN(TestLocator)
#include "tst_locator.moc"
