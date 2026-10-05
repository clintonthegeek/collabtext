// tst_decode_validation.cpp — P04: the JSON reader's failure modes, as evidence.
//
// Finding F10 was that `parse_operation()` returned a value for
// `{"t":"u"}garbage`, and for an object missing fields the decoder needed. A
// decoder that succeeds has to mean something. These tests pin the meaning:
// complete consumption, no duplicate keys, arbitrary key order, strict numbers,
// and every malformed input rejected with an offset a human can act on.
//
// The malformed corpus at the bottom is deliberately a table rather than a pile
// of QCOMPAREs. Corpus entries name the rule they exist to protect, so a future
// reader that has to relax one of them has to write down why.

#include <QTest>

#include "codec/JsonReader.h"
#include "codec/Utf.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

using namespace CollabText::Codec;

namespace {

struct CorpusCase {
    const char* input;
    JsonErrorKind expected;
    const char* rule;
};

const CorpusCase kMalformedCorpus[] = {
    // Missing syntax
    {"", JsonErrorKind::empty_input, "no input is not an empty document"},
    {"{", JsonErrorKind::unexpected_end, "unterminated object"},
    {"[", JsonErrorKind::unexpected_end, "unterminated array"},
    {"{\"t\"", JsonErrorKind::unexpected_end, "truncated key"},
    {"{\"t\":", JsonErrorKind::unexpected_end, "no value after colon"},
    {"{\"t\":\"u\"", JsonErrorKind::unexpected_end, "unterminated object body"},
    {"\"unterminated", JsonErrorKind::unexpected_end, "unterminated string"},
    {"tru", JsonErrorKind::unexpected_character, "partial literal keyword"},
    {"nul", JsonErrorKind::unexpected_character, "partial null keyword"},
    {"}", JsonErrorKind::unexpected_character, "a value is required, not an object"},
    {"]", JsonErrorKind::unexpected_character, "a value is required, not an array"},
    {",", JsonErrorKind::unexpected_character, "a value is required, not a comma"},
    {"{t:\"u\"}", JsonErrorKind::unexpected_character, "keys must be quoted"},
    {"{'t':'u'}", JsonErrorKind::unexpected_character, "single quotes are not JSON"},
    {"{\"t\":\"u\",}", JsonErrorKind::unexpected_character, "trailing comma in object"},
    {"{\"t\":\"u\",,}", JsonErrorKind::unexpected_character, "missing key after comma"},
    {"[1,]", JsonErrorKind::unexpected_character, "trailing comma in array"},
    {"{\"t\" \"u\"}", JsonErrorKind::unexpected_character, "missing colon"},
    {"{\"t\":\"u\" \"x\":1}", JsonErrorKind::unexpected_character, "missing comma"},
    {"// comment\x0a{}", JsonErrorKind::unexpected_character, "no comments"},
    // Trailing garbage: F10's first clause
    {"{\"t\":\"u\"}// comment", JsonErrorKind::trailing_garbage, "no trailing comments"},
    {"{\"t\":\"u\"}garbage", JsonErrorKind::trailing_garbage, "complete consumption"},
    {"{} {}", JsonErrorKind::trailing_garbage, "complete consumption after an object"},
    {"{}\x0a{}", JsonErrorKind::trailing_garbage, "whitespace is not a second value"},
    {"1 2", JsonErrorKind::trailing_garbage, "complete consumption after a scalar"},
    {"[1][2]", JsonErrorKind::trailing_garbage, "complete consumption after an array"},
    // Escapes
    {"\"\\x\"", JsonErrorKind::invalid_escape, "unknown escape"},
    {"\"\\u12\"", JsonErrorKind::invalid_unicode_escape, "short unicode escape"},
    {"\"\\u12g4\"", JsonErrorKind::invalid_unicode_escape, "non-hex in a unicode escape"},
    // Lone surrogates: valid UTF-16 syntax, invalid text
    {"\"\\ud800\"", JsonErrorKind::lone_surrogate_escape, "high surrogate alone"},
    {"\"\\udc00\"", JsonErrorKind::lone_surrogate_escape, "low surrogate alone"},
    {"\"\\ud800\\ud800\"", JsonErrorKind::lone_surrogate_escape, "two high surrogates"},
    {"\"\\ud800x\"", JsonErrorKind::lone_surrogate_escape, "high surrogate unpaired by text"},
    {"\"\\ud800\\udc00\\ud800\"", JsonErrorKind::lone_surrogate_escape, "trailing high surrogate"},
    {"\"\\udc00\\ud800\"", JsonErrorKind::lone_surrogate_escape, "reversed surrogate pair"},
    // Raw text inside strings
    {"\"\xff\"", JsonErrorKind::invalid_utf8, "0xFF is not a lead byte"},
    {"\"\x80\"", JsonErrorKind::invalid_utf8, "stray continuation byte"},
    {"\"\xe4\xb8\"", JsonErrorKind::invalid_utf8, "truncated UTF-8 tail"},
    {"\"\xc0\x80\"", JsonErrorKind::invalid_utf8, "overlong UTF-8"},
    {"\"\xed\xa0\x80\"", JsonErrorKind::invalid_utf8, "encoded surrogate in text"},
    {"\"a\x0a" "b\"", JsonErrorKind::unexpected_character, "raw newline in string"},
    {"\"\x01\"", JsonErrorKind::unexpected_character, "raw C0 control in string"},
    // Numbers
    {"-", JsonErrorKind::invalid_number, "no digits after the sign"},
    {"01", JsonErrorKind::invalid_number, "leading zero"},
    {"-01", JsonErrorKind::invalid_number, "leading zero after a sign"},
    {"1.", JsonErrorKind::invalid_number, "no fraction digits"},
    {"1.e5", JsonErrorKind::invalid_number, "no fraction digits before an exponent"},
    {"1e", JsonErrorKind::invalid_number, "no exponent digits"},
    {"1e+", JsonErrorKind::invalid_number, "no exponent digits after a sign"},
    {"1e999", JsonErrorKind::number_out_of_range, "exponent beyond the limit"},
    {"1e-999", JsonErrorKind::number_out_of_range, "exponent beyond the limit, negative"},
    {"123456789012345678901", JsonErrorKind::number_out_of_range, "too many significant digits"},
    // Duplicate keys: last-wins hides a disagreement
    {"{\"t\":\"u\",\"t\":\"v\"}", JsonErrorKind::duplicate_key, "duplicate key"},
    {"{\"a\":1,\"b\":2,\"a\":3}", JsonErrorKind::duplicate_key, "duplicate key that is not adjacent"},
    {"{\"\\u0061\":1,\"a\":2}", JsonErrorKind::duplicate_key, "duplicate key after unescaping"},
    {"{\"a\":{\"a\":1}}", JsonErrorKind::none, "the same key in a nested object is fine"},
};

}  // namespace

