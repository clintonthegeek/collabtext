#include "storage/CheckpointCodec.h"

#include "core/Sha256.h"

#include <QtTest>

using namespace CollabText;
using namespace CollabText::Storage;

namespace {

std::vector<CheckpointSection> shared_sections() {
  return {{1, 1, "ctx"}, {2, 1, "engine"}, {3, 1, "causal"},
          {4, 1, "deferred"}, {5, 1, "coverage"}, {6, 1, "pins"},
          {7, 1, "translations"}};
}

std::vector<CheckpointSection> local_sections() {
  return {{1, 1, "owner"}, {2, 1, "history"}, {3, 1, "writer"}};
}

std::string raw_digest(std::string_view input) {
  const auto hex = sha256_hex(input);
  std::string out;
  out.reserve(32);
  auto digit = [](char c) -> unsigned char {
    return static_cast<unsigned char>(c <= '9' ? c - '0' : c - 'a' + 10);
  };
  for (size_t i = 0; i < hex.size(); i += 2)
    out.push_back(static_cast<char>((digit(hex[i]) << 4) | digit(hex[i + 1])));
  return out;
}

void resign(std::string &frame) {
  frame.replace(frame.size() - 32, 32,
                raw_digest(std::string_view(frame).substr(0, frame.size() - 32)));
}

}  // namespace

class CheckpointCodecTest final : public QObject {
  Q_OBJECT

private slots:
  void roundTripsBothKinds() {
    for (const auto kind : {CheckpointKind::Shareable, CheckpointKind::Local}) {
      auto input = kind == CheckpointKind::Shareable ? shared_sections() : local_sections();
      auto encoded = encode_checkpoint(kind, input);
      QVERIFY(encoded);
      auto decoded = decode_checkpoint(encoded.value(), kind);
      QVERIFY(decoded);
      QCOMPARE(decoded.value().kind, kind);
      QCOMPARE(decoded.value().sections, input);
    }
  }

  void contextSectionRoundTripsAndValidates() {
    CheckpointContext context{"doc-雪", "comments", 12,
                              std::string(64, 'a'),
                              CheckpointEngineKind::IdList};
    auto encoded = encode_checkpoint_context(context);
    QVERIFY(encoded);
    QCOMPARE(encoded.value().id, uint16_t{1});
    QCOMPARE(encoded.value().version, uint16_t{1});
    auto decoded = decode_checkpoint_context(encoded.value());
    QVERIFY(decoded);
    QCOMPARE(decoded.value(), context);
    for (size_t length = 0; length < encoded.value().bytes.size(); ++length) {
      auto truncated = encoded.value();
      truncated.bytes.resize(length);
      QVERIFY2(!decode_checkpoint_context(truncated),
               qPrintable(QString::number(length)));
    }

    auto invalid = context;
    invalid.document.clear();
    QVERIFY(!encode_checkpoint_context(invalid));
    invalid = context;
    invalid.stream = std::string("bad\0stream", 10);
    QVERIFY(!encode_checkpoint_context(invalid));
    invalid = context;
    invalid.epoch = 0;
    QVERIFY(!encode_checkpoint_context(invalid));
    invalid = context;
    invalid.engine = static_cast<CheckpointEngineKind>(7);
    QVERIFY(!encode_checkpoint_context(invalid));
    invalid = context;
    invalid.parent_descriptor_hash = std::string(64, 'A');
    QVERIFY(!encode_checkpoint_context(invalid));

    auto bad_utf8 = encoded.value();
    bad_utf8.bytes[4] = static_cast<char>(0xff);
    auto failed = decode_checkpoint_context(bad_utf8);
    QVERIFY(!failed);

    auto bad_parent_flag = encoded.value();
    const size_t flag_at = 14 + context.document.size() + context.stream.size();
    bad_parent_flag.bytes[flag_at] = 2;
    QVERIFY(!decode_checkpoint_context(bad_parent_flag));

    auto trailing = encoded.value();
    trailing.bytes.push_back('x');
    QVERIFY(!decode_checkpoint_context(trailing));
  }

