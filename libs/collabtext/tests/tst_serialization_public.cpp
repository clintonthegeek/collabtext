#include "CodecTest.h"
// Installed-header-only behavior checks. No concrete operation payload access.
#include <collabtext/Serialization.h>
#include <collabtext/Operations.h>
#include <collabtext/IdListOperations.h>
#include <collabtext/CrdtEngine.h>
#include <collabtext/IdListEngine.h>
#include <QTest>
#include <QFile>
#include "TestActor.h"
#include <type_traits>
#include <variant>

using CollabText::Test::actor;
using namespace CollabText::Crdt;

template<class T> concept HasTimestamp = requires(T value) { value.timestamp; };
template<class T> concept HasPayload = requires(T value) { value.ranges; };
static_assert(!HasTimestamp<Operation> && !HasPayload<Operation>);
static_assert(!HasTimestamp<IdListOperation>);
static_assert(!std::is_aggregate_v<Operation>);
static_assert(!std::is_aggregate_v<IdListOperation>);
static_assert(!std::is_convertible_v<Operation, IdListOperation>);

template<class T> bool has_error_code(const T& result, CollabText::ErrorCode code) {
    if constexpr (requires { result.ok(); result.error().code(); })
        return !result.ok() && result.error().code() == code;
    return false;
}

