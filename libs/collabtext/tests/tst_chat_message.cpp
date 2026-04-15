#include <QTest>
#include "crdt/ChatMessage.h"
#include "crdt/Anchor.h"

using namespace CollabText::Crdt;

class TestChatMessage : public QObject {
    Q_OBJECT
private slots:
    void round_trip() {
        ChatMessage msg;
        msg.id          = "1-42";
        msg.replica_id  = 1;
        msg.seq         = 42;
        msg.timestamp   = "2026-04-13T10:00:00Z";
        msg.author      = "alice@example.com";
        msg.author_name = "Alice";
        msg.body        = "Hello, world!";

        StreamEntry entry  = chat_message_to_entry(msg);
        auto result        = chat_message_from_entry(entry);

        QVERIFY(result.has_value());
        QCOMPARE(result->id,          msg.id);
        QCOMPARE(result->replica_id,  msg.replica_id);
        QCOMPARE(result->seq,         msg.seq);
        QCOMPARE(result->timestamp,   msg.timestamp);
        QCOMPARE(result->author,      msg.author);
        QCOMPARE(result->author_name, msg.author_name);
        QCOMPARE(result->body,        msg.body);
    }

    void payload_contains_expected_fields() {
        ChatMessage msg;
        msg.id          = "2-1";
        msg.replica_id  = 2;
        msg.seq         = 1;
        msg.timestamp   = "2026-04-13T11:00:00Z";
        msg.author      = "bob@example.com";
        msg.author_name = "Bob Smith";
        msg.body        = "Hi there";

        StreamEntry entry = chat_message_to_entry(msg);

        QVERIFY(entry.payload.find("bob@example.com") != std::string::npos);
        QVERIFY(entry.payload.find("Bob Smith")       != std::string::npos);
        QVERIFY(entry.payload.find("Hi there")        != std::string::npos);
    }

    void body_with_special_characters() {
        ChatMessage msg;
        msg.id          = "3-7";
        msg.replica_id  = 3;
        msg.seq         = 7;
        msg.timestamp   = "2026-04-13T12:00:00Z";
        msg.author      = "carol@example.com";
        msg.author_name = "Carol";
        // body contains newline, tab, and double-quote
        msg.body        = "line1\nline2\ttabbed\"quoted\"";

        StreamEntry entry = chat_message_to_entry(msg);
        auto result       = chat_message_from_entry(entry);

        QVERIFY(result.has_value());
        QCOMPARE(result->body, msg.body);
    }

    void invalid_payload_returns_nullopt() {
        StreamEntry entry;
        entry.id         = "bad-1";
        entry.replica_id = 1;
        entry.seq        = 1;
        entry.timestamp  = "2026-04-13T10:00:00Z";
        entry.payload    = "not valid json at all !!!";

        auto result = chat_message_from_entry(entry);
        QVERIFY(!result.has_value());
    }

    void round_trip_with_anchor() {
        ChatMessage msg;
        msg.id = "1-10";
        msg.replica_id = 1;
        msg.seq = 10;
        msg.timestamp = "2026-04-15T10:00:00Z";
        msg.author = "alice-a1b2c3";
        msg.author_name = "Alice";
        msg.body = "Check this paragraph";
        msg.anchor = Anchor(3, 200, Bias::Left);

        StreamEntry entry = chat_message_to_entry(msg);
        QVERIFY(entry.payload.find("anchor") != std::string::npos);

        auto decoded = chat_message_from_entry(entry);
        QVERIFY(decoded.has_value());
        QVERIFY(decoded->anchor.has_value());
        QCOMPARE(decoded->anchor->replica_id, uint16_t(3));
        QCOMPARE(decoded->anchor->char_value, uint32_t(200));
        QCOMPARE(decoded->anchor->bias, Bias::Left);
        QCOMPARE(decoded->body, msg.body);
        QCOMPARE(decoded->author, msg.author);
    }

    void round_trip_without_anchor_still_works() {
        ChatMessage msg;
        msg.id = "1-1";
        msg.replica_id = 1;
        msg.seq = 1;
        msg.timestamp = "2026-04-15T10:00:00Z";
        msg.author = "bob-x1y2z3";
        msg.author_name = "Bob";
        msg.body = "Plain message, no position attached";

        StreamEntry entry = chat_message_to_entry(msg);
        QVERIFY(entry.payload.find("anchor") == std::string::npos);

        auto decoded = chat_message_from_entry(entry);
        QVERIFY(decoded.has_value());
        QVERIFY(!decoded->anchor.has_value());
        QCOMPARE(decoded->body, msg.body);
    }
};

QTEST_APPLESS_MAIN(TestChatMessage)
#include "tst_chat_message.moc"
