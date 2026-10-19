#include <QTest>
#include <collabtext/AnchorPins.h>
#include <collabtext/CrdtEngine.h>
#include <collabtext/Operations.h>
#include <collabtext/Serialization.h>
#include "TestActor.h"
#include <thread>

using namespace CollabText;

class TestAnchorRetention : public QObject {
  Q_OBJECT
private slots:
  void durable_projection_recovery_and_release_are_idempotent() {
    const PositionScope scope{"doc-pin", "stream-pin", Primitive::text, 7};
    auto engine = CrdtEngine::create(Test::test_counter(1), scope);
    QVERIFY(engine.ok());
    auto boundary = engine.value()->boundary_at(0, Affinity::left);
    QVERIFY(boundary.ok());
    auto bytes = encode_position(PositionToken{boundary.value()});
    QVERIFY(bytes.ok());

    std::vector<AnchorPinEvent> canonical;
    auto opened = AnchorPinRegistry::create(scope, [&](const AnchorPinEvent& event) {
      canonical.push_back(event); // represents a durable consumer append
      return Result<void>{};
    });
    QVERIFY(opened.ok());
    auto pins = std::move(opened).value();
    QVERIFY(!pins->recovery_complete(1));
    QVERIFY(pins->register_pin(1, "e1", "reply-1", bytes.value()).ok());
    QCOMPARE(pins->projected_frontier(), uint64_t(1));
    QVERIFY(pins->recovery_complete(1));
    QCOMPARE(pins->live_pins().size(), size_t(1));
    auto found_pin = pins->lookup_pin("reply-1");
    QVERIFY(found_pin.ok());
    QCOMPARE(found_pin.value().token_bytes, bytes.value());

    // Rebuild from the canonical consumer log after process loss.
    bool replay_persisted = false;
    auto recovered = AnchorPinRegistry::create(scope, [&](const AnchorPinEvent&) {
      replay_persisted = true;
      return Result<void>(Error(ErrorCode::io_error, "unexpected replay append"));
    });
    QVERIFY(recovered.ok());
    auto restored = std::move(recovered).value();
    QVERIFY(restored->replay(canonical[0]).ok());
    QVERIFY(restored->replay(canonical[0]).ok());
    QVERIFY(!replay_persisted);
    QCOMPARE(restored->live_pins().size(), size_t(1));

    QVERIFY(pins->release_pin(2, "e2", "reply-1").ok());
    QVERIFY(pins->replay(canonical[1]).ok());
    QVERIFY(pins->live_pins().empty());
    auto released_lookup = pins->lookup_pin("reply-1");
    QVERIFY(!released_lookup.ok());
    QCOMPARE(released_lookup.error().code(), ErrorCode::not_found);
    auto after_release = AnchorPinRegistry::create(scope, [](const AnchorPinEvent&) {
      return Result<void>(Error(ErrorCode::io_error, "release replay must not append"));
    });
    QVERIFY(after_release.ok());
    auto released_projection = std::move(after_release).value();
    QVERIFY(released_projection->replay(canonical[0]).ok());
    QVERIFY(released_projection->replay(canonical[1]).ok());
    QVERIFY(released_projection->recovery_complete(2));
    QVERIFY(released_projection->live_pins().empty());
    auto reuse = pins->register_pin(3, "e3", "reply-1", bytes.value());
    QVERIFY(!reuse.ok());
    QCOMPARE(reuse.error().code(), ErrorCode::pin_conflict);
    QCOMPARE(canonical.size(), size_t(2));
  }

