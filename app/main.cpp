#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QRandomGenerator>
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
///
/// Integration pattern (following y-codemirror6 binding):
/// - Local edits: QTextDocument contentsChange → Buffer::apply_local_edit
/// - Remote edits: Buffer::apply_ops → edits_since() → surgical QTextCursor ops
/// - No full document replacement. Ever.
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
        auto *gremlinBtn = new QPushButton(QStringLiteral("Gremlin: OFF"), this);
        gremlinBtn->setCheckable(true);
        auto *bottomRow = new QHBoxLayout;
        bottomRow->addWidget(m_statusLabel);
        bottomRow->addStretch();
        bottomRow->addWidget(gremlinBtn);
        layout->addLayout(bottomRow);

        m_gremlinTimer = new QTimer(this);
        m_gremlinTimer->setInterval(80);
        connect(m_gremlinTimer, &QTimer::timeout, this, &EditorPane::gremlinTick);
        connect(gremlinBtn, &QPushButton::toggled, this, [this, gremlinBtn](bool on) {
            if (on) {
                m_gremlinTimer->start();
                gremlinBtn->setText(QStringLiteral("Gremlin: ON"));
            } else {
                m_gremlinTimer->stop();
                gremlinBtn->setText(QStringLiteral("Gremlin: OFF"));
            }
        });

        connect(m_edit->multiCursorController(),
                &MultiCursorController::cursorsChanged, this,
                [this]() {
                    int n = m_edit->multiCursorController()->cursorCount();
                    m_statusLabel->setText(
                        n > 1 ? QStringLiteral("%1 cursors").arg(n)
                              : QStringLiteral("1 cursor"));
                });

        // Local edits: QTextDocument → Buffer → FileSync
        connect(m_qtDoc, &QTextDocument::contentsChange,
                this, &EditorPane::onContentsChange);

        m_sync.start();
    }

    CollabPlainTextEdit *editor() const { return m_edit; }
    Buffer &buffer() { return m_buffer; }
    QColor cursorColor() const { return m_color; }
    QString label() const { return m_label; }

    /// Run one sync cycle. Saves version before polling, then applies
    /// remote edits surgically to the QTextDocument via edits_since().
    void poll() {
        Global before = m_buffer.version();
        size_t applied = m_sync.poll();
        if (applied > 0) {
            applyEditsToQt(m_buffer.edits_since(before));
        }
    }

    /// Convert a Qt character position to a byte offset.
    uint32_t qtPosToByteOffset(int qtPos) const {
        QString docText = m_qtDoc->toPlainText();
        return docText.left(qMin(qtPos, docText.length())).toUtf8().size();
    }

    /// Convert a byte offset to a Qt character position.
    int byteOffsetToQtPos(uint32_t byteOffset) const {
        // Use the QTextDocument text (which is in sync with the Buffer)
        QString docText = m_qtDoc->toPlainText();
        QByteArray utf8 = docText.toUtf8();
        uint32_t clamped = qMin(byteOffset, static_cast<uint32_t>(utf8.size()));
        return QString::fromUtf8(utf8.data(), clamped).length();
    }