class TestDecodeValidation : public QObject {
    Q_OBJECT

private slots:
    void literal_and_mixed_strings_obey_decoded_byte_limit() {
        JsonLimits limits;
        limits.max_text_bytes = 3;
        for (const std::string input : {std::string("\"abcd\""),
                                        std::string("\"a\\nbc\""),
                                        std::string("\"\\u00e9ab\"")}) {
            JsonReader reader(limits);
            JsonValue value;
            JsonError error;
            QVERIFY2(!reader.parse(input, value, error), input.c_str());
            QCOMPARE(error.kind, JsonErrorKind::text_too_large);
        }
    }

    void object_keys_have_their_own_decoded_byte_limit() {
        JsonLimits limits;
        limits.max_text_bytes = 16;
        limits.max_key_bytes = 3;
        JsonReader reader(limits);
        JsonValue value;
        JsonError error;
        QVERIFY(!reader.parse("{\"a\\nbc\":0}", value, error));
        QCOMPARE(error.kind, JsonErrorKind::key_too_large);
    }


    // ---- the corpus ----

    void malformed_corpus_is_rejected() {
        for (const auto& entry : kMalformedCorpus) {
            JsonReader reader;
            JsonValue value;
            JsonError error;
            const std::string input(entry.input);
            const bool ok = reader.parse(input, value, error);

            if (entry.expected == JsonErrorKind::none) {
                QVERIFY2(ok, qPrintable(QString("expected success for %1: %2")
                                            .arg(QString::fromUtf8(input.data(),
                                                                   int(input.size())))
                                            .arg(QString::fromUtf8(error.context.data(),
                                                                   int(error.context.size())))));
                continue;
            }

            QVERIFY2(!ok, qPrintable(QString("accepted malformed input (%1): %2")
                                         .arg(QString::fromUtf8(input.data(),
                                                                int(input.size())))
                                         .arg(entry.rule)));
            if (error.kind != entry.expected) {
                qDebug() << "corpus input" << QString::fromUtf8(input.data(),
                                                                 int(input.size()))
                         << "expected" << to_string(entry.expected) << "got"
                         << to_string(error.kind) << error.context.c_str();
            }
            QCOMPARE(error.kind, entry.expected);
            // Every failure carries an offset and context: a decoder that says
            // "no" without saying where is not much better than one that says
            // "yes" when it should not.
            QVERIFY2(error.context.size() > 0, entry.rule);
            if (entry.expected != JsonErrorKind::empty_input)
                QVERIFY2(error.offset <= input.size(), entry.rule);
        }
    }

