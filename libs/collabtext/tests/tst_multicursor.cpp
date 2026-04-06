#include <QTest>
#include <QTextDocument>
#include "ui/MultiCursorController.h"

using namespace CollabText::Ui;

class TestMultiCursor : public QObject {
    Q_OBJECT

private slots:
    void single_cursor_insert() {
        QTextDocument doc;
        MultiCursorController ctrl(&doc);
        ctrl.insertText("hello");
        QCOMPARE(doc.toPlainText(), QString("hello"));
        QCOMPARE(ctrl.cursorCount(), 1);
    }

    void two_cursors_insert() {
        QTextDocument doc;
        doc.setPlainText("aa bb");
        MultiCursorController ctrl(&doc);

        QTextCursor primary(&doc);
        primary.setPosition(2);
        ctrl.setPrimaryCursor(primary);
        ctrl.addCursorAt(5);
        QCOMPARE(ctrl.cursorCount(), 2);

        ctrl.insertText("X");
        QCOMPARE(doc.toPlainText(), QString("aaX bbX"));
    }

    void three_cursors_insert() {
        QTextDocument doc;
        doc.setPlainText("aaa bbb ccc");
        MultiCursorController ctrl(&doc);

        QTextCursor primary(&doc);
        primary.setPosition(3);
        ctrl.setPrimaryCursor(primary);
        ctrl.addCursorAt(7);
        ctrl.addCursorAt(11);
        QCOMPARE(ctrl.cursorCount(), 3);

        ctrl.insertText("!");
        QCOMPARE(doc.toPlainText(), QString("aaa! bbb! ccc!"));
    }

    void multi_cursor_backspace() {
        QTextDocument doc;
        doc.setPlainText("aaX bbX");
        MultiCursorController ctrl(&doc);

        QTextCursor primary(&doc);
        primary.setPosition(3);
        ctrl.setPrimaryCursor(primary);
        ctrl.addCursorAt(7);

        ctrl.deletePreviousChar();
        QCOMPARE(doc.toPlainText(), QString("aa bb"));
    }

    void multi_cursor_delete() {
        QTextDocument doc;
        doc.setPlainText("aXa bXb");
        MultiCursorController ctrl(&doc);

        QTextCursor primary(&doc);
        primary.setPosition(1);
        ctrl.setPrimaryCursor(primary);
        ctrl.addCursorAt(5);

        ctrl.deleteChar();
        QCOMPARE(doc.toPlainText(), QString("aa bb"));
    }

    void clear_secondary_cursors() {
        QTextDocument doc;
        doc.setPlainText("hello");
        MultiCursorController ctrl(&doc);
        ctrl.addCursorAt(1);
        ctrl.addCursorAt(3);
        QVERIFY(ctrl.cursorCount() >= 2);
        ctrl.clearSecondaryCursors();
        QCOMPARE(ctrl.cursorCount(), 1);
    }

    void move_cursors() {
        QTextDocument doc;
        doc.setPlainText("hello\nworld");
        MultiCursorController ctrl(&doc);

        QTextCursor primary(&doc);
        primary.setPosition(0);
        ctrl.setPrimaryCursor(primary);
        ctrl.addCursorAt(6);

        ctrl.moveCursors(QTextCursor::EndOfLine);

        auto cursors = ctrl.allCursors();
        QCOMPARE(cursors[0].position(), 5);
        bool foundEnd = false;
        for (int i = 1; i < cursors.size(); ++i) {
            if (cursors[i].position() == 11) foundEnd = true;
        }
        QVERIFY(foundEnd);
    }

    void duplicate_cursors_merged() {
        QTextDocument doc;
        doc.setPlainText("hello");
        MultiCursorController ctrl(&doc);

        QTextCursor primary(&doc);
        primary.setPosition(3);
        ctrl.setPrimaryCursor(primary);
        ctrl.addCursorAt(3);
        QCOMPARE(ctrl.cursorCount(), 1);
    }

    void undo_reverses_multi_insert() {
        QTextDocument doc;
        doc.setPlainText("aa bb");
        MultiCursorController ctrl(&doc);

        QTextCursor primary(&doc);
        primary.setPosition(2);
        ctrl.setPrimaryCursor(primary);
        ctrl.addCursorAt(5);

        ctrl.insertText("X");
        QCOMPARE(doc.toPlainText(), QString("aaX bbX"));

        doc.undo();
        QCOMPARE(doc.toPlainText(), QString("aa bb"));
    }

    void secondary_selections_generated() {
        QTextDocument doc;
        doc.setPlainText("hello world");
        MultiCursorController ctrl(&doc);

        QTextCursor primary(&doc);
        primary.setPosition(0);
        primary.setPosition(5, QTextCursor::KeepAnchor);
        ctrl.setPrimaryCursor(primary);

        ctrl.addCursorAt(6);

        auto sels = ctrl.secondarySelections();
        QCOMPARE(sels.size(), 0);
    }

    void remote_cursors() {
        QTextDocument doc;
        doc.setPlainText("hello world");
        MultiCursorController ctrl(&doc);

        QList<RemoteCursor> remotes;
        remotes.append({5, 5, Qt::red, "Alice"});
        remotes.append({3, 8, Qt::blue, "Bob"});
        ctrl.setRemoteCursors(remotes);

        auto sels = ctrl.remoteSelections();
        QCOMPARE(sels.size(), 2);
        QCOMPARE(ctrl.remoteCursors().size(), 2);
    }

    void add_cursor_below() {
        QTextDocument doc;
        doc.setPlainText("aaa\nbbb\nccc");
        MultiCursorController ctrl(&doc);

        QTextCursor primary(&doc);
        primary.setPosition(1);
        ctrl.setPrimaryCursor(primary);

        ctrl.addCursorBelow();
        QCOMPARE(ctrl.cursorCount(), 2);

        ctrl.addCursorBelow();
        QCOMPARE(ctrl.cursorCount(), 3);

        ctrl.insertText("X");
        QCOMPARE(doc.toPlainText(), QString("aXaa\nbXbb\ncXcc"));
    }
};

QTEST_MAIN(TestMultiCursor)
#include "tst_multicursor.moc"
