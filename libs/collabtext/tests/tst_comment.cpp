#include <QTest>
#include "crdt/Comment.h"
#include "crdt/Anchor.h"
#include "TestActor.h"
#include <collabtext/CrdtEngine.h>

using CollabText::Test::actor;

using namespace CollabText::Crdt;

class TestComment : public QObject {
    Q_OBJECT
private slots:
    void round_trip() {
        Comment comment;
        comment.id          = "1-42";
        comment.replica_id  = 1;
        comment.seq         = 42;
        comment.timestamp   = "2026-04-13T10:00:00Z";
        comment.author      = "alice@example.com";
        comment.author_name = "Alice";
        comment.body        = "This is a comment on the selected text.";
        comment.range_start = Anchor(Identity(actor(2), 100), Bias::Left);
        comment.range_end   = Anchor(Identity(actor(3), 200), Bias::Right);

        StreamEntry entry = comment_to_entry(comment);
        auto result       = comment_from_entry(entry);

        QVERIFY(result.has_value());
        QCOMPARE(result->id,                    comment.id);
        QCOMPARE(result->replica_id,            comment.replica_id);
        QCOMPARE(result->seq,                   comment.seq);
        QCOMPARE(result->timestamp,             comment.timestamp);
        QCOMPARE(result->author,                comment.author);
        QCOMPARE(result->author_name,           comment.author_name);
        QCOMPARE(result->body,                  comment.body);
        QCOMPARE(result->range_start.position.actor, actor(2));
        QCOMPARE(result->range_start.position.counter, quint64(100));
        QCOMPARE(result->range_start.bias,       Bias::Left);
        QCOMPARE(result->range_end.position.actor, actor(3));
        QCOMPARE(result->range_end.position.counter, quint64(200));
        QCOMPARE(result->range_end.bias,         Bias::Right);
    }

    void payload_contains_range() {
        Comment comment;
        comment.id          = "2-7";
        comment.replica_id  = 2;
        comment.seq         = 7;
        comment.timestamp   = "2026-04-13T11:00:00Z";
        comment.author      = "bob@example.com";
        comment.author_name = "Bob";
        comment.body        = "Some note";
        comment.range_start = Anchor(Identity(actor(1), 50), Bias::Left);
        comment.range_end   = Anchor(Identity(actor(1), 80), Bias::Right);

        StreamEntry entry = comment_to_entry(comment);

        QVERIFY(entry.payload.find("range") != std::string::npos);
        QVERIFY(entry.payload.find("start") != std::string::npos);
        QVERIFY(entry.payload.find("end")   != std::string::npos);
    }

    void missing_range_returns_nullopt() {
        StreamEntry entry;
        entry.id         = "3-1";
        entry.replica_id = 3;
        entry.seq        = 1;
        entry.timestamp  = "2026-04-13T12:00:00Z";
        // payload has author and body but no range
        entry.payload    = "{\"author\":\"carol@example.com\",\"author_name\":\"Carol\",\"body\":\"No range here\"}";

        auto result = comment_from_entry(entry);
        QVERIFY(!result.has_value());
    }

    void resolved_round_trip() {
        Comment comment;
        comment.id          = "4-1";
        comment.replica_id  = 4;
        comment.seq         = 1;
        comment.timestamp   = "2026-04-30T09:00:00Z";
        comment.author      = "dave@example.com";
        comment.author_name = "Dave";
        comment.body        = "Looks good now";
        comment.range_start = Anchor(Identity(actor(1), 10), Bias::Left);
        comment.range_end   = Anchor(Identity(actor(1), 20), Bias::Right);
        comment.resolved    = true;

        StreamEntry entry = comment_to_entry(comment);
        auto result       = comment_from_entry(entry);

        QVERIFY(result.has_value());
        QCOMPARE(result->resolved, true);
    }

    void resolved_defaults_false_when_missing() {
        StreamEntry entry;
        entry.id         = "5-1";
        entry.replica_id = 5;
        entry.seq        = 1;
        entry.timestamp  = "2026-04-30T10:00:00Z";
        entry.payload =
            "{\"author\":\"e@example.com\",\"author_name\":\"Eve\","
            "\"body\":\"old comment\","
            "\"range\":{\"start\":{\"r\":1,\"s\":5,\"b\":\"left\"},"
            "\"end\":{\"r\":1,\"s\":7,\"b\":\"right\"}}}";

        auto result = comment_from_entry(entry);
        QVERIFY(result.has_value());
        QCOMPARE(result->resolved, false);
    }

    void scoped_position_bytes_require_context_and_survive_causal_delay() {
        const CollabText::PositionScope scope{"doc-comments", "body", CollabText::Primitive::text, 3};
        auto engine = CollabText::CrdtEngine::create(CollabText::Test::test_counter(10), scope);
        QVERIFY(engine.ok());
        auto left = engine.value()->boundary_at(0, CollabText::Affinity::left);
        auto right = engine.value()->boundary_at(0, CollabText::Affinity::right);
        QVERIFY(left.ok()); QVERIFY(right.ok());
        auto left_bytes = CollabText::encode_position(CollabText::PositionToken{left.value()});
        auto right_bytes = CollabText::encode_position(CollabText::PositionToken{right.value()});
        QVERIFY(left_bytes.ok()); QVERIFY(right_bytes.ok());
        Comment comment;
        comment.id = "durable-1"; comment.author = "alice"; comment.body = "note";
        comment.range_start_token = left_bytes.value();
        comment.range_end_token = right_bytes.value();
        comment.range_start = Anchor(Identity(actor(1), 1), Bias::Left);
        comment.range_end = Anchor(Identity(actor(1), 2), Bias::Right);
        const auto entry = comment_to_entry(comment);
        QVERIFY(!comment_from_entry(entry).has_value()); // no implicit scope
        auto restored = comment_from_entry(entry, scope);
        QVERIFY(restored.ok());
        QCOMPARE(restored.value().range_start_token.value(), left_bytes.value());
        QCOMPARE(restored.value().range_end_token.value(), right_bytes.value());
        auto wrong_epoch = comment_from_entry(entry, CollabText::PositionScope{
            "doc-comments", "body", CollabText::Primitive::text, 4});
        QVERIFY(!wrong_epoch.ok());
        QCOMPARE(wrong_epoch.error().code(), CollabText::ErrorCode::wrong_epoch);
    }
};

QTEST_APPLESS_MAIN(TestComment)
#include "tst_comment.moc"
