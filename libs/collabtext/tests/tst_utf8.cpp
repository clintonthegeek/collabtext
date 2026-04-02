#include <QTest>
#include "crdt/Buffer.h"

using namespace CollabText::Crdt;

class TestUtf8 : public QObject {
    Q_OBJECT

private:
    static constexpr const char* ACUTE_E = "\xc3\xa9";
    static constexpr const char* CJK_MID = "\xe4\xb8\xad";
    static constexpr const char* ROCKET  = "\xf0\x9f\x9a\x80";

    static std::string repeat(const char* s, int n) {
        std::string r;
        for (int i = 0; i < n; ++i) r += s;
        return r;
    }

private slots:

    void insert_2byte_chars() {
        Buffer buf(1);
        std::string text = repeat(ACUTE_E, 5);
        buf.apply_local_edit({{0, 0}}, {text});
        QCOMPARE(buf.text(), text);
        QCOMPARE(buf.visible_length(), 10u);
    }

    void insert_3byte_chars() {
        Buffer buf(1);
        std::string text = repeat(CJK_MID, 4);
        buf.apply_local_edit({{0, 0}}, {text});
        QCOMPARE(buf.text(), text);
        QCOMPARE(buf.visible_length(), 12u);
    }

    void insert_4byte_chars() {
        Buffer buf(1);
        std::string text = repeat(ROCKET, 3);
        buf.apply_local_edit({{0, 0}}, {text});
        QCOMPARE(buf.text(), text);
        QCOMPARE(buf.visible_length(), 12u);
    }

    void delete_mid_multibyte() {
        Buffer buf(1);
        std::string text = std::string("a") + CJK_MID + "b";
        buf.apply_local_edit({{0, 0}}, {text});
        QCOMPARE(buf.visible_length(), 5u);
        buf.apply_local_edit({{1, 4}}, {""});
        QCOMPARE(buf.text(), std::string("ab"));
        QCOMPARE(buf.visible_length(), 2u);
    }

    void replace_with_multibyte() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        buf.apply_local_edit({{1, 4}}, {std::string(ROCKET)});
        std::string expected = std::string("h") + ROCKET + "o";
        QCOMPARE(buf.text(), expected);
        QCOMPARE(buf.visible_length(), 6u);
    }

    void split_between_multibyte_chars() {
        Buffer buf(1);
        std::string text = repeat(CJK_MID, 3);
        buf.apply_local_edit({{0, 0}}, {text});
        buf.apply_local_edit({{3, 3}}, {"X"});
        std::string expected = std::string(CJK_MID) + "X" + CJK_MID + CJK_MID;
        QCOMPARE(buf.text(), expected);
    }

    void delete_partial_multibyte_sequence() {
        Buffer buf(1);
        std::string text = repeat(ROCKET, 3);
        buf.apply_local_edit({{0, 0}}, {text});
        buf.apply_local_edit({{0, 4}}, {""});
        std::string expected = repeat(ROCKET, 2);
        QCOMPARE(buf.text(), expected);
        QCOMPARE(buf.visible_length(), 8u);
    }

    void mixed_ascii_and_multibyte() {
        Buffer buf(1);
        std::string text = std::string("hello") + CJK_MID + "world" + ROCKET + "!";
        buf.apply_local_edit({{0, 0}}, {text});
        QCOMPARE(buf.visible_length(), 18u);
        buf.apply_local_edit({{5, 13}}, {""});
        std::string expected = std::string("hello") + ROCKET + "!";
        QCOMPARE(buf.text(), expected);
        QCOMPARE(buf.visible_length(), 10u);
    }

    void concurrent_multibyte_inserts_converge() {
        Buffer bufA(1), bufB(2);
        auto opA = bufA.apply_local_edit({{0, 0}}, {repeat(ROCKET, 2)});
        auto opB = bufB.apply_local_edit({{0, 0}}, {repeat(CJK_MID, 3)});
        bufA.apply_ops({opB});
        bufB.apply_ops({opA});
        QCOMPARE(bufA.text(), bufB.text());
        QCOMPARE(bufA.visible_length(), 17u);
    }

    void undo_multibyte_insert() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"abc"});
        buf.apply_local_edit({{3, 3}}, {repeat(ROCKET, 2)});
        QCOMPARE(buf.visible_length(), 11u);
        buf.undo();
        QCOMPARE(buf.text(), std::string("abc"));
        QCOMPARE(buf.visible_length(), 3u);
        buf.redo();
        std::string expected = std::string("abc") + repeat(ROCKET, 2);
        QCOMPARE(buf.text(), expected);
    }

    void anchor_on_multibyte_char() {
        Buffer buf(1);
        std::string text = std::string("a") + CJK_MID + "b";
        buf.apply_local_edit({{0, 0}}, {text});
        auto anchor = buf.anchor_at(1, Bias::Left);
        QCOMPARE(buf.resolve_anchor(anchor), 1u);
        buf.apply_local_edit({{0, 0}}, {"X"});
        QCOMPARE(buf.resolve_anchor(anchor), 2u);
    }

    void anchor_survives_multibyte_delete() {
        Buffer buf(1);
        std::string text = repeat(CJK_MID, 3);
        buf.apply_local_edit({{0, 0}}, {text});
        auto anchor = buf.anchor_at(6, Bias::Left);
        QCOMPARE(buf.resolve_anchor(anchor), 6u);
        buf.apply_local_edit({{0, 3}}, {""});
        QCOMPARE(buf.resolve_anchor(anchor), 3u);
    }
};

QTEST_MAIN(TestUtf8)
#include "tst_utf8.moc"
