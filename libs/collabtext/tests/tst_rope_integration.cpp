#include <QTest>
#include "crdt/Buffer.h"
#include <random>

using namespace CollabText::Crdt;

class TestRopeIntegration : public QObject {
    Q_OBJECT
private slots:

    void rope_tracks_inserts() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello"});
        QCOMPARE(buf.visible_rope_len(), 5u);
        QCOMPARE(buf.deleted_rope_len(), 0u);
        QCOMPARE(buf.text(), std::string("hello"));

        buf.apply_local_edit({{5, 5}}, {" world"});
        QCOMPARE(buf.visible_rope_len(), 11u);
        QCOMPARE(buf.deleted_rope_len(), 0u);
        QCOMPARE(buf.text(), std::string("hello world"));
    }

    void rope_tracks_deletes() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello world"});
        QCOMPARE(buf.visible_rope_len(), 11u);
        QCOMPARE(buf.deleted_rope_len(), 0u);

        // Delete " world"
        buf.apply_local_edit({{5, 11}}, {""});
        QCOMPARE(buf.visible_rope_len(), 5u);
        QCOMPARE(buf.deleted_rope_len(), 6u);
        QCOMPARE(buf.text(), std::string("hello"));
    }

    void rope_tracks_undo() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello world"});
        buf.apply_local_edit({{5, 11}}, {""}); // delete " world"
        QCOMPARE(buf.visible_rope_len(), 5u);
        QCOMPARE(buf.deleted_rope_len(), 6u);

        buf.undo(); // undo the delete — " world" becomes visible again
        QCOMPARE(buf.visible_rope_len(), 11u);
        QCOMPARE(buf.text(), std::string("hello world"));
    }

    void rope_tracks_redo() {
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"hello world"});
        buf.apply_local_edit({{5, 11}}, {""}); // delete " world"
        buf.undo();
        QCOMPARE(buf.visible_rope_len(), 11u);

        buf.redo(); // re-delete " world"
        QCOMPARE(buf.visible_rope_len(), 5u);
        QCOMPARE(buf.deleted_rope_len(), 6u);
        QCOMPARE(buf.text(), std::string("hello"));
    }

    void rope_tracks_remote_edit() {
        Buffer a(1), b(2);

        auto op1 = a.apply_local_edit({{0, 0}}, {"hello"});
        b.apply_ops({op1});
        QCOMPARE(b.visible_rope_len(), 5u);
        QCOMPARE(b.deleted_rope_len(), 0u);
        QCOMPARE(b.text(), std::string("hello"));

        // Remote replace: delete "hello", insert "world"
        auto op2 = a.apply_local_edit({{0, 5}}, {"world"});
        b.apply_ops({op2});
        QCOMPARE(b.text(), std::string("world"));
        QCOMPARE(b.visible_rope_len(), 5u);
        QCOMPARE(b.deleted_rope_len(), 5u);
    }

    void rope_consistency_invariant() {
        // 100 random edits — after each, assert rope byte totals match fragments.
        std::mt19937 rng(42);
        Buffer buf(1);
        buf.apply_local_edit({{0, 0}}, {"the quick brown fox jumps over the lazy dog"});

        for (int i = 0; i < 100; ++i) {
            uint32_t len = buf.visible_length();
            if (len == 0) {
                buf.apply_local_edit({{0, 0}}, {"x"});
            } else {
                std::string text = buf.text();
                uint32_t start = rng() % len;
                uint32_t end = start + (rng() % (len - start + 1));
                // Snap to UTF-8 boundaries
                while (start < text.size() &&
                       (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80)
                    start++;
                while (end < text.size() &&
                       (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
                    end++;
                if (end > static_cast<uint32_t>(text.size()))
                    end = static_cast<uint32_t>(text.size());
                if (start > end) start = end;

                std::string replacement = (rng() % 2) ? "XX" : "";
                buf.apply_local_edit({{start, end}}, {replacement});
            }

            // Rope totals must match fragment byte sums
            auto frags = buf.fragments();
            uint32_t total_bytes = 0;
            for (auto& f : frags)
                total_bytes += f.byte_length;
            QCOMPARE(buf.visible_rope_len() + buf.deleted_rope_len(), total_bytes);
            QCOMPARE(buf.visible_rope_len(), buf.visible_length());
        }
    }

    void rope_survives_convergence() {
        Buffer a(1), b(2), c(3);

        auto op1 = a.apply_local_edit({{0, 0}}, {"hello"});
        auto op2 = b.apply_local_edit({{0, 0}}, {"world"});

        // Cross-apply
        a.apply_ops({op2});
        b.apply_ops({op1});
        c.apply_ops({op1, op2});

        // All replicas converge
        QCOMPARE(a.text(), b.text());
        QCOMPARE(b.text(), c.text());

        // Rope consistency on every replica
        for (auto* buf : {&a, &b, &c}) {
            auto frags = buf->fragments();
            uint32_t total = 0;
            for (auto& f : frags)
                total += f.byte_length;
            QCOMPARE(buf->visible_rope_len() + buf->deleted_rope_len(), total);
            QCOMPARE(buf->visible_rope_len(), buf->visible_length());
        }
    }
};

QTEST_MAIN(TestRopeIntegration)
#include "tst_rope_integration.moc"