    void malformed_corpus_is_fixed_and_small() {
        // "Fixed corpus" means every entry is pinned here, so a change to the
        // reader shows up as a change to this table and not as a silent
        // relaxation somewhere downstream.
        constexpr size_t kEntries =
            sizeof(kMalformedCorpus) / sizeof(kMalformedCorpus[0]);
        QCOMPARE(kEntries, size_t(56));

        // Every entry names the rule it protects, and no two entries share one:
        // a duplicate rule means one of them has stopped testing anything new.
        std::set<std::string> rules;
        for (const auto& entry : kMalformedCorpus) {
            QVERIFY2(entry.rule[0] != '\0', "every corpus entry needs a rule");
            QVERIFY2(rules.insert(entry.rule).second,
                     qPrintable(QString("duplicate rule: %1").arg(entry.rule)));
        }
    }

    // ---- complete consumption, stated directly ----

    void valid_object_with_trailing_bytes_fails() {
        JsonReader reader;
        JsonValue value;
        JsonError error;
        QVERIFY(!reader.parse("{\"t\":\"u\"}garbage", value, error));
        QCOMPARE(error.kind, JsonErrorKind::trailing_garbage);
        QCOMPARE(error.offset, size_t(9));
    }

    void a_trailing_nul_is_not_whitespace() {
        // A NUL inside the buffer cannot come from a const char* corpus entry,
        // so it is built explicitly: the point is that the reader stops at the
        // NUL rather than treating the string as C-terminated.
        const std::string input = std::string("{\"t\":\"u\"}") + '\0' + "x";
        QCOMPARE(input.size(), size_t(11));
        JsonReader reader;
        JsonValue value;
        JsonError error;
        QVERIFY(!reader.parse(input, value, error));
        QCOMPARE(error.kind, JsonErrorKind::trailing_garbage);
        QCOMPARE(error.offset, size_t(9));
    }

    void surrounding_whitespace_is_fine_but_only_whitespace() {
        JsonReader reader;
        JsonValue value;
        JsonError error;
        QVERIFY2(reader.parse("  {\"t\":\"u\"}\n\t ", value, error), qPrintable(QString::fromStdString(error.context)));
        QCOMPARE(reader.root_identifier(), std::string("u"));

        QVERIFY(!reader.parse(" {\"t\":\"u\"} x ", value, error));
        QCOMPARE(error.kind, JsonErrorKind::trailing_garbage);
    }

    // ---- key order and duplicates ----

    void equivalent_reordered_objects_decode_identically() {
        JsonReader reader;
        JsonValue a, b;
        JsonError error;
        QVERIFY2(reader.parse("{\"t\":\"u\",\"v\":1,\"n\":{\"x\":[1,2],\"y\":\"z\"}}", a, error),
                 qPrintable(QString::fromStdString(error.context)));
        QVERIFY2(reader.parse("{\"n\":{\"y\":\"z\",\"x\":[1,2]},\"v\":1,\"t\":\"u\"}", b, error),
                 qPrintable(QString::fromStdString(error.context)));

        // Compared as text, recursively, because a structural operator== on the
        // value would hide which field drifted.
        QVERIFY(encode_canonical(a) == encode_canonical(b));
        QCOMPARE(reader.root_identifier(), std::string("u"));
        QCOMPARE(reader.root_version(), uint64_t(1));
    }