  void gaps_wrong_scope_and_failed_durable_append_do_not_advance() {
    const PositionScope scope{"doc-pin", "stream-pin", Primitive::text, 7};
    auto engine = CrdtEngine::create(Test::test_counter(1), scope);
    QVERIFY(engine.ok());
    auto token = engine.value()->boundary_at(0, Affinity::right);
    QVERIFY(token.ok());
    auto bytes = encode_position(PositionToken{token.value()});
    QVERIFY(bytes.ok());

    bool fail_write = true;
    std::vector<AnchorPinEvent> canonical;
    auto opened = AnchorPinRegistry::create(scope, [&](const AnchorPinEvent& event) {
      if (fail_write) return Result<void>(Error(ErrorCode::io_error, "injected append failure"));
      canonical.push_back(event);
      return Result<void>{};
    });
    QVERIFY(opened.ok());
    auto pins = std::move(opened).value();
    auto failed = pins->register_pin(1, "e1", "reply-1", bytes.value());
    QVERIFY(!failed.ok());
    QCOMPARE(failed.error().code(), ErrorCode::io_error);
    QCOMPARE(pins->projected_frontier(), uint64_t(0));
    QVERIFY(pins->live_pins().empty());

    fail_write = false;
    auto gap = pins->register_pin(2, "e2", "reply-1", bytes.value());
    QVERIFY(!gap.ok());
    QCOMPARE(gap.error().code(), ErrorCode::awaiting_state);

    auto foreign = CrdtEngine::create(Test::test_counter(2),
        PositionScope{"another-doc", "stream-pin", Primitive::text, 7});
    QVERIFY(foreign.ok());
    auto foreign_token = foreign.value()->boundary_at(0, Affinity::left);
    QVERIFY(foreign_token.ok());
    auto foreign_bytes = encode_position(PositionToken{foreign_token.value()});
    QVERIFY(foreign_bytes.ok());
    auto rejected = pins->register_pin(1, "e1", "reply-1", foreign_bytes.value());
    QVERIFY(!rejected.ok());
    QCOMPARE(rejected.error().code(), ErrorCode::wrong_document);
    auto wrong_epoch = validate_position_scope(
        bytes.value(), PositionScope{"doc-pin", "stream-pin", Primitive::text, 8});
    QVERIFY(!wrong_epoch.ok());
    QCOMPARE(wrong_epoch.error().code(), ErrorCode::wrong_epoch);
    auto wrong_stream = validate_position_scope(
        bytes.value(), PositionScope{"doc-pin", "other-stream", Primitive::text, 7});
    QVERIFY(!wrong_stream.ok());
    QCOMPARE(wrong_stream.error().code(), ErrorCode::wrong_stream);
    auto list_scope = validate_position_scope(
        bytes.value(), PositionScope{"doc-pin", "stream-pin", Primitive::list, 7});
    QVERIFY(!list_scope.ok());
    QCOMPARE(list_scope.error().code(), ErrorCode::wrong_primitive);
    QVERIFY(canonical.empty());

    QVERIFY(pins->register_pin(1, "e1", "reply-1", bytes.value()).ok());
    QCOMPARE(canonical.size(), size_t(1));
  }

  void replay_requires_contiguous_frontier_and_release_tombstone_wins() {
    const PositionScope scope{"doc-pin", "stream-pin", Primitive::text, 7};
    auto engine = CrdtEngine::create(Test::test_counter(1), scope);
    QVERIFY(engine.ok());
    auto token = engine.value()->boundary_at(0, Affinity::left);
    QVERIFY(token.ok());
    auto bytes = encode_position(PositionToken{token.value()});
    QVERIFY(bytes.ok());
    auto opened = AnchorPinRegistry::create(scope, [](const AnchorPinEvent&) {
      return Result<void>{};
    });
    QVERIFY(opened.ok());
    auto pins = std::move(opened).value();
    AnchorPinEvent release{1, "release-first", AnchorPinAction::release_pin,
                           "ref-x", scope, {}};
    QVERIFY(pins->replay(release).ok());
    AnchorPinEvent registration{2, "late-register", AnchorPinAction::register_pin,
                                "ref-x", scope, bytes.value()};
    auto late = pins->replay(registration);
    QVERIFY(!late.ok());
    QCOMPARE(late.error().code(), ErrorCode::pin_conflict);
    QCOMPARE(pins->projected_frontier(), uint64_t(1));
    QVERIFY(pins->live_pins().empty());

    auto gap = registration;
    gap.sequence = 3;
    auto refused = pins->replay(gap);
    QVERIFY(!refused.ok());
    QCOMPARE(refused.error().code(), ErrorCode::awaiting_state);
  }

  void concurrent_retry_of_same_event_persists_once() {
    const PositionScope scope{"doc-pin", "stream-pin", Primitive::text, 7};
    auto engine = CrdtEngine::create(Test::test_counter(1), scope);
    QVERIFY(engine.ok());
    auto token = engine.value()->boundary_at(0, Affinity::left);
    QVERIFY(token.ok());
    auto bytes = encode_position(PositionToken{token.value()});
    QVERIFY(bytes.ok());
    std::vector<AnchorPinEvent> canonical;
    auto opened = AnchorPinRegistry::create(scope, [&](const AnchorPinEvent& event) {
      canonical.push_back(event);
      return Result<void>{};
    });
    QVERIFY(opened.ok());
    auto pins = std::move(opened).value();
    bool first_ok = false, second_ok = false;
    std::thread first([&] {
      first_ok = pins->register_pin(1, "e1", "reply-1", bytes.value()).ok();
    });
    std::thread second([&] {
      second_ok = pins->register_pin(1, "e1", "reply-1", bytes.value()).ok();
    });
    first.join();
    second.join();
    QVERIFY(first_ok);
    QVERIFY(second_ok);
    QCOMPARE(canonical.size(), size_t(1));
    QCOMPARE(pins->live_pins().size(), size_t(1));
  }

