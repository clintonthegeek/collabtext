#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <QTextCursor>
#include <QPlainTextDocumentLayout>

#include "crdt/Buffer.h"
#include "crdt/FileSync.h"
#include "ui/CollabPlainTextEdit.h"
#include "ui/MultiCursorController.h"

using namespace CollabText::Crdt;
using namespace CollabText::Ui;

/// A single editor pane with its own CRDT Buffer and FileSync.
/// Edits in the QTextDocument are intercepted and fed to the Buffer,
/// which produces Operations for FileSync. Remote ops arriving via
/// FileSync update the Buffer, which updates the QTextDocument.
class EditorPane : public QWidget {
    Q_OBJECT
public:
    EditorPane(const QString &label, uint16_t replicaId,
               const QString &replicaName,
               const std::filesystem::path &sharedFolder,
               const QColor &cursorColor, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_buffer(replicaId)
        , m_sync(m_buffer, sharedFolder, replicaName.toStdString())
        , m_edit(new CollabPlainTextEdit(this))
        , m_qtDoc(new QTextDocument(this))
        , m_color(cursorColor)
        , m_label(label)
    {
        m_qtDoc->setDocumentLayout(new QPlainTextDocumentLayout(m_qtDoc));
        m_qtDoc->setUndoRedoEnabled(false);
        m_edit->setDocument(m_qtDoc);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);

        auto *header = new QLabel(label, this);
        header->setStyleSheet(QStringLiteral("font-weight: bold; color: %1;")
                                  .arg(cursorColor.name()));
        layout->addWidget(header);
        layout->addWidget(m_edit);

        m_edit->setPlaceholderText(
            QStringLiteral("Type here... (Alt+Click for multi-cursor, "
                           "Ctrl+Alt+Up/Down to add cursors)"));

        m_statusLabel = new QLabel(this);
        layout->addWidget(m_statusLabel);

        connect(m_edit->multiCursorController(),
                &MultiCursorController::cursorsChanged, this,
                [this]() {
                    int n = m_edit->multiCursorController()->cursorCount();
                    m_statusLabel->setText(
                        n > 1 ? QStringLiteral("%1 cursors").arg(n)
                              : QStringLiteral("1 cursor"));
                });

        // Intercept QTextDocument changes → feed to CRDT Buffer → push to FileSync
        connect(m_qtDoc, &QTextDocument::contentsChange,
                this, &EditorPane::onContentsChange);

        m_sync.start();
        m_sync.set_on_remote_ops([this](size_t) {
            // Remote ops applied to Buffer — sync Buffer text back to QTextDocument
            QMetaObject::invokeMethod(this, &EditorPane::syncBufferToQt,
                                      Qt::QueuedConnection);
        });
    }

    CollabPlainTextEdit *editor() const { return m_edit; }
    Buffer &buffer() { return m_buffer; }
    FileSync &fileSync() { return m_sync; }
    QColor cursorColor() const { return m_color; }
    QString label() const { return m_label; }

    /// Called by the main window's sync timer.
    void poll() {
        m_sync.poll();
    }

private slots:
    void onContentsChange(int position, int charsRemoved, int charsAdded) {
        if (m_syncing) return;

        // Convert UTF-16 positions to byte offsets in the Buffer's text
        std::string bufText = m_buffer.text();
        QString qBufText = QString::fromStdString(bufText);

        // Qt positions are in UTF-16 code units
        uint32_t byteStart = qBufText.left(position).toUtf8().size();
        uint32_t byteEnd = byteStart;
        if (charsRemoved > 0) {
            byteEnd = qBufText.left(position + charsRemoved).toUtf8().size();
        }

        std::string inserted;
        if (charsAdded > 0) {
            QTextCursor cursor(m_qtDoc);
            cursor.setPosition(position);
            cursor.setPosition(position + charsAdded, QTextCursor::KeepAnchor);
            QString sel = cursor.selectedText();
            sel.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
            inserted = sel.toStdString();
        }

        auto op = m_buffer.apply_local_edit({{byteStart, byteEnd}}, {inserted});
        m_sync.push_local_op(op);
    }

