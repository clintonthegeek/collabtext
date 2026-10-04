#include <QTest>
#include "codec/Utf.h"
#include "crdt/Buffer.h"

#include <string>

using namespace CollabText::Crdt;
using namespace CollabText::Codec;

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

    // ---- validation: the corpus that a permissive decoder lets through ----

    void accepts_every_scalar_class() {
        QVERIFY(is_valid_utf8(""));
        QVERIFY(is_valid_utf8("ascii"));
        QVERIFY(is_valid_utf8("\xc3\xa9"));            // U+00E9
        QVERIFY(is_valid_utf8("\xe4\xb8\xad"));        // U+4E2D
        QVERIFY(is_valid_utf8("\xf0\x9f\x9a\x80"));    // U+1F680
        QVERIFY(is_valid_utf8("\xef\xbf\xbf"));        // U+FFFF
        QVERIFY(is_valid_utf8("\xef\xbf\xbe"));        // U+FFFE
        QVERIFY(is_valid_utf8("\xf4\x8f\xbf\xbf"));    // U+10FFFF, the maximum
    }

    void rejects_overlong_forms() {
        // Every overlong encoding is a shorter scalar wearing a longer hat.
        const char* cases[] = {
            "\xc0\x80",          // U+0000 in two bytes
            "\xc1\xbf",          // U+007F in two bytes
            "\xe0\x80\x80",      // U+0000 in three
            "\xe0\x9f\xbf",      // U+07FF in three
            "\xf0\x80\x80\x80",  // U+0000 in four
            "\xf0\x8f\xbf\xbf",  // U+FFFF in four
        };
        for (const char* input : cases) {
            const UtfError error = validate_utf8(input);
            QVERIFY2(!error.ok(), "overlong form accepted");
            QVERIFY2(error.kind == UtfError::Kind::overlong,
                     describe(error, "case").c_str());
        }
    }

    void rejects_encoded_surrogates() {
        // CESU-8 and WTF-8 encode these; UTF-8 does not.
        const char* cases[] = {
            "\xed\xa0\x80",  // U+D800
            "\xed\xad\xbf",  // U+DB7F
            "\xed\xae\x80",  // U+DB80
            "\xed\xbf\xbf",  // U+DFFF
        };
        for (const char* input : cases) {
            const UtfError error = validate_utf8(input);
            QVERIFY2(!error.ok(), "encoded surrogate accepted");
            QVERIFY2(error.kind == UtfError::Kind::surrogate,
                     describe(error, "case").c_str());
        }
    }

    void rejects_scalars_above_the_maximum() {
        const UtfError error = validate_utf8("\xf4\x90\x80\x80");  // U+110000
        QCOMPARE(error.kind, UtfError::Kind::too_large);
        QCOMPARE(error.offset, size_t(0));
    }

    void rejects_stray_continuation_bytes() {
        const UtfError error = validate_utf8("a\x80\x80");
        QCOMPARE(error.kind, UtfError::Kind::invalid_lead);
        QCOMPARE(error.offset, size_t(1));   // the offending byte, not 'a'
    }

    void rejects_illegal_lead_bytes() {
        for (uint8_t lead : {0xFEu, 0xFFu, 0xF8u}) {
            const std::string input(1, static_cast<char>(lead));
            QCOMPARE(validate_utf8(input).kind, UtfError::Kind::invalid_lead);
        }
    }

    void rejects_wrong_continuation_bytes() {
        // A three-byte lead followed by an ASCII byte: the sequence does not
        // exist, and the error points at the lead rather than the intruder.
        const UtfError error = validate_utf8("\xe4\x41\x80");
        QCOMPARE(error.kind, UtfError::Kind::bad_continuation);
        QCOMPARE(error.offset, size_t(0));
    }

    void truncation_reports_the_lead_byte() {
        // A truncated sequence points at where the fix starts, not at the end.
        QCOMPARE(validate_utf8("ab\xe4").offset, size_t(2));
        QCOMPARE(validate_utf8("\xf0\x9f").offset, size_t(0));
        QCOMPARE(validate_utf8("abc").offset, size_t(0));
        const UtfError error = validate_utf8("ab\xe4\xb8");
        QCOMPARE(error.kind, UtfError::Kind::truncated);
        QCOMPARE(error.offset, size_t(2));
    }

    void error_offset_points_at_the_start_of_a_multibyte_scalar() {
        // The acceptance criterion: never a byte inside a scalar or pair.
        const std::string input = std::string("\xf0\x9f\x9a\x80") + "\xe4\xb8\xad";
        QCOMPARE(input.size(), size_t(7));
        // A stray continuation byte right after a four-byte scalar must not be
        // blamed on the scalar's last byte.
        QCOMPARE(validate_utf8(input + "\x80").offset, size_t(7));
        QCOMPARE(validate_utf8("\xe4\xb8\xad\xf8").offset, size_t(3));
    }

    void rejects_raw_control_characters() {
        // 0x7F is legal UTF-8 and always has been. It is a control character
        // here because these strings travel into logs and file names.
        QVERIFY(is_valid_utf8("\x7f"));
    }

    // ---- checked conversion ----

    void utf16_round_trips_every_class() {
        const char* inputs[] = {"", "a", "\xc3\xa9", "\xe4\xb8\xad",
                                "\xf0\x9f\x9a\x80", "\xf4\x8f\xbf\xbf"};
        for (const char* raw : inputs) {
            std::u16string utf16;
            const UtfError to_utf16 = utf8_to_utf16(raw, utf16);
            QVERIFY2(to_utf16.ok(), describe(to_utf16, "to utf16").c_str());

            std::string back;
            const UtfError to_utf8 = utf16_to_utf8(utf16, back);
            QVERIFY2(to_utf8.ok(), describe(to_utf8, "to utf8").c_str());
            QCOMPARE(back, std::string(raw));
        }
    }

    void utf16_counts_code_units_not_scalars() {
        size_t units = 0;
        QVERIFY(utf16_code_unit_count("abc", units).ok());
        QCOMPARE(units, size_t(3));
        QVERIFY(utf16_code_unit_count("\xf0\x9f\x9a\x80", units).ok());
        QCOMPARE(units, size_t(2));          // one scalar, a surrogate pair
        QVERIFY(utf16_code_unit_count("\xe4\xb8\xad", units).ok());
        QCOMPARE(units, size_t(1));

        size_t scalars = 0;
        QVERIFY(utf8_length("\xf0\x9f\x9a\x80", scalars).ok());
        QCOMPARE(scalars, size_t(1));
    }

    void conversion_reports_the_surrogate_it_cannot_pair() {
        std::u16string lone_high;
        lone_high.push_back(0xD83D);
        lone_high.push_back(u'x');
        std::string out;
        // At the high surrogate's own index, not one unit later.
        QCOMPARE(utf16_to_utf8(lone_high, out).offset, size_t(0));

        std::u16string lone_low;
        lone_low.push_back(u'x');
        lone_low.push_back(0xDE00);
        QCOMPARE(utf16_to_utf8(lone_low, out).offset, size_t(1));

        std::u16string trailing_high;
        trailing_high.push_back(0xD83D);
        QCOMPARE(utf16_to_utf8(trailing_high, out).offset, size_t(0));
    }

    void failed_conversion_leaves_the_output_untouched() {
        // A half-built string is not a value; leaving one behind is how a caller
        // ends up applying half an operation.
        std::string out = "previous";
        QCOMPARE(utf16_to_utf8(std::u16string(1, static_cast<char16_t>(0xD800)), out).kind,
                 UtfError::Kind::not_convertible);
        QCOMPARE(out, std::string("previous"));

        std::u16string utf16 = u"previous";
        QCOMPARE(utf8_to_utf16("\xe4", utf16).kind, UtfError::Kind::truncated);
        QCOMPARE(utf16, std::u16string(u"previous"));
    }

    void scalar_encoding_matches_the_unicode_tables() {
        // Explicit lengths: one entry is U+0000, so both strlen and
        // sizeof-on-a-pointer would be wrong in different ways.
        struct Case { uint32_t scalar; const char* utf8; size_t length; };
        const Case cases[] = {
            {0x000000, "\x00", 1},
            {0x00007F, "\x7f", 1},
            {0x000080, "\xc2\x80", 2},
            {0x0007FF, "\xdf\xbf", 2},
            {0x000800, "\xe0\xa0\x80", 3},
            {0x00FFFF, "\xef\xbf\xbf", 3},
            {0x010000, "\xf0\x90\x80\x80", 4},
            {0x10FFFF, "\xf4\x8f\xbf\xbf", 4},
        };
        for (const auto& entry : cases) {
            std::string out;
            QVERIFY2(append_scalar(out, entry.scalar), "append_scalar rejected a scalar");
            const std::string want(entry.utf8, entry.length);
            QVERIFY2(out == want, describe(validate_utf8(out), "encoded form").c_str());
            QCOMPARE(out.size(), want.size());
            QCOMPARE(out, want);
        }

        std::string out;
        QVERIFY(!append_scalar(out, 0x110000));   // above the maximum
        QVERIFY(!append_scalar(out, 0xD800));      // a surrogate is not a scalar
        QVERIFY(out.empty());                      // and nothing was appended
    }

    void every_valid_two_byte_prefix_round_trips() {
        // A cheap sweep over the whole two-byte range, which is where most
        // hand-rolled encoders get the boundary wrong.
        for (uint32_t scalar = 0x80; scalar <= 0x7FF; ++scalar) {
            std::string out;
            QVERIFY(append_scalar(out, scalar));
            QVERIFY2(is_valid_utf8(out), describe(validate_utf8(out), "round trip").c_str());
        }
    }
};

QTEST_MAIN(TestUtf8)
#include "tst_utf8.moc"