  void uncertain_canonical_append_recovers_pin_before_collection_gate() {
    const PositionScope scope{"doc-pin", "stream-pin", Primitive::text, 7};
    auto engine = CrdtEngine::create(Test::test_counter(1), scope);
    QVERIFY(engine.ok());
    auto token = engine.value()->boundary_at(0, Affinity::left);
    QVERIFY(token.ok());
    auto bytes = encode_position(PositionToken{token.value()});
    QVERIFY(bytes.ok());
    std::vector<AnchorPinEvent> canonical;
    auto opened = AnchorPinRegistry::create(scope, [&](const AnchorPinEvent& event) {
      canonical.push_back(event); // publication happened, but durability is uncertain
      return Result<void>(Error(ErrorCode::commit_uncertain, "barrier outcome unknown"));
    });
    QVERIFY(opened.ok());
    auto before_crash = std::move(opened).value();
    auto uncertain = before_crash->register_pin(1, "event-1", "reply-1", bytes.value());
    QVERIFY(!uncertain.ok());
    QCOMPARE(uncertain.error().code(), ErrorCode::commit_uncertain);
    QVERIFY(!before_crash->recovery_complete(1));

    auto recovered = AnchorPinRegistry::create(scope, [](const AnchorPinEvent&) {
      return Result<void>(Error(ErrorCode::io_error, "replay must not append"));
    });
    QVERIFY(recovered.ok());
    auto projection = std::move(recovered).value();
    QVERIFY(projection->replay(canonical.front()).ok());
    QVERIFY(projection->recovery_complete(1));
    QCOMPARE(projection->live_pins().size(), size_t(1));
    QCOMPARE(projection->live_pins().front().token_bytes, bytes.value());
  }

  void durable_comment_token_survives_deleted_text_undo_and_author_retirement() {
    const PositionScope scope{"doc-comment", "body", Primitive::text, 2};
    std::vector<std::string> operations;
    auto author = CrdtEngine::create(Test::test_counter(1), scope);
    QVERIFY(author.ok());
    author.value()->setOnLocalOp([&](const Crdt::Operation& op) {
      auto encoded = Crdt::encode_operation(op);
      if (encoded.ok()) operations.push_back(std::move(encoded).value());
    });
    author.value()->insert(0, "abc");
    auto attachment = author.value()->attachment_at(1, Affinity::left,
                                                    ReferentSide::following);
    QVERIFY(attachment.ok());
    auto token_bytes = encode_position(PositionToken{attachment.value()});
    QVERIFY(token_bytes.ok());
    std::vector<AnchorPinEvent> canonical;
    auto registry = AnchorPinRegistry::create(scope, [&](const AnchorPinEvent& event) {
      canonical.push_back(event);
      return Result<void>{};
    });
    QVERIFY(registry.ok());
    auto pins = std::move(registry).value();
    QVERIFY(pins->register_pin(1, "comment-create", "comment-1",
                               token_bytes.value()).ok());

    author.value()->remove(1, 1);
    auto detached = author.value()->resolve(attachment.value());
    QVERIFY(!detached.ok());
    QCOMPARE(detached.error().code(), ErrorCode::detached);
    QVERIFY(author.value()->undo());
    auto restored = author.value()->resolve(attachment.value());
    QVERIFY(restored.ok());
    QCOMPARE(restored.value().byte_offset, uint32_t(1));

    // Device retirement destroys its engine/allocator. The consumer-owned pin
    // remains live and the original bytes resolve in a fresh replica.
    author.value().reset();
    auto fresh = CrdtEngine::create(Test::test_counter(2), scope);
    QVERIFY(fresh.ok());
    for (const auto& wire : operations) {
      auto op = Crdt::decode_operation(wire);
      QVERIFY(op.ok());
      QVERIFY(CollabText::Test::apply_succeeded(fresh.value()->apply_remote(op.value())));
    }
    auto recovered = AnchorPinRegistry::create(scope, [](const AnchorPinEvent&) {
      return Result<void>(Error(ErrorCode::io_error, "replay must not append"));
    });
    QVERIFY(recovered.ok());
    auto recovered_pins = std::move(recovered).value();
    QVERIFY(recovered_pins->replay(canonical.front()).ok());
    auto live = recovered_pins->lookup_pin("comment-1");
    QVERIFY(live.ok());
    QCOMPARE(live.value().token_bytes, token_bytes.value());
    auto original = decode_position_as<TextAttachment>(live.value().token_bytes);
    QVERIFY(original.ok());
    auto resolved = fresh.value()->resolve(original.value());
    QVERIFY(resolved.ok());
    QCOMPARE(resolved.value().byte_offset, uint32_t(1));
  }
};

QTEST_GUILESS_MAIN(TestAnchorRetention)
#include "tst_anchor_retention.moc"
