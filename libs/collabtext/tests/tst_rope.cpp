#include <QTest>
#include "crdt/Rope.h"

using namespace CollabText::Crdt;

class TestRope : public QObject {
    Q_OBJECT
private slots:

    void empty_rope() {
        Rope r;
        QVERIFY(r.empty());
        QCOMPARE(r.len(), 0u);
        QCOMPARE(r.to_string(), std::string(""));
    }

    void from_string() {
        auto r = Rope::from("hello world");
        QVERIFY(!r.empty());
        QCOMPARE(r.len(), 11u);
        QCOMPARE(r.to_string(), std::string("hello world"));
    }

    void push_str_small() {
        Rope r;
        r.push_str("abc");
        r.push_str("def");
        QCOMPARE(r.len(), 6u);
        QCOMPARE(r.to_string(), std::string("abcdef"));
    }

    void push_str_triggers_chunking() {
        Rope r;
        std::string big(500, 'x');
        r.push_str(big);
        QCOMPARE(r.len(), 500u);
        QCOMPARE(r.to_string(), big);
    }

    void append_ropes() {
        auto a = Rope::from("hello ");
        auto b = Rope::from("world");
        a.append(std::move(b));
        QCOMPARE(a.len(), 11u);
        QCOMPARE(a.to_string(), std::string("hello world"));
    }

    void substr_basic() {
        auto r = Rope::from("hello world");
        QCOMPARE(r.substr(0, 5), std::string("hello"));
        QCOMPARE(r.substr(6, 5), std::string("world"));
        QCOMPARE(r.substr(3, 4), std::string("lo w"));
    }

    void substr_empty() {
        auto r = Rope::from("hello");
        QCOMPARE(r.substr(2, 0), std::string(""));
    }

    void substr_entire() {
        auto r = Rope::from("hello");
        QCOMPARE(r.substr(0, 5), std::string("hello"));
    }

    void slice_to_basic() {
        auto r = Rope::from("hello world");
        auto prefix = r.slice_to(5);
        QCOMPARE(prefix.to_string(), std::string("hello"));
        QCOMPARE(r.to_string(), std::string(" world"));
    }

    void slice_to_zero() {
        auto r = Rope::from("hello");
        auto prefix = r.slice_to(0);
        QVERIFY(prefix.empty());
        QCOMPARE(r.to_string(), std::string("hello"));
    }

    void slice_to_full() {
        auto r = Rope::from("hello");
        auto prefix = r.slice_to(5);
        QCOMPARE(prefix.to_string(), std::string("hello"));
        QVERIFY(r.empty() || r.len() == 0);
    }

    void push_str_utf8_boundary() {
        Rope r;
        std::string text;
        for (int i = 0; i < 42; ++i) text += "aa";
        text += "\xe4\xb8\xad";
        text += "\xe4\xb8\xad";
        r.push_str(text);
        QCOMPARE(r.len(), static_cast<uint32_t>(text.size()));
        QCOMPARE(r.to_string(), text);
    }

    void substr_across_chunks() {
        Rope r;
        std::string big;
        for (int i = 0; i < 100; ++i) {
            std::string chunk(10, 'a' + (i % 26));
            r.push_str(chunk);
            big += chunk;
        }
        QCOMPARE(r.len(), 1000u);
        QCOMPARE(r.substr(125, 10), big.substr(125, 10));
        QCOMPARE(r.substr(0, 1000), big);
    }

    void slice_to_mid_chunk() {
        Rope r;
        std::string text(200, 'x');
        r.push_str(text);
        auto prefix = r.slice_to(75);
        QCOMPARE(prefix.len(), 75u);
        QCOMPARE(r.len(), 125u);
        QCOMPARE(prefix.to_string() + r.to_string(), text);
    }
};

QTEST_MAIN(TestRope)
#include "tst_rope.moc"
