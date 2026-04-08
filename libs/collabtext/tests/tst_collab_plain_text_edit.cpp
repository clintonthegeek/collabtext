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
};

QTEST_MAIN(TestCollabPlainTextEdit)
#include "tst_collab_plain_text_edit.moc"