    void duplicate_keys_are_rejected_with_the_key_in_the_message() {
        JsonReader reader;
        JsonValue value;
        JsonError error;
        QVERIFY(!reader.parse("{\"t\":\"u\",\"v\":1,\"t\":\"w\"}", value, error));
        QCOMPARE(error.kind, JsonErrorKind::duplicate_key);
        QVERIFY2(error.context.find("\"t\"") != std::string::npos,
                 qPrintable(QString::fromStdString(error.context)));
    }

    // ---- escapes and Unicode ----

    void utf8_failures_keep_their_specific_reason() {
        // "overlong" and "encoded surrogate" are different bugs for whoever has
        // to fix the writer, so the JSON error carries the UTF-8 error whole.
        struct Case { const char* input; UtfError::Kind kind; size_t offset; };
        const Case cases[] = {
            {"{\"s\":\"\xc0\x80\"}", UtfError::Kind::overlong, 6},
            {"{\"s\":\"\xed\xa0\x80\"}", UtfError::Kind::surrogate, 6},
            {"{\"s\":\"\xe4\xb8\"}", UtfError::Kind::truncated, 6},
            {"{\"s\":\"ok\xff\"}", UtfError::Kind::invalid_lead, 8},
        };
        for (const auto& [input, kind, offset] : cases) {
            JsonReader reader;
            JsonValue value;
            JsonError error;
            const std::string text(input);
            QVERIFY2(!reader.parse(text, value, error), input);
            QCOMPARE(error.kind, JsonErrorKind::invalid_utf8);
            QCOMPARE(error.utf.kind, kind);
            QCOMPARE(error.utf.offset, offset);
            QCOMPARE(error.offset, offset);
        }
    }

    void valid_escapes_round_trip() {
        JsonReader reader;
        JsonValue value;
        JsonError error;
        // Escaped rather than a raw string literal: Qt's moc cannot parse a
        // raw string containing braces, which silently drops every slot in the
        // file. Worth knowing before anyone "simplifies" this back.
        const std::string json = "{\"s\":\"a\\\"b\\\\c\\/d\\be\\ff\\ng\\rh\\ti\"}";
        QVERIFY2(reader.parse(json, value, error),
                 qPrintable(QString::fromStdString(error.context)));
        const JsonValue* s = value.find("s");
        QVERIFY(s != nullptr);
        QCOMPARE(s->string_value(), std::string("a\"b\\c/d\be\ff\ng\rh\ti"));
        QCOMPARE(s->string_value().size(), size_t(17));
    }

    void non_bmp_escapes_decode_to_utf8() {
        JsonReader reader;
        JsonValue value;
        JsonError error;
        // U+1F680 as a surrogate pair, U+10FFFF as the maximum scalar.
        const std::string json = "{\"a\":\"\\ud83d\\ude80\",\"b\":\"\\udbff\\udfff\"}";
        QVERIFY2(reader.parse(json, value, error),
                 qPrintable(QString::fromStdString(error.context)));
        QCOMPARE(value.find("a")->string_value(), std::string("\xf0\x9f\x9a\x80"));
        QCOMPARE(value.find("b")->string_value(), std::string("\xf4\x8f\xbf\xbf"));
    }

    void surrogate_pairs_round_trip_through_utf16() {
        JsonReader reader;
        JsonValue value;
        JsonError error;
        QVERIFY(reader.parse("{\"a\":\"\\ud83d\\ude80\"}", value, error));
        std::u16string utf16;
        QVERIFY(utf8_to_utf16(value.find("a")->string_value(), utf16).ok());
        QCOMPARE(utf16, std::u16string(u"\U0001F680"));
        std::string back;
        QVERIFY(utf16_to_utf8(utf16, back).ok());
        QCOMPARE(back, std::string("\xf0\x9f\x9a\x80"));
    }