private slots:
    void onContentsChange(int position, int charsRemoved, int charsAdded) {
        if (m_syncing) return;

        // Position conversion uses the Buffer text, which is in sync with
        // the QTextDocument (guaranteed because applyEditsToQt is synchronous
        // and surgical, and this signal only fires for local user edits).
        std::string bufText = m_buffer.text();
        QString qBufText = QString::fromStdString(bufText);

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

    void gremlinTick() {
        static const char *lorem[] = {
            "lorem ", "ipsum ", "dolor ", "sit ", "amet ", "consectetur ",
            "adipiscing ", "elit ", "sed ", "do ", "eiusmod ", "tempor ",
            "incididunt ", "ut ", "labore ", "et ", "dolore ", "magna ",
            "aliqua ", "enim ", "ad ", "minim ", "veniam ", "quis ",
            "nostrud ", "exercitation ", "ullamco ", "laboris ", "nisi ",
        };
        static constexpr int nwords = sizeof(lorem) / sizeof(lorem[0]);

        auto *rng = QRandomGenerator::global();
        uint32_t docLen = m_buffer.visible_length();
        int roll = rng->bounded(50);

        // Save version before the edit so we can get the delta
        Global before = m_buffer.version();
        Operation op;

        if (roll == 0 && docLen > 10) {
            uint32_t delLen = qMin(static_cast<uint32_t>(rng->bounded(3, 9)), docLen);
            uint32_t pos = rng->bounded(docLen - delLen + 1);
            op = m_buffer.apply_local_edit({{pos, pos + delLen}}, {""});
        } else if (roll < 3 && docLen > 0) {
            uint32_t pos = rng->bounded(docLen + 1);
            op = m_buffer.apply_local_edit({{pos, pos}}, {"\n"});
        } else {
            uint32_t pos = docLen;
            if (rng->bounded(5) == 0 && docLen > 0)
                pos = rng->bounded(docLen + 1);
            const char *word = lorem[rng->bounded(nwords)];
            op = m_buffer.apply_local_edit({{pos, pos}}, {word});
        }

        m_sync.push_local_op(op);

        // Apply the gremlin's edit to the QTextDocument surgically
        applyEditsToQt(m_buffer.edits_since(before));
    }

private:
    /// Apply a list of TextEdits from the engine to the QTextDocument
    /// as surgical QTextCursor operations. Each edit modifies only the
    /// changed region; Qt's automatic cursor adjustment handles all
    /// other cursors (primary, secondary, remote) without manual
    /// save/restore.
    void applyEditsToQt(const std::vector<TextEdit> &edits) {
        if (edits.empty()) return;
        m_syncing = true;

        // Apply in reverse order so earlier edits don't shift the
        // positions of later edits (same pattern as multi-cursor dispatch).
        for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
            int qtStart = byteOffsetToQtPos(it->old_start);
            int qtEnd = byteOffsetToQtPos(it->old_end);
            QTextCursor cursor(m_qtDoc);
            cursor.setPosition(qtStart);
            if (qtEnd > qtStart)
                cursor.setPosition(qtEnd, QTextCursor::KeepAnchor);
            QString replacement = QString::fromUtf8(
                it->new_text.data(),
                static_cast<int>(it->new_text.size()));
            cursor.insertText(replacement);
        }

        m_syncing = false;
    }

    bool m_syncing = false;
    Buffer m_buffer;
    FileSync m_sync;
    CollabPlainTextEdit *m_edit;
    QTextDocument *m_qtDoc;
    QColor m_color;
    QString m_label;
    QLabel *m_statusLabel;
    QTimer *m_gremlinTimer = nullptr;
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

        auto *syncTimer = new QTimer(this);
        connect(syncTimer, &QTimer::timeout, this, &MainWindow::syncCycle);
        syncTimer->start(100);

        statusBar()->showMessage(
            QStringLiteral("Full-stack sync via FileSync | Alt+Click: add cursor | "
                           "Ctrl+Alt+Up/Down: column cursor | Escape: clear"));
    }

private slots:
    void syncCycle() {
        m_paneA->poll();
        m_paneB->poll();

        syncRemoteCursor(m_paneA, m_paneB);
        syncRemoteCursor(m_paneB, m_paneA);
    }

    void syncRemoteCursor(EditorPane *from, EditorPane *to) {
        auto fromCursor = from->editor()->textCursor();

        uint32_t bytePos = from->qtPosToByteOffset(fromCursor.position());
        uint32_t byteAnchor = from->qtPosToByteOffset(fromCursor.anchor());

        auto posAnchor = from->buffer().anchor_at(bytePos, Bias::Right);
        auto selAnchor = from->buffer().anchor_at(byteAnchor, Bias::Left);

        uint32_t resolvedPos = to->buffer().resolve_anchor(posAnchor);
        uint32_t resolvedAnchor = to->buffer().resolve_anchor(selAnchor);

        RemoteCursor rc;
        rc.bytePosition = resolvedPos;
        rc.byteAnchor = resolvedAnchor;
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

    QTemporaryDir tmpDir;
    tmpDir.setAutoRemove(true);
    std::filesystem::path sharedFolder = tmpDir.path().toStdString();

    MainWindow window(sharedFolder);
    window.show();
    return app.exec();
}

#include "main.moc"
