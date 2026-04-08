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
};

QTEST_MAIN(TestCollabPlainTextEdit)
#include "tst_collab_plain_text_edit.moc"