    void literal_non_ascii_text_is_validated_and_kept() {
        JsonReader reader;
        JsonValue value;
        JsonError error;
        QVERIFY2(reader.parse("{\"s\":\"\xc3\xa9\xe4\xb8\xad\xf0\x9f\x9a\x80\"}", value, error),
                 qPrintable(QString::fromStdString(error.context)));
        QCOMPARE(value.find("s")->string_value(),
                 std::string("\xc3\xa9\xe4\xb8\xad\xf0\x9f\x9a\x80"));
    }

    // ---- numbers ----

    void number_text_is_preserved_exactly() {
        // 1.0 and 1 are different tokens; a decoder that collapses them has
        // discarded information it will be asked to reproduce on encode.
        JsonReader reader;
        JsonValue value;
        JsonError error;
        QVERIFY2(reader.parse("{\"a\":1.0,\"b\":1,\"c\":1e10,\"d\":-0.5}", value, error),
                 qPrintable(QString::fromStdString(error.context)));
        QCOMPARE(value.find("a")->number_text(), std::string("1.0"));
        QCOMPARE(value.find("b")->number_text(), std::string("1"));
        QCOMPARE(value.find("c")->number_text(), std::string("1e10"));
        QCOMPARE(value.find("d")->number_text(), std::string("-0.5"));
    }

    void typed_number_access_rejects_non_integers() {
        JsonReader reader;
        JsonValue value;
        JsonError error;
        QVERIFY(reader.parse("{\"u\":7,\"neg\":-7,\"frac\":7.5,\"exp\":7e2}", value, error));

        uint64_t u = 0;
        QVERIFY(value.find("u")->as_uint64(u, error));
        QCOMPARE(u, uint64_t(7));

        QVERIFY(!value.find("neg")->as_uint64(u, error));
        QCOMPARE(error.kind, JsonErrorKind::type_mismatch);
        QVERIFY(!value.find("frac")->as_uint64(u, error));
        QVERIFY(!value.find("exp")->as_uint64(u, error));

        int64_t i = 0;
        QVERIFY(value.find("neg")->as_int64(i, error));
        QCOMPARE(i, int64_t(-7));

        double d = 0;
        QVERIFY(value.find("exp")->as_double(d, error));
        QCOMPARE(d, 700.0);
    }

    void typed_access_reports_the_type_it_wanted() {
        JsonReader reader;
        JsonValue value;
        JsonError error;
        QVERIFY(reader.parse("{\"s\":\"x\",\"n\":null,\"o\":{}}", value, error));

        bool b = false;
        QVERIFY(!value.find("s")->as_bool(b, error, "flag"));
        QCOMPARE(error.kind, JsonErrorKind::type_mismatch);
        QVERIFY2(error.context.find("flag") != std::string::npos, "error should name the field");
        QVERIFY2(error.context.find("wanted boolean") != std::string::npos, "error should name the wanted type");
        QVERIFY2(error.context.find("got string") != std::string::npos, "error should name the actual type");

        std::string_view text;
        QVERIFY(value.find("null_absent") == nullptr);   // absent, not null
        QVERIFY(!value.find("o")->as_string(text, error));
        QVERIFY(!value.find("n")->as_bool(b, error));
    }

    // ---- bounds ----

    void input_size_limit_is_checked_before_parsing() {
        JsonLimits limits;
        limits.max_input_bytes = 16;
        JsonReader reader(limits);
        JsonValue value;
        JsonError error;
        QVERIFY(!reader.parse("{\"t\":\"buffer-op\",\"padding\":\"........\"}", value, error));
        QCOMPARE(error.kind, JsonErrorKind::input_too_large);
        QVERIFY2(error.context.find("limit") != std::string::npos, "should state the limit");

        QVERIFY(reader.parse("{\"t\":\"u\"}", value, error));   // under it
    }

    void nesting_depth_limit_is_enforced() {
        JsonLimits limits;
        limits.max_nesting_depth = 4;
        JsonReader reader(limits);

        std::string ok = "[[[[]]]]";
        JsonValue value;
        JsonError error;
        QVERIFY2(reader.parse(ok, value, error), qPrintable(QString::fromStdString(error.context)));

        std::string too_deep = "[[[[[]]]]]";
        QVERIFY(!reader.parse(too_deep, value, error));
        QCOMPARE(error.kind, JsonErrorKind::too_deep);
    }

