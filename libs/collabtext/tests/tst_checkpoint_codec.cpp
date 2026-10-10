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
