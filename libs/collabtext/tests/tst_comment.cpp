#include <QTest>
#include "crdt/Comment.h"
#include "crdt/Anchor.h"

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
        comment.range_start = Anchor(2, 100, Bias::Left);
        comment.range_end   = Anchor(3, 200, Bias::Right);

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
        QCOMPARE(result->range_start.replica_id, uint16_t(2));
        QCOMPARE(result->range_start.char_value, uint32_t(100));
        QCOMPARE(result->range_start.bias,       Bias::Left);
        QCOMPARE(result->range_end.replica_id,   uint16_t(3));
        QCOMPARE(result->range_end.char_value,   uint32_t(200));
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
        comment.range_start = Anchor(1, 50, Bias::Left);
        comment.range_end   = Anchor(1, 80, Bias::Right);

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
};

QTEST_APPLESS_MAIN(TestComment)
#include "tst_comment.moc"
