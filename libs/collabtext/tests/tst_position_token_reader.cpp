/// tst_position_token_reader.cpp — the supported-reader fixture for CTPT version 1.
///
/// Roadmap task E07b. This is the executable half of "CTPT/1 is a supported
/// serialized token version". Four separate things are asserted, and they are
/// deliberately not the same assertion:
///
///   1. **An independent literal reader.** `parse_literal` walks the retained
///      bytes using only the field order and widths written in §4 of
///      `vnext-boundary-semantics.md` — it shares no code with `AnchorCodec.cpp`.
///      It must consume the buffer exactly, each fence tag must be START or END,
///      each item identity must have a nonzero actor and an in-range counter, the
///      two fence identities must differ, and every identity must be covered by
///      the mint frontier. This is what actually catches an internal field
///      leaking into the token: a length check cannot tell "two item fences" from
///      "one item fence plus an extra 24 bytes", and an earlier draft of this
///      test proved that by passing on exactly that injected field.
///
///   2. **Retention.** Every golden decodes and re-encodes to exactly itself, and
///      survives all 256 trailing suffixes, every proper truncation, a bumped
///      version field, and a typed read of the wrong kind. Note what this does
///      *not* prove: a golden whose bytes were corrupted in a still-valid way
///      passes it. Correctness of the bytes comes from (4).
///
///   3. **Representation independence, as recorded fact.** Each golden's
///      `byte_length` equals the length §4 implies from its declared logical
///      fields. Together with (1) this is the executable form of "the token does
///      not depend on fragment or locator layout".
///
///   4. **Meaning preserved.** Replaying each scenario on the current engines
///      must mint exactly the retained bytes. S03 replaced fractional locators
///      with FugueMax tree links and did not touch CTPT/1; if a future placement
///      change alters token bytes, that is a breaking change to a supported
///      format and belongs in a version 2 with a translation, not in a silently
///      different version 1.

#include <QTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <collabtext/ActorId.h>
#include <collabtext/Anchor.h>
#include <collabtext/CrdtEngine.h>
#include <collabtext/Engine.h>
#include <collabtext/IdListEngine.h>
#include <collabtext/Identity.h>
#include <collabtext/Operations.h>
#include <collabtext/Serialization.h>

#include "CodecTest.h"
#include "TestActor.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace CollabText;

namespace {

constexpr const char *kFixture = "fixtures/position-tokens/ctpt-v1-supported.json";

/// §4 fixed widths, restated here on purpose. A reader fixture that imported the
/// widths from the codec would agree with the codec by construction and would
/// check nothing.
constexpr size_t kActorBytes = 16;
constexpr size_t kItemWidth = kActorBytes + 8;
constexpr size_t kEdgeTagWidth = 1;
constexpr size_t kFenceBothEdges = 2 * kEdgeTagWidth;
constexpr size_t kFenceOneItem = kFenceBothEdges + kItemWidth;
constexpr size_t kFenceBothItems = kFenceBothEdges + 2 * kItemWidth;
constexpr size_t kMagic = 4, kVersion = 2, kKind = 1, kAffinity = 1, kEpoch = 8;
constexpr size_t kLengthPrefix = 2, kFrontierCount = 2;
constexpr size_t kMaxFrontierEntries = 512;
constexpr uint64_t kLastCounter = UINT64_MAX - 1;

/// Fixture text arrives as QString; Qt 6 removed QString::toStdString.
std::string text_of(const QJsonValue &value) {
    const QByteArray utf8 = value.toString().toUtf8();
    return std::string(utf8.constData(), static_cast<size_t>(utf8.size()));
}

struct Golden {
    QString id;
    QString kind;
    QString affinity;
    std::optional<int> selection;
    PositionScope scope;
    size_t byte_length = 0;
    std::string hex;

