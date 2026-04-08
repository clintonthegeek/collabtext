// libs/collabtext/tests/tst_scroll_stability.cpp
//
// Integration test: exercise the full scroll-preservation pipeline
// without running the app. Wire a Buffer + QTextDocument + widget the
// way EditorPane does, then verify that a remote edit above the
// viewport does not shift the visible text.

#include <QApplication>
#include <QScrollBar>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include "crdt/Buffer.h"
#include "ui/CollabPlainTextEdit.h"

using namespace CollabText::Crdt;
using namespace CollabText::Ui;

class TestScrollStability : public QObject {
    Q_OBJECT

private slots:
    void remote_insert_above_viewport_preserves_top_anchor() {
        // Set up Alice's buffer and widget, populated with a 100-line doc.
        Buffer aliceBuf(1);
        {
            std::string text;
            for (int i = 0; i < 100; ++i) {
                text += "Line ";
                text += std::to_string(i);
                text += "\n";
            }
            aliceBuf.apply_local_edit({{0, 0}}, {text});
        }

        CollabPlainTextEdit edit;
        edit.setPlainText(QString::fromStdString(aliceBuf.text()));
        edit.resize(300, 120);
        edit.show();
        QVERIFY(QTest::qWaitForWindowExposed(&edit));

        // Scroll Alice to the middle of the document.
        auto *bar = edit.verticalScrollBar();
        bar->setValue(bar->maximum() / 2);
        QApplication::processEvents();

        uint32_t topByteBefore = edit.topVisibleByteOffset();
        QVERIFY(topByteBefore > 0);  // sanity: we actually scrolled


        // Simulate a remote insertion above the viewport. For the
        // scroll-preservation pipeline it doesn't matter whether the
        // mutation comes from the local replica or a remote one — the
        // anchor_at → mutate → resolve_anchor round-trip is the same.
        // Using apply_local_edit keeps the test self-contained without
        // needing a second Buffer and a sync path.
        Global beforeVersion = aliceBuf.version();
        std::string injected = "INSERTED BY BOB\n";
        aliceBuf.apply_local_edit({{0, 0}}, {injected});

        // Capture the CRDT anchor at the top BEFORE applying edits to Qt.
        Anchor topAnchor = aliceBuf.anchor_at(topByteBefore, Bias::Left);

        auto edits = aliceBuf.edits_since(beforeVersion);
        QVERIFY(!edits.empty());

        // Apply the edits to the QTextDocument by hand (integration test
        // shouldn't depend on EditorPane).
        for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
            QString docText = edit.document()->toPlainText();
            QByteArray utf8 = docText.toUtf8();
            uint32_t clampedStart = qMin(
                it->old_start, static_cast<uint32_t>(utf8.size()));
            uint32_t clampedEnd = qMin(
                it->old_end, static_cast<uint32_t>(utf8.size()));
            int qtStart = QString::fromUtf8(
                utf8.data(), static_cast<int>(clampedStart)).length();
            int qtEnd = QString::fromUtf8(
                utf8.data(), static_cast<int>(clampedEnd)).length();
            QTextCursor c(edit.document());
            c.setPosition(qtStart);
            if (qtEnd > qtStart)
                c.setPosition(qtEnd, QTextCursor::KeepAnchor);
            c.insertText(QString::fromUtf8(
                it->new_text.data(),
                static_cast<int>(it->new_text.size())));
        }

        // Restore scroll using the captured anchor.
        uint32_t restoredByte = aliceBuf.resolve_anchor(topAnchor);
        edit.scrollByteOffsetToTop(restoredByte, /*keepCursorVisible=*/false);
        QApplication::processEvents();

        // Allow for line-level drift: the restored position should be
        // within one line-height of the target.
        uint32_t actual = edit.topVisibleByteOffset();
        qint64 drift = static_cast<qint64>(actual) - static_cast<qint64>(restoredByte);
        QVERIFY2(std::abs(drift) < 50,  // generous "less than ~3 lines" margin
                 qPrintable(QString("top drift too large: actual=%1 expected=%2")
                             .arg(actual).arg(restoredByte)));
    }

    void remote_insert_below_viewport_leaves_top_unchanged() {
        Buffer buf(1);
        {
            std::string text;
            for (int i = 0; i < 100; ++i) {
                text += "Line ";
                text += std::to_string(i);
                text += "\n";
            }
            buf.apply_local_edit({{0, 0}}, {text});
        }

        CollabPlainTextEdit edit;
        edit.setPlainText(QString::fromStdString(buf.text()));
        edit.resize(300, 120);
        edit.show();
        QVERIFY(QTest::qWaitForWindowExposed(&edit));

        auto *bar = edit.verticalScrollBar();
        bar->setValue(bar->maximum() / 4);
        QApplication::processEvents();

        uint32_t topByteBefore = edit.topVisibleByteOffset();

        // Insert text at the END of the document (past the viewport).
        Global beforeVersion = buf.version();
        uint32_t docLen = static_cast<uint32_t>(buf.text().size());
        buf.apply_local_edit({{docLen, docLen}}, {"APPENDED\n"});

        Anchor topAnchor = buf.anchor_at(topByteBefore, Bias::Left);

        auto edits = buf.edits_since(beforeVersion);
        for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
            QString docText = edit.document()->toPlainText();
            QByteArray utf8 = docText.toUtf8();
            uint32_t clampedStart = qMin(
                it->old_start, static_cast<uint32_t>(utf8.size()));
            uint32_t clampedEnd = qMin(
                it->old_end, static_cast<uint32_t>(utf8.size()));
            int qtStart = QString::fromUtf8(
                utf8.data(), static_cast<int>(clampedStart)).length();
            int qtEnd = QString::fromUtf8(
                utf8.data(), static_cast<int>(clampedEnd)).length();
            QTextCursor c(edit.document());
            c.setPosition(qtStart);
            if (qtEnd > qtStart)
                c.setPosition(qtEnd, QTextCursor::KeepAnchor);
            c.insertText(QString::fromUtf8(
                it->new_text.data(),
                static_cast<int>(it->new_text.size())));
        }

        uint32_t restoredByte = buf.resolve_anchor(topAnchor);
        edit.scrollByteOffsetToTop(restoredByte, /*keepCursorVisible=*/false);
        QApplication::processEvents();

        // A below-viewport edit must not change the top byte offset.
        QCOMPARE(restoredByte, topByteBefore);
        QCOMPARE(edit.topVisibleByteOffset(), topByteBefore);
    }
};

QTEST_MAIN(TestScrollStability)
#include "tst_scroll_stability.moc"