    void deeply_nested_input_fails_without_overflowing_the_stack() {
        // The classic parser vulnerability: depth is a bound, not a promise.
        JsonLimits limits;
        limits.max_nesting_depth = 32;
        JsonReader reader(limits);
        std::string input(200000, '[');
        JsonValue value;
        JsonError error;
        QVERIFY(!reader.parse(input, value, error));
        QCOMPARE(error.kind, JsonErrorKind::too_deep);
    }

    void element_count_limit_is_enforced() {
        JsonLimits limits;
        limits.max_elements = 8;
        JsonReader reader(limits);
        JsonValue value;
        JsonError error;
        QVERIFY(!reader.parse("[1,2,3,4,5,6,7,8,9]", value, error));
        QCOMPARE(error.kind, JsonErrorKind::too_many_elements);
        QVERIFY(reader.parse("[1,2,3]", value, error));
    }

    void container_element_limit_is_enforced() {
        JsonLimits limits;
        limits.max_container_elements = 4;
        JsonReader reader(limits);
        JsonValue value;
        JsonError error;
        QVERIFY(!reader.parse("[1,2,3,4,5]", value, error));
        QCOMPARE(error.kind, JsonErrorKind::too_many_elements);
        QVERIFY(!reader.parse("{\"a\":1,\"b\":2,\"c\":3,\"d\":4,\"e\":5}", value, error));
    }

    void text_and_key_limits_are_enforced_before_allocation() {
        JsonLimits limits;
        limits.max_text_bytes = 8;
        limits.max_key_bytes = 4;
        JsonReader reader(limits);
        JsonValue value;
        JsonError error;

        QVERIFY(!reader.parse("{\"keyname\":\"x\"}", value, error));
        QCOMPARE(error.kind, JsonErrorKind::key_too_large);

        // A string that is short in source but long once unescaped must also be
        // caught: `\u0041` costs six bytes and one character, and repeating it
        // is the cheap way to ask for a large allocation.
        std::string bomb = "{\"s\":\"";
        for (int i = 0; i < 16; ++i) bomb += "\\u0041";
        bomb += "\"}";
        QVERIFY(!reader.parse(bomb, value, error));
        QCOMPARE(error.kind, JsonErrorKind::text_too_large);
    }

    // ---- envelope resolution: retained vs quarantined ----

    void unknown_identifier_is_retained_not_rejected() {
        // §3 step 2: a newer writer's bytes are valid and must be kept so an
        // upgrade can read them. Reporting this as corruption would throw away
        // work (R1).
        JsonLimits limits;
        JsonReader probe(limits);
        JsonValue probe_value;
        JsonError probe_error;
        QVERIFY(probe.parse("{\"t\":\"buffer-op\",\"v\":1}", probe_value, probe_error));

        JsonReader reader(limits);
        reader.set_known_identifiers({"idlist-op"});
        JsonValue value;
        JsonError error;
        QVERIFY2(reader.parse("{\"t\":\"buffer-op\",\"v\":1}", value, error),
                 qPrintable(QString::fromStdString(error.context)));
        QVERIFY(reader.unsupported_version());

        JsonReader known(limits);
        known.set_known_identifiers({"buffer-op"});
        QVERIFY(known.parse("{\"t\":\"buffer-op\",\"v\":1}", value, error));
        QVERIFY(!known.unsupported_version());
    }

    void parse_object_envelope_requires_an_identifier() {
        JsonLimits limits;
        JsonValue value;
        JsonError error;
        bool retained = true;

        QVERIFY(!parse_object_envelope("{\"v\":1}", limits, value, error, retained));
        QCOMPARE(error.kind, JsonErrorKind::missing_field);

        QVERIFY(!parse_object_envelope("[]", limits, value, error, retained));
        QCOMPARE(error.kind, JsonErrorKind::type_mismatch);

        QVERIFY(!parse_object_envelope("{\"t\":\"u\"", limits, value, error, retained));
        QCOMPARE(error.kind, JsonErrorKind::unexpected_end);

        QVERIFY(parse_object_envelope("{\"t\":\"u\",\"v\":1}", limits, value, error, retained));
        QVERIFY(!retained);
    }

    // ---- structural shapes ----

