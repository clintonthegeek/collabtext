#include <QApplication>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTest>
#include "ui/CollabPlainTextEdit.h"

using namespace CollabText::Ui;

class TestCollabPlainTextEdit : public QObject {
    Q_OBJECT

private slots:
    void widget_can_be_constructed() {
        CollabPlainTextEdit edit;
        QVERIFY(edit.document() != nullptr);
    }

    void top_visible_byte_offset_empty_doc() {
        CollabPlainTextEdit edit;
        edit.resize(200, 100);
        edit.show();
        QVERIFY(QTest::qWaitForWindowExposed(&edit));
        QCOMPARE(edit.topVisibleByteOffset(), 0u);
    }

    void top_visible_byte_offset_matches_cursor_for_position() {
        CollabPlainTextEdit edit;
        // 30 short ASCII lines, long enough to force scrolling in the
        // small viewport. Each line "L<i>\n" is 3 or 4 bytes.
        QString text;
        for (int i = 0; i < 30; ++i) text += QString("L%1\n").arg(i);
        edit.setPlainText(text);
        edit.resize(200, 80);
        edit.show();
        QVERIFY(QTest::qWaitForWindowExposed(&edit));

        // Scroll to roughly the middle of the document.
        auto *bar = edit.verticalScrollBar();
        bar->setValue(bar->maximum() / 2);
        QApplication::processEvents();

        // Ground truth: use Qt's own cursorForPosition at (0, 0) and
        // convert the resulting Qt char position to UTF-8 bytes ourselves.
        QTextCursor topCursor = edit.cursorForPosition(QPoint(0, 0));
        QString prefix = edit.toPlainText().left(topCursor.position());
        uint32_t expected = static_cast<uint32_t>(prefix.toUtf8().size());

        QCOMPARE(edit.topVisibleByteOffset(), expected);
    }

    void bottom_visible_byte_offset_empty_doc() {
        CollabPlainTextEdit edit;
        edit.resize(200, 100);
        edit.show();
        QVERIFY(QTest::qWaitForWindowExposed(&edit));
        QCOMPARE(edit.bottomVisibleByteOffset(), 0u);
    }

    void bottom_visible_byte_offset_greater_than_top_in_nonempty_doc() {
        CollabPlainTextEdit edit;
        QString text;
        for (int i = 0; i < 30; ++i) text += QString("L%1\n").arg(i);
        edit.setPlainText(text);
        edit.resize(200, 80);
        edit.show();
        QVERIFY(QTest::qWaitForWindowExposed(&edit));

        uint32_t top = edit.topVisibleByteOffset();
        uint32_t bottom = edit.bottomVisibleByteOffset();
        QVERIFY(bottom > top);
        QVERIFY(bottom <= static_cast<uint32_t>(edit.toPlainText().toUtf8().size()));
    }

    void bottom_visible_byte_offset_end_of_doc_when_scrolled_to_bottom() {
        CollabPlainTextEdit edit;
        QString text;
        for (int i = 0; i < 30; ++i) text += QString("L%1\n").arg(i);
        edit.setPlainText(text);
        edit.resize(200, 80);
        edit.show();
        QVERIFY(QTest::qWaitForWindowExposed(&edit));

        auto *bar = edit.verticalScrollBar();
        bar->setValue(bar->maximum());
        QApplication::processEvents();

        uint32_t expected = static_cast<uint32_t>(edit.toPlainText().toUtf8().size());
        QCOMPARE(edit.bottomVisibleByteOffset(), expected);
    }

    void scroll_byte_offset_to_top_roundtrip() {
        CollabPlainTextEdit edit;
        QString text;
        for (int i = 0; i < 50; ++i) text += QString("Line%1\n").arg(i);
        edit.setPlainText(text);
        edit.resize(200, 80);
        edit.show();
        QVERIFY(QTest::qWaitForWindowExposed(&edit));

        // Scroll to somewhere in the middle, capture the top byte offset.
        auto *bar = edit.verticalScrollBar();
        bar->setValue(bar->maximum() / 2);
        QApplication::processEvents();
        uint32_t captured = edit.topVisibleByteOffset();

        // Scroll away from that position, then restore.
        bar->setValue(0);
        QApplication::processEvents();
        edit.scrollByteOffsetToTop(captured, /*keepCursorVisible=*/false);
        QApplication::processEvents();

        QCOMPARE(edit.topVisibleByteOffset(), captured);
    }

    void scroll_byte_offset_to_top_keeps_cursor_visible() {
        CollabPlainTextEdit edit;
        QString text;
        for (int i = 0; i < 50; ++i) text += QString("Line%1\n").arg(i);
        edit.setPlainText(text);
        edit.resize(200, 80);
        edit.show();
        QVERIFY(QTest::qWaitForWindowExposed(&edit));

        // Put the local cursor at the end of the document (far below).
        QTextCursor tc = edit.textCursor();
        tc.movePosition(QTextCursor::End);
        edit.setTextCursor(tc);
        QApplication::processEvents();

        // Try to anchor the viewport at byte offset 0 (document start).
        // Without the safety net, the cursor would be scrolled off-screen.
        edit.scrollByteOffsetToTop(0, /*keepCursorVisible=*/true);
        QApplication::processEvents();

        // The cursor must be inside the viewport rectangle.
        QRect cursorR = edit.cursorRect(edit.textCursor());
        QRect viewR = edit.viewport()->rect();
        QVERIFY2(viewR.intersects(cursorR),
                 "cursor must remain visible when keepCursorVisible=true");
    }

    void scroll_byte_offset_to_top_ignores_cursor_when_flag_false() {
        CollabPlainTextEdit edit;
        QString text;
        for (int i = 0; i < 50; ++i) text += QString("Line%1\n").arg(i);
        edit.setPlainText(text);
        edit.resize(200, 80);
        edit.show();
        QVERIFY(QTest::qWaitForWindowExposed(&edit));

        // Cursor at end (far below).
        QTextCursor tc = edit.textCursor();
        tc.movePosition(QTextCursor::End);
        edit.setTextCursor(tc);
        QApplication::processEvents();

        // Scroll to a middle position first to have a baseline.
        auto *bar = edit.verticalScrollBar();
        bar->setValue(bar->maximum() / 2);
        QApplication::processEvents();

        // Anchor viewport at byte offset 0 with the safety net disabled.
        edit.scrollByteOffsetToTop(0, /*keepCursorVisible=*/false);
        QApplication::processEvents();

        // The top of the document must be what's shown, regardless of
        // where the cursor is.
        QCOMPARE(edit.topVisibleByteOffset(), 0u);
    }

    void viewport_scrolled_signal_fires_on_scroll() {
        CollabPlainTextEdit edit;
        QString text;
        for (int i = 0; i < 30; ++i) text += QString("L%1\n").arg(i);
        edit.setPlainText(text);
        edit.resize(200, 80);
        edit.show();
        QVERIFY(QTest::qWaitForWindowExposed(&edit));

        QSignalSpy spy(&edit, &CollabPlainTextEdit::viewportScrolled);
        QVERIFY(spy.isValid());

        auto *bar = edit.verticalScrollBar();
        bar->setValue(bar->maximum() / 2);
        QApplication::processEvents();

        QVERIFY(spy.count() >= 1);
    }
};

QTEST_MAIN(TestCollabPlainTextEdit)
#include "tst_collab_plain_text_edit.moc"
