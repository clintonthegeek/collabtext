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

#include "crdt/Anchor.h"
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
        auto *gremlinBtn = new QPushButton(QStringLiteral("Gremlin: OFF"), this);
        gremlinBtn->setCheckable(true);
        auto *bottomRow = new QHBoxLayout;
        bottomRow->addWidget(m_statusLabel);
        bottomRow->addStretch();
        bottomRow->addWidget(gremlinBtn);
        layout->addLayout(bottomRow);

        m_gremlinTimer = new QTimer(this);
        m_gremlinTimer->setInterval(80); // ~12 chars/sec, fast typist
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

        connect(m_qtDoc, &QTextDocument::contentsChange,
                this, &EditorPane::onContentsChange);

        m_sync.start();
        m_sync.set_on_remote_ops([this](size_t) {
            // Sync Buffer → QTextDocument immediately. Must be synchronous
            // so the two models never diverge — if the user types between
            // apply_ops and syncBufferToQt, onContentsChange would compute
            // wrong byte offsets from the stale QTextDocument.
            syncBufferToQt();
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

    /// Convert a Qt character position to a byte offset in the Buffer's text.
    uint32_t qtPosToByteOffset(int qtPos) const {
        QString docText = m_qtDoc->toPlainText();
        return docText.left(qMin(qtPos, docText.length())).toUtf8().size();
    }

    /// Convert a byte offset to a Qt character position.
    int byteOffsetToQtPos(uint32_t byteOffset) const {
        std::string bufText = m_buffer.text();
        uint32_t clamped = qMin(byteOffset, static_cast<uint32_t>(bufText.size()));
        return QString::fromUtf8(bufText.data(), clamped).length();
    }

private slots:
    void onContentsChange(int position, int charsRemoved, int charsAdded) {
        if (m_syncing) return;

        // Convert UTF-16 positions to byte offsets in the Buffer's text.
        // Safe because syncBufferToQt is synchronous — the Buffer and
        // QTextDocument are always in sync when this fires for user edits.
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

    void syncBufferToQt() {
        m_syncing = true;
        QString newText = QString::fromStdString(m_buffer.text());
        QString oldText = m_qtDoc->toPlainText();
        if (newText != oldText) {
            // Save ALL cursors (primary + secondary) as CRDT anchors
            auto *ctrl = m_edit->multiCursorController();
            auto allCursors = ctrl->allCursors();

            struct SavedCursor {
                Anchor posAnchor;
                Anchor selAnchor;  // selection anchor (if different from pos)
                bool hasSelection;
            };
            QList<SavedCursor> saved;
            saved.reserve(allCursors.size());
            for (auto &c : allCursors) {
                SavedCursor sc;
                uint32_t bytePos = qtPosToByteOffset(c.position());
                sc.posAnchor = m_buffer.anchor_at(bytePos, Bias::Right);
                sc.hasSelection = c.hasSelection();
                if (sc.hasSelection) {
                    uint32_t byteAnchor = qtPosToByteOffset(c.anchor());
                    sc.selAnchor = m_buffer.anchor_at(byteAnchor, Bias::Left);
                }
                saved.append(sc);
            }

            // Replace document content
            QTextCursor cursor(m_qtDoc);
            cursor.select(QTextCursor::Document);
            cursor.insertText(newText);

            // Restore all cursors from anchors
            int maxPos = m_qtDoc->characterCount() - 1;
            if (maxPos < 0) maxPos = 0;

            // Restore primary
            if (!saved.isEmpty()) {
                int newPos = qMin(byteOffsetToQtPos(m_buffer.resolve_anchor(saved[0].posAnchor)), maxPos);
                QTextCursor primary(m_qtDoc);
                if (saved[0].hasSelection) {
                    int newAnchor = qMin(byteOffsetToQtPos(m_buffer.resolve_anchor(saved[0].selAnchor)), maxPos);
                    primary.setPosition(newAnchor);
                    primary.setPosition(newPos, QTextCursor::KeepAnchor);
                } else {
                    primary.setPosition(newPos);
                }
                m_edit->setTextCursor(primary);
                ctrl->setPrimaryCursor(primary);
            }

            // Restore secondary cursors
            ctrl->clearSecondaryCursors();
            for (int i = 1; i < saved.size(); ++i) {
                int newPos = qMin(byteOffsetToQtPos(m_buffer.resolve_anchor(saved[i].posAnchor)), maxPos);
                ctrl->addCursorAt(newPos);
            }
        }
        m_syncing = false;
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

        if (roll == 0 && docLen > 10) {
            // 1/50: backspace a few words (3-8 chars) at a random position
            uint32_t delLen = qMin(static_cast<uint32_t>(rng->bounded(3, 9)), docLen);
            uint32_t pos = rng->bounded(docLen - delLen + 1);
            auto op = m_buffer.apply_local_edit({{pos, pos + delLen}}, {""});
            m_sync.push_local_op(op);
        } else if (roll < 3 && docLen > 0) {
            // 2/50: move cursor to a random position (just insert a newline)
            uint32_t pos = rng->bounded(docLen + 1);
            auto op = m_buffer.apply_local_edit({{pos, pos}}, {"\n"});
            m_sync.push_local_op(op);
        } else {
            // 47/50: type a word at the end (or at a random spot 1/5 of the time)
            uint32_t pos = docLen;
            if (rng->bounded(5) == 0 && docLen > 0)
                pos = rng->bounded(docLen + 1);
            const char *word = lorem[rng->bounded(nwords)];
            auto op = m_buffer.apply_local_edit({{pos, pos}}, {word});
            m_sync.push_local_op(op);
        }

        // Update the QTextDocument to reflect the gremlin's edit
        syncBufferToQt();
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

        // Convert Qt positions to byte offsets in the sender's Buffer
        uint32_t bytePos = from->qtPosToByteOffset(fromCursor.position());
        uint32_t byteAnchor = from->qtPosToByteOffset(fromCursor.anchor());

        // Create CRDT anchors in the sender's Buffer, then resolve them
        // in the receiver's Buffer. This handles the case where the two
        // buffers are temporarily out of sync (different ops applied).
        auto posAnchor = from->buffer().anchor_at(bytePos, Bias::Right);
        auto selAnchor = from->buffer().anchor_at(byteAnchor, Bias::Left);

        // Resolve in receiver's buffer space
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

    // Shared folder for FileSync (temporary for this demo)
    QTemporaryDir tmpDir;
    tmpDir.setAutoRemove(true);
    std::filesystem::path sharedFolder = tmpDir.path().toStdString();

    MainWindow window(sharedFolder);
    window.show();
    return app.exec();
}

#include "main.moc"