    void empty_containers_and_nulls_parse() {
        JsonReader reader;
        JsonValue value;
        JsonError error;
        QVERIFY2(reader.parse("{\"a\":{},\"b\":[],\"c\":null,\"d\":true,\"e\":false}",
                              value, error),
                 qPrintable(QString::fromStdString(error.context)));
        QVERIFY(value.find("a")->is_object());
        QVERIFY(value.find("a")->members().empty());
        QVERIFY(value.find("b")->is_array());
        QVERIFY(value.find("b")->elements().empty());
        QVERIFY(value.find("c")->is_null());
        QVERIFY(value.find("d")->boolean());
        QVERIFY(!value.find("e")->boolean());

        bool found = false;
        QVERIFY(value.find("c", &found) != nullptr);
        QVERIFY(found);
        QVERIFY(value.find("zz", &found) == nullptr);
        QVERIFY(!found);
    }

    void non_object_roots_parse_but_report_no_identifier() {
        JsonReader reader;
        JsonValue value;
        JsonError error;
        QVERIFY(reader.parse("[1,2,3]", value, error));
        QVERIFY(reader.root_identifier().empty());
        QVERIFY(!reader.has_root_version());
        QVERIFY(reader.parse("42", value, error));
        QVERIFY(value.is_number());
        QVERIFY(reader.parse("null", value, error));
        QVERIFY(value.is_null());
    }

    void reader_is_reusable_across_parses() {
        JsonReader reader;
        JsonValue value;
        JsonError error;
        reader.set_known_identifiers({"known"});

        QVERIFY(reader.parse("{\"t\":\"known\",\"v\":3}", value, error));
        QCOMPARE(reader.root_version(), uint64_t(3));
        QVERIFY(!reader.unsupported_version());

        // A failed parse must not leave the previous one's version behind.
        // A failed parse leaves no header state at all: no version from the
        // failure, and none carried over from the parse before it.
        QVERIFY(!reader.parse("{\"t\":\"other\"", value, error));
        QVERIFY(!reader.has_root_version());
        QCOMPARE(reader.root_version(), uint64_t(0));

        // The next parse works and is free to report something different.
        QVERIFY(reader.parse("{\"t\":\"other\",\"v\":9}", value, error));
        QCOMPARE(reader.root_version(), uint64_t(9));
        QVERIFY(reader.unsupported_version());

        QVERIFY(reader.parse("{}", value, error));
        QVERIFY(!reader.has_root_version());
        QCOMPARE(reader.root_version(), uint64_t(0));
    }

    void error_context_is_single_line_and_bounded() {
        // These strings go into log lines and quarantine records.
        JsonReader reader;
        JsonValue value;
        JsonError error;
        const std::string input = "{\n\n\"a\":\"bbbbbbbbbbbbbbbbbbbb\\\"\\\n\n\n";
        QVERIFY(!reader.parse(input, value, error));
        QVERIFY(error.context.find('\n') == std::string::npos);
        QVERIFY2(error.context.size() < 200, "context should be bounded");
    }

private:
    /// Deterministic canonical rendering used to compare two parsed documents.
    /// Deliberately not `operator==`: the point is to see *which* field differs.
    static std::string encode_canonical(const JsonValue& value) {
        switch (value.type()) {
            case JsonValue::Type::null: return "null";
            case JsonValue::Type::boolean:
                return value.boolean() ? "true" : "false";
            case JsonValue::Type::number: return "N(" + value.number_text() + ")";
            case JsonValue::Type::string: return "S(" + value.string_value() + ")";
            case JsonValue::Type::array: {
                std::string out = "[";
                for (const auto& element : value.elements()) {
                    if (out.size() > 1) out += ",";
                    out += encode_canonical(element);
                }
                return out + "]";
            }
            case JsonValue::Type::object: {
                std::vector<std::string> parts;
                for (const auto& [key, member] : value.members())
                    parts.push_back(key + ":" + encode_canonical(member));
                std::sort(parts.begin(), parts.end());
                std::string out = "{";
                for (size_t i = 0; i < parts.size(); ++i) {
                    if (i) out += ",";
                    out += parts[i];
                }
                return out + "}";
            }
        }
        return "?";
    }
};

QTEST_MAIN(TestDecodeValidation)
#include "tst_decode_validation.moc"