    std::string bytes() const {
        std::string out;
        out.reserve(hex.size() / 2);
        for (size_t i = 0; i + 1 < hex.size(); i += 2)
            out.push_back(static_cast<char>(std::stoul(hex.substr(i, 2), nullptr, 16)));
        return out;
    }
    bool has_selection() const { return selection.has_value(); }
    bool is_element() const { return kind == "ListElement"; }
    /// Everything §4 places before the mint frontier, for this declared scope.
    size_t fixed_prefix() const {
        return kMagic + kVersion + kKind + kAffinity + kEpoch
            + kLengthPrefix + scope.document.size()
            + kLengthPrefix + scope.stream.size()
            + (has_selection() ? 1 : 0);
    }
};

/// Load the retained fixture. Returns false with a reason rather than aborting, so
/// a fixture that declares the wrong format reports a failed test instead of
/// dumping a stack trace.
bool load_goldens(std::vector<Golden> &out, QString &error) {
    QFile file(QFINDTESTDATA(kFixture));
    if (!file.open(QIODevice::ReadOnly)) {
        error = "missing the retained position-token fixture";
        return false;
    }
    QJsonParseError parse_error{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        error = QString("fixture is not valid JSON: %1").arg(parse_error.errorString());
        return false;
    }
    const QJsonObject root = document.object();
    if (root.value("schema").toString() != "collabtext-position-token-goldens/1") {
        error = "unexpected position-token fixture schema";
        return false;
    }
    if (root.value("format").toString() != "CTPT" || root.value("version").toInt() != 1) {
        error = QString("this reader fixture covers CTPT version 1; the fixture declares %1/%2")
                    .arg(root.value("format").toString()).arg(root.value("version").toInt());
        return false;
    }
    out.clear();
    for (const QJsonValue &value : root.value("cases").toArray()) {
        const QJsonObject entry = value.toObject();
        const QJsonObject scope = entry.value("scope").toObject();
        Golden golden;
        golden.id = entry.value("id").toString();
        golden.kind = entry.value("kind").toString();
        golden.affinity = entry.value("affinity").toString();
        if (!entry.value("selection").isNull()) golden.selection = entry.value("selection").toInt();
        golden.scope = PositionScope{
            text_of(scope.value("document")), text_of(scope.value("stream")),
            scope.value("primitive").toString() == "list" ? Primitive::list : Primitive::text,
            static_cast<uint64_t>(scope.value("epoch").toDouble())};
        golden.byte_length = static_cast<size_t>(entry.value("byte_length").toDouble());
        golden.hex = text_of(entry.value("hex"));
        if (golden.hex.size() % 2 != 0) {
            error = QString("fixture case %1 has an odd hex length").arg(golden.id);
            return false;
        }
        out.push_back(std::move(golden));
    }
    if (out.empty()) {
        error = "the retained position-token fixture has no cases";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// An independent, literal §4 reader. No production codec is called.
// ---------------------------------------------------------------------------
struct Literal {
    uint16_t version = 0;
    uint8_t kind = 0;
    uint8_t affinity = 0;
    std::string document;
    std::string stream;
    uint64_t epoch = 0;
    std::optional<uint8_t> selection;
    /// For kinds 1/2/4: each fence is a tag plus an optional identity.
    struct Fence { uint8_t tag = 0; std::optional<Crdt::Identity> identity; };
    std::optional<Fence> left_fence, right_fence;
    std::optional<Crdt::Identity> referent;
    size_t frontier_entries = 0;
};

class LiteralReader {
public:
    explicit LiteralReader(const std::string &bytes) : m_bytes(bytes) {}

    bool u8(uint8_t &out) {
        if (m_at + 1 > m_bytes.size()) return fail("truncated uint8");
        out = static_cast<uint8_t>(m_bytes[m_at++]);
        return true;
    }
    bool u16(uint16_t &out) {
        if (m_at + 2 > m_bytes.size()) return fail("truncated uint16");
        out = static_cast<uint16_t>((static_cast<uint8_t>(m_bytes[m_at]) << 8)
                                    | static_cast<uint8_t>(m_bytes[m_at + 1]));
        m_at += 2;
        return true;
    }
    bool u64(uint64_t &out) {
        if (m_at + 8 > m_bytes.size()) return fail("truncated uint64");
        out = 0;
        for (int i = 0; i < 8; ++i) out = (out << 8) | static_cast<uint8_t>(m_bytes[m_at++]);
        return true;
    }
    bool magic() {
        if (m_bytes.size() < 4 || m_bytes.compare(0, 4, "CTPT") != 0) return fail("not a CTPT frame");
        m_at = 4;
        return true;
    }
    bool length_prefixed(std::string &out) {
        uint16_t length = 0;
        if (!u16(length)) return false;
        if (length < 1 || length > 256) return fail("scope string length out of range");
        if (m_at + length > m_bytes.size()) return fail("truncated scope string");
        out = m_bytes.substr(m_at, length);
        m_at += length;
        return true;
    }
    bool identity(Crdt::Identity &out) {
        if (m_at + kActorBytes + 8 > m_bytes.size()) return fail("truncated identity");
        std::array<uint8_t, ActorId::kBytes> actor{};
        for (size_t i = 0; i < kActorBytes; ++i) actor[i] = static_cast<uint8_t>(m_bytes[m_at + i]);
        m_at += kActorBytes;
        uint64_t counter = 0;
        if (!u64(counter)) return false;
        out = Crdt::Identity{ActorId::from_bytes(actor), counter};
        if (!out.actor.valid()) return fail("zero actor");
        if (counter == 0 || counter > kLastCounter) return fail("counter out of range");
        return true;
    }
    bool fence(Literal::Fence &out) {
        uint8_t tag = 0;
        if (!u8(tag)) return false;
        if (tag > 1) return fail("fence tag is neither START nor END");
        out.tag = tag;
        if (tag == 0) { out.identity.reset(); return true; }
        Crdt::Identity named;
        if (!identity(named)) return false;
        out.identity = named;
        return true;
    }
    bool at_end() const { return m_at == m_bytes.size(); }
    size_t consumed() const { return m_at; }
    const QString &error() const { return m_error; }

private:
    bool fail(const char *what) {
        if (m_error.isEmpty()) m_error = QString(what);
        return false;
    }
    const std::string &m_bytes;
    size_t m_at = 0;
    QString m_error;
};

/// Parse the retained bytes under §4 alone. `declared` supplies only the two
/// scope strings, so a wrong length prefix is caught rather than accommodated.
bool parse_literal(const std::string &bytes, const Golden &declared, Literal &out, QString &error) {
    LiteralReader reader(bytes);
    Literal parsed;
    if (!reader.magic()) { error = reader.error(); return false; }
    if (!reader.u16(parsed.version)) { error = reader.error(); return false; }
    if (!reader.u8(parsed.kind)) { error = reader.error(); return false; }
    if (!reader.u8(parsed.affinity)) { error = reader.error(); return false; }
    if (!reader.length_prefixed(parsed.document)) { error = reader.error(); return false; }
    if (!reader.length_prefixed(parsed.stream)) { error = reader.error(); return false; }
    if (!reader.u64(parsed.epoch)) { error = reader.error(); return false; }
    if (parsed.document != declared.scope.document) { error = "document does not match the fixture"; return false; }
    if (parsed.stream != declared.scope.stream) { error = "stream does not match the fixture"; return false; }

    if (parsed.kind == 4) {
        uint8_t selection = 0;
        if (!reader.u8(selection)) { error = reader.error(); return false; }
        if (selection < 1 || selection > 2) { error = "attachment selection is neither preceding nor following"; return false; }
        parsed.selection = selection;
    }
    if (parsed.kind == 3) {
        Crdt::Identity referent;
        if (!reader.identity(referent)) { error = reader.error(); return false; }
        parsed.referent = referent;
    } else if (parsed.kind == 1 || parsed.kind == 2 || parsed.kind == 4) {
        Literal::Fence left, right;
        if (!reader.fence(left)) { error = reader.error(); return false; }
        if (!reader.fence(right)) { error = reader.error(); return false; }
        if (left.identity && right.identity && *left.identity == *right.identity) {
            error = "left and right fence identities are equal";
            return false;
        }
        parsed.left_fence = left;
        parsed.right_fence = right;
    } else {
        error = "unknown token kind";
        return false;
    }

    uint16_t entries = 0;
    if (!reader.u16(entries)) { error = reader.error(); return false; }
    if (entries > kMaxFrontierEntries) { error = "frontier has too many entries"; return false; }
    parsed.frontier_entries = entries;

    // Canonical frontier: strictly increasing raw actor bytes, no zero actor,
    // no zero head. Every item identity in the token must be covered by it.
    std::vector<std::pair<ActorId, uint64_t>> frontier;
    for (size_t i = 0; i < entries; ++i) {
        Crdt::Identity entry;
        if (!reader.identity(entry)) { error = reader.error(); return false; }
        if (!frontier.empty() && !(frontier.back().first < entry.actor)) {
            error = "frontier entries are not strictly increasing by actor";
            return false;
        }
        frontier.emplace_back(entry.actor, entry.counter);
    }
    if (!reader.at_end()) { error = "frontier entries do not fill the buffer"; return false; }

    auto covered = [&](const Crdt::Identity &id) {
        for (const auto &entry : frontier)
            if (entry.first == id.actor && id.counter <= entry.second) return true;
        return false;
    };
    for (const Crdt::Identity *id : {parsed.referent ? &*parsed.referent : nullptr,
                                     parsed.left_fence && parsed.left_fence->identity ? &*parsed.left_fence->identity : nullptr,
                                     parsed.right_fence && parsed.right_fence->identity ? &*parsed.right_fence->identity : nullptr}) {
        if (id && !covered(*id)) { error = "a token identity is not covered by the mint frontier"; return false; }
    }

    out = parsed;
    return true;
}

/// The length §4 implies, computed from the declared logical fields and the
/// fence shapes the literal reader found. Equal to the recorded byte_length only
/// if the token contains exactly those fields.
size_t specified_length(const Golden &golden, const Literal &parsed) {
    // fixed_prefix() already counts the selection byte for a kind-4 token from
    // the fixture's declared fields; adding it again here double-counted one byte
    // and was the only disagreement this check ever produced.
    size_t length = golden.fixed_prefix();
    if (parsed.selection && !golden.has_selection()) length += 1;
    if (parsed.referent) length += kItemWidth;
    auto fence_width = [](const std::optional<Literal::Fence> &fence) {
        return fence ? kEdgeTagWidth + (fence->identity ? kItemWidth : 0) : 0;
    };
    length += fence_width(parsed.left_fence) + fence_width(parsed.right_fence);
    length += kFrontierCount + parsed.frontier_entries * kItemWidth;
    return length;
}

/// Replay a golden's scenario on the current engines and return the bytes it
/// mints. Every id in the fixture must be listed here; an unknown id returns
/// nullopt so a fixture case can never be added without a replay.
std::optional<std::string> mint(const QString &id) {
    namespace Crdt = CollabText::Crdt;
    auto encode = [](auto token) -> std::optional<std::string> {
        auto bytes = encode_position(PositionToken{std::move(token)});
        return bytes.ok() ? std::optional<std::string>(bytes.value()) : std::nullopt;
    };
    auto text = [](uint8_t actor_index) { return Test::test_counter(actor_index); };

    if (id == "text-boundary-empty-start-left" || id == "text-boundary-empty-end-right") {
        auto engine = checked_codec(CrdtEngine::create(text(1), {"d", "s", Primitive::text, 1}));
        return encode(checked_codec(engine->boundary_at(0, id.endsWith("left") ? Affinity::left : Affinity::right)));
    }
    if (id == "text-boundary-populated-end-right" || id == "text-boundary-populated-start-left") {
        auto engine = checked_codec(CrdtEngine::create(text(1), {"d", "s", Primitive::text, 1}));
        engine->insert(0, "a");
        const uint32_t offset = id.endsWith("start-left") ? 0u : 1u;
        return encode(checked_codec(engine->boundary_at(offset, id.endsWith("left") ? Affinity::left : Affinity::right)));
    }
    if (id == "text-boundary-interior-left" || id == "text-boundary-interior-right") {
        auto engine = checked_codec(CrdtEngine::create(text(1), {"d", "s2", Primitive::text, 1}));
        engine->insert(0, "ab");
        return encode(checked_codec(engine->boundary_at(1, id.endsWith("left") ? Affinity::left : Affinity::right)));
    }
    if (id == "text-attachment-preceding-left" || id == "text-attachment-following-right") {
        auto engine = checked_codec(CrdtEngine::create(text(1), {"d", "s2", Primitive::text, 1}));
        engine->insert(0, "ab");
        const bool preceding = id.contains("preceding");
        return encode(checked_codec(engine->attachment_at(1, preceding ? Affinity::left : Affinity::right,
                                                        preceding ? ReferentSide::preceding : ReferentSide::following)));
    }
    if (id == "text-boundary-nonbmp-4byte") {
        // U+1F600 is one scalar in four UTF-8 bytes, so byte 4 is the boundary.
        auto engine = checked_codec(CrdtEngine::create(text(2), {"d", "s3", Primitive::text, 7}));
        engine->insert(0, "\xF0\x9F\x98\x80z");
        return encode(checked_codec(engine->boundary_at(4, Affinity::left)));
    }
    if (id == "text-boundary-two-actor-frontier") {
        auto mine = checked_codec(CrdtEngine::create(text(1), {"d", "s4", Primitive::text, 1}));
        auto peer = checked_codec(CrdtEngine::create(text(2), {"d", "s4", Primitive::text, 1}));
        std::vector<std::string> from_peer;
        peer->setOnLocalOp([&](const Crdt::Operation &op) {
            auto bytes = Crdt::encode_operation(op);
            if (bytes.ok()) from_peer.push_back(bytes.value());
        });
        mine->insert(0, "xy");
        peer->insert(0, "ab");
        for (const std::string &bytes : from_peer) {
            auto decoded = Crdt::decode_operation(bytes);
            if (!decoded.ok()) return std::nullopt;   // QVERIFY cannot return a value
            if (!CollabText::Test::apply_not_rejected(mine->apply_remote(decoded.value()))) return std::nullopt;
        }
        return encode(checked_codec(mine->boundary_at(4, Affinity::right)));
    }
    if (id.startsWith("list-boundary-empty")) {
        auto list = checked_codec(IdListEngine::create(text(1), {"d", "ls", Primitive::list, 1}));
        return encode(checked_codec(list->boundary_at(0, id.endsWith("left") ? Affinity::left : Affinity::right)));
    }
    if (id == "list-boundary-end-right" || id == "list-boundary-interior-left") {
        auto list = checked_codec(IdListEngine::create(text(1), {"d", "ls", Primitive::list, 1}));
        (void)checked_codec(list->try_insert_after(Crdt::Anchor::min(), UINT64_MAX));
        (void)checked_codec(list->try_insert_after(Crdt::Anchor::min(), 200));
        if (id == "list-boundary-end-right")
            return encode(checked_codec(list->boundary_at(2, Affinity::right)));
        return encode(checked_codec(list->boundary_at(1, Affinity::left)));
    }
    if (id == "list-element-first" || id == "list-element-second") {
        auto list = checked_codec(IdListEngine::create(text(1), {"d", "ls", Primitive::list, 1}));
        (void)checked_codec(list->try_insert_after(Crdt::Anchor::min(), UINT64_MAX));
        (void)checked_codec(list->try_insert_after(Crdt::Anchor::min(), 200));
        const uint64_t value = id.endsWith("first") ? UINT64_MAX : 200;
        return encode(checked_codec(list->element_of(value)));
    }
    return std::nullopt;
}

} // namespace

class TestPositionTokenReader : public QObject {
    Q_OBJECT
private slots:
    /// An independent §4 reader must accept every retained golden, and must
    /// account for every byte from the declared logical fields.
    void an_independent_literal_reader_accepts_every_golden() {
        std::vector<Golden> goldens;
        QString error;
        QVERIFY2(load_goldens(goldens, error), qPrintable(error));
        for (const Golden &golden : goldens) {
            Literal parsed;
            QVERIFY2(parse_literal(golden.bytes(), golden, parsed, error),
                     qPrintable(QString("%1: %2").arg(golden.id).arg(error)));
            QCOMPARE(golden.byte_length, specified_length(golden, parsed));
            QCOMPARE(parsed.version, static_cast<uint16_t>(1));
        }
    }

    /// Retention: the retained bytes decode, and re-encode to exactly themselves.
    void retained_goldens_decode_and_re_encode_identically() {
        std::vector<Golden> goldens;
        QString error;
        QVERIFY2(load_goldens(goldens, error), qPrintable(error));
        for (const Golden &golden : goldens) {
            const std::string bytes = golden.bytes();
            QCOMPARE(bytes.substr(0, 4), std::string("CTPT"));
            auto decoded = decode_position(bytes);
            QVERIFY2(decoded.ok(), qPrintable(golden.id));
            QCOMPARE(checked_codec(encode_position(decoded.value())), bytes);

            // The decoded token is the *declared* kind, not merely some token.
            QCOMPARE(golden.kind == "TextBoundary", std::holds_alternative<TextBoundary>(decoded.value()));
            QCOMPARE(golden.kind == "TextAttachment", std::holds_alternative<TextAttachment>(decoded.value()));
            QCOMPARE(golden.kind == "ListBoundary", std::holds_alternative<ListBoundary>(decoded.value()));
            QCOMPARE(golden.kind == "ListElement", std::holds_alternative<ListElement>(decoded.value()));
        }
    }

    /// The version digits are the version. A newer one is an explicit
    /// unsupported-version error, never a malformed read, so a consumer can tell
    /// "written by a later release" from "corrupt".
    void a_different_version_is_unsupported_rather_than_malformed() {
        std::vector<Golden> goldens;
        QString error;
        QVERIFY2(load_goldens(goldens, error), qPrintable(error));
        for (const Golden &golden : goldens) {
            std::string bytes = golden.bytes();
            bytes[4] = 0x00;
            bytes[5] = 0x02;
            auto decoded = decode_position(bytes);
            QVERIFY(!decoded.ok());
            QCOMPARE(decoded.error().code(), ErrorCode::unsupported_version);
        }
    }

    /// Exhaustive negative coverage over every retained golden.
    void every_golden_rejects_all_truncations_and_all_trailing_bytes() {
        std::vector<Golden> goldens;
        QString error;
        QVERIFY2(load_goldens(goldens, error), qPrintable(error));
        for (const Golden &golden : goldens) {
            const std::string bytes = golden.bytes();
            for (size_t n = 0; n < bytes.size(); ++n)
                QVERIFY2(!decode_position(std::string_view(bytes).substr(0, n)).ok(), qPrintable(golden.id));
            for (unsigned extra = 0; extra < 256; ++extra) {
                auto padded = decode_position(bytes + std::string(1, static_cast<char>(extra)));
                QVERIFY(!padded.ok());
                QCOMPARE(padded.error().code(), ErrorCode::malformed);
            }
        }
    }

    /// A typed read of another kind is refused, not converted.
    void a_typed_read_of_another_kind_is_refused() {
        std::vector<Golden> goldens;
        QString error;
        QVERIFY2(load_goldens(goldens, error), qPrintable(error));
        for (const Golden &golden : goldens) {
            const std::string bytes = golden.bytes();
            if (golden.kind == "TextBoundary") {
                auto wrong = decode_position_as<ListBoundary>(bytes);
                QVERIFY(!wrong.ok());
                QCOMPARE(wrong.error().code(), ErrorCode::wrong_primitive);
            } else if (golden.kind == "ListElement") {
                QVERIFY(!decode_position_as<TextBoundary>(bytes).ok());
            }
        }
    }

    /// Meaning preserved: replaying a scenario on the current engines must mint
    /// exactly the retained bytes. This is what fails if a future placement
    /// change alters a supported format's bytes.
    void replayed_scenarios_still_mint_the_retained_bytes() {
        std::vector<Golden> goldens;
        QString error;
        QVERIFY2(load_goldens(goldens, error), qPrintable(error));
        for (const Golden &golden : goldens) {
            auto minted = mint(golden.id);
            QVERIFY2(minted.has_value(),
                     qPrintable(QString("no replay scenario for %1").arg(golden.id)));
            QCOMPARE(minted.value(), golden.bytes());
        }
    }
};

QTEST_APPLESS_MAIN(TestPositionTokenReader)
#include "tst_position_token_reader.moc"