class TestSerializationPublic : public QObject {
    Q_OBJECT
private:
    static Operation text_op(uint16_t actor_number, const std::string& text, bool undo = false) {
        CollabText::CrdtEngine engine(CollabText::Test::test_counter(actor_number));
        Operation captured;
        engine.setOnLocalOp([&](const Operation& op) { captured = op; });
        engine.insert(0, text);
        if (undo) engine.undo();
        return captured;  // survives callback and engine destruction
    }
    static std::string golden(const char* name) {
        const auto relative = QString("fixtures/operation-codec/%1.wire").arg(name).toUtf8();
        QFile file(QFINDTESTDATA(relative.constData()));
        if (!file.open(QIODevice::ReadOnly)) qFatal("missing codec golden fixture");
        return file.readAll().toStdString();
    }
private slots:
    void exact_golden_frames_from_actual_operations() {
        CollabText::CrdtEngine text(CollabText::Test::test_counter(1));
        Operation op;
        text.setOnLocalOp([&](const Operation& value) { op = value; });
        text.insert(0, "a");
        QCOMPARE(checked_codec(encode_operation(op)), golden("text-edit"));
        QVERIFY(text.undo());
        QCOMPARE(checked_codec(encode_operation(op)), golden("text-undo"));
        CollabText::IdListEngine list(CollabText::Test::test_counter(1));
        auto inserted = list.try_insert_after(Anchor::min(), UINT64_MAX);
        QVERIFY(inserted.ok());
        QCOMPARE(checked_codec(encode_idlist_operation(inserted.value())), golden("list-insert"));
        auto at = list.try_anchor_of(UINT64_MAX);
        QVERIFY(at.ok());
        auto removed = list.try_remove_at(at.value());
        QVERIFY(removed.ok());
        QCOMPARE(checked_codec(encode_idlist_operation(removed.value())), golden("list-remove"));
        auto undo = list.undo();
        QVERIFY(undo.has_value());
        QCOMPARE(checked_codec(encode_idlist_operation(*undo)), golden("list-undo"));
        Global g;
        g.observe({actor(1), (uint64_t(1) << 53) + 1});
        auto full_actor = CollabText::ActorId::parse("fedcba98765432100123456789abcdef");
        QVERIFY(full_actor.ok());
        g.observe({full_actor.value(), UINT64_MAX - 1});
        QCOMPARE(checked_codec(encode_global(g)), golden("global"));
        CounterGap gap{actor(1), 4, 2, {}};
        gap.prior.observe({actor(1), 3});
        QCOMPARE(checked_codec(encode_counter_gap(gap)), golden("gap"));
        for (auto name : {"text-edit", "text-undo"}) {
            const auto bytes = golden(name);
            auto decoded = decode_operation(bytes);
            QVERIFY(decoded.ok());
            QCOMPARE(checked_codec(encode_operation(decoded.value())), bytes);
        }
        for (auto name : {"list-insert", "list-remove", "list-undo"}) {
            const auto bytes = golden(name);
            auto decoded = decode_idlist_operation(bytes);
            QVERIFY(decoded.ok());
            QCOMPARE(checked_codec(encode_idlist_operation(decoded.value())), bytes);
        }
        QCOMPARE(checked_codec(encode_global(checked_codec(decode_global(golden("global"))))), golden("global"));
        QCOMPARE(checked_codec(encode_counter_gap(checked_codec(decode_counter_gap(golden("gap"))))), golden("gap"));
    }
    void full_width_values_and_equal_shape_wire_sizes() {
        for (auto start : {uint64_t(1), (uint64_t(1) << 53) + 1, UINT64_MAX - 2}) {
            CollabText::CrdtEngine sender(CollabText::Test::test_counter(1, start));
            Operation op;
            sender.setOnLocalOp([&](const Operation& value) { op = value; });
            sender.insert(0, "a");
            auto wire = encode_operation(op);
            QVERIFY(wire.ok());
            auto decoded = decode_operation(wire.value());
            QVERIFY(decoded.ok());
            QCOMPARE(op_identity(decoded.value()), Identity(actor(1), start + 1));
            QCOMPARE(wire.value().size(), golden("text-edit").size());
        }
        CollabText::IdListEngine sender(CollabText::Test::test_counter(1, UINT64_MAX - 1));
        auto inserted = sender.try_insert_after(Anchor::min(), UINT64_MAX);
        QVERIFY(inserted.ok());
        auto wire = encode_idlist_operation(inserted.value());
        QVERIFY(wire.ok());
        auto decoded = decode_idlist_operation(wire.value());
        QVERIFY(decoded.ok());
        QCOMPARE(idlist_op_identity(decoded.value()).counter, UINT64_MAX - 1);
        CollabText::IdListEngine receiver(CollabText::Test::test_counter(2));
        QCOMPARE(receiver.apply_ops({decoded.value()})[0].kind(), CollabText::ApplyOutcomeKind::applied);
        QCOMPARE(receiver.ids(), std::vector<uint64_t>{UINT64_MAX});
        Global small, large;
        small.observe({actor(1), 1}); small.observe({actor(2), 2});
        large.observe({actor(1), UINT64_MAX - 1}); large.observe({actor(2), (uint64_t(1) << 53) + 1});
        QCOMPARE(checked_codec(encode_global(small)).size(), checked_codec(encode_global(large)).size());
    }
    void frame_classification_and_all_truncated_prefixes() {
        using E = CollabText::ErrorCode;
        auto check_family = []<class Decode>(const std::string& bytes, Decode decode) {
            for (size_t n = 0; n < bytes.size(); ++n) {
                if (decode(std::string_view(bytes).substr(0, n)).ok()) return false;
            }
            for (auto suffix : {"x", "{}", "\nnull"}) if (decode(bytes + suffix).ok()) return false;
            if (!decode(bytes + " \n\t\r").ok()) return false;
            return has_error_code(decode("{}"), E::unknown_format)
                && has_error_code(decode("ZZZZ0001:{}"), E::unknown_format)
                && has_error_code(decode(bytes.substr(0, 4) + "0002:{}"), E::unsupported_version)
                && has_error_code(decode(bytes.substr(0, 4) + "0001!{}"), E::malformed);
        };
        QVERIFY(check_family(golden("text-edit"), decode_operation));
        QVERIFY(check_family(golden("list-insert"), decode_idlist_operation));
        QVERIFY(check_family(golden("global"), decode_global));
        QVERIFY(check_family(golden("gap"), decode_counter_gap));
        QVERIFY(has_error_code(decode_operation("CTBO0001:{\"t\":\"future\"}"), E::unsupported_operation));
        QVERIFY(has_error_code(decode_idlist_operation("CTLO0001:{\"t\":\"future\"}"), E::unsupported_operation));
        QVERIFY(has_error_code(decode_operation(golden("list-insert")), E::unknown_format));
        QVERIFY(has_error_code(decode_idlist_operation(golden("text-edit")), E::unknown_format));
    }
    void strict_shape_identity_allocation_and_unicode() {
        using E = CollabText::ErrorCode;
        const auto wire = golden("text-edit");
        auto replace = [&](std::string from, std::string to) {
            auto altered = wire;
            const auto at = altered.find(from);
            if (at == std::string::npos) qFatal("inactive codec corruption test");
            altered.replace(at, from.size(), to);
            return altered;
        };
        for (auto bytes : {
            replace("\"a\":[\"0000000000000001\",\"0000000000000002\"]", "\"a\":[\"0000000000000000\",\"0000000000000002\"]"),
            replace("\"a\":[\"0000000000000001\",\"0000000000000002\"]", "\"a\":[\"0000000000000001\",\"ffffffffffffffff\"]"),
            replace("\"r\":[[\"0000000000000000\",\"0000000000000000\"]]", "\"r\":[[\"0000000100000000\",\"0000000000000000\"]]"),
            replace("\"nt\":[\"a\"]", "\"nt\":[]"),
            replace("\"n\":\"0000000000000001\"", "\"n\":\"0000000000000002\""),
            replace(",\"sr\":[]", ""),
            replace(",\"sr\":[]", ",\"sr\":[],\"future\":true"),
            replace("\"v\":{}", "\"v\":{\"00000000000000000000000000000001\":\"0000000000000001\"}"),
            replace("\"n\":\"0000000000000001\"", "\"n\":1")
        }) QVERIFY(has_error_code(decode_operation(bytes), E::malformed));
        QVERIFY(has_error_code(decode_operation(replace("\"00000000000000000000000000000001\"", "\"00000000000000000000000000000000\"")), E::invalid_identity));
        QVERIFY(has_error_code(decode_operation(replace("\"ts\":[\"00000000000000000000000000000001\",\"0000000000000002\"]", "\"ts\":[\"00000000000000000000000000000001\",\"ffffffffffffffff\"]")), E::invalid_identity));
        QVERIFY(has_error_code(decode_operation(replace("\"nt\":[\"a\"]", "\"nt\":[\"\\ud800\"]")), E::invalid_unicode));
        QVERIFY(has_error_code(decode_operation(replace("\"nt\":[\"a\"]", "\"nt\":[\"" + std::string(1, char(0xc0)) + "\"]")), E::invalid_unicode));
        QVERIFY(has_error_code(encode_operation(Operation{}), E::invalid_identity));
        QVERIFY(has_error_code(encode_idlist_operation(IdListOperation{}), E::invalid_identity));
        auto original = text_op(1, "a");
        auto moved = std::move(original);
        QVERIFY(has_error_code(encode_operation(original), E::invalid_identity));
        QVERIFY(encode_operation(moved).ok());
        QVERIFY(has_error_code(decode_global("CTGV0001:{\"FEDCBA98765432100123456789ABCDEF\":\"0000000000000001\"}"), E::invalid_identity));
        QVERIFY(has_error_code(decode_global("CTGV0001:{\"00000000000000000000000000000001\":\"0000000000000000\"}"), E::invalid_identity));
    }
    void gap_interval_edges_and_codec_bounds() {
        using E = CollabText::ErrorCode;
        CounterGap gap{actor(1), UINT64_MAX - 1, 1, {}};
        auto wire = encode_counter_gap(gap);
        QVERIFY(wire.ok());
        auto decoded = decode_counter_gap(wire.value());
        QVERIFY(decoded.ok());
        QCOMPARE(decoded.value().first, UINT64_MAX - 1);
        QCOMPARE(decoded.value().count, uint64_t(1));
        gap.count = 2;
        QVERIFY(has_error_code(encode_counter_gap(gap), E::malformed));
        gap.count = 1; gap.prior.observe({actor(1), gap.first});
        QVERIFY(has_error_code(encode_counter_gap(gap), E::malformed));
        gap.prior = {}; gap.first = 0;
        QVERIFY(has_error_code(encode_counter_gap(gap), E::invalid_identity));
        gap.first = 1; gap.count = 0;
        QVERIFY(has_error_code(encode_counter_gap(gap), E::malformed));
        Global frontier;
        for (uint64_t n = 1; n <= 512; ++n) frontier.observe({CollabText::Test::actor64(n), n});
        QVERIFY(encode_global(frontier).ok());
        frontier.observe({CollabText::Test::actor64(513), 513});
        QVERIFY(has_error_code(encode_global(frontier), E::malformed));
        auto many = std::string("CTGV0001:{");
        for (uint64_t n = 1; n <= 513; ++n) {
            if (n > 1) many += ',';
            many += "\"" + CollabText::Test::actor64(n).to_string() + "\":\"0000000000000001\"";
        }
        many += '}';
        QVERIFY(has_error_code(decode_global(many), E::malformed));
        const std::string huge((8u << 20) + 1, ' ');
        QVERIFY(has_error_code(decode_operation(huge), E::malformed));
        QVERIFY(has_error_code(decode_idlist_operation(huge), E::malformed));
        QVERIFY(has_error_code(decode_global(huge), E::malformed));
        QVERIFY(has_error_code(decode_counter_gap(huge), E::malformed));
        auto oversized_string = golden("text-edit");
        oversized_string.replace(oversized_string.find("\"nt\":[\"a\"]"), 10,
            "\"nt\":[\"" + std::string((4u << 20) + 1, 'a') + "\"]");
        QVERIFY(has_error_code(decode_operation(oversized_string), E::malformed));
    }
    void unsupported_version_has_a_typed_verdict() {
        QVERIFY(has_error_code(decode_operation("CTBO0002:{}"),
                               CollabText::ErrorCode::unsupported_version));
    }
    void identity_carries_an_actor_and_a_counter() {
        Identity id(actor(3), 42);
        QCOMPARE(id.counter, uint64_t(42));
        QCOMPARE(id.actor, actor(3));
    }
    void op_lamport_for_edit_operation() {
        const auto op = text_op(1, "hello");
        QCOMPARE(op_identity(op), Identity(actor(1), 6));
    }
    void op_lamport_for_undo_operation() {
        const auto op = text_op(2, "hello", true);
        QCOMPARE(op_identity(op), Identity(actor(2), 7));
    }
    void idlist_op_lamport_for_insert() {
        CollabText::IdListEngine engine(CollabText::Test::test_counter(5));
        auto op = engine.try_insert_after(Anchor::min(), 99);
        QVERIFY(op.ok());
        QCOMPARE(idlist_op_identity(op.value()), Identity(actor(5), 1));
    }
    void edit_operation_roundtrip() {
        const auto op = text_op(1, "hello");
        const auto wire = checked_codec(encode_operation(op));
        auto decoded = decode_operation(wire);
        QVERIFY(decoded.ok());
        QCOMPARE(checked_codec(encode_operation(decoded.value())), wire);
        QCOMPARE(op_identity(decoded.value()), op_identity(op));
        CollabText::CrdtEngine receiver(CollabText::Test::test_counter(9));
        QVERIFY(receiver.applyRemoteOp(decoded.value()));
        QCOMPARE(receiver.text(), std::string("hello"));
    }
    void undo_operation_roundtrip() {
        CollabText::CrdtEngine sender(CollabText::Test::test_counter(1));
        CollabText::CrdtEngine receiver(CollabText::Test::test_counter(9));
        Operation captured;
        sender.setOnLocalOp([&](const Operation& op) { captured = op; });
        sender.insert(0, "hello");
        QVERIFY(receiver.applyRemoteOp(captured));
        QVERIFY(sender.undo());
        const auto wire = checked_codec(encode_operation(captured));
        auto decoded = decode_operation(wire);
        QVERIFY(decoded.ok());
        QCOMPARE(checked_codec(encode_operation(decoded.value())), wire);
        QCOMPARE(op_identity(decoded.value()), op_identity(captured));
        QVERIFY(receiver.applyRemoteOp(decoded.value()));
        QVERIFY(receiver.text().empty());
    }
    void idlist_insert_roundtrip() {
        CollabText::IdListEngine sender(CollabText::Test::test_counter(1));
        CollabText::IdListEngine receiver(CollabText::Test::test_counter(9));
        const uint64_t value = 0xDEADBEEFCAFE0001ULL;
        auto op = sender.try_insert_after(Anchor::min(), value);
        QVERIFY(op.ok());
        const auto wire = checked_codec(encode_idlist_operation(op.value()));
        auto decoded = decode_idlist_operation(wire);
        QVERIFY(decoded.ok());
        QCOMPARE(checked_codec(encode_idlist_operation(decoded.value())), wire);
        QCOMPARE(idlist_op_identity(decoded.value()), idlist_op_identity(op.value()));
        QCOMPARE(receiver.apply_ops({decoded.value()})[0].kind(), CollabText::ApplyOutcomeKind::applied);
        QCOMPARE(receiver.ids(), std::vector<uint64_t>{value});
    }
    void idlist_remove_roundtrip() {
        CollabText::IdListEngine sender(CollabText::Test::test_counter(1));
        CollabText::IdListEngine receiver(CollabText::Test::test_counter(9));
        auto inserted = sender.try_insert_after(Anchor::min(), 42);
        QVERIFY(inserted.ok());
        QCOMPARE(receiver.apply_ops({inserted.value()})[0].kind(), CollabText::ApplyOutcomeKind::applied);
        auto anchor = sender.try_anchor_of(42);
        QVERIFY(anchor.ok());
        auto removed = sender.try_remove_at(anchor.value());
        QVERIFY(removed.ok());
        const auto wire = checked_codec(encode_idlist_operation(removed.value()));
        auto decoded = decode_idlist_operation(wire);
        QVERIFY(decoded.ok());
        QCOMPARE(checked_codec(encode_idlist_operation(decoded.value())), wire);
        QCOMPARE(receiver.apply_ops({decoded.value()})[0].kind(), CollabText::ApplyOutcomeKind::applied);
        QVERIFY(receiver.ids().empty());
    }
    void idlist_undo_roundtrip() {
        CollabText::IdListEngine sender(CollabText::Test::test_counter(1));
        CollabText::IdListEngine receiver(CollabText::Test::test_counter(9));
        auto inserted = sender.try_insert_after(Anchor::min(), 42);
        QVERIFY(inserted.ok());
        QCOMPARE(receiver.apply_ops({inserted.value()})[0].kind(), CollabText::ApplyOutcomeKind::applied);
        auto undone = sender.undo();
        QVERIFY(undone.has_value());
        const auto wire = checked_codec(encode_idlist_operation(*undone));
        auto decoded = decode_idlist_operation(wire);
        QVERIFY(decoded.ok());
        QCOMPARE(checked_codec(encode_idlist_operation(decoded.value())), wire);
        QCOMPARE(receiver.apply_ops({decoded.value()})[0].kind(), CollabText::ApplyOutcomeKind::applied);
        QVERIFY(receiver.ids().empty());
    }
    void escaped_non_bmp_text_preserves_the_actual_operation() {
        const std::string emoji = "\xf0\x9f\x98\x80";
        const auto original = text_op(1, emoji);
        const auto literal_wire = checked_codec(encode_operation(original));
        auto escaped_wire = literal_wire;
        const std::string escape = "\\ud83d\\ude00";
        size_t start = 0, replacements = 0;
        while ((start = escaped_wire.find(emoji, start)) != std::string::npos) {
            escaped_wire.replace(start, emoji.size(), escape);
            start += escape.size(); ++replacements;
        }
        QVERIFY(replacements > 0);
        auto decoded = decode_operation(escaped_wire);
        QVERIFY(decoded.ok());
        QCOMPARE(checked_codec(encode_operation(decoded.value())), literal_wire);
        CollabText::CrdtEngine receiver(CollabText::Test::test_counter(9));
        QVERIFY(receiver.applyRemoteOp(decoded.value()));
        QCOMPARE(receiver.text(), emoji);
    }
    void public_decoders_reject_untrusted_json_syntax() {
        const auto wire = checked_codec(encode_operation(text_op(1, "hello")));
        QVERIFY(decode_operation(wire).ok());
        QVERIFY(!decode_operation(wire + "garbage").ok());
        QVERIFY(!decode_operation(wire + std::string(1, '\0')).ok());
        auto duplicate = wire;
        duplicate.insert(duplicate.size() - 1, ",\"t\":\"u\"");
        QVERIFY(!decode_operation(duplicate).ok());
        auto invalid_utf8 = wire;
        invalid_utf8.replace(invalid_utf8.find("hello"), 1, std::string(1, char(0xc0)));
        QVERIFY(!decode_operation(invalid_utf8).ok());
        auto invalid_escape = wire;
        invalid_escape.replace(invalid_escape.find("hello"), 1, "\\q");
        QVERIFY(!decode_operation(invalid_escape).ok());
        CollabText::IdListEngine list(CollabText::Test::test_counter(2));
        auto inserted = list.try_insert_after(Anchor::min(), 42);
        QVERIFY(inserted.ok());
        auto list_wire = checked_codec(encode_idlist_operation(inserted.value()));
        QVERIFY(!decode_idlist_operation(list_wire + "garbage").ok());
        list_wire.insert(list_wire.size() - 1, ",\"t\":\"il-u\"");
        QVERIFY(!decode_idlist_operation(list_wire).ok());
        Global version;
        version.observe(Identity(actor(1), (uint64_t(1) << 53) + 1));
        auto version_wire = checked_codec(encode_global(version));
        QVERIFY(!decode_global(version_wire + "garbage").ok());
        auto empty = checked_codec(encode_global(Global{}));
        empty.insert(empty.size() - 1, "\"" + actor(1).to_string() + "\":1,\"" + actor(1).to_string() + "\":2");
        QVERIFY(!decode_global(empty).ok());
        // Same entry point as normal decoding; no unbounded old-parser allocation.
        const std::string oversized((8u << 20) + 1, ' ');
        QVERIFY(!decode_operation(oversized).ok());
        QVERIFY(!decode_idlist_operation(oversized).ok());
        QVERIFY(!decode_global(oversized).ok());
    }
    void decode_operation_error_on_invalid() {
        for (auto bytes : {"", "not json", "{}", "{\"t\":\"x\"}"})
            QVERIFY(!decode_operation(bytes).ok());
    }
    void decode_idlist_operation_error_on_invalid() {
        for (auto bytes : {"", "not json", "{}", "{\"t\":\"unknown\"}"})
            QVERIFY(!decode_idlist_operation(bytes).ok());
    }
};
QTEST_GUILESS_MAIN(TestSerializationPublic)
#include "tst_serialization_public.moc"

template<class T> concept VariantPayload = requires { std::variant_size<T>::value; };
static_assert(!VariantPayload<CollabText::Crdt::Operation>);
static_assert(!VariantPayload<CollabText::Crdt::IdListOperation>);