  void causalSectionRoundTripsAndRejectsMalformedState() {
    const auto actor_a = ActorId::parse("01000000000000000000000000000001").value();
    const auto actor_b = ActorId::parse("02000000000000000000000000000001").value();
    CheckpointCausalState state;
    state.version.observe(Crdt::Identity(actor_a, 9));
    state.version.observe(Crdt::Identity(actor_b, 4));
    state.undo_entries = {
        {Crdt::UndoTreeKey{Crdt::Identity(actor_a, 2), Crdt::Identity(actor_b, 7)}, 1},
        {Crdt::UndoTreeKey{Crdt::Identity(actor_a, 3), Crdt::Identity(actor_b, 8)}, 2}};
    auto encoded = encode_checkpoint_causal(state);
    QVERIFY(encoded);
    QCOMPARE(encoded.value().id, uint16_t{3});
    auto decoded = decode_checkpoint_causal(encoded.value());
    QVERIFY(decoded);
    QCOMPARE(decoded.value().version, state.version);
    QCOMPARE(decoded.value().undo_entries.size(), state.undo_entries.size());
    for (size_t i = 0; i < state.undo_entries.size(); ++i) {
      QCOMPARE(decoded.value().undo_entries[i].key.edit_id,
               state.undo_entries[i].key.edit_id);
      QCOMPARE(decoded.value().undo_entries[i].key.undo_id,
               state.undo_entries[i].key.undo_id);
      QCOMPARE(decoded.value().undo_entries[i].undo_count,
               state.undo_entries[i].undo_count);
    }
    for (size_t length = 0; length < encoded.value().bytes.size(); ++length) {
      auto truncated = encoded.value();
      truncated.bytes.resize(length);
      QVERIFY2(!decode_checkpoint_causal(truncated),
               qPrintable(QString::number(length)));
    }

    auto unordered = state;
    std::reverse(unordered.undo_entries.begin(), unordered.undo_entries.end());
    QVERIFY(!encode_checkpoint_causal(unordered));
    auto zero_count = state;
    zero_count.undo_entries[0].undo_count = 0;
    QVERIFY(!encode_checkpoint_causal(zero_count));
    auto trailing = encoded.value();
    trailing.bytes.push_back('\0');
    QVERIFY(!decode_checkpoint_causal(trailing));
    auto bad_identity = encoded.value();
    std::fill(bad_identity.bytes.begin() + 4, bad_identity.bytes.begin() + 20, '\0');
    QVERIFY(!decode_checkpoint_causal(bad_identity));
  }

  void typedSectionsRoundTripInsideCompleteArtifact() {
    CheckpointContext context{"project", "main", 7, "",
                              CheckpointEngineKind::Text};
    CheckpointCausalState causal;
    auto context_section = encode_checkpoint_context(context);
    auto causal_section = encode_checkpoint_causal(causal);
    QVERIFY(context_section);
    QVERIFY(causal_section);

    auto sections = shared_sections();
    sections[0] = context_section.value();
    sections[2] = causal_section.value();
    const auto framed = encode_checkpoint(CheckpointKind::Shareable, sections);
    QVERIFY(framed);
    const auto artifact = decode_checkpoint(framed.value(), CheckpointKind::Shareable);
    QVERIFY(artifact);
    const auto decoded_context = decode_checkpoint_context(artifact.value().sections[0]);
    const auto decoded_causal = decode_checkpoint_causal(artifact.value().sections[2]);
    QVERIFY(decoded_context);
    QVERIFY(decoded_causal);
    QCOMPARE(decoded_context.value(), context);
    QCOMPARE(decoded_causal.value().version, causal.version);
    QVERIFY(decoded_causal.value().undo_entries.empty());
  }

  void engineSectionRoundTripsTextAndIdList() {
    const auto actor = ActorId::parse("11000000000000000000000000000001").value();
    CheckpointEngineState text;
    Crdt::Fragment fragment{Crdt::Identity(actor, 1), {}, 3, 1, "雪"};
    text.fragments.push_back(fragment);
    auto text_section = encode_checkpoint_engine(text);
    QVERIFY(text_section);
    auto text_decoded = decode_checkpoint_engine(text_section.value());
    QVERIFY(text_decoded);
    QCOMPARE(text_decoded.value().kind, CheckpointEngineKind::Text);
    QCOMPARE(text_decoded.value().fragments.size(), size_t{1});
    QCOMPARE(text_decoded.value().fragments[0].origin, fragment.origin);
    QCOMPARE(text_decoded.value().fragments[0].link, fragment.link);
    QCOMPARE(text_decoded.value().fragments[0].text, fragment.text);
    QCOMPARE(text_decoded.value().fragments[0].length, uint32_t{1});
    for (size_t length = 0; length < text_section.value().bytes.size(); ++length) {
      auto truncated = text_section.value();
      truncated.bytes.resize(length);
      QVERIFY2(!decode_checkpoint_engine(truncated),
               qPrintable(QString::number(length)));
    }

    CheckpointEngineState list;
    list.kind = CheckpointEngineKind::IdList;
    list.entries.emplace_back(Crdt::Identity(actor, 3), Crdt::Link{}, 0xfeedu);
    auto list_section = encode_checkpoint_engine(list);
    QVERIFY(list_section);
    auto list_decoded = decode_checkpoint_engine(list_section.value());
    QVERIFY(list_decoded);
    QCOMPARE(list_decoded.value().kind, CheckpointEngineKind::IdList);
    QCOMPARE(list_decoded.value().entries.size(), size_t{1});
    QCOMPARE(list_decoded.value().entries[0].origin, list.entries[0].origin);
    QCOMPARE(list_decoded.value().entries[0].id, uint64_t{0xfeedu});
    for (size_t length = 0; length < list_section.value().bytes.size(); ++length) {
      auto truncated = list_section.value();
      truncated.bytes.resize(length);
      QVERIFY2(!decode_checkpoint_engine(truncated),
               qPrintable(QString::number(length)));
    }

    auto invalid_utf8 = text_section.value();
    invalid_utf8.bytes.back() = static_cast<char>(0xff);
    QVERIFY(!decode_checkpoint_engine(invalid_utf8));
    auto trailing = list_section.value();
    trailing.bytes.push_back('\0');
    QVERIFY(!decode_checkpoint_engine(trailing));
    text.entries.push_back(list.entries[0]);
    QVERIFY(!encode_checkpoint_engine(text));
  }

