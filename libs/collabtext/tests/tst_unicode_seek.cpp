#include <QTest>
#include <vector>

#include "crdt/Buffer.h"
#include "TestActor.h"

using namespace CollabText::Crdt;

class TestUnicodeSeek : public QObject {
    Q_OBJECT
private slots:
    void cached_summary_matches_mixed_unicode_text() {
        Buffer buffer(CollabText::Test::test_counter(101));
        const std::string text = "a\xf0\x9f\x99\x82" "e\xcc\x81" "z";
        QVERIFY(buffer.try_apply_local_edit({{0, 0}}, {text}));
        QCOMPARE(buffer.utf16_length(), size_t{6});
        QVERIFY(buffer.try_insert_marker(1));

        // Independent scalar walk is the oracle for every pair of boundaries.
        std::vector<std::pair<int, uint32_t>> boundaries{{0, 0}};
        int units = 0;
        uint32_t bytes = 0;
        while (bytes < text.size()) {
            const auto lead = static_cast<unsigned char>(text[bytes]);
            const uint32_t width = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
            units += width == 4 ? 2 : 1;
            bytes += width;
            boundaries.emplace_back(units, bytes);
        }
        for (const auto& [start_units, start_bytes] : boundaries) {
            for (const auto& [end_units, end_bytes] : boundaries) {
                if (end_units < start_units) continue;
                const auto actual = buffer.utf16_range(start_units, end_units - start_units);
                QVERIFY(actual);
                QCOMPARE(actual.value().first, start_bytes);
                QCOMPARE(actual.value().second, end_bytes);
            }
        }
        auto range = buffer.utf16_range(1, 2);
        QVERIFY(range);
        QCOMPARE(range.value().first, uint32_t{1});
        QCOMPARE(range.value().second, uint32_t{5});
        range = buffer.utf16_range(2, 1);
        QVERIFY(!range);
        QCOMPARE(range.error().code(), CollabText::ErrorCode::malformed);
        range = buffer.utf16_range(6, 0);
        QVERIFY(range);
        QCOMPARE(range.value().first, uint32_t{9});
        QCOMPARE(range.value().second, uint32_t{9});
    }

    void checkpoints_bound_scans_in_a_long_run() {
        Buffer buffer(CollabText::Test::test_counter(102));
        std::string text;
        for (int i = 0; i < 4096; ++i) text += "a\xf0\x9f\x99\x82";
        QVERIFY(buffer.try_apply_local_edit({{0, 0}}, {text}));
        QCOMPARE(buffer.utf16_length(), size_t{12288});
        const auto range = buffer.utf16_range(6144, 3);
        QVERIFY(range);
        QCOMPARE(range.value().first, uint32_t{10240});
        QCOMPARE(range.value().second, uint32_t{10245});
    }

    void summary_survives_split_delete_undo_and_restore() {
        Buffer buffer(CollabText::Test::test_counter(103));
        QVERIFY(buffer.try_apply_local_edit({{0, 0}}, {"a\xf0\x9f\x99\x82" "bc"}));
        QVERIFY(buffer.try_apply_local_edit({{1, 1}}, {"x"}));
        QCOMPARE(buffer.utf16_length(), size_t{6});
        QVERIFY(buffer.try_apply_local_edit({{1, 6}}, {""}));
        QCOMPARE(buffer.utf16_length(), size_t{3});
        QVERIFY(buffer.undo());
        QCOMPARE(buffer.utf16_length(), size_t{6});

        const auto state = buffer.checkpoint_state();
        Buffer restored(CollabText::Test::test_counter(104));
        QVERIFY(restored.restore_checkpoint_state(state));
        QCOMPARE(restored.utf16_length(), size_t{6});
        const auto range = restored.utf16_range(1, 3);
        QVERIFY(range);
        QCOMPARE(range.value().first, uint32_t{1});
        QCOMPARE(range.value().second, uint32_t{6});
    }
};

QTEST_GUILESS_MAIN(TestUnicodeSeek)
#include "tst_unicode_seek.moc"
