#include "storage/CheckpointCodec.h"
#include "crdt/Buffer.h"
#include "crdt/IdList.h"
#include <collabtext/CrdtEngine.h>
#include "TestActor.h"
#include <collabtext/Serialization.h>

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
    auto engine_section = encode_checkpoint_engine(CheckpointEngineState{});
    auto causal_section = encode_checkpoint_causal(causal);
    auto deferred_section = encode_checkpoint_deferred({}, context, causal.version);
    auto coverage_section = encode_checkpoint_coverage({});
    CheckpointShareableCore::Pins empty_pins;
    empty_pins.canonical_projection_hash = sha256_hex("");
    auto pins_section = encode_checkpoint_pins(empty_pins, context);
    QVERIFY(context_section);
    QVERIFY(engine_section);
    QVERIFY(causal_section);
    QVERIFY(deferred_section);
    QVERIFY(coverage_section);
    QVERIFY(pins_section);

    auto sections = shared_sections();
    sections[0] = context_section.value();
    sections[1] = engine_section.value();
    sections[2] = causal_section.value();
    sections[3] = deferred_section.value();
    sections[4] = coverage_section.value();
    sections[5] = pins_section.value();
    const auto framed = encode_checkpoint(CheckpointKind::Shareable, sections);
    QVERIFY(framed);
    const auto artifact = decode_checkpoint(framed.value(), CheckpointKind::Shareable);
    QVERIFY(artifact);
    const auto decoded_context = decode_checkpoint_context(artifact.value().sections[0]);
    const auto decoded_causal = decode_checkpoint_causal(artifact.value().sections[2]);
    const auto core = decode_checkpoint_core(artifact.value());
    QVERIFY(decoded_context);
    QVERIFY(decoded_causal);
    QVERIFY(core);
    QCOMPARE(decoded_context.value(), context);
    QCOMPARE(decoded_causal.value().version, causal.version);
    QVERIFY(decoded_causal.value().undo_entries.empty());

    auto wrong_context = encode_checkpoint_context(
        CheckpointContext{"project", "main", 7, "",
                          CheckpointEngineKind::IdList});
    QVERIFY(wrong_context);
    sections[0] = wrong_context.value();
    const auto mismatched_frame = encode_checkpoint(CheckpointKind::Shareable, sections);
    QVERIFY(mismatched_frame);
    const auto mismatched_artifact = decode_checkpoint(
        mismatched_frame.value(), CheckpointKind::Shareable);
    QVERIFY(mismatched_artifact);
    QVERIFY(!decode_checkpoint_core(mismatched_artifact.value()));
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
    CheckpointSection huge_count{2, 1, std::string("\0\1\0\x0f\x42\x40", 6)};
    QVERIFY(!decode_checkpoint_engine(huge_count));
    text.entries.push_back(list.entries[0]);
    QVERIFY(!encode_checkpoint_engine(text));
  }

  void enginePayloadsRestoreIntoBothPrimitives() {
    Crdt::Buffer source_text(CollabText::Test::test_counter(1));
    QVERIFY(source_text.try_apply_local_edit({{0, 0}}, {"A\xf0\x9f\x99\x82" "B"}));
    QVERIFY(source_text.try_apply_local_edit({{1, 5}}, {""}));
    auto text_snapshot = source_text.checkpoint_state();
    CheckpointEngineState text_state;
    text_state.fragments = text_snapshot.fragments;
    auto text_payload = encode_checkpoint_engine(text_state);
    QVERIFY(text_payload);
    CheckpointContext text_context{"doc", "main", 1, "",
                                   CheckpointEngineKind::Text};
    CheckpointCausalState text_causal{text_snapshot.version,
                                      text_snapshot.undo_entries};
    auto context_section = encode_checkpoint_context(text_context);
    auto causal_section = encode_checkpoint_causal(text_causal);
    auto deferred_section = encode_checkpoint_deferred(
        {}, text_context, text_causal.version);
    auto coverage_section = encode_checkpoint_coverage({});
    CheckpointShareableCore::Pins empty_pins;
    empty_pins.canonical_projection_hash = sha256_hex("");
    auto pins_section = encode_checkpoint_pins(empty_pins, text_context);
    QVERIFY(context_section);
    QVERIFY(causal_section);
    QVERIFY(deferred_section);
    QVERIFY(coverage_section);
    QVERIFY(pins_section);
    auto sections = shared_sections();
    sections[0] = context_section.value();
    sections[1] = text_payload.value();
    sections[2] = causal_section.value();
    sections[3] = deferred_section.value();
    sections[4] = coverage_section.value();
    sections[5] = pins_section.value();
    auto framed = encode_checkpoint(CheckpointKind::Shareable, sections);
    QVERIFY(framed);
    auto artifact = decode_checkpoint(framed.value(), CheckpointKind::Shareable);
    QVERIFY(artifact);
    auto decoded_core = decode_checkpoint_core(artifact.value());
    QVERIFY(decoded_core);
    text_snapshot.version = decoded_core.value().causal.version;
    text_snapshot.undo_entries = decoded_core.value().causal.undo_entries;
    text_snapshot.fragments = std::move(decoded_core.value().engine.fragments);
    Crdt::Buffer restored_text(CollabText::Test::test_counter(2));
    QVERIFY(restored_text.restore_checkpoint_state(text_snapshot));
    QCOMPARE(restored_text.text(), source_text.text());
    QCOMPARE(restored_text.fragments().size(), source_text.fragments().size());
    for (uint32_t offset : {0u, 1u, 2u})
      QCOMPARE(restored_text.resolve_anchor(source_text.anchor_at(offset)),
               source_text.resolve_anchor(source_text.anchor_at(offset)));

    Crdt::IdList source_list(CollabText::Test::test_counter(3));
    source_list.insert_after(Crdt::Anchor::min(), 10);
    source_list.insert_after(source_list.anchor_of(10), 20);
    QVERIFY(source_list.try_remove_at(source_list.anchor_of(10)));
    auto list_snapshot = source_list.checkpoint_state();
    CheckpointEngineState list_state;
    list_state.kind = CheckpointEngineKind::IdList;
    list_state.entries = list_snapshot.entries;
    auto list_payload = encode_checkpoint_engine(list_state);
    QVERIFY(list_payload);
    CheckpointContext list_context{"doc", "main", 1, "",
                                   CheckpointEngineKind::IdList};
    CheckpointCausalState list_causal{list_snapshot.version,
                                      list_snapshot.undo_entries};
    context_section = encode_checkpoint_context(list_context);
    causal_section = encode_checkpoint_causal(list_causal);
    deferred_section = encode_checkpoint_deferred(
        {}, list_context, list_causal.version);
    coverage_section = encode_checkpoint_coverage({});
    pins_section = encode_checkpoint_pins(empty_pins, list_context);
    QVERIFY(context_section);
    QVERIFY(causal_section);
    QVERIFY(deferred_section);
    QVERIFY(coverage_section);
    QVERIFY(pins_section);
    sections = shared_sections();
    sections[0] = context_section.value();
    sections[1] = list_payload.value();
    sections[2] = causal_section.value();
    sections[3] = deferred_section.value();
    sections[4] = coverage_section.value();
    sections[5] = pins_section.value();
    framed = encode_checkpoint(CheckpointKind::Shareable, sections);
    QVERIFY(framed);
    artifact = decode_checkpoint(framed.value(), CheckpointKind::Shareable);
    QVERIFY(artifact);
    decoded_core = decode_checkpoint_core(artifact.value());
    QVERIFY(decoded_core);
    list_snapshot.version = decoded_core.value().causal.version;
    list_snapshot.undo_entries = decoded_core.value().causal.undo_entries;
    list_snapshot.entries = std::move(decoded_core.value().engine.entries);
    Crdt::IdList restored_list(CollabText::Test::test_counter(4));
    QVERIFY(restored_list.restore_checkpoint_state(list_snapshot));
    QCOMPARE(restored_list.ids(), source_list.ids());
    QCOMPARE(restored_list.entries().size(), source_list.entries().size());
    QCOMPARE(restored_list.resolve_anchor(source_list.anchor_of(20)),
             source_list.resolve_anchor(source_list.anchor_of(20)));
  }

  void deferredSectionPreservesDeliveryAndOpaqueQuarantine() {
    Crdt::Buffer producer(CollabText::Test::test_counter(1));
    auto first = producer.try_apply_local_edit({{0, 0}}, {"A"});
    QVERIFY(first);
    auto second = producer.try_apply_local_edit({{1, 1}}, {"B"});
    QVERIFY(second);
    const auto first_bytes_result = Crdt::encode_operation(first.value());
    const auto second_bytes_result = Crdt::encode_operation(second.value());
    QVERIFY(first_bytes_result);
    QVERIFY(second_bytes_result);
    const auto first_bytes = first_bytes_result.value();
    const auto second_bytes = second_bytes_result.value();
    CheckpointContext context{"doc", "main", 9, "",
                              CheckpointEngineKind::Text};
    CheckpointShareableCore::DeferredRecord waiting{
        DeliveryToken{"main", 9, CollabText::Test::actor(1), 2},
        second_bytes, false, {}};
    CheckpointShareableCore::DeferredRecord rejected{
        DeliveryToken{"main", 9, CollabText::Test::actor(2), 4},
        "opaque rejected envelope", true, "invalid payload"};
    const std::vector<CheckpointShareableCore::DeferredRecord> records{
        waiting, rejected};
    Crdt::Global applied;
    auto encoded = encode_checkpoint_deferred(records, context, applied);
    QVERIFY(encoded);
    auto decoded = decode_checkpoint_deferred(encoded.value(), context, applied);
    QVERIFY(decoded);
    QCOMPARE(decoded.value().size(), size_t{2});
    QCOMPARE(decoded.value()[0], waiting);
    QCOMPARE(decoded.value()[1], rejected);
    QVERIFY(decoded.value()[0].operation_bytes == second_bytes);
    QVERIFY(decoded.value()[0].operation_bytes != first_bytes);
    for (size_t length = 0; length < encoded.value().bytes.size(); ++length) {
      auto truncated = encoded.value();
      truncated.bytes.resize(length);
      QVERIFY2(!decode_checkpoint_deferred(truncated, context, applied),
               qPrintable(QString::number(length)));
    }

    auto wrong_producer = waiting;
    wrong_producer.token.producer = CollabText::Test::actor(3);
    QVERIFY(!encode_checkpoint_deferred({wrong_producer}, context, applied));
    auto duplicate_token = std::vector<CheckpointShareableCore::DeferredRecord>{waiting,
                                                                                waiting};
    QVERIFY(!encode_checkpoint_deferred(duplicate_token, context, applied));
    auto missing_reason = rejected;
    missing_reason.reason.clear();
    QVERIFY(!encode_checkpoint_deferred({missing_reason}, context, applied));
    auto malformed = encoded.value();
    malformed.bytes.push_back('\0');
    QVERIFY(!decode_checkpoint_deferred(malformed, context, applied));
    auto already_applied = applied;
    already_applied.observe(Crdt::get_op_timestamp(second.value()));
    QVERIFY(!decode_checkpoint_deferred(encoded.value(), context, already_applied));

    Crdt::IdList list_producer(CollabText::Test::test_counter(5));
    auto list_first = list_producer.try_insert_after(Crdt::Anchor::min(), 10);
    QVERIFY(list_first);
    auto list_second = list_producer.try_insert_after(
        list_producer.anchor_of(10), 20);
    QVERIFY(list_second);
    auto list_bytes = Crdt::encode_idlist_operation(list_second.value());
    QVERIFY(list_bytes);
    CheckpointContext list_context{"doc", "main", 9, "",
                                  CheckpointEngineKind::IdList};
    CheckpointShareableCore::DeferredRecord list_waiting{
        DeliveryToken{"main", 9, CollabText::Test::actor(5), 8},
        list_bytes.value(), false, {}};
    auto list_encoded = encode_checkpoint_deferred({list_waiting}, list_context,
                                                   Crdt::Global{});
    QVERIFY(list_encoded);
    auto list_decoded = decode_checkpoint_deferred(
        list_encoded.value(), list_context, Crdt::Global{});
    QVERIFY(list_decoded);
    QCOMPARE(list_decoded.value(),
             (std::vector<CheckpointShareableCore::DeferredRecord>{list_waiting}));
  }

  void coverageSectionBindsExactMembershipFrontiers() {
    const auto actor_a = CollabText::Test::actor(1);
    const auto actor_b = CollabText::Test::actor(2);
    CheckpointShareableCore::Coverage coverage{
        {{actor_a, 7}, {actor_b, 0}}, {{actor_a, 11}, {actor_b, 0}}};
    auto encoded = encode_checkpoint_coverage(coverage);
    QVERIFY(encoded);
    auto decoded = decode_checkpoint_coverage(encoded.value());
    QVERIFY(decoded);
    QCOMPARE(decoded.value(), coverage);
    for (size_t length = 0; length < encoded.value().bytes.size(); ++length) {
      auto truncated = encoded.value();
      truncated.bytes.resize(length);
      QVERIFY2(!decode_checkpoint_coverage(truncated),
               qPrintable(QString::number(length)));
    }

    auto duplicate_actor = encoded.value();
    std::copy(duplicate_actor.bytes.begin() + 4,
              duplicate_actor.bytes.begin() + 20,
              duplicate_actor.bytes.begin() + 28);
    QVERIFY(!decode_checkpoint_coverage(duplicate_actor));

    MembershipDescriptor descriptor;
    descriptor.epoch = 1;
    descriptor.members = {actor_a, actor_b};
    descriptor.base_checkpoint_hash = std::string(64, 'a');
    descriptor.translation_manifest_hash = std::string(64, 'b');
    descriptor.base_frontiers = {{"main", coverage.operation, coverage.delivery}};
    descriptor.supported_token_versions = {1};
    auto sealed = seal_membership_descriptor(descriptor);
    QVERIFY(sealed);
    CheckpointContext context{"doc", "main", 1, "",
                              CheckpointEngineKind::Text};
    QVERIFY(validate_checkpoint_coverage(context, coverage, sealed.value()));

    auto mismatch = coverage;
    mismatch.delivery[actor_a]++;
    QVERIFY(!validate_checkpoint_coverage(context, mismatch, sealed.value()));
    context.epoch = 2;
    const auto wrong_epoch = validate_checkpoint_coverage(
        context, coverage, sealed.value());
    QVERIFY(!wrong_epoch);
    QCOMPARE(wrong_epoch.error().code(), ErrorCode::wrong_epoch);
  }

  void pinsSectionPreservesCompleteAndIncompleteProjection() {
    const PositionScope scope{"doc-pin", "stream-pin", Primitive::text, 7};
    auto engine = CrdtEngine::create(CollabText::Test::test_counter(1), scope);
    QVERIFY(engine);
    auto boundary = engine.value()->boundary_at(0, Affinity::left);
    QVERIFY(boundary);
    auto token = encode_position(PositionToken{boundary.value()});
    QVERIFY(token);

    CheckpointContext context{"doc-pin", "stream-pin", 7, "",
                              CheckpointEngineKind::Text};
    CheckpointShareableCore::Pins pins;
    pins.projected_frontier = 2;
    pins.canonical_frontier = 2;
    pins.canonical_projection_hash = std::string(64, 'c');
    pins.live_pins.push_back({"reply-1", scope, token.value()});
    auto encoded = encode_checkpoint_pins(pins, context);
    QVERIFY(encoded);
    auto decoded = decode_checkpoint_pins(encoded.value(), context);
    QVERIFY(decoded);
    QCOMPARE(decoded.value().projected_frontier, uint64_t{2});
    QCOMPARE(decoded.value().canonical_frontier, uint64_t{2});
    QVERIFY(decoded.value().complete);
    QCOMPARE(decoded.value().canonical_projection_hash,
             pins.canonical_projection_hash);
    QCOMPARE(decoded.value().live_pins.size(), size_t{1});
    QCOMPARE(decoded.value().live_pins[0].reference_id, std::string("reply-1"));
    QCOMPARE(decoded.value().live_pins[0].token_bytes, token.value());
    for (size_t length = 0; length < encoded.value().bytes.size(); ++length) {
      auto truncated = encoded.value();
      truncated.bytes.resize(length);
      QVERIFY2(!decode_checkpoint_pins(truncated, context),
               qPrintable(QString::number(length)));
    }

    auto incomplete = pins;
    incomplete.projected_frontier = 1;
    incomplete.complete = false;
    auto partial = encode_checkpoint_pins(incomplete, context);
    QVERIFY(partial);
    auto decoded_partial = decode_checkpoint_pins(partial.value(), context);
    QVERIFY(decoded_partial);
    QVERIFY(!decoded_partial.value().complete);
    QCOMPARE(decoded_partial.value().projected_frontier, uint64_t{1});

    auto bad_scope = pins;
    bad_scope.live_pins[0].scope.epoch++;
    QVERIFY(!encode_checkpoint_pins(bad_scope, context));
    auto bad_token = pins;
    bad_token.live_pins[0].token_bytes[0] ^= 1;
    QVERIFY(!encode_checkpoint_pins(bad_token, context));
    auto inconsistent = pins;
    inconsistent.complete = false;
    QVERIFY(!encode_checkpoint_pins(inconsistent, context));
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