    void syncBufferToQt() {
        m_syncing = true;
        QString newText = QString::fromStdString(m_buffer.text());
        QString oldText = m_qtDoc->toPlainText();
        if (newText != oldText) {
            // Save cursor as a CRDT anchor (stable through edits)
            int qtPos = m_edit->textCursor().position();
            QString qBufText = QString::fromStdString(m_buffer.text());
            uint32_t bytePos = qBufText.left(qMin(qtPos, qBufText.length())).toUtf8().size();
            auto anchor = m_buffer.anchor_at(bytePos, Bias::Right);

            // Replace document content
            QTextCursor cursor(m_qtDoc);
            cursor.select(QTextCursor::Document);
            cursor.insertText(newText);

            // Resolve anchor back to a position in the updated document
            uint32_t newBytePos = m_buffer.resolve_anchor(anchor);
            // Convert byte offset back to Qt character position
            std::string bufText = m_buffer.text();
            QString prefix = QString::fromUtf8(bufText.data(), newBytePos);
            int newQtPos = prefix.length();

            QTextCursor restored(m_qtDoc);
            restored.setPosition(qMin(newQtPos, m_qtDoc->characterCount() - 1));
            m_edit->setTextCursor(restored);
        }
        m_syncing = false;
    }

private:
    bool m_syncing = false;
    Buffer m_buffer;
    FileSync m_sync;
    CollabPlainTextEdit *m_edit;
    QTextDocument *m_qtDoc;
    QColor m_color;
    QString m_label;
    QLabel *m_statusLabel;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const std::filesystem::path &sharedFolder)
    {
        setWindowTitle(QStringLiteral("CollabText — Full-Stack Demo"));
        resize(1000, 600);

        auto *central = new QWidget(this);
        auto *layout = new QHBoxLayout(central);

        m_paneA = new EditorPane(QStringLiteral("Alice"), 1,
                                  QStringLiteral("alice"),
                                  sharedFolder,
                                  QColor(65, 105, 225), central);
        m_paneB = new EditorPane(QStringLiteral("Bob"), 2,
                                  QStringLiteral("bob"),
                                  sharedFolder,
                                  QColor(220, 20, 60), central);
        layout->addWidget(m_paneA);
        layout->addWidget(m_paneB);
        setCentralWidget(central);

        // Sync timer: polls FileSync for both editors + exchanges cursor positions
        auto *syncTimer = new QTimer(this);
        connect(syncTimer, &QTimer::timeout, this, &MainWindow::syncCycle);
        syncTimer->start(100);

        statusBar()->showMessage(
            QStringLiteral("Full-stack sync via FileSync | Alt+Click: add cursor | "
                           "Ctrl+Alt+Up/Down: column cursor | Escape: clear"));
    }

private slots:
    void syncCycle() {
        // Poll both FileSyncs (flush local ops, read remote ops)
        m_paneA->poll();
        m_paneB->poll();

        // Exchange cursor positions as remote cursors.
        // Positions are in the Buffer's byte space — convert to Qt positions.
        syncRemoteCursor(m_paneA, m_paneB);
        syncRemoteCursor(m_paneB, m_paneA);
    }

    void syncRemoteCursor(EditorPane *from, EditorPane *to) {
        auto fromCursor = from->editor()->textCursor();
        int qtPos = fromCursor.position();
        int qtAnchor = fromCursor.anchor();

        // Clamp to the target document's length
        int maxPos = to->editor()->document()->characterCount() - 1;
        if (maxPos < 0) maxPos = 0;

        RemoteCursor rc;
        rc.position = qMin(qtPos, maxPos);
        rc.anchor = qMin(qtAnchor, maxPos);
        rc.color = from->cursorColor();
        rc.label = from->label();

        to->editor()->multiCursorController()->setRemoteCursors({rc});
    }

private:
    EditorPane *m_paneA;
    EditorPane *m_paneB;
};

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // Shared folder for FileSync (temporary for this demo)
    QTemporaryDir tmpDir;
    tmpDir.setAutoRemove(true);
    std::filesystem::path sharedFolder = tmpDir.path().toStdString();

    MainWindow window(sharedFolder);
    window.show();
    return app.exec();
}

#include "main.moc"