  void rejectsEveryTruncation() {
    auto encoded = encode_checkpoint(CheckpointKind::Shareable, shared_sections());
    QVERIFY(encoded);
    for (size_t length = 0; length < encoded.value().size(); ++length) {
      auto decoded = decode_checkpoint(std::string_view(encoded.value()).substr(0, length),
                                       CheckpointKind::Shareable);
      QVERIFY2(!decoded, qPrintable(QString::number(length)));
    }
  }

  void rejectsWrongKindAndDigestTampering() {
    auto encoded = encode_checkpoint(CheckpointKind::Shareable, shared_sections());
    QVERIFY(encoded);
    QVERIFY(!decode_checkpoint(encoded.value(), CheckpointKind::Local));
    auto changed = encoded.value();
    changed[25] ^= 1;
    const auto result = decode_checkpoint(changed, CheckpointKind::Shareable);
    QVERIFY(!result);
    QCOMPARE(result.error().code(), ErrorCode::corrupt_record);
  }

  void rejectsMissingReorderedAndUnknownSections() {
    auto sections = shared_sections();
    sections.pop_back();
    QVERIFY(!encode_checkpoint(CheckpointKind::Shareable, sections));
    sections = shared_sections();
    std::swap(sections[1], sections[2]);
    QVERIFY(!encode_checkpoint(CheckpointKind::Shareable, sections));
    sections = shared_sections();
    sections[3].id = 99;
    QVERIFY(!encode_checkpoint(CheckpointKind::Shareable, sections));

    auto frame = encode_checkpoint(CheckpointKind::Shareable, shared_sections());
    QVERIFY(frame);
    auto unknown = frame.value();
    unknown[20] = 99;  // first section ID is at byte 19..20
    resign(unknown);
    const auto unknown_result = decode_checkpoint(unknown, CheckpointKind::Shareable);
    QVERIFY(!unknown_result);
    QCOMPARE(unknown_result.error().code(), ErrorCode::unsupported_version);

    auto reordered = frame.value();
    reordered[20] = 2;
    reordered[34] = 1;  // second section ID follows the first payload
    resign(reordered);
    const auto reordered_result = decode_checkpoint(reordered, CheckpointKind::Shareable);
    QVERIFY(!reordered_result);
    QCOMPARE(reordered_result.error().code(), ErrorCode::unsupported_version);
  }

  void rejectsUnsupportedVersionAfterIntegrityCheck() {
    auto encoded = encode_checkpoint(CheckpointKind::Shareable, shared_sections());
    QVERIFY(encoded);
    auto changed = encoded.value();
    // Frame (17), count (2), then section id. The following two bytes are v1.
    changed[21] = 0;
    changed[22] = 2;
    resign(changed);
    const auto result = decode_checkpoint(changed, CheckpointKind::Shareable);
    QVERIFY(!result);
    QCOMPARE(result.error().code(), ErrorCode::unsupported_version);
  }

  void rejectsImpossibleAndTrailingLengths() {
    auto encoded = encode_checkpoint(CheckpointKind::Shareable, shared_sections());
    QVERIFY(encoded);
    auto trailing = encoded.value() + "x";
    QVERIFY(!decode_checkpoint(trailing, CheckpointKind::Shareable));

    auto huge = encoded.value();
    for (size_t i = 9; i < 17; ++i) huge[i] = static_cast<char>(0xff);
    const auto result = decode_checkpoint(huge, CheckpointKind::Shareable);
    QVERIFY(!result);
    QCOMPARE(result.error().code(), ErrorCode::limit_exceeded);

    auto bad_section_length = encoded.value();
    for (size_t i = 23; i < 31; ++i)
      bad_section_length[i] = static_cast<char>(0xff);
    resign(bad_section_length);
    const auto section_result = decode_checkpoint(
        bad_section_length, CheckpointKind::Shareable);
    QVERIFY(!section_result);
    QCOMPARE(section_result.error().code(), ErrorCode::malformed);
  }
};

QTEST_GUILESS_MAIN(CheckpointCodecTest)
#include "tst_checkpoint_codec.moc"
